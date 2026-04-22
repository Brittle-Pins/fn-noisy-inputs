#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

static const char *TAG = "RAILWAY_SYSTEM";

// --- Pin Definitions ---
#define HALL_SENSOR_GPIO 27
#define HALL_ANALOG_GPIO  34
#define HALL_ANALOG_CHANNEL ADC1_CHANNEL_6
#define BUTTON_GPIO      26
#define TOGGLE_GPIO      25
#define SERVO_GPIO       21
#define BUILTIN_LED      13 // Adafruit Feather Built-in LED
#define TRIG_GPIO        33
#define ECHO_GPIO        32
#define LED_R            14
#define LED_G            15
#define LED_B            12

// Ultrasonic measurement constraints
#define DIST_MIN_CM          1.0f
#define DIST_MAX_CM          70.0f
#define SAMPLE_PERIOD_MS     100
#define SAMPLE_PERIOD_US     (SAMPLE_PERIOD_MS * 1000)
#define DIST_SAMPLE_COUNT    15
#define INVALID_SAMPLE_THRESHOLD 8 // Mark run invalid when >= 8/15 samples are rejected

// --- Servo PWM Settings ---
#define SERVO_MIN_PULSEWIDTH_US 500  // Minimum pulse width in microsecond (0 degrees)
#define SERVO_MAX_PULSEWIDTH_US 2500 // Maximum pulse width in microsecond (180 degrees)
#define SERVO_MAX_DEGREE        180  // Maximum angle
#define SERVO_OPEN_ANGLE        0   // Angle to keep gate open
#define SERVO_CLOSE_ANGLE       180    // Angle to close gate
#define SERVO_CLOSE_APPROACH_ANGLE 176 // Stop a few degrees before hard end-stop
#define SERVO_RAMP_STEP_DEG     2      // Smaller steps reduce jerk and current spikes
#define SERVO_RAMP_STEP_MS      20
#define SERVO_SETTLE_MS         200
#define SERVO_RELEASE_PWM_AFTER_MOVE 1 // 1 = lowest buzz/current, 0 = actively hold position

// Hall module output is active-high on this board: D0 rises when the magnet is present.
// Use a shorter debounce so a fast moving train can still trigger reliably.
#define HALL_TRIGGER_DEBOUNCE_US 150000

// --- System States & Modes ---
typedef enum {
    MODE_COLLECTION,
    MODE_INFERENCE
} system_mode_t;

typedef enum {
    STATE_WAITING,
    STATE_READING,
    STATE_ACTION // Previously STATE_SAVED
} system_state_t;

volatile system_state_t current_state = STATE_WAITING;

// Debounce variables
volatile int64_t last_hall_trigger = 0;
volatile int64_t last_btn_trigger = 0;
volatile bool hall_sensor_armed = true;
static int current_servo_angle = SERVO_OPEN_ANGLE;

// --- Helpers ---
void set_rgb_color(int r, int g, int b) {
    gpio_set_level(LED_R, r);
    gpio_set_level(LED_G, g);
    gpio_set_level(LED_B, b);
}

void set_servo_angle(int angle) {
    if (angle < 0) angle = 0;
    if (angle > SERVO_MAX_DEGREE) angle = SERVO_MAX_DEGREE;

    uint32_t duty = (uint32_t)(((angle * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US)) / SERVO_MAX_DEGREE) + SERVO_MIN_PULSEWIDTH_US);
    // Convert microseconds to 13-bit duty cycle for 50Hz (20000us period)
    uint32_t duty_13bit = (duty * 8191) / 20000;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_13bit);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    current_servo_angle = angle;
}

void servo_release_pwm(void) {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

void move_servo_smooth(int target_angle) {
    if (target_angle < 0) target_angle = 0;
    if (target_angle > SERVO_MAX_DEGREE) target_angle = SERVO_MAX_DEGREE;

    if (target_angle > current_servo_angle) {
        for (int angle = current_servo_angle; angle <= target_angle; angle += SERVO_RAMP_STEP_DEG) {
            set_servo_angle(angle);
            vTaskDelay(pdMS_TO_TICKS(SERVO_RAMP_STEP_MS));
        }
    } else if (target_angle < current_servo_angle) {
        for (int angle = current_servo_angle; angle >= target_angle; angle -= SERVO_RAMP_STEP_DEG) {
            set_servo_angle(angle);
            vTaskDelay(pdMS_TO_TICKS(SERVO_RAMP_STEP_MS));
        }
    }

    set_servo_angle(target_angle);
}

void command_gate_open(void) {
    move_servo_smooth(SERVO_OPEN_ANGLE);
#if SERVO_RELEASE_PWM_AFTER_MOVE
    vTaskDelay(pdMS_TO_TICKS(SERVO_SETTLE_MS));
    servo_release_pwm();
#endif
}

void command_gate_close(void) {
    move_servo_smooth(SERVO_CLOSE_APPROACH_ANGLE);
#if SERVO_RELEASE_PWM_AFTER_MOVE
    vTaskDelay(pdMS_TO_TICKS(SERVO_SETTLE_MS));
    servo_release_pwm();
#endif
}

system_mode_t read_mode_from_toggle() {
    // Poll Toggle Switch (LOW = Collection, HIGH = Inference)
    if (gpio_get_level(TOGGLE_GPIO) == 0) {
        gpio_set_level(BUILTIN_LED, 1); // Red LED ON
        return MODE_COLLECTION;
    } else {
        gpio_set_level(BUILTIN_LED, 0); // Red LED OFF
        return MODE_INFERENCE;
    }
}

// --- Interrupt Service Routines ---
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    int64_t now = esp_timer_get_time();

    if (gpio_num == HALL_SENSOR_GPIO) {
        if (hall_sensor_armed && current_state == STATE_WAITING && (now - last_hall_trigger > HALL_TRIGGER_DEBOUNCE_US)) {
            current_state = STATE_READING;
            hall_sensor_armed = false;
            last_hall_trigger = now;
        }
    } else if (gpio_num == BUTTON_GPIO) {
        if (current_state == STATE_ACTION && (now - last_btn_trigger > 500000)) {
            current_state = STATE_WAITING;
            last_btn_trigger = now;
        }
    }
}

// --- Ultrasonic Read ---
float read_distance_cm() {
    gpio_set_level(TRIG_GPIO, 1);
    ets_delay_us(10);
    gpio_set_level(TRIG_GPIO, 0);

    int64_t start_time = esp_timer_get_time();
    int64_t timeout = start_time + 30000;

    while(gpio_get_level(ECHO_GPIO) == 0 && esp_timer_get_time() < timeout);
    int64_t echo_start = esp_timer_get_time();

    while(gpio_get_level(ECHO_GPIO) == 1 && esp_timer_get_time() < timeout);
    int64_t echo_end = esp_timer_get_time();

    int64_t duration = echo_end - echo_start;
    // If the loop broke because it hit the timeout limit, the reading is invalid
    if (duration <= 0 || echo_end >= timeout) return -1.0;

    return (duration * 0.0343) / 2.0; 
}

static bool is_distance_in_valid_range(float distance_cm) {
    return (distance_cm >= DIST_MIN_CM && distance_cm <= DIST_MAX_CM);
}

static float median3f(float a, float b, float c) {
    if ((a >= b && a <= c) || (a >= c && a <= b)) return a;
    if ((b >= a && b <= c) || (b >= c && b <= a)) return b;
    return c;
}

// --- Machine Learning Placeholder ---
bool predict_gate_action(float distance, float* v, float* a) {
    // TODO: Pass the features to Scikit-Learn DT or TFLite MLP
    // Return true to CLOSE gate, false to KEEP OPEN
    
    // Dummy logic: Close gate if object is closer than 50cm and accelerating towards it
    if (distance > 0 && distance < 50.0 && v[13] < -10.0) {
        return true; 
    }
    return false;
}

void app_main(void) {
    // 1. Configure Inputs
    gpio_config_t in_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << HALL_SENSOR_GPIO) | (1ULL << BUTTON_GPIO),
        .pull_up_en = 1,
    };
    gpio_config(&in_conf);

    // Toggle switch doesn't need an interrupt, we poll it in the WAITING state
    gpio_config_t toggle_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << TOGGLE_GPIO),
        .pull_up_en = 1,
    };
    gpio_config(&toggle_conf);

    // 2. Configure Outputs
    gpio_config_t out_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << TRIG_GPIO) | (1ULL << LED_R) | (1ULL << LED_G) | (1ULL << LED_B) | (1ULL << BUILTIN_LED),
        .pull_up_en = 0,
        .pull_down_en = 0,
    };
    gpio_config(&out_conf);

    gpio_config_t echo_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << ECHO_GPIO),
        .pull_up_en = 0,
    };
    gpio_config(&echo_conf);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(HALL_ANALOG_CHANNEL, ADC_ATTEN_DB_11);

    // 3. Configure Servo PWM (LEDC)
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_13_BIT,
        .freq_hz          = 50,  // Standard servo frequency
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = SERVO_GPIO,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);

    // Initialize Gate to OPEN
    command_gate_open();

    // 4. Attach Interrupts
    gpio_install_isr_service(0);
    gpio_isr_handler_add(HALL_SENSOR_GPIO, gpio_isr_handler, (void*) HALL_SENSOR_GPIO);
    gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, (void*) BUTTON_GPIO);

    ESP_LOGI(TAG, "Dual-Mode System Ready.");

    system_mode_t current_mode = read_mode_from_toggle();
    system_state_t previous_state = current_state;
    static float distances[DIST_SAMPLE_COUNT];
    static float distances_filtered[DIST_SAMPLE_COUNT];
    static float v[14];
    static float a[13];
    bool last_run_invalid = false;

    // 5. Main Loop
    while (1) {
        if (current_state != previous_state) {
            if (current_state == STATE_WAITING) {
                command_gate_open(); // Reset gate to open when returning to waiting
            } else if (current_state == STATE_ACTION) {
                if (current_mode == MODE_COLLECTION) {
                    if (last_run_invalid) {
                        ESP_LOGW(TAG, "Run invalid (too many rejected distance samples). DATA line skipped.");
                    } else {
                        printf("DATA,%.2f", distances_filtered[14]);
                        for (int i = 13; i >= 0; i--) printf(",%.2f", v[i]);
                        for (int i = 12; i >= 0; i--) printf(",%.2f", a[i]);
                        printf("\n");
                        ESP_LOGI(TAG, "Data saved. Waiting for reset.");
                    }
                } else {
                    bool close_gate = last_run_invalid ? true : predict_gate_action(distances_filtered[14], v, a);
                    if (close_gate) {
                        if (last_run_invalid) {
                            ESP_LOGW(TAG, "INFERENCE: Run invalid. Failsafe CLOSE gate.");
                        } else {
                            ESP_LOGW(TAG, "INFERENCE: Train approaching. CLOSING GATE.");
                        }
                        command_gate_close();
                    } else {
                        ESP_LOGI(TAG, "INFERENCE: Train stopping/safe. Gate remains open.");
                        command_gate_open();
                    }
                }
            }
            previous_state = current_state;
        }

        if (current_state == STATE_WAITING) {
            set_rgb_color(0, 0, 1); // Blue: WAITING
            current_mode = read_mode_from_toggle(); // Update mode based on toggle switch            

            if (gpio_get_level(HALL_SENSOR_GPIO) == 0) {
                hall_sensor_armed = true;
            }

            vTaskDelay(pdMS_TO_TICKS(100));

        } else if (current_state == STATE_READING) {
            set_rgb_color(1, 0, 0); // Red: READING
            ESP_LOGW(TAG, "Train Detected! Collecting features...");

            float last_valid_distance = DIST_MAX_CM;
            bool has_valid_distance = false;
            int rejected_samples = 0;
            int64_t next_sample_time_us = esp_timer_get_time();

            for (int i = 0; i < DIST_SAMPLE_COUNT; i++) {
                float raw_distance = read_distance_cm();
                if (is_distance_in_valid_range(raw_distance)) {
                    distances[i] = raw_distance;
                    last_valid_distance = raw_distance;
                    has_valid_distance = true;
                } else {
                    // Keep a stable series: reuse the last valid sample, or clamp to max range
                    // until the first valid sample arrives.
                    distances[i] = has_valid_distance ? last_valid_distance : DIST_MAX_CM;
                    ESP_LOGW(TAG, "Rejected ultrasonic sample %.2f cm at idx %d; substituted %.2f cm",
                             raw_distance, i, distances[i]);
                    rejected_samples++;
                }

                next_sample_time_us += SAMPLE_PERIOD_US;
                int64_t now_us = esp_timer_get_time();
                if (next_sample_time_us > now_us) {
                    int64_t wait_us = next_sample_time_us - now_us;
                    vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
                }
            }

            last_run_invalid = (rejected_samples >= INVALID_SAMPLE_THRESHOLD);
            ESP_LOGI(TAG, "Rejected samples: %d/%d. Run validity: %s",
                     rejected_samples, DIST_SAMPLE_COUNT, last_run_invalid ? "INVALID" : "VALID");

            printf("Raw Distances:");
            for (int i = 0; i < DIST_SAMPLE_COUNT; i++) {
                printf(" %.2f", distances[i]);
            }
            printf("\n");

            // 3-sample sliding median filter (centered). For edges, duplicate nearest neighbor.
            distances_filtered[0] = median3f(distances[0], distances[0], distances[1]);
            for (int i = 1; i < 14; i++) {
                distances_filtered[i] = median3f(distances[i - 1], distances[i], distances[i + 1]);
            }
            distances_filtered[14] = median3f(distances[13], distances[14], distances[14]);

            printf("Filtered Distances:");
            for (int i = 0; i < DIST_SAMPLE_COUNT; i++) {
                printf(" %.2f", distances_filtered[i]);
            }
            printf("\n");

            // Feature extraction
            float dt = 0.1;

            for (int i = 0; i < 14; i++) {
                v[i] = (distances_filtered[i+1] - distances_filtered[i]) / dt;
            }
            for (int i = 0; i < 13; i++) {
                a[i] = (v[i+1] - v[i]) / dt;
            }

            current_state = STATE_ACTION;

        } else if (current_state == STATE_ACTION) {
            if (last_run_invalid) {
                set_rgb_color(1, 1, 0); // Yellow: INVALID RUN / FAILSAFE ACTION
            } else {
                set_rgb_color(0, 1, 0); // Green: SAVED / ACTION COMPLETE
            }
            
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
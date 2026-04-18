#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

static const char *TAG = "RAILWAY_SYSTEM";

// --- Pin Definitions ---
#define HALL_SENSOR_GPIO 27
#define BUTTON_GPIO      26
#define TOGGLE_GPIO      25
#define SERVO_GPIO       21
#define BUILTIN_LED      13 // Adafruit Feather Built-in LED
#define TRIG_GPIO        33
#define ECHO_GPIO        32
#define LED_R            14
#define LED_G            15
#define LED_B            12

// --- Servo PWM Settings ---
#define SERVO_MIN_PULSEWIDTH_US 500  // Minimum pulse width in microsecond (0 degrees)
#define SERVO_MAX_PULSEWIDTH_US 2500 // Maximum pulse width in microsecond (180 degrees)
#define SERVO_MAX_DEGREE        180  // Maximum angle
#define SERVO_OPEN_ANGLE        0   // Angle to keep gate open
#define SERVO_CLOSE_ANGLE       170    // Angle to close gate

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

// --- Helpers ---
void set_rgb_color(int r, int g, int b) {
    gpio_set_level(LED_R, r);
    gpio_set_level(LED_G, g);
    gpio_set_level(LED_B, b);
}

void set_servo_angle(int angle) {
    uint32_t duty = (uint32_t)(((angle * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US)) / SERVO_MAX_DEGREE) + SERVO_MIN_PULSEWIDTH_US);
    // Convert microseconds to 13-bit duty cycle for 50Hz (20000us period)
    uint32_t duty_13bit = (duty * 8191) / 20000;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_13bit);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
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
        if (hall_sensor_armed && current_state == STATE_WAITING && (now - last_hall_trigger > 500000)) {
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
    if (duration <= 0 || duration >= 30000) return -1.0;

    return (duration * 0.0343) / 2.0; 
}

// --- Machine Learning Placeholder ---
bool predict_gate_action(float distance, float* v, float* a) {
    // TODO: Pass the 15 features to Scikit-Learn DT or TFLite MLP
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
        .intr_type = GPIO_INTR_NEGEDGE,
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
    set_servo_angle(SERVO_OPEN_ANGLE);

    // 4. Attach Interrupts
    gpio_install_isr_service(0);
    gpio_isr_handler_add(HALL_SENSOR_GPIO, gpio_isr_handler, (void*) HALL_SENSOR_GPIO);
    gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, (void*) BUTTON_GPIO);

    ESP_LOGI(TAG, "Dual-Mode System Ready.");

    system_mode_t current_mode = read_mode_from_toggle();
    system_state_t previous_state = current_state;
    static float distances[15];
    static float v[14];
    static float a[13];

    // 5. Main Loop
    while (1) {
        if (current_state != previous_state) {
            if (current_state == STATE_WAITING) {
                set_servo_angle(SERVO_OPEN_ANGLE); // Reset gate to open when returning to waiting
            } else if (current_state == STATE_ACTION) {
                if (current_mode == MODE_COLLECTION) {
                    printf("DATA,%.2f", distances[14]);
                    for (int i = 13; i >= 7; i--) printf(",%.2f", v[i]);
                    for (int i = 12; i >= 6; i--) printf(",%.2f", a[i]);
                    printf("\n");
                    ESP_LOGI(TAG, "Data saved. Waiting for reset.");
                } else {
                    bool close_gate = predict_gate_action(distances[14], v, a);
                    if (close_gate) {
                        ESP_LOGW(TAG, "INFERENCE: Train approaching. CLOSING GATE.");
                        set_servo_angle(SERVO_CLOSE_ANGLE);
                    } else {
                        ESP_LOGI(TAG, "INFERENCE: Train stopping/safe. Gate remains open.");
                        set_servo_angle(SERVO_OPEN_ANGLE);
                    }
                }
            }
            previous_state = current_state;
        }

        if (current_state == STATE_WAITING) {
            set_rgb_color(0, 0, 1); // Blue: WAITING
            current_mode = read_mode_from_toggle(); // Update mode based on toggle switch            

            if (gpio_get_level(HALL_SENSOR_GPIO) == 1) {
                hall_sensor_armed = true;
            }

            vTaskDelay(pdMS_TO_TICKS(100));

        } else if (current_state == STATE_READING) {
            set_rgb_color(1, 0, 0); // Red: READING
            ESP_LOGW(TAG, "Train Detected! Collecting features...");

            for (int i = 0; i < 15; i++) {
                distances[i] = read_distance_cm();
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            // Feature extraction
            float dt = 0.1;

            for (int i = 0; i < 14; i++) {
                v[i] = (distances[i+1] - distances[i]) / dt;
            }
            for (int i = 0; i < 13; i++) {
                a[i] = (v[i+1] - v[i]) / dt;
            }

            current_state = STATE_ACTION;

        } else if (current_state == STATE_ACTION) {
            set_rgb_color(0, 1, 0); // Green: SAVED / ACTION COMPLETE
            
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
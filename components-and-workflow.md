# Hardware components
- Slide switch: the position defines the operation mode (COLLECTION or INFERENCE)
- Built-in LED: ON in the COLLECTION mode, OFF in the INFERENCE mode
- State LED: the color indicates the system state
    - BLUE – WAITING
    - RED – READING
    - GREEN – ACTION
- Reset button: returns the system into the WAITING state
- Hall sensor: on trigger, brings the system into the READING state
- Ultrasound sensor: when the system is in the READING state, performs distance reading every 100 µs
- Servo motor: drives the gate; is in the 90 position when open, in the 0 position when closed

# Workflow

1. Operation mode detection:

If COLLECTION:

1. **Built-in LED turns ON**
2. State LED turns BLUE
3. The system is brought into the WAITING state
4. Waiting for the Hall sensor being triggered; on trigger:
    - State LED turns RED
    - The system is brought into the READING state
    - Ultrasound sensor readings are collected (1.5 s; 15 readings)
    - State LED turns GREEN
    - The system is brought into the ACTION state
    - **Data is output as CSV, ready to be fetched by the listener via the Serial Monitor to be saved into a file**
    - Waiting for the reset
5. On reset button press:
    - Return to the operation mode detection


If INFERENCE:

1. **Built-in LED turns OFF**
2. State LED turns BLUE
3. The system is brought into the WAITING state
4. Waiting for the Hall sensor being triggered; on trigger:
    - State LED turns RED
    - The system is brought into the READING state
    - Ultrasound sensor readings are collected (1.5 s; 15 readings)
    - State LED turns GREEN
    - The system is brought into the ACTION state
    - **If the gate action predictor returns CLOSE_GATE, set the servo motor to the 0 position; else leave the motor in the 90 position**
    - Waiting for the reset
5. On reset button press:
    - **Set the motor to the 90 position (open)**
    - Return to the operation mode detection
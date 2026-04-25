# Servo Dog Control Example

The implementation functions for actions such as standing, lying down, moving forward, moving backward, turning left, turning right, leaning forward, leaning backward, and twisting left and right have been written in the `components/servo_dog_ctrl` component. The command control of the servo dog is realized through `servo_dog_ctrl_task`.

To control it, you simply need to send a message of type `dog_action_msg_t` to `servo_dog_ctrl_task` via the message queue. The message structure is as follows:
```c
dog_action_msg_t msg = {
    .state = s_dog_state,
    .repeat_count = 2,
    .speed = 100,
    .hold_time_ms = 500,
    .angle_offset = 20
};
```
The meaning of the message structure members are as follows:
```c
typedef struct {
    servo_dog_state_t state;      // Action type, such as lie down, forward, backward, etc.
    uint16_t repeat_count;        // Number of times to repeat the action
    uint16_t speed;               // Action execution speed
    uint16_t hold_time_ms;        // Action hold time, used in leaning forward and backward actions
    uint8_t angle_offset;         // Turning angle, used in twisting actions
} dog_action_msg_t;
```

For specific usage, please refer to the `button_single_click_cb` function usage in `main/servo_dog.c`.

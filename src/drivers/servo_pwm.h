#ifndef DRIVERS_SERVO_PWM_H
#define DRIVERS_SERVO_PWM_H

#include <stdint.h>

// SRV1-4 signals on TIM4: 50 Hz, pulse width in µs.

// Starts all four channels with no pulses and then hands the pins to TIM4.
void servo_pwm_init(void);

// Sets one servo's pulse width in µs from the next period on; 0 stops its pulses.
// servo 0-3 = SRV1-4.
void servo_pwm_set(unsigned servo, uint32_t pulse_us);

#endif /* DRIVERS_SERVO_PWM_H */

#ifndef DRIVERS_SOLENOID_PWM_H
#define DRIVERS_SOLENOID_PWM_H

#include <stdint.h>

// SOL1-4 gates on TIM1, 20 kHz PWM.

// Starts all four channels at 0 % and then hands the pins to TIM1.
void solenoid_pwm_init(void);

// Sets one gate's duty, 0-100 %, from the next PWM period on. solenoid 0-3 = SOL1-4.
void solenoid_pwm_set(unsigned solenoid, uint32_t duty_percent);

#endif /* DRIVERS_SOLENOID_PWM_H */

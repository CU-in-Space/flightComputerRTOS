#ifndef DRIVERS_STEPPER_H
#define DRIVERS_STEPPER_H

#include <stdbool.h>
#include <stdint.h>

// DRV8825 on TIM2, full steps only. One STEP pulse per timer period, counted by
// the timer's interrupt. Positive steps move away from the limit switch.

// Sets up TIM2 and the STEP, FAULT and limit-switch pins. The driver starts asleep.
void stepper_init(void);

// Raises or lowers NSLEEP. Waking resets the driver and takes up to 1.7 ms, so wait
// before moving. Going to sleep also stops the move.
void stepper_set_awake(bool awake);

// Starts a move of steps (the sign is the direction) at steps_per_s. With
// stop_at_limit it also ends at the first step that finds the switch pressed.
// Does nothing while a move is running.
void stepper_move(int32_t steps, uint32_t steps_per_s, bool stop_at_limit);

// Ends the move once the step going out has finished.
void stepper_stop(void);

// True while a move is running.
bool stepper_moving(void);

// Steps from zero, counted as they go out.
int32_t stepper_position(void);

// Sets the current position. Only while stopped.
void stepper_set_position(int32_t position);

// True if the DRV8825 reports overcurrent or overtemperature. Only valid while awake.
bool stepper_fault(void);

// True if the limit switch is pressed or its wire is broken.
bool stepper_at_limit(void);

#endif /* DRIVERS_STEPPER_H */

/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "gpio.h"
#include "board/board_pins.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

// Every plain output pin. Pins that belong to a peripheral (ADC inputs, SPI, timers)
// are set up by that peripheral's driver.
static const struct {
  GPIO_TypeDef *port;
  uint16_t pin;
} outputs[] = {
  { LED1_GPIO_Port,       LED1_Pin },
  { HAT_LED_GPIO_Port,    HAT_LED_Pin },
  // Actuators. Low is off for every one of them.
  { SOL1_CTL_GPIO_Port,   SOL1_CTL_Pin },
  { SOL2_CTL_GPIO_Port,   SOL2_CTL_Pin },
  { SOL3_CTL_GPIO_Port,   SOL3_CTL_Pin },
  { SOL4_CTL_GPIO_Port,   SOL4_CTL_Pin },
  { SRV1_PWM_GPIO_Port,   SRV1_PWM_Pin },
  { SRV2_PWM_GPIO_Port,   SRV2_PWM_Pin },
  { SRV3_PWM_GPIO_Port,   SRV3_PWM_Pin },
  { SRV4_PWM_GPIO_Port,   SRV4_PWM_Pin },
  { STP_STEP_GPIO_Port,   STP_STEP_Pin },
  { STP_DIR_GPIO_Port,    STP_DIR_Pin },
  { STP_NSLEEP_GPIO_Port, STP_NSLEEP_Pin },
};

// The HAT's chip selects and radio lines aren't here. They stay high impedance
// until their drivers start, so nothing drives a HAT chip that has no power yet.
void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();

  GPIO_InitTypeDef pin_config = {0};
  pin_config.Mode  = GPIO_MODE_OUTPUT_PP;
  pin_config.Pull  = GPIO_NOPULL;
  pin_config.Speed = GPIO_SPEED_FREQ_LOW;

  for (unsigned i = 0; i < sizeof outputs / sizeof outputs[0]; i++) {
    // Level first, so the pin is already low when it becomes an output.
    HAL_GPIO_WritePin(outputs[i].port, outputs[i].pin, GPIO_PIN_RESET);
    pin_config.Pin = outputs[i].pin;
    HAL_GPIO_Init(outputs[i].port, &pin_config);
  }
}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */

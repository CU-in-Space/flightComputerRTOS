/* Sensor HAT and power board pin map, from the HAT's final netlist (2026-10-20).
 * AF numbers checked against CubeMX's STM32H753ZITx pin database.
 *
 * MX_GPIO_Init() clocks every GPIO port and sets up the plain outputs.
 */
#ifndef BOARD_BOARD_PINS_H
#define BOARD_BOARD_PINS_H

#include "stm32h7xx_hal.h"

// ---- Power board, ACT cable (HAT J3). All lines pulled down on the power board.

// Solenoid FET gates. SOL3 is TIM1_CH4 and SOL4 is TIM1_CH3.
#define SOL1_CTL_Pin         GPIO_PIN_9      // TIM1_CH1, J3-10
#define SOL1_CTL_GPIO_Port   GPIOE
#define SOL2_CTL_Pin         GPIO_PIN_11     // TIM1_CH2, J3-9
#define SOL2_CTL_GPIO_Port   GPIOE
#define SOL3_CTL_Pin         GPIO_PIN_14     // TIM1_CH4, J3-8
#define SOL3_CTL_GPIO_Port   GPIOE
#define SOL4_CTL_Pin         GPIO_PIN_13     // TIM1_CH3, J3-7
#define SOL4_CTL_GPIO_Port   GPIOE
#define SOL_CTL_AF           GPIO_AF1_TIM1

// Servo signals. SRV3 is TIM4_CH4 and SRV4 is TIM4_CH3.
#define SRV1_PWM_Pin         GPIO_PIN_12     // TIM4_CH1, J3-2
#define SRV1_PWM_GPIO_Port   GPIOD
#define SRV2_PWM_Pin         GPIO_PIN_13     // TIM4_CH2, J3-3
#define SRV2_PWM_GPIO_Port   GPIOD
#define SRV3_PWM_Pin         GPIO_PIN_9      // TIM4_CH4, J3-4
#define SRV3_PWM_GPIO_Port   GPIOB
#define SRV4_PWM_Pin         GPIO_PIN_8      // TIM4_CH3, J3-5
#define SRV4_PWM_GPIO_Port   GPIOB
#define SRV_PWM_AF           GPIO_AF2_TIM4

// ---- Power board, AUX cable (HAT J4)

// DRV8825 stepper, full step only.
#define STP_STEP_Pin         GPIO_PIN_0      // TIM2_CH1, J4-10
#define STP_STEP_GPIO_Port   GPIOA
#define STP_STEP_AF          GPIO_AF1_TIM2
#define STP_DIR_Pin          GPIO_PIN_10     // J4-11
#define STP_DIR_GPIO_Port    GPIOE
#define STP_NSLEEP_Pin       GPIO_PIN_12     // J4-9, drives SLEEP and RESET together
#define STP_NSLEEP_GPIO_Port GPIOE
#define STP_FAULT_Pin        GPIO_PIN_15     // EXTI15, J4-8, open drain: use the internal pull-up
#define STP_FAULT_GPIO_Port  GPIOE

// Normally-closed limit switch: low = OK, high = at limit or broken wire.
#define AUX_IN1_Pin          GPIO_PIN_11     // EXTI11, J4-6
#define AUX_IN1_GPIO_Port    GPIOB

#define VBAT_SENSE_Pin       GPIO_PIN_4      // ADC3_INP9, J4-2, 100k/22k divider
#define VBAT_SENSE_GPIO_Port GPIOF

// ---- Pressure transducers. PWR on ADC1 and DATA on ADC2, sampled together.

#define PT1_PWR_Pin          GPIO_PIN_1      // ADC12_INP5, A3
#define PT1_PWR_GPIO_Port    GPIOB
#define PT1_DATA_Pin         GPIO_PIN_3      // PC3_C, ADC12_INP13 once the PC3 analog switch is closed, A2
#define PT1_DATA_GPIO_Port   GPIOC
#define PT2_PWR_Pin          GPIO_PIN_0      // ADC123_INP10, A1
#define PT2_PWR_GPIO_Port    GPIOC
#define PT2_DATA_Pin         GPIO_PIN_3      // ADC12_INP15, A0
#define PT2_DATA_GPIO_Port   GPIOA

// ---- SPI4: 4 x MAX31856 + W25Q128 flash. Chip selects have 10k pull-ups.

#define SPI4_SCK_Pin         GPIO_PIN_2
#define SPI4_SCK_GPIO_Port   GPIOE
#define SPI4_MISO_Pin        GPIO_PIN_5
#define SPI4_MISO_GPIO_Port  GPIOE
#define SPI4_MOSI_Pin        GPIO_PIN_6
#define SPI4_MOSI_GPIO_Port  GPIOE
#define SPI4_AF              GPIO_AF5_SPI4

#define TC1_CS_Pin           GPIO_PIN_3      // U6, terminal P1
#define TC1_CS_GPIO_Port     GPIOD
#define TC2_CS_Pin           GPIO_PIN_2      // U7, terminal P2
#define TC2_CS_GPIO_Port     GPIOF
#define TC3_CS_Pin           GPIO_PIN_1      // U8, terminal P3
#define TC3_CS_GPIO_Port     GPIOF
#define TC4_CS_Pin           GPIO_PIN_3      // U9, terminal P4
#define TC4_CS_GPIO_Port     GPIOE
#define FLASH_CS_Pin         GPIO_PIN_4      // U12
#define FLASH_CS_GPIO_Port   GPIOE

// ---- SPI5: ADS1220 load-cell ADC

#define SPI5_SCK_Pin         GPIO_PIN_7
#define SPI5_SCK_GPIO_Port   GPIOF
#define SPI5_MISO_Pin        GPIO_PIN_8
#define SPI5_MISO_GPIO_Port  GPIOF
#define SPI5_MOSI_Pin        GPIO_PIN_9
#define SPI5_MOSI_GPIO_Port  GPIOF
#define SPI5_AF              GPIO_AF5_SPI5

#define LC_CS_Pin            GPIO_PIN_1      // 10k pull-up
#define LC_CS_GPIO_Port      GPIOG
#define LC_DRDY_Pin          GPIO_PIN_0      // EXTI0, falls when a conversion is ready
#define LC_DRDY_GPIO_Port    GPIOF

// ---- SPI1: E22-900M30S radio (SX1262). DIO2 drives TXEN; never raise RXEN while transmitting.

#define SPI1_SCK_Pin         GPIO_PIN_5
#define SPI1_SCK_GPIO_Port   GPIOA
#define SPI1_MISO_Pin        GPIO_PIN_6
#define SPI1_MISO_GPIO_Port  GPIOA
#define SPI1_MOSI_Pin        GPIO_PIN_5      // via CN7-13, not D11
#define SPI1_MOSI_GPIO_Port  GPIOB
#define SPI1_AF              GPIO_AF5_SPI1

#define RADIO_NSS_Pin        GPIO_PIN_12     // 10k pull-up
#define RADIO_NSS_GPIO_Port  GPIOB
#define RADIO_NRST_Pin       GPIO_PIN_14     // active low, 10k pull-up
#define RADIO_NRST_GPIO_Port GPIOD
#define RADIO_BUSY_Pin       GPIO_PIN_15     // poll it: EXTI15 belongs to STP_FAULT
#define RADIO_BUSY_GPIO_Port GPIOD
#define RADIO_DIO1_Pin       GPIO_PIN_3      // EXTI3, radio IRQ
#define RADIO_DIO1_GPIO_Port GPIOF
#define RADIO_RXEN_Pin       GPIO_PIN_15     // 100k pull-down
#define RADIO_RXEN_GPIO_Port GPIOB

// ---- SDMMC1: microSD, 4-bit, no card detect

#define SD_D0_Pin            GPIO_PIN_8
#define SD_D0_GPIO_Port      GPIOC
#define SD_D1_Pin            GPIO_PIN_9
#define SD_D1_GPIO_Port      GPIOC
#define SD_D2_Pin            GPIO_PIN_10
#define SD_D2_GPIO_Port      GPIOC
#define SD_D3_Pin            GPIO_PIN_11
#define SD_D3_GPIO_Port      GPIOC
#define SD_CK_Pin            GPIO_PIN_12
#define SD_CK_GPIO_Port      GPIOC
#define SD_CMD_Pin           GPIO_PIN_2
#define SD_CMD_GPIO_Port     GPIOD
#define SD_AF                GPIO_AF12_SDMMC1

// ---- HAT status LED (LED2), active high

#define HAT_LED_Pin          GPIO_PIN_14
#define HAT_LED_GPIO_Port    GPIOG

/* Reserved:
 *   J5 breakout (not fitted): SPI3 PB3/PB4/PB2, IMU CS PD11, IMU INT PG6,
 *   high-g CS PG12, spare CS PA15, I2C1 PB6/PB7.
 *   Free on the Zio for a later rev: UART7 PE7/PE8, sync PB10/PE0,
 *   FDCAN1 PD0/PD1, second ADS1220 CS PG0.
 * EXTI lines in use: 0, 3, 11, 15.
 */

#endif /* BOARD_BOARD_PINS_H */

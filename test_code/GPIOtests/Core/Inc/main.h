/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define B1_Pin GPIO_PIN_13
#define B1_GPIO_Port GPIOC
#define USART_TX_Pin GPIO_PIN_2
#define USART_TX_GPIO_Port GPIOA
#define USART_RX_Pin GPIO_PIN_3
#define USART_RX_GPIO_Port GPIOA
#define LD2_Pin GPIO_PIN_5
#define LD2_GPIO_Port GPIOA
#define TMP_T1_Pin GPIO_PIN_0
#define TMP_T1_GPIO_Port GPIOA
#define TMP_T2_Pin GPIO_PIN_1
#define TMP_T2_GPIO_Port GPIOA
#define TMP_NOZ_Pin GPIO_PIN_4
#define TMP_NOZ_GPIO_Port GPIOA
#define LVL_T1_Pin GPIO_PIN_0
#define LVL_T1_GPIO_Port GPIOB
#define LVL_T2_Pin GPIO_PIN_1
#define LVL_T2_GPIO_Port GPIOC
#define RELAY_D7_Pin GPIO_PIN_8
#define RELAY_D7_GPIO_Port GPIOA
#define RELAY_D8_Pin GPIO_PIN_9
#define RELAY_D8_GPIO_Port GPIOA
#define RELAY_D2_Pin GPIO_PIN_10
#define RELAY_D2_GPIO_Port GPIOA
#define RELAY_D3_Pin GPIO_PIN_3
#define RELAY_D3_GPIO_Port GPIOB
#define HTR_T1_Pin GPIO_PIN_5
#define HTR_T1_GPIO_Port GPIOB
#define HTR_T2_Pin GPIO_PIN_4
#define HTR_T2_GPIO_Port GPIOB
#define HTR_NOZ_Pin GPIO_PIN_10
#define HTR_NOZ_GPIO_Port GPIOB
#define MOTOR_Pin GPIO_PIN_7
#define MOTOR_GPIO_Port GPIOC
#define TMS_Pin GPIO_PIN_13
#define TMS_GPIO_Port GPIOA
#define TCK_Pin GPIO_PIN_14
#define TCK_GPIO_Port GPIOA
#define PI_TX_Pin GPIO_PIN_6
#define PI_TX_GPIO_Port GPIOB
#define PI_RX_Pin GPIO_PIN_7
#define PI_RX_GPIO_Port GPIOB
#define SWO_Pin GPIO_PIN_3
#define SWO_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

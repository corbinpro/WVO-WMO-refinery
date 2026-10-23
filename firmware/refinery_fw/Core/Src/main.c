/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "control.h"
#include "proto.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define ADC_MAX        4095.0f
#define R_FIXED        10000.0f   /* temp divider resistor to GND (ohm) */
#define NTC_R25        10000.0f   /* NTC3950 nominal at 25 C */
#define NTC_BETA       3950.0f
#define NTC_T25_K      298.15f
#define N_SAMPLES      16
#define N_CH           5          /* T1, T2, NOZ, LVL1, LVL2 (ADC rank order) */
#define LVL_R_FIXED    100.0f     /* level divider resistor to GND (ohm) */
#define LVL_R_EMPTY    240.0f     /* American/SAE sender: 240 ohm = empty */
#define LVL_R_FULL     33.0f      /*                       33 ohm  = full  */

/* Flame sensors are not wired yet: 0 = ignore PB1/PB2/PC5 (inputs read as "no flame") and report
 * fire detection as OFF in the status line. Set to 1 once all three sensors are wired and tested. */
#define FLAME_SENSORS_ENABLED  0

#define SAMPLE_MS      100        /* sensor read + control step period */
#define STATUS_MS      1000       /* status line period (REQ-PERF-030) */
#define STATUS_MIN_MS  200        /* earliest extra status line after a change */
#define SAVE_DELAY_MS  5000       /* settings are written this long after the last change */
#define RX_QUEUE       4          /* received command lines waiting for the main loop */
#define UART_TIMEOUT   100

/* Operator settings live here so they survive a reset / power cycle.
 * Last 2K page of bank 2 - kept out of the FLASH region in STM32L476RGTX_FLASH.ld. */
#define STATE_ADDR     0x080FF800UL
#define STATE_BANK     FLASH_BANK_2
#define STATE_PAGE     255
#define STATE_MAGIC    0x57565331UL  /* "WVS1" in the upper word of the first doubleword */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
static uint32_t adc_raw[N_CH]; /* T1 A0/PA0, T2 A1/PA1, NOZ A2/PA4, LVL1 A3/PB0, LVL2 A4/PC1 */

static ctl_t ctl;
static ctl_inputs_t inputs;

/* Command lines from the RPi (filled by the USART1 RX interrupt) */
static uint8_t rx_byte;
static char rx_buf[PROTO_LINE_MAX];
static uint8_t rx_len;
static char rx_queue[RX_QUEUE][PROTO_LINE_MAX];
static volatile uint8_t rx_head, rx_tail;

/* Outputs, indexed by bit number of the OUT_* masks in control.h.
 * on_level = pin level that turns the device ON. Relay boards assumed active-HIGH;
 * change to GPIO_PIN_RESET if one turns out to be active-LOW. */
typedef struct {
  GPIO_TypeDef  *port;
  uint16_t       pin;
  GPIO_PinState  on_level;
} Output_t;

static const Output_t outputs[8] = {
  { VALVE_T1_GPIO_Port, VALVE_T1_Pin, GPIO_PIN_SET },   /* OUT_V1 */
  { VALVE_T2_GPIO_Port, VALVE_T2_Pin, GPIO_PIN_SET },   /* OUT_V2 */
  { HTR_T1_GPIO_Port,   HTR_T1_Pin,   GPIO_PIN_SET },   /* OUT_H1 */
  { HTR_T2_GPIO_Port,   HTR_T2_Pin,   GPIO_PIN_SET },   /* OUT_H2 */
  { HTR_NOZ_GPIO_Port,  HTR_NOZ_Pin,  GPIO_PIN_SET },   /* OUT_HNOZ */
  { MOTOR_GPIO_Port,    MOTOR_Pin,    GPIO_PIN_SET },   /* OUT_CENT */
  { FIRE_EXT_GPIO_Port, FIRE_EXT_Pin, GPIO_PIN_SET },   /* OUT_EXT */
  { FIRE_RET_GPIO_Port, FIRE_RET_Pin, GPIO_PIN_SET },   /* OUT_RET */
};

/* Flame sensors (LM393 boards): output pulled LOW when they see flame */
static const struct { GPIO_TypeDef *port; uint16_t pin; } flames[3] = {
  { FLAME1_GPIO_Port, FLAME1_Pin },
  { FLAME2_GPIO_Port, FLAME2_Pin },
  { FLAME3_GPIO_Port, FLAME3_Pin },
};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Route printf to USART2 (ST-LINK virtual COM port) - debug only, the Pi link is USART1 */
int __io_putchar(int ch)
{
  uint8_t c = (uint8_t)ch;
  HAL_UART_Transmit(&huart2, &c, 1, HAL_MAX_DELAY);
  return ch;
}

/* Drive every output from a bit mask. Extend and retract are both written low first,
 * so the two extinguisher relays are never on together even for one write. */
static void apply_outputs(uint8_t bits)
{
  if (bits & OUT_EXT) bits &= (uint8_t)~OUT_RET;
  for (int pass = 0; pass < 2; pass++)
  {
    for (size_t i = 0; i < 8; i++)
    {
      int on = (bits >> i) & 1U;
      if (on != pass) continue;                       /* pass 0: turn things off, pass 1: on */
      GPIO_PinState off_level = (outputs[i].on_level == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET;
      HAL_GPIO_WritePin(outputs[i].port, outputs[i].pin, on ? outputs[i].on_level : off_level);
    }
  }
}

static uint8_t read_flames(void)
{
  uint8_t bits = 0;
  if (!FLAME_SENSORS_ENABLED) return 0;
  for (int i = 0; i < 3; i++)
  {
    if (HAL_GPIO_ReadPin(flames[i].port, flames[i].pin) == GPIO_PIN_RESET) bits |= (uint8_t)(1U << i);
  }
  return bits;
}

/* Average N_SAMPLES scans of the N_CH channels (order = ADC rank order) */
static void read_channels(uint32_t out[N_CH])
{
  uint32_t sum[N_CH] = {0};
  for (int n = 0; n < N_SAMPLES; n++)
  {
    for (int ch = 0; ch < N_CH; ch++)
    {
      HAL_ADC_Start(&hadc1);
      HAL_ADC_PollForConversion(&hadc1, 10);
      sum[ch] += HAL_ADC_GetValue(&hadc1);
    }
    HAL_ADC_Stop(&hadc1);
  }
  for (int ch = 0; ch < N_CH; ch++) out[ch] = sum[ch] / N_SAMPLES;
}

/* Temp divider: 3.3V -> NTC -> node(ADC) -> 10k -> GND
 * R_ntc = R_FIXED*(4095-raw)/raw, then Beta equation. Returns tenths of a degree F. */
static int16_t temp_f10(uint32_t raw)
{
  if (raw < 10 || raw > 4085) return CTL_NO_READING;  /* open / short */
  float r_ntc = R_FIXED * (ADC_MAX - (float)raw) / (float)raw;
  float t_k   = 1.0f / (1.0f / NTC_T25_K + logf(r_ntc / NTC_R25) / NTC_BETA);
  float t_f   = (t_k - 273.15f) * 9.0f / 5.0f + 32.0f;
  return (int16_t)lroundf(t_f * 10.0f);
}

/* Level divider: 3.3V -> sender -> node(ADC) -> 100 ohm -> GND
 * Fill % assumes resistance linear with float position (UNVERIFIED - see test_code/notes.txt).
 * Returns tenths of a percent, not clamped, so the calibration offset stays visible. */
static int16_t level_x10(uint32_t raw)
{
  if (raw < 10 || raw > 4085) return CTL_NO_READING;
  float r_s  = LVL_R_FIXED * (ADC_MAX - (float)raw) / (float)raw;
  float fill = (LVL_R_EMPTY - r_s) / (LVL_R_EMPTY - LVL_R_FULL) * 100.0f;
  return (int16_t)lroundf(fill * 10.0f);
}

static void read_inputs(ctl_inputs_t *in)
{
  read_channels(adc_raw);
  for (int i = 0; i < 3; i++) in->temp_f10[i] = temp_f10(adc_raw[i]);
  for (int i = 0; i < 2; i++) in->level_x10[i] = level_x10(adc_raw[3 + i]);
  in->flame = read_flames();
}

/* Settings record: dw0 = magic<<32 | noz<<16 | preset2<<8 | preset1, dw1 = timer2<<32 | timer1 */
static int load_settings(ctl_settings_t *s)
{
  uint64_t dw0 = *(volatile const uint64_t *)STATE_ADDR;
  uint64_t dw1 = *(volatile const uint64_t *)(STATE_ADDR + 8);
  if ((uint32_t)(dw0 >> 32) != STATE_MAGIC) return 0;
  s->temp_preset[0] = (uint16_t)(dw0 & 0xFF);
  s->temp_preset[1] = (uint16_t)((dw0 >> 8) & 0xFF);
  s->noz_f          = (uint16_t)((dw0 >> 16) & 0xFF);
  s->timer_s[0]     = (uint32_t)dw1;
  s->timer_s[1]     = (uint32_t)(dw1 >> 32);
  return 1;
}

/* Write the settings to flash (only if they changed). Returns 0 on success. */
static int save_settings(const ctl_settings_t *s)
{
  uint64_t dw0 = ((uint64_t)STATE_MAGIC << 32) | ((uint64_t)s->noz_f << 16)
               | ((uint64_t)s->temp_preset[1] << 8) | s->temp_preset[0];
  uint64_t dw1 = ((uint64_t)s->timer_s[1] << 32) | s->timer_s[0];
  if (*(volatile const uint64_t *)STATE_ADDR == dw0 && *(volatile const uint64_t *)(STATE_ADDR + 8) == dw1)
    return 0;

  FLASH_EraseInitTypeDef erase = {
    .TypeErase = FLASH_TYPEERASE_PAGES,
    .Banks     = STATE_BANK,
    .Page      = STATE_PAGE,
    .NbPages   = 1,
  };
  uint32_t bad_page;
  HAL_StatusTypeDef st;

  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);     /* stale flags (e.g. PGSERR) block programming */
  st = HAL_FLASHEx_Erase(&erase, &bad_page);
  if (st == HAL_OK) st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, STATE_ADDR, dw0);
  if (st == HAL_OK) st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, STATE_ADDR + 8, dw1);
  HAL_FLASH_Lock();
  return (st == HAL_OK) ? 0 : -1;
}

/* Send a line to the Pi, and mirror it to the ST-LINK port for debugging */
static void send_line(const char *s, size_t n)
{
  if (n == 0) return;
  HAL_UART_Transmit(&huart1, (const uint8_t *)s, (uint16_t)n, UART_TIMEOUT);
  HAL_UART_Transmit(&huart2, (const uint8_t *)s, (uint16_t)n, UART_TIMEOUT);
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_ADC1_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /* All outputs off; every tank idle and every override off after a reset.
   * Only the operator settings come back from flash. */
  apply_outputs(0);
  ctl_settings_t saved;
  ctl_init(&ctl, load_settings(&saved) ? &saved : NULL);
  ctl.flame_off = !FLAME_SENSORS_ENABLED;
  ctl_settings_t last_saved = ctl.set;
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_UART_Receive_IT(&huart1, &rx_byte, 1);         /* start interrupt-driven command input (RPi) */

  uint32_t seq = 0;
  uint32_t last_sample = HAL_GetTick() - SAMPLE_MS;
  uint32_t last_status = 0;
  uint32_t settings_changed = 0;
  int settings_dirty = 0;
  char line[PROTO_LINE_MAX + 64];
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t now = HAL_GetTick();

    /* Commands from the Pi: apply, then ack */
    while (rx_tail != rx_head)
    {
      char cmd[PROTO_LINE_MAX];
      memcpy(cmd, rx_queue[rx_tail], sizeof cmd);
      rx_tail = (uint8_t)((rx_tail + 1) % RX_QUEUE);
      send_line(line, proto_handle_line(&ctl, now, cmd, line, sizeof line));
    }

    if (now - last_sample >= SAMPLE_MS)
    {
      last_sample = now;
      read_inputs(&inputs);
      ctl_step(&ctl, now, &inputs);
      apply_outputs(ctl.out);
    }

    /* Status once a second, and soon after anything changes */
    if (now - last_status >= STATUS_MS || (ctl.changed && now - last_status >= STATUS_MIN_MS))
    {
      ctl.changed = 0;
      last_status = now;
      send_line(line, proto_status(&ctl, now, &inputs, seq++, line, sizeof line));
    }

    /* Save settings a few seconds after the operator stops changing them (saves flash wear) */
    if (memcmp(&ctl.set, &last_saved, sizeof last_saved) != 0)
    {
      settings_dirty = 1;
      settings_changed = now;
      last_saved = ctl.set;
    }
    if (settings_dirty && now - settings_changed >= SAVE_DELAY_MS)
    {
      settings_dirty = 0;
      if (save_settings(&ctl.set) != 0) printf("settings NOT saved to flash\r\n");
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 10;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 5;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_5;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_6;
  sConfig.Rank = ADC_REGULAR_RANK_2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_9;
  sConfig.Rank = ADC_REGULAR_RANK_3;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_15;
  sConfig.Rank = ADC_REGULAR_RANK_4;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_2;
  sConfig.Rank = ADC_REGULAR_RANK_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, LD2_Pin|FIRE_EXT_Pin|FIRE_RET_Pin|VALVE_T1_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, VALVE_T2_Pin|HTR_T1_Pin|HTR_T2_Pin|HTR_NOZ_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(MOTOR_GPIO_Port, MOTOR_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LD2_Pin FIRE_EXT_Pin FIRE_RET_Pin VALVE_T1_Pin */
  GPIO_InitStruct.Pin = LD2_Pin|FIRE_EXT_Pin|FIRE_RET_Pin|VALVE_T1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : VALVE_T2_Pin HTR_T1_Pin HTR_T2_Pin HTR_NOZ_Pin */
  GPIO_InitStruct.Pin = VALVE_T2_Pin|HTR_T1_Pin|HTR_T2_Pin|HTR_NOZ_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : MOTOR_Pin */
  GPIO_InitStruct.Pin = MOTOR_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(MOTOR_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : FLAME1_Pin FLAME2_Pin */
  GPIO_InitStruct.Pin = FLAME1_Pin|FLAME2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : FLAME3_Pin */
  GPIO_InitStruct.Pin = FLAME3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(FLAME3_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/* USART1 RX interrupt: collect a line, queue it for the main loop */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    char c = (char)rx_byte;
    if (c == '\r' || c == '\n')
    {
      uint8_t next = (uint8_t)((rx_head + 1) % RX_QUEUE);
      if (rx_len > 0 && next != rx_tail)              /* queue full: drop the line, the Pi retries */
      {
        rx_buf[rx_len] = '\0';
        memcpy(rx_queue[rx_head], rx_buf, (size_t)rx_len + 1);
        rx_head = next;
      }
      rx_len = 0;
    }
    else if (c == '$')                                /* a new line starts: drop any half line */
    {
      rx_buf[0] = c;
      rx_len = 1;
    }
    else if (c >= ' ' && rx_len < PROTO_LINE_MAX - 1)
    {
      rx_buf[rx_len++] = c;
    }
    HAL_UART_Receive_IT(huart, &rx_byte, 1);         /* re-arm for the next character */
  }
}

/* On any UART error (e.g. overrun) keep listening */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    rx_len = 0;
    HAL_UART_Receive_IT(huart, &rx_byte, 1);
  }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

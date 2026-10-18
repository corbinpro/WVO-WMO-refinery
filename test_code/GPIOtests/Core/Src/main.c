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
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
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

#define REFRESH_MS     250        /* console redraw period */
#define RX_BUF         32         /* max command length */
#define FRAME_SZ       2048       /* one-shot console frame buffer */
#define EOL            "\x1b[K\r\n"  /* clear to end of line, then newline */

/* Commanded output state is saved here so it survives a reset / power cycle.
 * Last 2K page of bank 2 - kept out of the FLASH region in STM32L476RGTX_FLASH.ld. */
#define STATE_ADDR     0x080FF800UL
#define STATE_BANK     FLASH_BANK_2
#define STATE_PAGE     255
#define STATE_MAGIC    0x57564F00UL  /* "WVO\0" in the upper word, output bits in the lower */

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

/* Serial command input from the RPi (filled by the USART1 RX interrupt) */
static uint8_t rx_byte;
static char rx_buf[RX_BUF];
static volatile uint8_t rx_len;
static char cmd_line[RX_BUF];
static volatile uint8_t cmd_ready;
static volatile uint8_t ui_dirty = 1;
static char last_msg[64] = "Ready. Type a command, e.g. t1";

/* Console frame is built in one buffer and sent in one transmit (no flicker) */
static char frame[FRAME_SZ];
static size_t fpos;

/* Outputs. on_level = pin level that turns the device ON.
 * Relay boards assumed active-HIGH; change to GPIO_PIN_RESET if yours are active-LOW.
 * name == NULL: no command, the output is forced ON at boot (tank heaters). */
typedef struct {
  const char    *name;      /* serial command name */
  const char    *label;     /* shown on the console */
  const char    *help;      /* what the command does */
  GPIO_TypeDef  *port;
  uint16_t       pin;
  GPIO_PinState  on_level;
} Output_t;

static const Output_t outputs[] = {
  { "t1",  "Tank 1 valves", "toggle Tank 1 valves (upper + lower)", RELAY_D2_GPIO_Port, RELAY_D2_Pin, GPIO_PIN_SET },
  { "t2",  "Tank 2 valves", "toggle Tank 2 valves (upper + lower)", RELAY_D3_GPIO_Port, RELAY_D3_Pin, GPIO_PIN_SET },
  { "htr", "Nozzle heater", "toggle nozzle heater",                 HTR_NOZ_GPIO_Port,  HTR_NOZ_Pin,  GPIO_PIN_SET },
  { "cent","Centrifuge",    "toggle centrifuge motor",              MOTOR_GPIO_Port,    MOTOR_Pin,    GPIO_PIN_SET },
  { NULL,  "Tank 1 heater", NULL,                                   HTR_T1_GPIO_Port,   HTR_T1_Pin,   GPIO_PIN_SET },
  { NULL,  "Tank 2 heater", NULL,                                   HTR_T2_GPIO_Port,   HTR_T2_Pin,   GPIO_PIN_SET },
};
#define N_OUT (sizeof(outputs) / sizeof(outputs[0]))

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

/* Route printf to USART2 (ST-LINK virtual COM port) - debug only, the console is on USART1 */
int __io_putchar(int ch)
{
  uint8_t c = (uint8_t)ch;
  HAL_UART_Transmit(&huart2, &c, 1, HAL_MAX_DELAY);
  return ch;
}

static void set_output(size_t i, int on)
{
  GPIO_PinState off_level = (outputs[i].on_level == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET;
  HAL_GPIO_WritePin(outputs[i].port, outputs[i].pin, on ? outputs[i].on_level : off_level);
}

static int output_is_on(size_t i)
{
  /* ReadPin reads IDR, which reflects the driven level of an output pin */
  return HAL_GPIO_ReadPin(outputs[i].port, outputs[i].pin) == outputs[i].on_level;
}

/* Bit i set = commandable output i is ON */
static uint32_t command_bits(void)
{
  uint32_t bits = 0;
  for (size_t i = 0; i < N_OUT; i++)
  {
    if (outputs[i].name != NULL && output_is_on(i)) bits |= 1UL << i;
  }
  return bits;
}

/* Restore commanded outputs from flash. Erased/unknown record = all OFF. */
static void load_state(void)
{
  uint64_t rec = *(volatile const uint64_t *)STATE_ADDR;
  if ((uint32_t)(rec >> 32) != STATE_MAGIC) return;
  for (size_t i = 0; i < N_OUT; i++)
  {
    if (outputs[i].name != NULL) set_output(i, (int)((rec >> i) & 1U));
  }
}

/* Write the commanded state to flash (only if it changed). Returns 0 on success. */
static int save_state(void)
{
  uint64_t rec = ((uint64_t)STATE_MAGIC << 32) | command_bits();
  if (*(volatile const uint64_t *)STATE_ADDR == rec) return 0;

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
  if (st == HAL_OK) st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, STATE_ADDR, rec);
  HAL_FLASH_Lock();
  return (st == HAL_OK) ? 0 : -1;
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

/* Append formatted text to the console frame */
static void emit(const char *fmt, ...)
{
  if (fpos >= FRAME_SZ - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(frame + fpos, FRAME_SZ - fpos, fmt, ap);
  va_end(ap);
  if (n > 0) fpos += (size_t)n;
  if (fpos > FRAME_SZ - 1) fpos = FRAME_SZ - 1;
}

/* "-12.3" style tenths without needing printf float support */
static void tenths(char *dst, size_t n, int32_t t10)
{
  int32_t a = (t10 < 0) ? -t10 : t10;
  snprintf(dst, n, "%s%ld.%ld", (t10 < 0) ? "-" : "", (long)(a / 10), (long)(a % 10));
}

/* Temp divider: 3.3V -> NTC -> node(ADC) -> 10k -> GND
 * R_ntc = R_FIXED*(4095-raw)/raw, then Beta equation. */
static void emit_temp(const char *label, uint32_t raw)
{
  if (raw < 10)   { emit("   %-13s: OPEN (no sensor?)" EOL, label); return; }
  if (raw > 4085) { emit("   %-13s: SHORT" EOL, label);             return; }

  float r_ntc = R_FIXED * (ADC_MAX - (float)raw) / (float)raw;
  float t_k   = 1.0f / (1.0f / NTC_T25_K + logf(r_ntc / NTC_R25) / NTC_BETA);
  float t_c   = t_k - 273.15f;
  float t_f   = t_c * 9.0f / 5.0f + 32.0f;
  char cs[12], fs[12];
  tenths(cs, sizeof cs, (int32_t)(t_c * 10.0f + (t_c >= 0 ? 0.5f : -0.5f)));
  tenths(fs, sizeof fs, (int32_t)(t_f * 10.0f + (t_f >= 0 ? 0.5f : -0.5f)));
  emit("   %-13s: %6s F   %6s C" EOL, label, fs, cs);
}

/* Level divider: 3.3V -> sender -> node(ADC) -> 100 ohm -> GND
 * Fill % assumes resistance linear with float position (UNVERIFIED - see notes.txt). */
static void emit_level(const char *label, uint32_t raw)
{
  if (raw < 10)   { emit("   %-13s: OPEN (no sender?)" EOL, label); return; }
  if (raw > 4085) { emit("   %-13s: SHORT" EOL, label);             return; }

  float r_s  = LVL_R_FIXED * (ADC_MAX - (float)raw) / (float)raw;
  float fill = (LVL_R_EMPTY - r_s) / (LVL_R_EMPTY - LVL_R_FULL) * 100.0f;
  char fs[12];
  tenths(fs, sizeof fs, (int32_t)(fill * 10.0f + (fill >= 0 ? 0.5f : -0.5f)));
  emit("   %-13s: %6s %%   (%lu ohm)" EOL, label, fs, (unsigned long)r_s);
}

static void render_ui(void)
{
  uint32_t up = HAL_GetTick() / 1000;
  fpos = 0;
  emit("\x1b[H");                                   /* cursor home (no full clear = no flicker) */
  emit(" WVO/WMO REFINERY CONSOLE        uptime %02lu:%02lu:%02lu" EOL,
       (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
  emit(" ------------------------------------------------" EOL);
  emit(" TEMPERATURES" EOL);
  emit_temp("Tank 1  (A0)", adc_raw[0]);
  emit_temp("Tank 2  (A1)", adc_raw[1]);
  emit_temp("Nozzle  (A2)", adc_raw[2]);
  emit(EOL);
  emit(" TANK LEVELS" EOL);
  emit_level("Tank 1  (A3)", adc_raw[3]);
  emit_level("Tank 2  (A4)", adc_raw[4]);
  emit(EOL);
  emit(" OUTPUTS" EOL);
  for (size_t i = 0; i < N_OUT; i++)
  {
    emit("   %-13s: %s   %s" EOL, outputs[i].label, output_is_on(i) ? "ON " : "OFF",
         outputs[i].name ? "" : "(always on)");
  }
  emit(EOL);
  emit(" COMMANDS  (type, then Enter)" EOL);
  for (size_t i = 0; i < N_OUT; i++)
  {
    if (outputs[i].name != NULL) emit("   %-5s %s" EOL, outputs[i].name, outputs[i].help);
  }
  emit("   add 'on' or 'off' to force a state, e.g. 't1 off'" EOL);
  emit("   settings are saved; USB can be unplugged after" EOL);
  emit(" ------------------------------------------------" EOL);
  emit(" %s" EOL, last_msg);
  emit(" > %.*s\x1b[K\x1b[J", (int)rx_len, rx_buf);  /* prompt + typed text, clear the rest */
  HAL_UART_Transmit(&huart1, (uint8_t *)frame, (uint16_t)fpos, HAL_MAX_DELAY);
}

/* "<name>" toggles, "<name> on|off" forces; case-insensitive */
static void handle_command(char *line)
{
  for (char *p = line; *p; p++) *p = (char)tolower((unsigned char)*p);
  char *name  = strtok(line, " \t");
  char *arg   = strtok(NULL, " \t");
  char *extra = strtok(NULL, " \t");
  if (name == NULL) return;

  for (size_t i = 0; i < N_OUT; i++)
  {
    if (outputs[i].name == NULL || strcmp(name, outputs[i].name) != 0) continue;

    int on;
    if (arg == NULL)                                  on = !output_is_on(i);
    else if (extra == NULL && strcmp(arg, "on") == 0)  on = 1;
    else if (extra == NULL && strcmp(arg, "off") == 0) on = 0;
    else
    {
      snprintf(last_msg, sizeof last_msg, "ERR: use '%s', '%s on' or '%s off'",
               outputs[i].name, outputs[i].name, outputs[i].name);
      return;
    }
    set_output(i, on);
    snprintf(last_msg, sizeof last_msg, "OK: %s %s%s", outputs[i].label, on ? "ON" : "OFF",
             save_state() == 0 ? "" : "  (WARNING: not saved to flash)");
    return;
  }
  snprintf(last_msg, sizeof last_msg, "ERR: unknown '%.20s' (commands: t1 t2 htr cent)", name);
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
  /* Tank heaters always ON; commanded outputs OFF, then restored from the last saved state */
  for (size_t i = 0; i < N_OUT; i++)
  {
    set_output(i, outputs[i].name == NULL);
  }
  load_state();
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_UART_Receive_IT(&huart1, &rx_byte, 1);         /* start interrupt-driven command input (RPi) */
  {
    static const char clr[] = "\x1b[2J\x1b[H";       /* clear screen once */
    HAL_UART_Transmit(&huart1, (uint8_t *)clr, sizeof clr - 1, HAL_MAX_DELAY);
  }
  uint32_t last_ui = 0;

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (cmd_ready)
    {
      char line[RX_BUF];
      __disable_irq();
      memcpy(line, cmd_line, RX_BUF);
      cmd_ready = 0;
      __enable_irq();
      handle_command(line);
      ui_dirty = 1;
    }
    if (ui_dirty || (HAL_GetTick() - last_ui) >= REFRESH_MS)
    {
      ui_dirty = 0;
      last_ui = HAL_GetTick();
      read_channels(adc_raw);
      render_ui();
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
  HAL_GPIO_WritePin(GPIOA, LD2_Pin|RELAY_D7_Pin|RELAY_D8_Pin|RELAY_D2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, RELAY_D3_Pin|HTR_T1_Pin|HTR_T2_Pin|HTR_NOZ_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(MOTOR_GPIO_Port, MOTOR_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LD2_Pin RELAY_D7_Pin RELAY_D8_Pin RELAY_D2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin|RELAY_D7_Pin|RELAY_D8_Pin|RELAY_D2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : RELAY_D3_Pin HTR_T1_Pin HTR_T2_Pin HTR_NOZ_Pin */
  GPIO_InitStruct.Pin = RELAY_D3_Pin|HTR_T1_Pin|HTR_T2_Pin|HTR_NOZ_Pin;
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

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USART1 RX interrupt: build a command line one character at a time */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    char c = (char)rx_byte;
    if (c == '\r' || c == '\n')
    {
      if (rx_len > 0)
      {
        rx_buf[rx_len] = '\0';
        memcpy(cmd_line, rx_buf, (size_t)rx_len + 1);
        cmd_ready = 1;
        rx_len = 0;
      }
    }
    else if (c == 0x08 || c == 0x7F)                 /* backspace / delete */
    {
      if (rx_len > 0) rx_len--;
    }
    else if (c >= ' ' && rx_len < RX_BUF - 1)
    {
      rx_buf[rx_len++] = c;
    }
    ui_dirty = 1;
    HAL_UART_Receive_IT(huart, &rx_byte, 1);         /* re-arm for the next character */
  }
}

/* On any UART error (e.g. overrun) keep listening */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
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

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
#include <string.h>
#include <stdio.h>
#include <ctype.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Pakkeformat på UART4 mellom MCU og SDR.
 * CRC8-polynom 0x07 er identisk med SDR-siden (uart_bridge.c),
 * slik at bakkesiden kan validere rammen med samme rutine. */
#define SDR_SYNC1       0xAAu
#define SDR_SYNC2       0x55u
#define SDR_MAX_DATA    60u
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

FDCAN_HandleTypeDef hfdcan1;

UART_HandleTypeDef huart4;
UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */
uint8_t uart3_rx_byte;   /* tegn fra PuTTY / terminal     */
uint8_t uart4_rx_byte;   /* rå byte fra ekstern UART4-enhet */

char cmd_buffer[128];
volatile uint16_t cmd_index = 0;
volatile uint8_t  cmd_ready = 0;

uint8_t tx_data_buffer[64];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_FDCAN1_Init(void);
static void MX_UART4_Init(void);
static void MX_USART3_UART_Init(void);
/* USER CODE BEGIN PFP */

void UART_SendString(const char *s);
void UART_ProcessReceivedChar(uint8_t ch);
int  HexStringToBytes(const char *str, uint8_t *out, int max_len);
void UART_PrintByteAsHex(uint8_t byte);
void UART_SendLine(const char *s);

uint8_t calculate_crc8(const uint8_t *buf, int len);
int     UART4_SendPacket(const uint8_t *data, int len);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

void UART_SendString(const char *s)
{
  HAL_UART_Transmit(&huart3, (uint8_t *)s, strlen(s), HAL_MAX_DELAY);
}

void UART_PrintByteAsHex(uint8_t byte)
{
  char msg[4];
  snprintf(msg, sizeof(msg), "%02X ", byte);
  HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
}

void UART_SendLine(const char *s)
{
  UART_SendString(s);
  UART_SendString("\r\n");
}

/*
 * Parser tekst som:
 *   "01 A0 FF 2B"
 * til bytes:
 *   0x01, 0xA0, 0xFF, 0x2B
 *
 * Returnerer antall bytes, eller -1 ved feil.
 */
int HexStringToBytes(const char *str, uint8_t *out, int max_len)
{
  int count = 0;
  int high_nibble = -1;

  while (*str != '\0')
  {
    char c = *str++;

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
    {
      continue;
    }

    int value;
    if (c >= '0' && c <= '9')
      value = c - '0';
    else if (c >= 'A' && c <= 'F')
      value = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f')
      value = c - 'a' + 10;
    else
      return -1;

    if (high_nibble < 0)
    {
      high_nibble = value;
    }
    else
    {
      if (count >= max_len)
        return -1;

      out[count++] = (high_nibble << 4) | value;
      high_nibble = -1;
    }
  }

  /* Ugyldig hvis det er oddetall hex-tegn */
  if (high_nibble >= 0)
    return -1;

  return count;
}

void UART_ProcessReceivedChar(uint8_t ch)
{
  if (ch == '\r' || ch == '\n')
  {
    if (cmd_index == 0)
    {
      UART_SendString("\r\n");
      return;
    }

    cmd_buffer[cmd_index] = '\0';
    cmd_ready = 1;
    cmd_index = 0;

    UART_SendString("\r\n[ENTER DETECTED]\r\n");
    return;
  }

  if (ch == '\b' || ch == 0x7F)
  {
    if (cmd_index > 0)
    {
      cmd_index--;
      UART_SendString("\b \b");
    }
    return;
  }

  if (cmd_index < sizeof(cmd_buffer) - 1)
  {
    cmd_buffer[cmd_index++] = (char)ch;
  }
  else
  {
    UART_SendString("\r\nInput buffer full\r\n");
    cmd_index = 0;
  }
}

/*
 * CRC8 med polynom 0x07, initial-verdi 0, ingen XOR-out.
 * Identisk med calculate_crc8_local() på SDR-siden (uart_bridge.c),
 * slik at bakkesiden kan validere rammen ende-til-ende.
 */
uint8_t calculate_crc8(const uint8_t *buf, int len)
{
  uint8_t crc = 0;
  for (int i = 0; i < len; i++)
  {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++)
    {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

/*
 * Pakker nyttedata i UART-rammen og sender hele rammen på UART4.
 *
 *   [0]      SYNC1 = 0xAA
 *   [1]      SYNC2 = 0x55
 *   [2]      LEN   (1..SDR_MAX_DATA)
 *   [3..]    DATA[LEN]
 *   [3+LEN]  SEQ    -- 8-bit løpende teller
 *   [4+LEN]  CRC8   over byte [2..3+LEN] (LEN, DATA, SEQ)
 *
 * Returnerer antall sendte byte (frame-lengde), eller -1 ved feil.
 */
int UART4_SendPacket(const uint8_t *data, int len)
{
  static uint8_t tx_seq = 0;

  if (len < 1 || (uint32_t)len > SDR_MAX_DATA)
    return -1;

  uint8_t frame[5 + SDR_MAX_DATA];
  frame[0] = SDR_SYNC1;
  frame[1] = SDR_SYNC2;
  frame[2] = (uint8_t)len;
  memcpy(&frame[3], data, (size_t)len);
  frame[3 + len] = tx_seq++;
  frame[4 + len] = calculate_crc8(&frame[2], 2 + len);

  int frame_len = 5 + len;

  if (HAL_UART_Transmit(&huart4, frame, (uint16_t)frame_len, HAL_MAX_DELAY) != HAL_OK)
    return -1;

  /* Skriv ut hele rammen på terminalen for verifisering */
  UART_SendString("UART4 frame: ");
  for (int i = 0; i < frame_len; i++)
    UART_PrintByteAsHex(frame[i]);
  UART_SendString("\r\n");

  return frame_len;
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

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

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
  MX_FDCAN1_Init();
  MX_UART4_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */

  UART_SendString("\r\n====================================\r\n");
  UART_SendString("UART bridge ready\r\n");
  UART_SendString("Terminal : USART3 (PuTTY)\r\n");
  UART_SendString("External : UART4 -> SDR\r\n");
  UART_SendString("Input    : hex bytes, e.g. 01 A0 FF 2B\r\n");
  UART_SendString("Max data : 60 byte per pakke\r\n");
  UART_SendString("Frame    : AA 55 LEN DATA[] SEQ CRC8\r\n");
  UART_SendString("====================================\r\n\r\n");

  if (HAL_UART_Receive_IT(&huart3, &uart3_rx_byte, 1) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_UART_Receive_IT(&huart4, &uart4_rx_byte, 1) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    if (cmd_ready)
    {
      cmd_ready = 0;

      int len = HexStringToBytes(cmd_buffer, tx_data_buffer, sizeof(tx_data_buffer));

      if (len > 0 && (uint32_t)len <= SDR_MAX_DATA)
      {
        UART_SendString("Sending DATA    : ");
        for (int i = 0; i < len; i++)
          UART_PrintByteAsHex(tx_data_buffer[i]);
        UART_SendString("\r\n");

        if (UART4_SendPacket(tx_data_buffer, len) < 0)
          UART_SendString("UART4 transmit error\r\n");
        else
          UART_SendString("TX done\r\n");
      }
      else if (len > 0 && (uint32_t)len > SDR_MAX_DATA)
      {
        UART_SendString("Too many bytes. Max 60 per pakke.\r\n");
      }
      else
      {
        UART_SendString("Invalid hex input. Example: 01 A0 FF 2B\r\n");
      }
    }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 32;
  RCC_OscInitStruct.PLL.PLLN = 129;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_1;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief FDCAN1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_FDCAN1_Init(void)
{

  /* USER CODE BEGIN FDCAN1_Init 0 */
  /* USER CODE END FDCAN1_Init 0 */

  /* USER CODE BEGIN FDCAN1_Init 1 */
  /* USER CODE END FDCAN1_Init 1 */
  hfdcan1.Instance = FDCAN1;
  hfdcan1.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
  hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;
  hfdcan1.Init.AutoRetransmission = DISABLE;
  hfdcan1.Init.TransmitPause = DISABLE;
  hfdcan1.Init.ProtocolException = DISABLE;
  hfdcan1.Init.NominalPrescaler = 16;
  hfdcan1.Init.NominalSyncJumpWidth = 1;
  hfdcan1.Init.NominalTimeSeg1 = 1;
  hfdcan1.Init.NominalTimeSeg2 = 1;
  hfdcan1.Init.DataPrescaler = 1;
  hfdcan1.Init.DataSyncJumpWidth = 1;
  hfdcan1.Init.DataTimeSeg1 = 1;
  hfdcan1.Init.DataTimeSeg2 = 1;
  hfdcan1.Init.MessageRAMOffset = 0;
  hfdcan1.Init.StdFiltersNbr = 1;
  hfdcan1.Init.ExtFiltersNbr = 0;
  hfdcan1.Init.RxFifo0ElmtsNbr = 1;
  hfdcan1.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan1.Init.RxFifo1ElmtsNbr = 0;
  hfdcan1.Init.RxFifo1ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan1.Init.RxBuffersNbr = 0;
  hfdcan1.Init.RxBufferSize = FDCAN_DATA_BYTES_8;
  hfdcan1.Init.TxEventsNbr = 0;
  hfdcan1.Init.TxBuffersNbr = 0;
  hfdcan1.Init.TxFifoQueueElmtsNbr = 0;
  hfdcan1.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  hfdcan1.Init.TxElmtSize = FDCAN_DATA_BYTES_8;
  if (HAL_FDCAN_Init(&hfdcan1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN FDCAN1_Init 2 */
  /* USER CODE END FDCAN1_Init 2 */

}

/**
  * @brief UART4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_UART4_Init(void)
{

  /* USER CODE BEGIN UART4_Init 0 */
  /* USER CODE END UART4_Init 0 */

  /* USER CODE BEGIN UART4_Init 1 */
  /* USER CODE END UART4_Init 1 */
  huart4.Instance = UART4;
  huart4.Init.BaudRate = 115200;
  huart4.Init.WordLength = UART_WORDLENGTH_8B;
  huart4.Init.StopBits = UART_STOPBITS_1;
  huart4.Init.Parity = UART_PARITY_NONE;
  huart4.Init.Mode = UART_MODE_TX_RX;
  huart4.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart4.Init.OverSampling = UART_OVERSAMPLING_16;
  huart4.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart4.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart4.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart4) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart4, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart4, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN UART4_Init 2 */
  /* USER CODE END UART4_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */
  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */
  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */
  /* USER CODE END USART3_Init 2 */

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
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PB10 PB11 */
  GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3)
  {
    /* Mottak fra terminal / PuTTY */
    UART_ProcessReceivedChar(uart3_rx_byte);

    if (HAL_UART_Receive_IT(&huart3, &uart3_rx_byte, 1) != HAL_OK)
    {
      Error_Handler();
    }
  }
  else if (huart->Instance == UART4)
  {
    /* Mottak fra ekstern UART4-enhet -> vis i terminal som hex */
    UART_PrintByteAsHex(uart4_rx_byte);

    if (HAL_UART_Receive_IT(&huart4, &uart4_rx_byte, 1) != HAL_OK)
    {
      Error_Handler();
    }
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3)
  {
    UART_SendLine("USART3 error");
    __HAL_UART_CLEAR_OREFLAG(&huart3);

    if (HAL_UART_Receive_IT(&huart3, &uart3_rx_byte, 1) != HAL_OK)
    {
      Error_Handler();
    }
  }
  else if (huart->Instance == UART4)
  {
    __HAL_UART_CLEAR_OREFLAG(&huart4);

    if (HAL_UART_Receive_IT(&huart4, &uart4_rx_byte, 1) != HAL_OK)
    {
      Error_Handler();
    }
  }
}

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
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

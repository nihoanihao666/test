/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    dma.c
  * @brief   This file provides code for the configuration
  *          of the DMA1 controller used by USART2_RX.
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "dma.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  Enable DMA1 clock and the USART2_RX DMA interrupt.
  * @note   USART2_RX uses DMA1_Channel6 on STM32F103.
  */
void MX_DMA_Init(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();

  HAL_NVIC_SetPriority(DMA1_Channel6_IRQn, 1U, 0U);
  HAL_NVIC_EnableIRQ(DMA1_Channel6_IRQn);
}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */

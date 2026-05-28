/*
 * call_back_functions.c
 *
 *  Created on: 2026年4月1日
 *      Author: UnikoZera
 */

#include "call_back_functions.h"

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1)
    {
        // 等待SPI完成最后一字节移位，防止LCD收到不完整数据导致白条
        while (__HAL_SPI_GET_FLAG(hspi, SPI_FLAG_BSY) != RESET) {}
        LCD_CS_Set();
        lcd_dma_busy = false;
    }
    if (hspi == &hspi2)
    {
        // 等待SPI完成最后一字节的移位输出，确保W25Q正确锁存数据
        while (__HAL_SPI_GET_FLAG(hspi, SPI_FLAG_BSY) != RESET) {}
        W25Q_CS_HIGH();
        w25q_tx_dma_busy = false;
    }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi2)
    {
        W25Q_CS_HIGH();
        w25q_rx_dma_busy = false;
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi2)
    {
        W25Q_CS_HIGH();
        w25q_tx_dma_busy = false;
        w25q_rx_dma_busy = false;
        w25q_on_spi_error_callback();
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3)
    {
        /* CODE */
    }
}

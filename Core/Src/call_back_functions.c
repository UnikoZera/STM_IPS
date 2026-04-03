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
		LCD_CS_Set();
		lcd_dma_busy = false;
	}
    if (hspi == &hspi2)
    {
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

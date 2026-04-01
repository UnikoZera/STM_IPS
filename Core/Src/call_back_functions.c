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
		dma_busy = false;
	}
    if (hspi == &hspi2)
    {
        // w25q的dma传输完成回调，暂时不需要处理
    }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi2)
    {
        // w25q的dma接收完成回调，暂时不需要处理
    }
}
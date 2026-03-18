/*
 * at24c_controller.c
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#include "at24c_controller.h"

// AT24C64 page size = 32 bytes
#define AT24C64_PAGE_SIZE 32
#define AT24C_WRITE_DELAY 5

void at24c_write_byte(uint16_t memAddress, uint8_t *data)
{
    HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, data, 1, 100);
    HAL_Delay(AT24C_WRITE_DELAY);
}

void at24c_read_byte(uint16_t memAddress, uint8_t *data)
{
    HAL_I2C_Mem_Read(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, data, 1, 100);
}

void at24c_write_buffer(uint16_t memAddress, uint8_t *pData, uint16_t size)
{
    if (size == 0)  return;

    uint16_t pageOffset = memAddress % AT24C64_PAGE_SIZE;
    uint16_t firstPageWriteSize = AT24C64_PAGE_SIZE - pageOffset;

    if (size <= firstPageWriteSize)
    {
        HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, size, 1000);
        HAL_Delay(AT24C_WRITE_DELAY);
    }
    else
    {
        HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, firstPageWriteSize, 1000);
        HAL_Delay(AT24C_WRITE_DELAY);

        memAddress += firstPageWriteSize;
        pData += firstPageWriteSize;
        size -= firstPageWriteSize;

        while (size >= AT24C64_PAGE_SIZE)
        {
            HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, AT24C64_PAGE_SIZE, 1000);
            HAL_Delay(AT24C_WRITE_DELAY);

            memAddress += AT24C64_PAGE_SIZE;
            pData += AT24C64_PAGE_SIZE;
            size -= AT24C64_PAGE_SIZE;
        }

        if (size > 0)
        {
            HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, size, 1000);
            HAL_Delay(AT24C_WRITE_DELAY);
        }
    }
}

void at24c_read_buffer(uint16_t memAddress, uint8_t *pData, uint16_t size)
{
    HAL_I2C_Mem_Read(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, size, 1000);
}
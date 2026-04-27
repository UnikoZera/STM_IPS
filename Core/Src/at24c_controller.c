/*
 * at24c_controller.c
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#include "at24c_controller.h"


static bool at24c_wait_ready(void)
{
    uint32_t tickstart = HAL_GetTick();
    while (HAL_I2C_IsDeviceReady(&hi2c1, AT24C64_ADDRESS, 3, 10) != HAL_OK) // 这里是尝试3次，每次10ms的超时.
    {
        if ((HAL_GetTick() - tickstart) > 30) // 因为有3次尝试，每次10ms，所以总等待时间超过30ms就认为是超时失败.
        {
            return false; // 超时
        }
    }
    return true;
}

/**
 * @brief 写入单个字节到AT24C64 EEPROM
 * 
 * @param memAddress 写入的内存地址，范围0~65535
 * @param data 注意这里是指针，调用时需要传入数据的地址(一个字节)，例如：uint8_t value = 0xAB; at24c_write_byte(0x0000, &value);
 * @return true 成功
 * @return false 失败
 */
bool at24c_write_byte(uint16_t memAddress, uint8_t *data)
{
    if (memAddress >= AT24C64_CAPACITY) return false;
    if (data == NULL) return false;

    if (HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, data, 1, AT24C_TIMEOUT) != HAL_OK)
    {
        return false;
    }
    return at24c_wait_ready();
}

/**
 * @brief 从AT24C64 EEPROM读取单个字节
 * 
 * @param memAddress 读取的内存地址，范围0~65535
 * @param data 注意这里是指针，调用时需要传入数据的地址(一个字节)，例如：uint8_t value; at24c_read_byte(0x0000, &value);
 * @return true 成功
 * @return false 失败
 */
bool at24c_read_byte(uint16_t memAddress, uint8_t *data)
{
    if (memAddress >= AT24C64_CAPACITY) return false;
    if (data == NULL) return false;

    if (HAL_I2C_Mem_Read(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, data, 1, AT24C_TIMEOUT) != HAL_OK)
    {
        return false;
    }
    return true;
}

/**
 * @brief 写入数据到AT24C64 EEPROM
 * 
 * @param memAddress 写入的内存地址，范围0~65535
 * @param pData 数据指针，调用时需要传入数据的地址
 * @param size 写入的数据大小
 * @return true 成功
 * @return false 失败
 */
bool at24c_write_buffer(uint16_t memAddress, uint8_t *pData, uint16_t size)
{
    if (size == 0) return true;
    if ((memAddress + size) > AT24C64_CAPACITY) return false; // 越界检查
    if (pData == NULL) return false;

    uint16_t pageOffset = memAddress % AT24C64_PAGE_SIZE;
    uint16_t firstPageWriteSize = AT24C64_PAGE_SIZE - pageOffset;

    if (size <= firstPageWriteSize)
    {
        if (HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, size, AT24C_TIMEOUT) != HAL_OK) return false;
        if (!at24c_wait_ready()) return false;
    }
    else
    {
        if (HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, firstPageWriteSize, AT24C_TIMEOUT) != HAL_OK) return false;
        if (!at24c_wait_ready()) return false;

        memAddress += firstPageWriteSize;
        pData += firstPageWriteSize;
        size -= firstPageWriteSize;

        while (size >= AT24C64_PAGE_SIZE)
        {
            if (HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, AT24C64_PAGE_SIZE, AT24C_TIMEOUT) != HAL_OK) return false;
            if (!at24c_wait_ready()) return false;

            memAddress += AT24C64_PAGE_SIZE;
            pData += AT24C64_PAGE_SIZE;
            size -= AT24C64_PAGE_SIZE;
        }

        if (size > 0)
        {
            if (HAL_I2C_Mem_Write(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, size, AT24C_TIMEOUT) != HAL_OK) return false;
            if (!at24c_wait_ready()) return false;
        }
    }
    return true;
}

/**
 * @brief 从AT24C64 EEPROM读取数据
 * 
 * @param memAddress 读取的内存地址，范围0~65535
 * @param pData 数据指针，调用时需要传入数据的地址
 * @param size 读取的数据大小
 * @return true 成功
 * @return false 失败
 */
bool at24c_read_buffer(uint16_t memAddress, uint8_t *pData, uint16_t size)
{
    if (size == 0) return true;
    if ((memAddress + size) > AT24C64_CAPACITY) return false;

    if (HAL_I2C_Mem_Read(&hi2c1, AT24C64_ADDRESS, memAddress, I2C_MEMADD_SIZE_16BIT, pData, size, AT24C_TIMEOUT) != HAL_OK)
    {
        return false;
    }
    return true;
}
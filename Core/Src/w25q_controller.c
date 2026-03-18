/*
 * w25q_controller.c
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#include "w25q_controller.h"

// static void w25q_spi_transmit_receive(uint8_t *txData, uint8_t *rxData, uint16_t size) // not using dma for now, just base functions.
// {
//     HAL_SPI_TransmitReceive(&hspi2, txData, rxData, size, W25Q_TIMEOUT);
// }

static void w25q_spi_transmit(uint8_t *pData, uint16_t size) // not using dma for now, just base functions.
{
    HAL_SPI_Transmit(&hspi2, pData, size, W25Q_TIMEOUT);
}

static void w25q_spi_receive(uint8_t *pData, uint16_t size) // not using dma for now, just base functions.
{
    HAL_SPI_Receive(&hspi2, pData, size, W25Q_TIMEOUT);
}

// static void w25q_dma_transmit(uint8_t *pData, uint16_t size)
// {
//     HAL_SPI_Transmit_DMA(&hspi2, pData, size);
// }

// static void w25q_dma_receive(uint8_t *pData, uint16_t size)
// {
//     HAL_SPI_Receive_DMA(&hspi2, pData, size);
// }

// 写使能
void w25q_write_enable(void)
{
    uint8_t cmd = W25Q_WriteEnable;
    W25Q_CS_LOW();
    w25q_spi_transmit(&cmd, 1);
    W25Q_CS_HIGH();
}

// 写禁止
void w25q_write_disable(void)
{
    W25Q_CS_LOW();
    uint8_t cmd = W25Q_WriteDisable;
    w25q_spi_transmit(&cmd, 1);
    W25Q_CS_HIGH();
}

// 读状态寄存器1，包含忙碌位
void w25q_read_status_reg1(uint8_t *status)
{
    W25Q_CS_LOW();
    uint8_t cmd = W25Q_ReadStatusReg1;
    w25q_spi_transmit(&cmd, 1);             // 先发指令
    w25q_spi_receive(status, 1);            // 再读状态
    W25Q_CS_HIGH();
}

// 为避免死锁，他的优先级最高，无视忙碌和设置忙碌状态
void w25q_read_status_reg2(uint8_t *status)
{
    W25Q_CS_LOW();
    uint8_t cmd = W25Q_ReadStatusReg2;
    w25q_spi_transmit(&cmd, 1);
    w25q_spi_receive(status, 1);
    W25Q_CS_HIGH();
}

// that is bad option. 堵塞式 I don't want to use it.
void w25q_check_busy(void)
{
    uint32_t tickstart = HAL_GetTick();
    uint8_t status;
    do
    {
        w25q_read_status_reg1(&status);
        if ((status & 0x01) == 0) return;
    } while ((HAL_GetTick() - tickstart) < W25Q_TIMEOUT);
}

void w25q_check_busy_nontimeout(void)
{
    uint8_t status;
    do
    {
        w25q_read_status_reg1(&status);
    } while ((status & 0x01) != 0);
}

// 读取设备ID
uint32_t w25q_read_id(void)
{
    W25Q_CS_LOW();
    uint8_t cmd = W25Q_JedecDeviceID;
    uint8_t id_bytes[3];
    w25q_spi_transmit(&cmd, 1);
    w25q_spi_receive(id_bytes, 3);
    W25Q_CS_HIGH();
    return ((uint32_t)id_bytes[0] << 16) | ((uint32_t)id_bytes[1] << 8) | (uint32_t)id_bytes[2]; // return the device ID
}

// FIXME: 如果有人用的不是 w25q128
bool w25q_init(void)
{
    uint32_t id = w25q_read_id();
    if (id != 0xEF4018) return false;
    return true;
}

void w25q_erase_sector(uint32_t address)
{
    w25q_write_enable();
    W25Q_CS_LOW();
    address &= 0xFFFFF000;  // sector address must be aligned to 4KB
    uint8_t cmd[4];
    cmd[0] = W25Q_SectorErase;
    cmd[1] = (address >> 16) & 0xFF;
    cmd[2] = (address >> 8) & 0xFF;
    cmd[3] = address & 0xFF;
    w25q_spi_transmit(cmd, 4);
    W25Q_CS_HIGH();
    w25q_check_busy_nontimeout();
}

void w25q_erase_chip(void)
{
    w25q_write_enable();
    uint8_t cmd = W25Q_ChipErase;
    W25Q_CS_LOW();
    w25q_spi_transmit(&cmd, 1);
    W25Q_CS_HIGH();
    w25q_check_busy_nontimeout();
}

void w25q_page_program(uint32_t address, uint8_t *data, uint16_t size)
{
    uint16_t current_page_remain = 256 - (address % 256);
    if (size > current_page_remain) return;
    w25q_write_enable();
    W25Q_CS_LOW();
    uint8_t cmd[4];
    cmd[0] = W25Q_PageProgram;
    cmd[1] = (address >> 16) & 0xFF;
    cmd[2] = (address >> 8) & 0xFF;
    cmd[3] = address & 0xFF;
    w25q_spi_transmit(cmd, 4);
    w25q_spi_transmit(data, size);
    W25Q_CS_HIGH();
    w25q_check_busy();
}

void w25q_write_data(uint32_t address, uint8_t *data, uint32_t size)
{
    uint32_t current_address = address;
    uint8_t *current_data = data;
    uint32_t remain_size = size;

    while (remain_size > 0) // sorry for the long func. Xp
    {
        uint16_t current_page_remain = 256 - (current_address % 256);
        uint16_t write_size = (remain_size > current_page_remain) ? current_page_remain : remain_size;

        w25q_page_program(current_address, current_data, write_size);
        current_address += write_size;
        current_data += write_size;
        remain_size -= write_size;
    }
}

void w25q_read_data(uint32_t address, uint8_t *data, uint32_t size)
{
    uint8_t cmd[4];
    cmd[0] = W25Q_ReadData;
    cmd[1] = (address >> 16) & 0xFF;
    cmd[2] = (address >> 8) & 0xFF;
    cmd[3] = address & 0xFF;
    W25Q_CS_LOW();
    w25q_spi_transmit(cmd, 4);
    w25q_spi_receive(data, size);
    W25Q_CS_HIGH();
}

void w25q_fast_read_data(uint32_t address, uint8_t *data, uint32_t size)
{
    uint8_t cmd[5];
    cmd[0] = W25Q_FastReadData;
    cmd[1] = (address >> 16) & 0xFF;
    cmd[2] = (address >> 8) & 0xFF;
    cmd[3] = address & 0xFF;
    cmd[4] = W25Q_DUMMY_BYTE;
    W25Q_CS_LOW();
    w25q_spi_transmit(cmd, 5);
    w25q_spi_receive(data, size);
    W25Q_CS_HIGH();
}
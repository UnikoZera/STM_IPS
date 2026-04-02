/*
 * w25q_controller.c
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#include "w25q_controller.h"

volatile bool w25q_rx_dma_busy = false;
volatile bool w25q_tx_dma_busy = false;

#define W25Q_PAGE_SIZE      256U
#define W25Q_ADDRESS_SPACE  0x01000000UL
#define W25Q_TASK_SPI_TIMEOUT 1U

typedef enum
{
    W25Q_DMA_WRITE_IDLE = 0,
    W25Q_DMA_WRITE_STARTING,
    W25Q_DMA_WRITE_WAIT_TX_DONE,
    W25Q_DMA_WRITE_WAIT_FLASH_READY,
    W25Q_DMA_WRITE_DONE,
    W25Q_DMA_WRITE_ERROR
} w25q_dma_write_state_t;

typedef struct
{
    uint32_t current_address;
    uint8_t *current_data;
    uint32_t remain_size;
    uint16_t current_write_size;
    uint32_t state_tick;
    w25q_dma_write_state_t state;
} w25q_dma_write_context_t;

static w25q_dma_write_context_t w25q_dma_write_ctx = {0U, NULL, 0U, 0U, 0U, W25Q_DMA_WRITE_IDLE};

// 可能不太有用
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

static HAL_StatusTypeDef w25q_dma_transmit(uint8_t *pData, uint16_t size)
{
    return HAL_SPI_Transmit_DMA(&hspi2, pData, size);
}

static HAL_StatusTypeDef w25q_dma_receive(uint8_t *pData, uint16_t size)
{
    return HAL_SPI_Receive_DMA(&hspi2, pData, size);
}

static bool w25q_is_transfer_range_valid(uint32_t address, uint32_t size)
{
    if ((address >= W25Q_ADDRESS_SPACE) || (size == 0U)) return false;
    if (size > (W25Q_ADDRESS_SPACE - address)) return false;
    return true;
}

static bool w25q_try_read_status_reg1(uint8_t *status, uint32_t timeout_ms)
{
    uint8_t cmd = W25Q_ReadStatusReg1;

    W25Q_CS_LOW();

    if (HAL_SPI_Transmit(&hspi2, &cmd, 1, timeout_ms) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }

    if (HAL_SPI_Receive(&hspi2, status, 1, timeout_ms) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }

    W25Q_CS_HIGH();
    return true;
}

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

void w25q_read_status_reg1(uint8_t *status)
{
    (void)w25q_try_read_status_reg1(status, W25Q_TIMEOUT);
}

void w25q_read_status_reg2(uint8_t *status)
{
    W25Q_CS_LOW();
    uint8_t cmd = W25Q_ReadStatusReg2;
    w25q_spi_transmit(&cmd, 1);
    w25q_spi_receive(status, 1);
    W25Q_CS_HIGH();
}

static bool w25q_is_dma_write_active(void)
{
    return (w25q_dma_write_ctx.state == W25Q_DMA_WRITE_STARTING) ||
           (w25q_dma_write_ctx.state == W25Q_DMA_WRITE_WAIT_TX_DONE) ||
           (w25q_dma_write_ctx.state == W25Q_DMA_WRITE_WAIT_FLASH_READY);
}

static void w25q_set_dma_write_error(void)
{
    (void)HAL_SPI_DMAStop(&hspi2);
    W25Q_CS_HIGH();
    w25q_tx_dma_busy = false;
    w25q_rx_dma_busy = false;
    w25q_dma_write_ctx.remain_size = 0U;
    w25q_dma_write_ctx.current_write_size = 0U;
    w25q_dma_write_ctx.state_tick = HAL_GetTick();
    w25q_dma_write_ctx.state = W25Q_DMA_WRITE_ERROR;
}

static bool w25q_is_flash_busy(void)
{
    uint8_t status = 0U;
    if (!w25q_try_read_status_reg1(&status, W25Q_TIMEOUT)) return true;
    return (status & 0x01U) != 0U;
}

static bool w25q_start_page_program_dma(uint32_t address, uint8_t *data, uint16_t size)
{
    uint16_t current_page_remain;
    uint8_t cmd[4];

    if ((data == NULL) || (size == 0U)) return false;
    if (!w25q_is_transfer_range_valid(address, size)) return false;
    if ((w25q_tx_dma_busy) || (w25q_rx_dma_busy)) return false;

    current_page_remain = (uint16_t)(W25Q_PAGE_SIZE - (address % W25Q_PAGE_SIZE));
    if (size > current_page_remain) return false;

    w25q_write_enable();
    cmd[0] = W25Q_PageProgram;
    cmd[1] = (address >> 16) & 0xFF;
    cmd[2] = (address >> 8) & 0xFF;
    cmd[3] = address & 0xFF;
    W25Q_CS_LOW();

    if (HAL_SPI_Transmit(&hspi2, cmd, 4, W25Q_TIMEOUT) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }

    w25q_tx_dma_busy = true;

    if (w25q_dma_transmit(data, size) != HAL_OK)
    {
        w25q_tx_dma_busy = false;
        W25Q_CS_HIGH();
        return false;
    }

    return true;
}

static bool w25q_start_next_write_chunk(void)
{
    uint16_t current_page_remain;
    uint16_t write_size;

    if (w25q_dma_write_ctx.remain_size == 0U)
    {
        w25q_dma_write_ctx.current_write_size = 0U;
        w25q_dma_write_ctx.state_tick = HAL_GetTick();
        w25q_dma_write_ctx.state = W25Q_DMA_WRITE_DONE;
        return true;
    }

    current_page_remain = (uint16_t)(W25Q_PAGE_SIZE - (w25q_dma_write_ctx.current_address % W25Q_PAGE_SIZE));
    write_size = (w25q_dma_write_ctx.remain_size > current_page_remain)
               ? current_page_remain
               : (uint16_t)w25q_dma_write_ctx.remain_size;

    w25q_dma_write_ctx.current_write_size = write_size;
    w25q_dma_write_ctx.state_tick = HAL_GetTick();
    w25q_dma_write_ctx.state = W25Q_DMA_WRITE_STARTING;

    if (!w25q_start_page_program_dma(w25q_dma_write_ctx.current_address, w25q_dma_write_ctx.current_data, write_size))
    {
        w25q_set_dma_write_error();
        return false;
    }

    if (w25q_dma_write_ctx.state == W25Q_DMA_WRITE_ERROR) return false;

    w25q_dma_write_ctx.state_tick = HAL_GetTick();
    w25q_dma_write_ctx.state = W25Q_DMA_WRITE_WAIT_TX_DONE;
    return true;
}

// that is bad option. BLOCKING I don't want to use it.
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

#pragma region base functions

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

#pragma endregion

#pragma region dma functions (experimental, not fully tested)

bool w25q_page_program_dma(uint32_t address, uint8_t *data, uint32_t size)
{
    if (size > UINT16_MAX) return false;
    if (w25q_is_dma_write_active()) return false;

    return w25q_start_page_program_dma(address, data, (uint16_t)size);
}

bool w25q_write_data_dma(uint32_t address, uint8_t *data, uint32_t size)
{
    if ((data == NULL) || !w25q_is_transfer_range_valid(address, size)) return false;
    if (w25q_is_dma_write_active()) return false;
    if ((w25q_tx_dma_busy) || (w25q_rx_dma_busy)) return false;
    if (w25q_is_flash_busy()) return false;

    w25q_dma_write_ctx.current_address = address;
    w25q_dma_write_ctx.current_data = data;
    w25q_dma_write_ctx.remain_size = size;
    w25q_dma_write_ctx.current_write_size = 0U;
    w25q_dma_write_ctx.state_tick = HAL_GetTick();
    w25q_dma_write_ctx.state = W25Q_DMA_WRITE_IDLE;

    return w25q_start_next_write_chunk();
}

bool w25q_read_data_dma(uint32_t address, uint8_t *data, uint32_t size)
{
    uint8_t cmd[4];

    if ((data == NULL) || (size == 0U)) return false;
    if (!w25q_is_transfer_range_valid(address, size)) return false;
    if (w25q_is_dma_write_active()) return false;
    if ((w25q_rx_dma_busy) || (w25q_tx_dma_busy)) return false;
    if (size > UINT16_MAX) return false;

    cmd[0] = W25Q_ReadData;
    cmd[1] = (address >> 16) & 0xFF;
    cmd[2] = (address >> 8) & 0xFF;
    cmd[3] = address & 0xFF;

    W25Q_CS_LOW();

    if (HAL_SPI_Transmit(&hspi2, cmd, 4, W25Q_TIMEOUT) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }

    w25q_rx_dma_busy = true;

    if (w25q_dma_receive(data, (uint16_t)size) != HAL_OK)
    {
        w25q_rx_dma_busy = false;
        W25Q_CS_HIGH();
        return false;
    }

    return true;
}

bool w25q_fast_read_data_dma(uint32_t address, uint8_t *data, uint32_t size)
{
    uint8_t cmd[5];

    if ((data == NULL) || (size == 0U)) return false;
    if (!w25q_is_transfer_range_valid(address, size)) return false;
    if (w25q_is_dma_write_active()) return false;
    if ((w25q_rx_dma_busy) || (w25q_tx_dma_busy)) return false;
    if (size > UINT16_MAX) return false;

    cmd[0] = W25Q_FastReadData;
    cmd[1] = (address >> 16) & 0xFF;
    cmd[2] = (address >> 8) & 0xFF;
    cmd[3] = address & 0xFF;
    cmd[4] = W25Q_DUMMY_BYTE;

    W25Q_CS_LOW();

    if (HAL_SPI_Transmit(&hspi2, cmd, 5, W25Q_TIMEOUT) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }

    w25q_rx_dma_busy = true;

    if (w25q_dma_receive(data, (uint16_t)size) != HAL_OK)
    {
        w25q_rx_dma_busy = false;
        W25Q_CS_HIGH();
        return false;
    }

    return true;
}

/**
 * @brief 请在主循环中定期调用此函数，以处理DMA写入的状态更新和错误处理。
 */
void w25q_write_data_dma_task(void)
{
    if (w25q_dma_write_ctx.state == W25Q_DMA_WRITE_STARTING)
    {
        if ((HAL_GetTick() - w25q_dma_write_ctx.state_tick) >= W25Q_TIMEOUT)
        {
            w25q_set_dma_write_error();
        }
        return;
    }

    if (w25q_dma_write_ctx.state == W25Q_DMA_WRITE_WAIT_TX_DONE)
    {
        if (!w25q_tx_dma_busy)
        {
            w25q_dma_write_ctx.state_tick = HAL_GetTick();
            w25q_dma_write_ctx.state = W25Q_DMA_WRITE_WAIT_FLASH_READY;
            return;
        }

        if ((HAL_GetTick() - w25q_dma_write_ctx.state_tick) >= W25Q_TIMEOUT)
        {
            w25q_set_dma_write_error();
        }
        return;
    }

    if (w25q_dma_write_ctx.state == W25Q_DMA_WRITE_WAIT_FLASH_READY)
    {
        uint8_t status = 0U;

        if (!w25q_try_read_status_reg1(&status, W25Q_TASK_SPI_TIMEOUT))
        {
            if ((HAL_GetTick() - w25q_dma_write_ctx.state_tick) >= W25Q_TIMEOUT)
            {
                w25q_set_dma_write_error();
            }
            return;
        }

        if ((status & 0x01U) != 0U)
        {
            if ((HAL_GetTick() - w25q_dma_write_ctx.state_tick) >= W25Q_TIMEOUT)
            {
                w25q_set_dma_write_error();
            }
            return;
        }

        w25q_dma_write_ctx.current_address += w25q_dma_write_ctx.current_write_size;
        w25q_dma_write_ctx.current_data += w25q_dma_write_ctx.current_write_size;
        w25q_dma_write_ctx.remain_size -= w25q_dma_write_ctx.current_write_size;
        w25q_dma_write_ctx.current_write_size = 0U;

        (void)w25q_start_next_write_chunk();
    }
}

bool w25q_write_data_dma_is_busy(void)
{
    return w25q_is_dma_write_active();
}

bool w25q_write_data_dma_is_done(void)
{
    return w25q_dma_write_ctx.state == W25Q_DMA_WRITE_DONE;
}

bool w25q_write_data_dma_is_error(void)
{
    return w25q_dma_write_ctx.state == W25Q_DMA_WRITE_ERROR;
}

void w25q_on_spi_error_callback(void)
{
    if ((w25q_dma_write_ctx.state != W25Q_DMA_WRITE_IDLE) &&
        (w25q_dma_write_ctx.state != W25Q_DMA_WRITE_DONE) &&
        (w25q_dma_write_ctx.state != W25Q_DMA_WRITE_ERROR))
    {
        w25q_set_dma_write_error();
    }
}

#pragma endregion

/*
 * w25q_controller.c
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#include "w25q_controller.h"
#include <string.h>

volatile bool w25q_rx_dma_busy = false;
volatile bool w25q_tx_dma_busy = false;

#define W25Q_PAGE_SIZE      256U
#define W25Q_ADDRESS_SPACE  0x01000000UL
#define W25Q_TASK_SPI_TIMEOUT 5U

typedef enum
{
    W25Q_DMA_IDLE = 0,
    W25Q_DMA_WRITE_PENDING_START,        // [挂起] 收到写请求且 Flash 正忙，自动排队等待后续调用
    W25Q_DMA_READ_PENDING_START,         // [挂起] 收到普通读请求，排队等待总线或 Flash 空闲
    W25Q_DMA_FAST_READ_PENDING_START,    // [挂起] 收到快速读请求，排队等待空闲
    W25Q_DMA_WRITE_STARTING,             // [启动] 正在发送写使能和页编程命令
    W25Q_DMA_WAIT_TX_DONE,               // [传输] 等待 SPI DMA 写数据搬运到 Flash 完成
    W25Q_DMA_WAIT_RX_DONE,               // [传输] 等待 SPI DMA 读数据搬运到内存完成
    W25Q_DMA_WAIT_FLASH_READY,           // [烧录] 等待 Flash 内部将单页数据固化入介质
    W25Q_DMA_DONE,                       // [完成] 本次请求的所有页面写入或读取完成
    W25Q_DMA_ERROR                       // [错误] 发生通信或重试超时错误
} w25q_dma_state_t;

typedef struct
{
    uint32_t current_address;
    uint8_t *current_data;
    uint32_t remain_size;
    uint16_t current_write_size;
    uint32_t state_tick;
    w25q_dma_state_t state;
} w25q_dma_context_t;

static w25q_dma_context_t w25q_dma_ctx = {0U, NULL, 0U, 0U, 0U, W25Q_DMA_IDLE};

/* 清空 RX FIFO + 清除 OVR 错误标志 */
static inline void w25q_flush_rx_fifo(void)
{
    while (SPI2->SR & SPI_FLAG_RXNE) { (void)SPI2->DR; }
    (void)SPI2->SR;
    (void)SPI2->DR;
}

/* ---- SPI 同步/DMA 统一封装：统一先 flush RX FIFO，避免 OVR/脏数据 ---- */

static HAL_StatusTypeDef w25q_spi_transmit(uint8_t *pData, uint16_t size)
{
    if (pData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
    return HAL_SPI_Transmit(&hspi2, pData, size, W25Q_TIMEOUT);
}

/* 纯接收（当前读路径多用全双工 TransmitReceive，此接口保留备用） */
__attribute__((unused))
static HAL_StatusTypeDef w25q_spi_receive(uint8_t *pData, uint16_t size)
{
    if (pData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
    return HAL_SPI_Receive(&hspi2, pData, size, W25Q_TIMEOUT);
}

static HAL_StatusTypeDef w25q_spi_transmit_receive(uint8_t *pTxData, uint8_t *pRxData, uint16_t size)
{
    if (pTxData == NULL || pRxData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
    return HAL_SPI_TransmitReceive(&hspi2, pTxData, pRxData, size, W25Q_TIMEOUT);
}

/** 可指定超时的全双工传输（状态轮询等场景） */
static HAL_StatusTypeDef w25q_spi_transmit_receive_to(uint8_t *pTxData, uint8_t *pRxData,
                                                      uint16_t size, uint32_t timeout_ms)
{
    if (pTxData == NULL || pRxData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
    return HAL_SPI_TransmitReceive(&hspi2, pTxData, pRxData, size, timeout_ms);
}

static HAL_StatusTypeDef w25q_dma_transmit(uint8_t *pData, uint16_t size)
{
    if (pData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
    return HAL_SPI_Transmit_DMA(&hspi2, pData, size);
}

/* 全双工 DMA（备用；当前写路径用 TX DMA + 同步命令） */
__attribute__((unused))
static HAL_StatusTypeDef w25q_dma_transmit_receive(uint8_t *pTxData, uint8_t *pRxData, uint16_t size)
{
    if (pTxData == NULL || pRxData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
    return HAL_SPI_TransmitReceive_DMA(&hspi2, pTxData, pRxData, size);
}

static HAL_StatusTypeDef w25q_dma_receive(uint8_t *pData, uint16_t size)
{
    if (pData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
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
    uint8_t cmd[2] = {W25Q_ReadStatusReg1, 0xFF};
    if (status == NULL)
    {
        return false;
    }
    W25Q_CS_LOW();
    if (w25q_spi_transmit_receive_to(cmd, cmd, 2, timeout_ms) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }
    W25Q_CS_HIGH();
    *status = cmd[1]; /* cmd[0] 命令回显, cmd[1] 实际状态 */
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

void w25q_read_status_reg1(uint8_t *status) // 直接调用带超时的函数，避免 SPI 异常时长时阻塞
{
    (void)w25q_try_read_status_reg1(status, W25Q_TIMEOUT);
}

void w25q_read_status_reg2(uint8_t *status)
{
    uint8_t cmd[2] = {W25Q_ReadStatusReg2, 0xFF};
    if (status == NULL)
    {
        return;
    }
    W25Q_CS_LOW();
    (void)w25q_spi_transmit_receive(cmd, cmd, 2);
    W25Q_CS_HIGH();
    *status = cmd[1]; /* cmd[0] 是命令回显, cmd[1] 是状态寄存器值 */
}

static bool w25q_is_flash_busy(void)
{
    uint8_t status = 0U;
    if (!w25q_try_read_status_reg1(&status, W25Q_TIMEOUT)) return true;
    return (status & 0x01U) != 0U;
}

// that is bad option. BLOCKING the mcu waiting for flash ready.
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

// 读取设备 ID
uint32_t w25q_read_id(void)
{
    uint8_t cmd[4] = {W25Q_JedecDeviceID, 0xFF, 0xFF, 0xFF};
    uint8_t resp[4] = {0};
    W25Q_CS_LOW();
    if (w25q_spi_transmit_receive(cmd, resp, 4) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return 0U;
    }
    W25Q_CS_HIGH();
    /* resp[0] 是命令回显(0x9F)，[1..3] 是 3 字节 Device ID */
    return ((uint32_t)resp[1] << 16) | ((uint32_t)resp[2] << 8) | (uint32_t)resp[3];
}

// FIXME: 如果有人用的不是 w25q128
bool w25q_init(void)
{
    uint32_t id = w25q_read_id();
    if (id != 0xC84018) return false;
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
    /* 轮询等待：等 SPI 移完最后一字节再拉 CS，否则 W25Q 锁存不完整 */
    {
        volatile uint32_t cnt = 50000;
        while (__HAL_SPI_GET_FLAG(&hspi2, SPI_FLAG_BSY) != RESET && --cnt) {}
    }
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

/* 4 字节命令 + 最多 2048 字节数据 = 2052，FAST_READ_BUF_SIZE(2053) 够用 */
/* 全双工读共用缓冲 */
#define FAST_READ_BUF_SIZE (5U + W25Q_PAGE_SIZE * 8U)
static uint8_t fast_read_buf[FAST_READ_BUF_SIZE];

void w25q_read_data(uint32_t address, uint8_t *data, uint32_t size)
{
    uint16_t total = (uint16_t)(4U + size);
    if (total > FAST_READ_BUF_SIZE) return;

    fast_read_buf[0] = W25Q_ReadData;
    fast_read_buf[1] = (uint8_t)(address >> 16);
    fast_read_buf[2] = (uint8_t)(address >> 8);
    fast_read_buf[3] = (uint8_t)(address);
    memset(&fast_read_buf[4], 0xFF, size);

    W25Q_CS_LOW();
    if (w25q_spi_transmit_receive(fast_read_buf, fast_read_buf, total) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return;
    }
    W25Q_CS_HIGH();

    memcpy(data, &fast_read_buf[4], size);
}

/* 全双工 FastRead 共用缓冲：5 字节命令 + 最多 8 页(2048 字节)数据 */

void w25q_fast_read_data(uint32_t address, uint8_t *data, uint32_t size)
{
    uint16_t total = (uint16_t)(5U + size);
    if (total > FAST_READ_BUF_SIZE) return;

    fast_read_buf[0] = W25Q_FastReadData;
    fast_read_buf[1] = (uint8_t)(address >> 16);
    fast_read_buf[2] = (uint8_t)(address >> 8);
    fast_read_buf[3] = (uint8_t)(address);
    fast_read_buf[4] = W25Q_DUMMY_BYTE;
    memset(&fast_read_buf[5], 0xFF, size);

    W25Q_CS_LOW();
    if (w25q_spi_transmit_receive(fast_read_buf, fast_read_buf, total) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return;
    }
    W25Q_CS_HIGH();

    memcpy(data, &fast_read_buf[5], size);
}

#pragma endregion

#pragma region dma functions

static bool w25q_is_dma_active(void)
{
    return (w25q_dma_ctx.state != W25Q_DMA_IDLE) &&
           (w25q_dma_ctx.state != W25Q_DMA_DONE) &&
           (w25q_dma_ctx.state != W25Q_DMA_ERROR);
}

// 在 SPI DMA 传输过程中发生错误时调用，执行必要的状态复位和错误记录
static void w25q_set_dma_error(void)
{
    (void)HAL_SPI_DMAStop(&hspi2);
    W25Q_CS_HIGH();
    w25q_tx_dma_busy = false;
    w25q_rx_dma_busy = false;
    w25q_dma_ctx.remain_size = 0U;
    w25q_dma_ctx.current_write_size = 0U;
    w25q_dma_ctx.state_tick = HAL_GetTick();
    w25q_dma_ctx.state = W25Q_DMA_ERROR;
}

static bool w25q_start_dma_read_internal(uint8_t cmd_type)
{
    uint8_t cmd[5];
    uint8_t cmd_len;

    if (cmd_type == W25Q_ReadData)
    {
        cmd[0] = W25Q_ReadData;
        cmd[1] = (uint8_t)(w25q_dma_ctx.current_address >> 16);
        cmd[2] = (uint8_t)(w25q_dma_ctx.current_address >> 8);
        cmd[3] = (uint8_t)(w25q_dma_ctx.current_address);
        cmd_len = 4;
    }
    else
    {
        cmd[0] = W25Q_FastReadData;
        cmd[1] = (uint8_t)(w25q_dma_ctx.current_address >> 16);
        cmd[2] = (uint8_t)(w25q_dma_ctx.current_address >> 8);
        cmd[3] = (uint8_t)(w25q_dma_ctx.current_address);
        cmd[4] = W25Q_DUMMY_BYTE;
        cmd_len = 5;
    }

    W25Q_CS_LOW();

    if (w25q_spi_transmit(cmd, cmd_len) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }

    w25q_rx_dma_busy = true;

    if (w25q_dma_receive(w25q_dma_ctx.current_data, (uint16_t)w25q_dma_ctx.remain_size) != HAL_OK)
    {
        w25q_rx_dma_busy = false;
        W25Q_CS_HIGH();
        return false;
    }

    return true;
}

bool w25q_read_data_dma(uint32_t address, uint8_t *data, uint32_t size)
{
    if ((data == NULL) || (size == 0U)) return false;
    if (!w25q_is_transfer_range_valid(address, size)) return false;
    if (w25q_is_dma_active()) return false;
    if (size > UINT16_MAX) return false;

    w25q_dma_ctx.current_address = address;
    w25q_dma_ctx.current_data = data;
    w25q_dma_ctx.remain_size = size;
    w25q_dma_ctx.state_tick = HAL_GetTick();

    if (w25q_rx_dma_busy || w25q_tx_dma_busy || w25q_is_flash_busy())
    {
        w25q_dma_ctx.state = W25Q_DMA_READ_PENDING_START;
        return true;
    }

    w25q_dma_ctx.state = W25Q_DMA_WAIT_RX_DONE;
    if (!w25q_start_dma_read_internal(W25Q_ReadData))
    {
        w25q_set_dma_error();
        return false;
    }

    return true;
}

bool w25q_fast_read_data_dma(uint32_t address, uint8_t *data, uint32_t size)
{
    if ((data == NULL) || (size == 0U)) return false;
    if (!w25q_is_transfer_range_valid(address, size)) return false;
    if (w25q_is_dma_active()) return false;
    if (size > UINT16_MAX) return false;

    w25q_dma_ctx.current_address = address;
    w25q_dma_ctx.current_data = data;
    w25q_dma_ctx.remain_size = size;
    w25q_dma_ctx.state_tick = HAL_GetTick();

    if (w25q_rx_dma_busy || w25q_tx_dma_busy || w25q_is_flash_busy())
    {
        w25q_dma_ctx.state = W25Q_DMA_FAST_READ_PENDING_START;
        return true;
    }

    w25q_dma_ctx.state = W25Q_DMA_WAIT_RX_DONE;
    if (!w25q_start_dma_read_internal(W25Q_FastReadData))
    {
        w25q_set_dma_error();
        return false;
    }

    return true;
}

static bool w25q_start_page_program_dma(uint32_t address, uint8_t *data, uint16_t size)
{
    uint16_t current_page_remain;
    uint8_t cmd[4];

    if ((data == NULL) || (size == 0U)) return false;
    if (!w25q_is_transfer_range_valid(address, size)) return false;
    if (w25q_tx_dma_busy || w25q_rx_dma_busy) return false;

    current_page_remain = (uint16_t)(W25Q_PAGE_SIZE - (address % W25Q_PAGE_SIZE));
    if (size > current_page_remain) return false;

    w25q_write_enable();
    cmd[0] = W25Q_PageProgram;
    cmd[1] = (uint8_t)(address >> 16);
    cmd[2] = (uint8_t)(address >> 8);
    cmd[3] = (uint8_t)(address);
    
    W25Q_CS_LOW();

    if (w25q_spi_transmit(cmd, 4) != HAL_OK)
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

    if (w25q_dma_ctx.remain_size == 0U)
    {
        w25q_dma_ctx.current_write_size = 0U;
        w25q_dma_ctx.state_tick = HAL_GetTick();
        w25q_dma_ctx.state = W25Q_DMA_DONE;
        return true;
    }

    current_page_remain = (uint16_t)(W25Q_PAGE_SIZE - (w25q_dma_ctx.current_address % W25Q_PAGE_SIZE));
    write_size = (w25q_dma_ctx.remain_size > current_page_remain)
               ? current_page_remain
               : (uint16_t)w25q_dma_ctx.remain_size;

    w25q_dma_ctx.current_write_size = write_size;
    w25q_dma_ctx.state_tick = HAL_GetTick();
    w25q_dma_ctx.state = W25Q_DMA_WRITE_STARTING;

    if (!w25q_start_page_program_dma(w25q_dma_ctx.current_address, w25q_dma_ctx.current_data, write_size))
    {
        w25q_set_dma_error();
        return false;
    }

    if (w25q_dma_ctx.state == W25Q_DMA_ERROR) return false;

    w25q_dma_ctx.state_tick = HAL_GetTick();
    w25q_dma_ctx.state = W25Q_DMA_WAIT_TX_DONE;
    return true;
}

bool w25q_page_program_dma(uint32_t address, uint8_t *data, uint32_t size)
{
    if (size > W25Q_PAGE_SIZE) return false;
    return w25q_write_data_dma(address, data, size);
}

bool w25q_write_data_dma(uint32_t address, uint8_t *data, uint32_t size)
{
    if ((data == NULL) || !w25q_is_transfer_range_valid(address, size)) return false;
    if (w25q_is_dma_active()) return false;

    w25q_dma_ctx.current_address = address;
    w25q_dma_ctx.current_data = data;
    w25q_dma_ctx.remain_size = size;
    w25q_dma_ctx.current_write_size = 0U;
    w25q_dma_ctx.state_tick = HAL_GetTick();

    if (w25q_tx_dma_busy || w25q_rx_dma_busy || w25q_is_flash_busy())
    {
        w25q_dma_ctx.state = W25Q_DMA_WRITE_PENDING_START;
        return true; 
    }

    w25q_dma_ctx.state = W25Q_DMA_IDLE;
    return w25q_start_next_write_chunk();
}

void w25q_dma_task(void)
{
    switch (w25q_dma_ctx.state)
    {
        case W25Q_DMA_IDLE:
        case W25Q_DMA_DONE:
        case W25Q_DMA_ERROR:
            break;

        case W25Q_DMA_WRITE_PENDING_START:
        case W25Q_DMA_READ_PENDING_START:
        case W25Q_DMA_FAST_READ_PENDING_START:
        {
            if (w25q_tx_dma_busy || w25q_rx_dma_busy)
            {
                break;
            }

            uint8_t status = 0U;
            if (w25q_try_read_status_reg1(&status, W25Q_TASK_SPI_TIMEOUT))
            {
                if ((status & 0x01U) == 0U)
                {
                    if (w25q_dma_ctx.state == W25Q_DMA_WRITE_PENDING_START)
                    {
                        (void)w25q_start_next_write_chunk();
                    }
                    else if (w25q_dma_ctx.state == W25Q_DMA_READ_PENDING_START)
                    {
                        w25q_dma_ctx.state = W25Q_DMA_WAIT_RX_DONE;
                        if (!w25q_start_dma_read_internal(W25Q_ReadData)) w25q_set_dma_error();
                    }
                    else if (w25q_dma_ctx.state == W25Q_DMA_FAST_READ_PENDING_START)
                    {
                        w25q_dma_ctx.state = W25Q_DMA_WAIT_RX_DONE;
                        if (!w25q_start_dma_read_internal(W25Q_FastReadData)) w25q_set_dma_error();
                    }
                }
            }
            break;
        }

        case W25Q_DMA_WRITE_STARTING:
        {
            if ((HAL_GetTick() - w25q_dma_ctx.state_tick) >= W25Q_TIMEOUT)
            {
                w25q_set_dma_error();
            }
            break;
        }

        case W25Q_DMA_WAIT_TX_DONE:
        {
            if (!w25q_tx_dma_busy)
            {
                w25q_dma_ctx.state_tick = HAL_GetTick();
                w25q_dma_ctx.state = W25Q_DMA_WAIT_FLASH_READY;
                break;
            }

            if ((HAL_GetTick() - w25q_dma_ctx.state_tick) >= W25Q_TIMEOUT)
            {
                w25q_set_dma_error();
            }
            break;
        }

        case W25Q_DMA_WAIT_RX_DONE:
        {
            if (!w25q_rx_dma_busy)
            {
                W25Q_CS_HIGH();
                w25q_dma_ctx.state_tick = HAL_GetTick();
                w25q_dma_ctx.remain_size = 0;
                w25q_dma_ctx.state = W25Q_DMA_DONE;
                break;
            }

            if ((HAL_GetTick() - w25q_dma_ctx.state_tick) >= W25Q_TIMEOUT)
            {
                w25q_set_dma_error();
            }
            break;
        }

        case W25Q_DMA_WAIT_FLASH_READY:
        {
            uint8_t status = 0U;

            if (!w25q_try_read_status_reg1(&status, W25Q_TASK_SPI_TIMEOUT))
            {
                if ((HAL_GetTick() - w25q_dma_ctx.state_tick) >= W25Q_TIMEOUT)
                {
                    w25q_set_dma_error();
                }
                break;
            }

            if ((status & 0x01U) != 0U)
            {
                if ((HAL_GetTick() - w25q_dma_ctx.state_tick) >= W25Q_TIMEOUT)
                {
                    w25q_set_dma_error();
                }
                break;
            }

            w25q_dma_ctx.current_address += w25q_dma_ctx.current_write_size;
            w25q_dma_ctx.current_data += w25q_dma_ctx.current_write_size;
            w25q_dma_ctx.remain_size -= w25q_dma_ctx.current_write_size;
            w25q_dma_ctx.current_write_size = 0U;

            (void)w25q_start_next_write_chunk();
            break;
        }
    }
}

bool w25q_dma_is_busy(void)
{
    return w25q_is_dma_active();
}

bool w25q_dma_is_done(void)
{
    return w25q_dma_ctx.state == W25Q_DMA_DONE;
}

bool w25q_dma_is_error(void)
{
    return w25q_dma_ctx.state == W25Q_DMA_ERROR;
}

void w25q_on_spi_error_callback(void)
{
    if ((w25q_dma_ctx.state != W25Q_DMA_IDLE) &&
        (w25q_dma_ctx.state != W25Q_DMA_DONE) &&
        (w25q_dma_ctx.state != W25Q_DMA_ERROR))
    {
        w25q_set_dma_error();
    }
}

#pragma endregion

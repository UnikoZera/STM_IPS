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

/**
 * @brief W25Q DMA 状态机
 *
 * 状态转移图（写路径）：
 *   IDLE → WRITE_PENDING_START → WRITE_STARTING → WAIT_TX_DONE
 *       → WAIT_FLASH_READY ──→ (还有数据) → WRITE_STARTING (循环)
 *                           └─→ (写完) → DONE → IDLE
 *
 * 状态转移图（读路径）：
 *   IDLE → READ_PENDING_START / FAST_READ_PENDING_START → WAIT_RX_DONE → DONE → IDLE
 *
 * 任何状态遇到超时/错误 → ERROR → 手动复位到 IDLE
 */
typedef enum
{
    W25Q_DMA_IDLE = 0,
    W25Q_DMA_WRITE_PENDING_START,        /* [挂起] 收到写请求且 Flash 正忙，自动排队 */
    W25Q_DMA_READ_PENDING_START,         /* [挂起] 普通读请求排队中 */
    W25Q_DMA_FAST_READ_PENDING_START,    /* [挂起] 快速读请求排队中 */
    W25Q_DMA_WRITE_STARTING,             /* [启动] 发送写使能和页编程命令 */
    W25Q_DMA_WAIT_TX_DONE,               /* [传输] 等待 SPI DMA 写完成 */
    W25Q_DMA_WAIT_RX_DONE,               /* [传输] 等待 SPI DMA 读完成 */
    W25Q_DMA_WAIT_FLASH_READY,           /* [烧录] 等待 Flash 内部固化单页数据 */
    W25Q_DMA_DONE,                       /* [完成] 本次请求所有页面写入/读取完成 */
    W25Q_DMA_ERROR                       /* [错误] 通信或重试超时 */
} w25q_dma_state_t;

/**
 * @brief DMA 操作上下文
 *
 * 记录当前 DMA 请求的地址、数据指针、剩余大小和状态机进度。
 * 写操作按页拆分（每页 256B），逐页通过状态机推进。
 */
typedef struct
{
    uint32_t current_address;      /* 当前操作的 W25Q 地址 */
    uint8_t *current_data;         /* 当前数据缓冲指针 */
    uint32_t remain_size;          /* 剩余待传输字节数 */
    uint16_t current_write_size;   /* 当前页写入大小（≤256B） */
    uint32_t state_tick;           /* 当前状态进入时间戳（超时检测用） */
    w25q_dma_state_t state;        /* 状态机当前状态 */
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

/**
 * @brief 多页数据写入（同步方式）
 *
 * 自动按页边界拆分，逐页调用 w25q_page_program。
 *
 * @param address W25Q 起始地址
 * @param data    待写入数据
 * @param size    数据字节数
 */
void w25q_write_data(uint32_t address, uint8_t *data, uint32_t size)
{
    uint32_t current_address = address;
    uint8_t *current_data = data;
    uint32_t remain_size = size;

    while (remain_size > 0)
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
    if ((data == NULL) || (size == 0U) || (total > FAST_READ_BUF_SIZE)) return;

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

/* 双读校验用第二缓冲：与单次 FastRead 最大数据量一致 */
static uint8_t s_verify_read_buf[W25Q_PAGE_SIZE * 8U];
#define W25Q_READ_VERIFY_RETRY 4U

static bool w25q_fast_read_once_ok(uint32_t address, uint8_t *data, uint32_t size)
{
    uint16_t total = (uint16_t)(5U + size);
    if ((data == NULL) || (size == 0U) || (total > FAST_READ_BUF_SIZE))
    {
        return false;
    }
    if (!w25q_is_transfer_range_valid(address, size))
    {
        return false;
    }

    /* 等内部编程结束，避免读到半写状态 */
    w25q_check_busy();

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
        return false;
    }
    W25Q_CS_HIGH();

    memcpy(data, &fast_read_buf[5], size);
    return true;
}

bool w25q_fast_read_verified(uint32_t address, uint8_t *data, uint32_t size)
{
    if ((data == NULL) || (size == 0U) || (size > sizeof(s_verify_read_buf)))
    {
        return false;
    }

    /* 若有 DMA 事务在跑，先推进/等完，避免与同步 SPI 抢总线 */
    {
        uint32_t t0 = HAL_GetTick();
        while (w25q_dma_is_busy())
        {
            w25q_dma_task();
            if ((uint32_t)(HAL_GetTick() - t0) > 200U)
            {
                return false;
            }
        }
    }

    for (uint32_t retry = 0U; retry < W25Q_READ_VERIFY_RETRY; retry++)
    {
        if (!w25q_fast_read_once_ok(address, data, size))
        {
            continue;
        }
        if (!w25q_fast_read_once_ok(address, s_verify_read_buf, size))
        {
            continue;
        }
        if (memcmp(data, s_verify_read_buf, size) == 0)
        {
            return true;
        }
        /* 两次不一致：可能总线毛刺/时序问题，重试 */
    }
    return false;
}

#pragma endregion

#pragma region DMA 函数

/* ---------------------------------------------------------------------------
 * DMA 状态查询与错误处理
 * --------------------------------------------------------------------------- */

static bool w25q_is_dma_active(void)
{
    return (w25q_dma_ctx.state != W25Q_DMA_IDLE) &&
           (w25q_dma_ctx.state != W25Q_DMA_DONE) &&
           (w25q_dma_ctx.state != W25Q_DMA_ERROR);
}

/**
 * @brief 设置 DMA 错误状态：停止传输、拉高 CS、复位所有繁忙标志
 */
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

/**
 * @brief DMA 状态机轮询任务（主循环中周期调用）
 *
 * 处理三种挂起请求（写/读/快速读）的启动、传输进度跟踪、等待 Flash 固化。
 * 每次调用只推进当前状态，不会阻塞。
 */
void w25q_dma_task(void)
{
    switch (w25q_dma_ctx.state)
    {
        /* ── 空闲 / 完成 / 错误：不处理 ── */
        case W25Q_DMA_IDLE:
        case W25Q_DMA_DONE:
        case W25Q_DMA_ERROR:
            break;

        /* ── 挂起状态：等待 DMA/Flash 空闲后启动 ── */
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

        /* ── 写启动：等待写使能 + 页编程命令发送完成 ── */
        case W25Q_DMA_WRITE_STARTING:
        {
            if ((HAL_GetTick() - w25q_dma_ctx.state_tick) >= W25Q_TIMEOUT)
            {
                w25q_set_dma_error();
            }
            break;
        }

        /* ── 等待 TX DMA 完成 → 转到等待 Flash 内部固化 ── */
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

        /* ── 等待 RX DMA 完成 → 读结束，标记完成 ── */
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

        /* ── 等待 Flash 内部编程完成 → 继续下一块或标记完成 ── */
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

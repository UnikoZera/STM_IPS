/*
 * w25q_controller.c
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#include "w25q_controller.h"
#include "crc16.h"
#include <string.h>

volatile bool w25q_rx_dma_busy = false;
volatile bool w25q_tx_dma_busy = false;

#define W25Q_SPI_BAUD 21000000UL
#define W25Q_BASE_TIMEOUT 5U
#define W25Q_PAGE_TIMEOUT 30U
#define W25Q_PAGE_SIZE 256U
#define W25Q_ADDRESS_SPACE 0x01000000UL

static inline uint32_t get_timeout_ms(uint32_t size)
{
    uint32_t estimated_ms = (size * 8 * 1000U + W25Q_SPI_BAUD - 1U) / W25Q_SPI_BAUD; // 理论传输时间（ms）向上取整
    estimated_ms += W25Q_BASE_TIMEOUT;
    if (estimated_ms < 10U)
    {
        estimated_ms = 10U;
    }
    if (estimated_ms > HAL_MAX_DELAY)
    {
        estimated_ms = HAL_MAX_DELAY;
    }
    return estimated_ms;
}

/* 清空 RX FIFO + 清除 OVR 错误标志 */
static inline void w25q_flush_rx_fifo(void)
{
    while (SPI2->SR & SPI_FLAG_RXNE)
    {
        (void)SPI2->DR;
    }
    (void)SPI2->SR;
    (void)SPI2->DR;
}

static HAL_StatusTypeDef w25q_spi_transmit(uint8_t *pData, uint16_t size)
{
    if (pData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
    uint32_t timeout = get_timeout_ms(size);
    return HAL_SPI_Transmit(&hspi2, pData, size, timeout);
}

__attribute__((unused)) static HAL_StatusTypeDef w25q_spi_receive(uint8_t *pData, uint16_t size)
{
    if (pData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
    uint32_t timeout = get_timeout_ms(size);
    return HAL_SPI_Receive(&hspi2, pData, size, timeout);
}

static HAL_StatusTypeDef w25q_spi_transmit_receive(uint8_t *pTxData, uint8_t *pRxData, uint16_t size)
{
    if (pTxData == NULL || pRxData == NULL || size == 0U)
    {
        return HAL_ERROR;
    }
    w25q_flush_rx_fifo();
    uint32_t timeout = get_timeout_ms(size);
    return HAL_SPI_TransmitReceive(&hspi2, pTxData, pRxData, size, timeout);
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

__attribute__((unused)) static HAL_StatusTypeDef w25q_dma_transmit_receive(uint8_t *pTxData, uint8_t *pRxData, uint16_t size)
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
    if ((address >= W25Q_ADDRESS_SPACE) || (size == 0U))
        return false;
    if (size > (W25Q_ADDRESS_SPACE - address))
        return false;
    return true;
}

static void w25q_write_enable(void)
{
    uint8_t cmd = W25Q_WriteEnable;
    W25Q_CS_LOW();
    w25q_spi_transmit(&cmd, 1);
    W25Q_CS_HIGH();
}

static bool w25q_read_status_reg1(uint8_t *status)
{
    uint8_t cmd[2] = {W25Q_ReadStatusReg1, 0xFF};
    if (status == NULL)
    {
        return false;
    }
    W25Q_CS_LOW();
    if (w25q_spi_transmit_receive(cmd, cmd, 2) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }
    W25Q_CS_HIGH();
    *status = cmd[1]; /* cmd[0] 命令回显, cmd[1] 实际状态 */
    return true;
}

static bool w25q_is_flash_busy(void)
{
    uint8_t status = 0U;
    if (!w25q_read_status_reg1(&status))
        return true;
    return (status & 0x01U) != 0U;
}

// 直接获取当前的w25q的状态 无堵塞 返回true=忙，false=空闲
bool w25q_check_busy(void)
{
    uint8_t status;
    if (!w25q_read_status_reg1(&status))
    {
        return true; // 读取失败，认为忙
    }
    return (status & 0x01U) != 0U; // WIP 位为 1 表示忙
}

// 堵塞 包含page超时
static void w25q_check_busy_page(void)
{
    uint32_t tickstart = HAL_GetTick();
    uint8_t status = 0xFF; /* 初始化：RDSR 失败时视为仍忙（不误判完成），由超时兜底 */
    do
    {
        w25q_read_status_reg1(&status);
        if ((status & 0x01) == 0)
            return;
    } while ((HAL_GetTick() - tickstart) < W25Q_PAGE_TIMEOUT);
}

// 永久性堵塞（擦除等 WIP 清零）：带 ≥5ms RDSR 节流，避免擦除期间空转刷 SPI 拉高 CPU（与 DMA 状态机一致）
static void w25q_check_busy_nontimeout(void)
{
    uint8_t status = 0U; /* 初始化：RDSR 失败时不读未初始化栈垃圾 */
    uint32_t tick = HAL_GetTick();
    do
    {
        if (w25q_read_status_reg1(&status) && ((status & 0x01U) == 0U))
        {
            return; /* Flash 空闲 */
        }
        /* 轮询节流：≥5ms 才做下一次 RDSR；擦除 150ms 内从数百次降到 ~30 次 SPI 事务 */
        while ((HAL_GetTick() - tick) < 5U) {}
        tick = HAL_GetTick();
    } while (1);
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
    if (id != 0xC84018)
        return false;
    return true;
}

#pragma region base functions

void w25q_erase_sector(uint32_t address)
{
    w25q_write_enable();
    W25Q_CS_LOW();
    address &= 0xFFFFF000; // sector address must be aligned to 4KB
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

static void w25q_page_program(uint32_t address, uint8_t *data, uint16_t size)
{
    uint16_t current_page_remain = 256 - (address % 256);
    if (size > current_page_remain)
        return;
    w25q_write_enable();
    W25Q_CS_LOW();
    uint8_t cmd[4];
    cmd[0] = W25Q_PageProgram;
    cmd[1] = (address >> 16) & 0xFF;
    cmd[2] = (address >> 8) & 0xFF;
    cmd[3] = address & 0xFF;
    w25q_spi_transmit(cmd, 4);
    w25q_spi_transmit(data, size);
    while (__HAL_SPI_GET_FLAG(&hspi2, SPI_FLAG_BSY) != RESET)
    {
    }
    W25Q_CS_HIGH();
    w25q_check_busy_page();
}

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

/* 4 字节命令 + 1 字节 dummy + 最多 1024 字节数据 */
/* 全双工读共用缓冲 */
#define READ_BUF_SIZE (5U + 1024U + 1U)
static uint8_t read_buf[READ_BUF_SIZE];

void w25q_read_data(uint32_t address, uint8_t *data, uint16_t size) //! size 最大为1024
{
    uint16_t total = (uint16_t)(4U + size);
    if (total > READ_BUF_SIZE)
        return;

    read_buf[0] = W25Q_ReadData;
    read_buf[1] = (uint8_t)(address >> 16);
    read_buf[2] = (uint8_t)(address >> 8);
    read_buf[3] = (uint8_t)(address);
    memset(&read_buf[4], 0xFF, size);

    W25Q_CS_LOW();
    if (w25q_spi_transmit_receive(read_buf, read_buf, total) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return;
    }
    W25Q_CS_HIGH();

    memcpy(data, &read_buf[4], size);
}

void w25q_fast_read_data(uint32_t address, uint8_t *data, uint16_t size) //! size 最大为1024
{
    uint16_t total = (uint16_t)(5U + size);
    if ((data == NULL) || (size == 0U) || (total > READ_BUF_SIZE))
        return;

    read_buf[0] = W25Q_FastReadData;
    read_buf[1] = (uint8_t)(address >> 16);
    read_buf[2] = (uint8_t)(address >> 8);
    read_buf[3] = (uint8_t)(address);
    read_buf[4] = W25Q_DUMMY_BYTE;
    memset(&read_buf[5], 0xFF, size);

    W25Q_CS_LOW();
    if (w25q_spi_transmit_receive(read_buf, read_buf, total) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return;
    }
    W25Q_CS_HIGH();

    memcpy(data, &read_buf[5], size);
}

/**
 * @brief 读取数据到指定缓冲区，可选择普通读或快速读。
 * @param address W25Q 内存地址
 * @param data 输出缓冲区指针
 * @param size 要读取的字节数（最大 1024）
 * @param use_fast_read true=使用快速读，false=使用普通读
 * @attention 这个函数直接操作 SPI 总线 而且是直接读取数据到指定缓冲区 没有任何协议 可以直接使用 适合读取原始数据
 */
void w25q_read_data_to(uint32_t address, uint8_t *data, uint32_t size, bool use_fast_read)
{
    if ((data == NULL) || (size == 0U))
        return;
    if (use_fast_read)
    {
        read_buf[0] = W25Q_FastReadData;
        read_buf[1] = (uint8_t)(address >> 16);
        read_buf[2] = (uint8_t)(address >> 8);
        read_buf[3] = (uint8_t)(address);
        read_buf[4] = W25Q_DUMMY_BYTE;
        W25Q_CS_LOW();
        w25q_spi_transmit(read_buf, 5);
        if (w25q_spi_receive(data, size) != HAL_OK)
        {
            W25Q_CS_HIGH();
            return;
        }
        W25Q_CS_HIGH();
    }
    else
    {
        read_buf[0] = W25Q_ReadData;
        read_buf[1] = (uint8_t)(address >> 16);
        read_buf[2] = (uint8_t)(address >> 8);
        read_buf[3] = (uint8_t)(address);
        W25Q_CS_LOW();
        w25q_spi_transmit(read_buf, 4);
        if (w25q_spi_receive(data, size) != HAL_OK)
        {
            W25Q_CS_HIGH();
            return;
        }
        W25Q_CS_HIGH();
    }
}

#define W25Q_READ_VERIFY_RETRY 3U

/**
 * @brief           读取 W25Q 的一个物理块并校验 CRC-16。
 * @param total_len 物理块总长度 = 数据字节 + 2B CRC（大文件恒 1024，小文件尾块 = x+2，≤1024）
 * @return          true=读取成功且 CRC 校验通过，false=读取失败或 CRC 校验失败
 * @note            读回数据放在 read_buf[5..5+total_len-1]（前 5 字节是命令和地址），
 *                  其中 [5..5+total_len-3] 是数据、最后 2B 是 CRC-16（小端）
 */
static bool w25q_crc_fetch_block(uint32_t phys_addr, uint16_t total_len)
{
    if (total_len > 1024U || total_len < 2U)
    {
        return false; // 超过单块最大长度或小于 CRC 长度
    }

    for (uint32_t retry = 0U; retry < W25Q_READ_VERIFY_RETRY; retry++)
    {
        read_buf[0] = W25Q_FastReadData;
        read_buf[1] = (uint8_t)(phys_addr >> 16);
        read_buf[2] = (uint8_t)(phys_addr >> 8);
        read_buf[3] = (uint8_t)(phys_addr);
        read_buf[4] = W25Q_DUMMY_BYTE;
        memset(&read_buf[5], 0xFF, total_len);

        W25Q_CS_LOW();
        if (w25q_spi_transmit_receive(read_buf, read_buf,
                                      5U + total_len) != HAL_OK)
        {
            W25Q_CS_HIGH();
            continue;
        }
        W25Q_CS_HIGH();

        if (crc16_usb_packing(&read_buf[5], total_len, true))
        {
            return true;
        }
        /* CRC 不符：总线毛刺或写入不完整，重试 */
    }
    return false;
}

/**
 * @brief           计算第 blk 个数据块的数据长度（不含 CRC）。
 * @param raw_total 文件原始数据总大小（不含 CRC）
 * @param blk       数据块索引（0 起）
 * @param tail_pad  true=大文件（每块数据区恒 1022B，尾块含 0xFF 填充）；
 *                  false=小文件（尾块按实际数据长度，不填充）
 * @return          本块数据长度（大文件恒 1022；小文件尾块 = raw_total - blk*1022，1..1021）
 * @note            物理块长度 = 返回值 + 2B CRC
 */
static inline uint32_t w25q_crc_block_len(uint32_t raw_total, uint32_t blk, bool tail_pad)
{
    uint32_t start = blk * W25Q_CRC_BLOCK_DATA_SIZE;
    if (tail_pad || (start + W25Q_CRC_BLOCK_DATA_SIZE) <= raw_total)
    {
        return W25Q_CRC_BLOCK_DATA_SIZE;
    }
    return raw_total - start;
}

/**
 * @brief           按原始偏移读取文件数据：逐 1022B 数据块读取物理块并校验 CRC-16。
 * @param phys_base 文件物理起点地址（原始偏移 0 对应的 W25Q 地址）
 * @param raw_off   文件内原始偏移（0 起，不含 CRC 字节）
 * @param data      输出缓冲
 * @param size      要读取的原始数据字节数（可跨多个块）
 * @param data_total 文件原始数据总大小（不含 CRC；仅 tail_pad=false 时用于定位尾块边界）
 * @param tail_pad  true =大文件：每块数据区恒 1022B，尾块以 0xFF 补齐（物理块恒 1024B）；
 *                  false=小文件：尾块不补齐，物理块 = 实际数据 x + 2B CRC
 * @return true=全部块读取成功且 CRC 校验通过；false=参数非法或多次重试仍失败
 *
 * 物理块布局（每块）：
 *   大文件 tail_pad=true : [1022B 数据 | 2B CRC-16 LE]（尾块不足 1022B 时以 0xFF 填满）
 *   小文件 tail_pad=false: [x 数据    | 2B CRC-16 LE]（尾块 x<1022，不填充）
 * 原始偏移 r → 物理偏移 = (r/1022)*1024 + (r%1022)
 * 每块先 w25q_crc_fetch_block 读整个物理块（数据+CRC）并校验，
 * 再从 read_buf[5 + 块内偏移] 拷贝本块内的数据部分。
 */
bool w25q_crc_read(uint32_t phys_base, uint32_t raw_off, uint8_t *data, uint32_t size,
                   uint32_t data_total, bool tail_pad)
{
    uint32_t done = 0;

    if ((data == NULL) || (size == 0U))
    {
        return false;
    }
    /* 小文件（tail_pad=false）不允许越过文件原始末尾：避免尾块边界下溢/死循环 */
    if (!tail_pad && (data_total == 0U || (raw_off + size) > data_total))
    {
        return false;
    }

    while (done < size)
    {
        uint32_t off = raw_off + done;                                 /* 当前文件内原始偏移 */
        uint32_t blk = off / W25Q_CRC_BLOCK_DATA_SIZE;                 /* 数据块号（1022B/块） */
        uint32_t in = off % W25Q_CRC_BLOCK_DATA_SIZE;                  /* 块内数据偏移 */
        uint32_t dlen = w25q_crc_block_len(data_total, blk, tail_pad); /* 本块数据长度（不含 CRC） */
        uint32_t copy = size - done;
        if (copy > (dlen - in))
        {
            copy = dlen - in; /* 不超过本块剩余数据 */
        }

        if (!w25q_crc_fetch_block(phys_base + blk * W25Q_CRC_BLOCK_PHYS_SIZE,
                                  (uint16_t)(dlen + 2U)))
        {
            return false;
        }
        memcpy(data + done, &read_buf[5U + in], copy);
        done += copy;
    }
    return true;
}

#pragma endregion

#pragma region DMA 函数

/**
 * @brief W25Q DMA 状态机
 *
 * 状态转移图（写路径）：
 *   IDLE → WRITE_STARTING → WAIT_TX_DONE
 *       → WAIT_FLASH_READY ──→ (还有数据) → WRITE_STARTING (循环)
 *                           └─→ (写完) → DONE → IDLE
 *
 * 状态转移图（读路径）：
 *   IDLE → WAIT_RX_DONE → DONE → IDLE
 *
 * 状态转移图（擦除路径，逐扇区）:
 *   IDLE → ERASE_WAIT ──→ (还有扇区) → 发下一扇区命令 → ERASE_WAIT (循环)
 *                      └─→ (全部完成) → DONE → IDLE
 *
 * 状态机忙时（*_dma 提交接口）直接返回 false 拒绝新请求；
 * 无内部超时：等待 WIP/DMA 完成由状态机一直轮询（擦除/固化多久等多久），
 * SPI 出错 → ERROR。
 */
typedef enum
{
    W25Q_DMA_IDLE = 0,
    W25Q_DMA_WRITE_STARTING,          /* [启动] 发送写使能和页编程命令 */
    W25Q_DMA_WAIT_TX_DONE,            /* [传输] 等待 SPI DMA 写完成 */
    W25Q_DMA_WAIT_RX_DONE,            /* [传输] 等待 SPI DMA 读完成 */
    W25Q_DMA_WAIT_FLASH_READY,        /* [烧录] 等待 Flash 内部固化单页数据 */
    W25Q_DMA_ERASE_WAIT,              /* [擦除] 等待 Flash 内部擦除完成清零 */
    W25Q_DMA_DONE,                    /* [完成] 本次请求所有页面写入/读取完成 */
    W25Q_DMA_ERROR                    /* [错误] 通信错误或操作失败 */
} w25q_dma_state_t;

/**
 * @brief DMA 操作上下文
 *
 * 记录当前 DMA 请求的地址、数据指针、剩余大小和状态机进度。
 * 写操作按页拆分（每页 256B），逐页通过状态机推进。
 */
typedef struct
{
    uint32_t current_address;    /* 当前操作的 W25Q 地址 */
    uint8_t *current_data;       /* 当前数据缓冲指针 */
    uint32_t remain_size;        /* 剩余待传输字节数 */
    uint16_t current_write_size; /* 当前页写入大小（≤256B） */
    uint32_t erase_address;      /* 批量擦除：当前正在擦除的扇区地址 */
    uint32_t erase_remain;       /* 批量擦除：已发出命令后剩余待擦扇区数 */
    uint32_t erase_poll_tick;    /* 擦除轮询节流时间戳（≥1ms 一次 RDSR） */
    uint32_t write_poll_tick;    /* 页编程固化轮询节流时间戳（≥1ms 一次 RDSR） */
    w25q_dma_state_t state;      /* 状态机当前状态 */
} w25q_dma_context_t;

static w25q_dma_context_t w25q_dma_ctx = {0U, NULL, 0U, 0U, 0U, 0U, 0U, 0U, W25Q_DMA_IDLE};

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
    w25q_dma_ctx.state = W25Q_DMA_ERROR;
}

/**
 * @brief 非阻塞等待 Flash 空闲（WIP=0）：单次 RDSR，忙/失败则返回 false 待下次轮询。
 * @return true=Flash 已空闲；false=仍忙 / RDSR 失败
 */
static bool w25q_wait_flash_idle(void)
{
    uint8_t status = 0U;

    if (!w25q_read_status_reg1(&status))
    {
        return false; /* RDSR 失败，下次轮询再试 */
    }
    if ((status & 0x01U) != 0U)
    {
        return false; /* 仍忙 */
    }
    return true;
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

bool w25q_read_data_dma(uint32_t address, uint8_t *data, uint32_t size, bool use_fast_read)
{
    if ((data == NULL) || (size == 0U))
        return false;
    if (!w25q_is_transfer_range_valid(address, size))
        return false;
    if (w25q_is_dma_active())
        return false;
    if (size > UINT16_MAX)
        return false;

    w25q_dma_ctx.current_address = address;
    w25q_dma_ctx.current_data = data;
    w25q_dma_ctx.remain_size = size;

    if (w25q_rx_dma_busy || w25q_tx_dma_busy || w25q_is_flash_busy())
    {
        return false; /* DMA 忙或 Flash 忙：拒绝，调用方稍后重试 */
    }

    w25q_dma_ctx.state = W25Q_DMA_WAIT_RX_DONE;
    if (use_fast_read)
    {
        if (!w25q_start_dma_read_internal(W25Q_FastReadData))
        {
            w25q_set_dma_error();
            return false;
        }
    }
    else
    {
        if (!w25q_start_dma_read_internal(W25Q_ReadData))
        {
            w25q_set_dma_error();
            return false;
        }
    }
    return true;
}

static bool w25q_start_page_program_dma(uint32_t address, uint8_t *data, uint16_t size)
{
    uint16_t current_page_remain;
    uint8_t cmd[4];

    if ((data == NULL) || (size == 0U))
        return false;
    if (!w25q_is_transfer_range_valid(address, size))
        return false;
    if (w25q_tx_dma_busy || w25q_rx_dma_busy)
        return false;

    current_page_remain = (uint16_t)(W25Q_PAGE_SIZE - (address % W25Q_PAGE_SIZE));
    if (size > current_page_remain)
        return false;

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
        w25q_dma_ctx.state = W25Q_DMA_DONE;
        return true;
    }

    current_page_remain = (uint16_t)(W25Q_PAGE_SIZE - (w25q_dma_ctx.current_address % W25Q_PAGE_SIZE));
    write_size = (w25q_dma_ctx.remain_size > current_page_remain)
                     ? current_page_remain
                     : (uint16_t)w25q_dma_ctx.remain_size;

    w25q_dma_ctx.current_write_size = write_size;
    w25q_dma_ctx.state = W25Q_DMA_WRITE_STARTING;

    if (!w25q_start_page_program_dma(w25q_dma_ctx.current_address, w25q_dma_ctx.current_data, write_size))
    {
        w25q_set_dma_error();
        return false;
    }

    w25q_dma_ctx.state = W25Q_DMA_WAIT_TX_DONE;
    return true;
}

bool w25q_write_data_dma(uint32_t address, uint8_t *data, uint32_t size)
{
    if ((data == NULL) || !w25q_is_transfer_range_valid(address, size))
        return false;
    if (w25q_is_dma_active())
        return false;

    if (w25q_tx_dma_busy || w25q_rx_dma_busy || w25q_is_flash_busy())
    {
        return false; /* DMA 忙或 Flash 忙：拒绝，调用方稍后重试 */
    }

    w25q_dma_ctx.current_address = address;
    w25q_dma_ctx.current_data = data;
    w25q_dma_ctx.remain_size = size;
    w25q_dma_ctx.current_write_size = 0U;

    return w25q_start_next_write_chunk();
}

/**
 * @brief 异步扇区擦除：只发擦除命令，不等待完成（由 w25q_dma_task 轮询 WIP）。
 * @param address 目标地址（自动按 4KB 对齐）
 * @return true=已提交；false=DMA 状态机忙（有其他读写/擦除在途）或发送失败
 * @note 擦除期间 w25q_read_data_dma / w25q_write_data_dma 会因状态机忙而拒绝，
 *       调用方需先 w25q_erase_sector_busy() 或 w25q_dma_is_busy() 确认完成。
 */
/**
 * @brief 发送单个扇区擦除命令（写使能 + SectorErase），不等待完成。
 * @param address 目标地址（须已 4KB 对齐）
 * @return true=命令已发出；false=SPI 发送失败
 */
static bool w25q_start_sector_erase_cmd(uint32_t address)
{
    uint8_t cmd[4];

    w25q_write_enable();
    cmd[0] = W25Q_SectorErase;
    cmd[1] = (uint8_t)(address >> 16);
    cmd[2] = (uint8_t)(address >> 8);
    cmd[3] = (uint8_t)(address);

    W25Q_CS_LOW();
    if (w25q_spi_transmit(cmd, 4) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }
    W25Q_CS_HIGH();
    return true;
}

/**
 * @brief 异步批量扇区擦除：一次提交连续 sector_count 个 4KB 扇区。
 *        状态机逐扇区擦除（发命令 → 等 WIP 清零 → 下一个），全部完成后 DONE。
 * @param start_address 起始地址（自动按 4KB 对齐）
 * @param sector_count  要擦除的扇区数（≥1）
 * @return true=已提交；false=状态机忙或参数非法
 * @note 擦除期间状态机忙，读/写 DMA 提交会被拒绝；总耗时 ≈ 扇区数 × 150ms（W25Q128 典型值）。
 *       进度可用 w25q_erase_get_remaining() 查询。
 */
bool w25q_erase_range_async(uint32_t start_address, uint32_t sector_count)
{
    if ((sector_count == 0U) || w25q_is_dma_active())
    {
        return false;
    }

    start_address &= 0xFFFFF000;

    if (!w25q_start_sector_erase_cmd(start_address))
    {
        return false;
    }

    w25q_dma_ctx.erase_address = start_address;
    w25q_dma_ctx.erase_remain = sector_count - 1U;
    w25q_dma_ctx.erase_poll_tick = HAL_GetTick();
    w25q_dma_ctx.state = W25Q_DMA_ERASE_WAIT;
    return true;
}

bool w25q_erase_sector_async(uint32_t address)
{
    return w25q_erase_range_async(address, 1U);
}

bool w25q_erase_chip_async(void)
{
    if (w25q_is_dma_active())
    {
        return false;
    }

    w25q_write_enable();
    uint8_t cmd = W25Q_ChipErase;
    W25Q_CS_LOW();
    if (w25q_spi_transmit(&cmd, 1) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return false;
    }
    W25Q_CS_HIGH();

    w25q_dma_ctx.erase_poll_tick = HAL_GetTick();
    w25q_dma_ctx.erase_remain = 0U;
    w25q_dma_ctx.state = W25Q_DMA_ERASE_WAIT;
    return true;
}

/**
 * @brief DMA 状态机轮询任务（主循环中周期调用）
 *
 * 处理写/读/擦除三种操作的启动、传输进度跟踪、等待 Flash 固化。
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

    /* ── 写命令发送阶段：瞬时状态，无需处理 ── */
    case W25Q_DMA_WRITE_STARTING:
        break;

    /* ── 等待 TX DMA 完成 → 转到等待 Flash 内部固化 ── */
    case W25Q_DMA_WAIT_TX_DONE:
    {
        if (!w25q_tx_dma_busy)
        {
            w25q_dma_ctx.write_poll_tick = HAL_GetTick(); /* 进入固化等待，节流计时起点 */
            w25q_dma_ctx.state = W25Q_DMA_WAIT_FLASH_READY;
        }
        break;
    }

    /* ── 等待 RX DMA 完成 → 读结束，标记完成 ── */
    case W25Q_DMA_WAIT_RX_DONE:
    {
        if (!w25q_rx_dma_busy)
        {
            W25Q_CS_HIGH();
            w25q_dma_ctx.remain_size = 0U;
            w25q_dma_ctx.state = W25Q_DMA_DONE;
        }
        break;
    }

    /* ── 等待 Flash 内部编程完成 → 继续下一块或标记完成 ── */
    case W25Q_DMA_WAIT_FLASH_READY:
    {
        uint32_t now = HAL_GetTick();

        /* 轮询节流：≥5ms 才做一次 RDSR，避免主循环空转刷 SPI 拉高 CPU */
        if ((now - w25q_dma_ctx.write_poll_tick) < 5U)
        {
            break;
        }
        w25q_dma_ctx.write_poll_tick = now;

        if (!w25q_wait_flash_idle())
        {
            break; /* 固化中 / RDSR 失败，下次轮询 */
        }

        w25q_dma_ctx.current_address += w25q_dma_ctx.current_write_size;
        w25q_dma_ctx.current_data += w25q_dma_ctx.current_write_size;
        w25q_dma_ctx.remain_size -= w25q_dma_ctx.current_write_size;
        w25q_dma_ctx.current_write_size = 0U;

        (void)w25q_start_next_write_chunk();
        break;
    }

    /* ── 等待 Flash 内部擦除完成（WIP 清零）→ 标记完成 ── */
    case W25Q_DMA_ERASE_WAIT:
    {
        uint32_t now = HAL_GetTick();

        /* 轮询节流：≥5ms 才做一次 RDSR，避免主循环空转刷 SPI 拉高 CPU */
        if ((now - w25q_dma_ctx.erase_poll_tick) < 5U)
        {
            break;
        }
        w25q_dma_ctx.erase_poll_tick = now;

        if (!w25q_wait_flash_idle())
        {
            break; /* 擦除中 / RDSR 失败，下次轮询 */
        }

        /* 当前扇区擦除完成 → 提交下一个，或全部完成 */
        if (w25q_dma_ctx.erase_remain > 0U)
        {
            w25q_dma_ctx.erase_address += W25Q_SECTOR_SIZE;
            w25q_dma_ctx.erase_remain--;
            if (!w25q_start_sector_erase_cmd(w25q_dma_ctx.erase_address))
            {
                w25q_set_dma_error();
                break;
            }
            break;
        }

        w25q_dma_ctx.state = W25Q_DMA_DONE;
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

/**
 * @brief 查询是否有擦除任务（单个或批量）正在进行。
 * @return true=擦除进行中；false=无擦除任务
 */
bool w25q_erase_sector_busy(void)
{
    return w25q_dma_ctx.state == W25Q_DMA_ERASE_WAIT;
}

/**
 * @brief 查询批量擦除剩余扇区数（含正在擦除的 1 个）。
 * @return 0=无擦除任务或已全部完成；否则返回剩余扇区数
 */
uint32_t w25q_erase_get_remaining(void)
{
    if (w25q_dma_ctx.state != W25Q_DMA_ERASE_WAIT)
    {
        return 0U;
    }
    return w25q_dma_ctx.erase_remain + 1U;
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

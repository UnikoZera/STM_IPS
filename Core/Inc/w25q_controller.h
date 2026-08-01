/*
 * w25q_controller.h
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#ifndef INC_W25Q_CONTROLLER_H_
#define INC_W25Q_CONTROLLER_H_

#include <stdint.h>
#include <stdbool.h>
#include "gpio.h"
#include "spi.h"

#define W25Q_CS_LOW() HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_RESET)
#define W25Q_CS_HIGH() HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_SET)

#define W25Q_WriteEnable 0x06
#define W25Q_WriteDisable 0x04
#define W25Q_ReadStatusReg1 0x05
#define W25Q_ReadStatusReg2 0x35
#define W25Q_WriteStatusReg 0x01
#define W25Q_PageProgram 0x02
#define W25Q_SectorErase 0x20   // 4KB
#define W25Q_BlockErase32K 0x52 // 32KB
#define W25Q_BlockErase64K 0xD8 // 64KB
#define W25Q_ChipErase 0xC7     // or 0x60 i think both works.
#define W25Q_PowerDown 0xB9
#define W25Q_ReleasePowerDown 0xAB
#define W25Q_ReadData 0x03
#define W25Q_FastReadData 0x0B
#define W25Q_FastReadDual 0x3B
#define W25Q_DeviceID 0xAB
#define W25Q_ManufactDeviceID 0x90
#define W25Q_JedecDeviceID 0x9F

#define W25Q_DUMMY_BYTE 0xA5

#define W25Q_SECTOR_SIZE 0x1000U /* 4KB 扇区大小（擦除步进/分区计算统一用） */

/* ============================================================================
 * CRC-16 存储校验块格式
 *  ---------------------------------------------------------------------------
 * 数据按 1022B 分块，每块末尾附加 2B CRC-16（小端 LE，复用工程 crc16_usb_calc
 * 查表引擎）。读取时按物理块读并验证 CRC，替代旧的"双读对比"校验。
 *
 * 尾块策略（大小文件不同）：
 *   - 大文件（MJPEG/RAW5 视频/图片）：尾块以 0xFF 补齐，物理块恒 1024B，
 *     4 块 = 4096B = 恰好 1 个 W25Q 扇区，块永不跨扇区；
 *   - 小文件：尾块不补齐，物理 = 实际数据 + 2B CRC，节省小文件区空间。
 * 物理大小用 w25q_crc_phys_size_padded / w25q_crc_phys_size_tight 分别计算。
 * ============================================================================ */
#define W25Q_CRC_BLOCK_DATA_SIZE 1022U
#define W25Q_CRC_BLOCK_PHYS_SIZE  1024U

static inline uint32_t w25q_crc_phys_size_padded(uint32_t data_size)
{
    uint32_t blocks = (data_size + W25Q_CRC_BLOCK_DATA_SIZE - 1U) / W25Q_CRC_BLOCK_DATA_SIZE;
    return blocks * W25Q_CRC_BLOCK_PHYS_SIZE;
}

// 这个函数是输入 data_size，输出物理大小（尾块不补齐，物理 = 实际数据 + 2B CRC）
static inline uint32_t w25q_crc_phys_size_tight(uint32_t data_size)
{
    if (data_size == 0U)
    {
        return 0U;
    }
    uint32_t blocks = (data_size + W25Q_CRC_BLOCK_DATA_SIZE - 1U) / W25Q_CRC_BLOCK_DATA_SIZE;
    uint32_t last   = data_size - (blocks - 1U) * W25Q_CRC_BLOCK_DATA_SIZE;
    return (blocks - 1U) * W25Q_CRC_BLOCK_PHYS_SIZE + last + 2U;
}

bool w25q_check_busy(void);
uint32_t w25q_read_id(void);
bool w25q_init(void);
void w25q_erase_sector(uint32_t address);
void w25q_erase_chip(void);
void w25q_write_data(uint32_t address, uint8_t *data, uint32_t size);
void w25q_read_data(uint32_t address, uint8_t *data, uint16_t size);
void w25q_fast_read_data(uint32_t address, uint8_t *data, uint16_t size);
void w25q_read_data_to(uint32_t address, uint8_t *data, uint32_t size, bool use_fast_read);

bool w25q_write_data_dma(uint32_t address, uint8_t *data, uint32_t size);
bool w25q_read_data_dma(uint32_t address, uint8_t *data, uint32_t size, bool use_fast_read);
bool w25q_crc_read(uint32_t phys_base, uint32_t raw_off, uint8_t *data, uint32_t size,
                   uint32_t data_total, bool tail_pad);

/* DMA 版异步扇区擦除（非阻塞）：提交后由 w25q_dma_task() 轮询 WIP 完成 */
bool w25q_erase_sector_async(uint32_t address);
bool w25q_erase_range_async(uint32_t start_address, uint32_t sector_count);
bool w25q_erase_sector_busy(void);
bool w25q_erase_chip_async(void);
uint32_t w25q_erase_get_remaining(void);

void w25q_dma_task(void);
bool w25q_dma_is_busy(void);
bool w25q_dma_is_done(void);
bool w25q_dma_is_error(void);
void w25q_on_spi_error_callback(void);

extern volatile bool w25q_rx_dma_busy;
extern volatile bool w25q_tx_dma_busy;

#endif /* INC_W25Q_CONTROLLER_H_ */

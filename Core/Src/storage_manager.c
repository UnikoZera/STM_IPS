/*
 * storage_manager.c
 *
 *  Created on: 2026年4月27日
 *      Author: UnikoZera
 *
 *  Host command protocol frame:
 *    [0][1]: Frame header 0xBB 0x44 (2B)
 *    [2]:   Command (1B)
 *    [3-6]: Total file size uint32 LE (4B) — 0 for non-data commands (first packet only)
 *    [7-8]: Packet length uint16 LE (2B) = payload_len + 2 (CRC16)
 *    [9+]:  Payload data
 *    [last-2][last-1]: CRC16 (2B) over header + payload (before CRC)
 *
 *  Commands:
 *    0x11 - Start/continue downloading large file, payload: data + CRC16
 *    0x45 - Start/continue downloading small file, payload: data + CRC16
 *    0x14 - End download, payload: filename(<=16B) + CRC16
 *    0x19 - Delete file, payload: file_type(1B) + file_index(1B) + CRC16
 *    0x20 - Query file list + slot info, no payload (only CRC16)
 *    0x10 - LCD stream control, payload: [sub_cmd(1B)] + CRC16
 */

#include "storage_manager.h"
#include "lcd.h"

#pragma region 文件系统与分配表实现

// ======================== 文件系统/分配表定义 ========================
#define FAT_MAGIC_NUMBER 0x0D000722   // bumped version for new bitmap-based FAT
#define W25Q_SECTOR_SIZE 4096
#define W25Q_TOTAL_SECTORS 4096
// --- 分区映射表 ---
// [区段 1] 保留区: Sector 0 ~ 1 (8KB) 纯空置
#define AREA_RESERVED_START_SECTOR 0
#define AREA_RESERVED_SECTORS 2
// [区段 2] 小文件区: Sector 2 ~ 63 (共 62 个扇区 / 248KB)
#define AREA_SMALL_START_SECTOR 2
#define AREA_SMALL_SECTORS 62
#define AREA_SMALL_START_ADDR (AREA_SMALL_START_SECTOR * W25Q_SECTOR_SIZE)
#define AREA_SMALL_END_ADDR ((AREA_SMALL_START_SECTOR + AREA_SMALL_SECTORS) * W25Q_SECTOR_SIZE)
// [区段 3] 大文件区: Sector 64 ~ 4031 (共 3968 个扇区 / 15.5MB) — 位图管理
#define AREA_LARGE_START_SECTOR 64
#define AREA_LARGE_SECTORS 3968
#define LARGE_BITMAP_SIZE ((AREA_LARGE_SECTORS + 7) / 8)  // 496 bytes
// [区段 4] 用户自定义区: Sector 4032 ~ 4095 (共 64 个扇区 / 256KB)
#define AREA_USER_START_SECTOR 4032
#define AREA_USER_SECTORS 64

// 存储在 AT24C 中的总分配表 (FAT)
typedef struct
{
    uint32_t magic;
    // 小文件分配器状态 (线性挤压式)
    uint32_t small_next_addr; // 小文件区下一个可分配的地址，向上挤压分配，不回收碎片
    uint16_t small_file_count;
    small_file_info_t small_files[MAX_SMALL_FILES];
    // 大文件位图: 每个bit代表一个sector, 1=已分配, 0=空闲
    uint8_t large_sector_bitmap[LARGE_BITMAP_SIZE];
    uint16_t large_file_count;
    large_file_info_t large_files[MAX_LARGE_FILES];
} storage_fat_t;

static storage_fat_t global_fat;

#define FAT_STORAGE_ADDR 0x0000  // FAT在AT24C中的存储地址

#pragma endregion

#pragma region FAT持久化与文件查找功能

static void storage_fat_init_default(void)
{
    memset(&global_fat, 0, sizeof(storage_fat_t));
    global_fat.magic = FAT_MAGIC_NUMBER;
    global_fat.small_next_addr = AREA_SMALL_START_ADDR;
    global_fat.small_file_count = 0;
    memset(global_fat.large_sector_bitmap, 0, LARGE_BITMAP_SIZE);
    global_fat.large_file_count = 0;
}

bool storage_fat_load(void)
{
    storage_fat_t temp_fat;
    if (!at24c_read_buffer(FAT_STORAGE_ADDR, (uint8_t *)&temp_fat, sizeof(storage_fat_t)))
    {
        storage_fat_init_default();
        return false;
    }
    if (temp_fat.magic == FAT_MAGIC_NUMBER)
    {
        memcpy(&global_fat, &temp_fat, sizeof(storage_fat_t));
        return true;
    }
    storage_fat_init_default();
    storage_fat_save();
    return false;
}

void storage_fat_save(void)
{
    at24c_write_buffer(FAT_STORAGE_ADDR, (uint8_t *)&global_fat, sizeof(storage_fat_t));
}

int16_t find_small_file_by_name(const char *name)
{
    for (uint16_t i = 0; i < global_fat.small_file_count; i++)
    {
        if (global_fat.small_files[i].is_valid && strcmp(global_fat.small_files[i].filename, name) == 0)
        {
            return (int16_t)i;
        }
    }
    return -1;
}

int16_t find_large_file_by_name(const char *name)
{
    for (uint16_t i = 0; i < global_fat.large_file_count; i++)
    {
        if (global_fat.large_files[i].is_valid && strcmp(global_fat.large_files[i].filename, name) == 0)
        {
            return (int16_t)i;
        }
    }
    return -1;
}

bool get_small_file_info(uint8_t file_id, small_file_info_t *info)
{
    if (file_id < global_fat.small_file_count && global_fat.small_files[file_id].is_valid)
    {
        memcpy(info, &global_fat.small_files[file_id], sizeof(small_file_info_t));
        return true;
    }
    return false;
}

bool get_large_file_info(uint8_t file_id, large_file_info_t *info)
{
    if (file_id < global_fat.large_file_count && global_fat.large_files[file_id].is_valid)
    {
        memcpy(info, &global_fat.large_files[file_id], sizeof(large_file_info_t));
        return true;
    }
    return false;
}

#pragma endregion

#pragma region 大文件区位图管理

// 位图辅助宏
#define BITMAP_BYTE(b)    ((b) >> 3)
#define BITMAP_MASK(b)    (1 << ((b) & 7))

static inline bool bitmap_test_used(uint32_t sector)
{
    uint32_t idx = sector - AREA_LARGE_START_SECTOR;
    return (global_fat.large_sector_bitmap[BITMAP_BYTE(idx)] & BITMAP_MASK(idx)) != 0;
}

static inline void bitmap_set_used(uint32_t sector)
{
    uint32_t idx = sector - AREA_LARGE_START_SECTOR;
    global_fat.large_sector_bitmap[BITMAP_BYTE(idx)] |= BITMAP_MASK(idx);
}

static inline void bitmap_clear_used(uint32_t sector)
{
    uint32_t idx = sector - AREA_LARGE_START_SECTOR;
    global_fat.large_sector_bitmap[BITMAP_BYTE(idx)] &= ~BITMAP_MASK(idx);
}

/**
 * @brief 在大文件区寻找连续的 free_sectors 个空闲扇区
 * @param free_sectors 需要的连续扇区数
 * @return 起始sector号，失败返回 0xFFFFFFFF
 */
static uint32_t bitmap_find_free_block(uint32_t free_sectors)
{
    uint32_t consecutive = 0;
    for (uint32_t s = AREA_LARGE_START_SECTOR; s < AREA_LARGE_START_SECTOR + AREA_LARGE_SECTORS; s++)
    {
        if (!bitmap_test_used(s))
        {
            consecutive++;
            if (consecutive >= free_sectors)
            {
                return s - free_sectors + 1;
            }
        }
        else
        {
            consecutive = 0;
        }
    }
    return 0xFFFFFFFF; // 没找到足够的连续空间
}

/**
 * @brief 标记一段连续sector为已分配
 */
static void bitmap_mark_block_used(uint32_t start_sector, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        bitmap_set_used(start_sector + i);
    }
}

/**
 * @brief 擦除并释放大文件占用的扇区（用于删除操作）
 */
static void erase_and_free_large_sectors(uint32_t start_sector, uint32_t sector_count)
{
    for (uint32_t i = 0; i < sector_count; i++)
    {
        w25q_erase_sector((start_sector + i) * W25Q_SECTOR_SIZE);
        bitmap_clear_used(start_sector + i);
    }
}

#pragma endregion

// 清空大文件区（用于格式化）
static inline void erase_sector(uint32_t sector)
{
    w25q_erase_sector(sector * W25Q_SECTOR_SIZE);
}

void clear_all_files(void)
{
    for (uint32_t i = AREA_LARGE_START_SECTOR; i < AREA_LARGE_START_SECTOR + AREA_LARGE_SECTORS; i++)
    {
        if (bitmap_test_used(i))
        {
            erase_sector(i);
        }
    }
    for (uint16_t i = AREA_SMALL_START_SECTOR; i < AREA_SMALL_START_SECTOR + AREA_SMALL_SECTORS; i++)
    {
        erase_sector(i);
    }
    storage_fat_init_default();
    storage_fat_save();
}

#pragma region 分配器核心

/**
 * @brief 小文件区线性挤压式分配
 */
static uint32_t allocate_small_space(uint32_t required_bytes)
{
    uint32_t addr = global_fat.small_next_addr;
    if (addr + required_bytes > AREA_SMALL_END_ADDR)
    {
        return 0xFFFFFFFF;
    }
    global_fat.small_next_addr += required_bytes;
    return addr;
}

/**
 * @brief 大文件区位图分配：找到连续的 required_sectors 个空闲扇区并标记
 */
static uint32_t allocate_large_sectors(uint32_t required_sectors)
{
    uint32_t sector = bitmap_find_free_block(required_sectors);
    if (sector == 0xFFFFFFFF)
    {
        return 0xFFFFFFFF;
    }
    bitmap_mark_block_used(sector, required_sectors);
    return sector;
}

#pragma endregion

#pragma region 协议常量与状态机

#define HOST_FRAME_HEAD_0 0xBBU
#define HOST_FRAME_HEAD_1 0x44U
#define FRAME_HDR_SIZE 9U
#define RETRY_SEND_ERROR_CODE 0xE0U
#define CONTINUE_SEND_CODE 0xA1U

typedef enum
{
    STATE_WAIT_HEAD0,
    STATE_WAIT_HEAD1,
    STATE_WAIT_CMD,
    STATE_WAIT_TOTAL_SIZE_0,
    STATE_WAIT_TOTAL_SIZE_1,
    STATE_WAIT_TOTAL_SIZE_2,
    STATE_WAIT_TOTAL_SIZE_3,
    STATE_WAIT_LEN_L,
    STATE_WAIT_LEN_H,
    STATE_WAIT_PAYLOAD // 包括crc16在内的完整数据段
} host_cmd_state_t;

#pragma endregion

#pragma region 协议解析缓冲区与下载状态

static host_cmd_state_t host_state = STATE_WAIT_HEAD0;
static uint32_t host_state_tick = 0;
static uint8_t host_cmd;
// host_total_file_size 是发送文件的完整大小（仅第一包有效）, host_payload_len 是本包数据段长度(含CRC)
static uint32_t host_total_file_size = 0;
static uint16_t host_payload_len;
static uint16_t host_payload_idx;
// host_payload存储布局：[0..8]=帧头9字节，[9..]=payload数据(含CRC16)
static uint8_t host_payload[FRAME_HDR_SIZE + 2048 + 2] = {0};
static uint8_t rx_buffer[FRAME_HDR_SIZE + 2048 + 2];
static uint8_t dma_write_buf[2048];

static bool is_downloading = false;
static bool lcd_stream_was_enabled = false;
static uint32_t current_write_addr = 0;
static uint32_t current_file_size = 0;
static uint32_t current_allocated_size = 0;
static uint8_t current_file_type = 0;
static char current_filename[MAX_FILENAME_LEN] = {0};
static uint32_t current_start_sector = 0;
static uint32_t current_sector_count = 0;
static uint32_t small_file_start_addr = 0;
static uint8_t error_payload = 0x00;
static uint32_t small_last_erased_sector = 0xFFFFFFFF;

#pragma endregion

#pragma region 辅助函数

static void clear_host_payload(void)
{
    memset(host_payload, 0x00, sizeof(host_payload));
}

static void reset_host_state(void)
{
    host_state = STATE_WAIT_HEAD0;
    host_state_tick = HAL_GetTick();
    clear_host_payload();
}

extern volatile bool lcd_usb_stream_enabled; // from lcd_ui.c

static void send_error(uint8_t error_type)
{
    error_payload = error_type;
    usb_controller_send(&g_usb_controller, RETRY_SEND_ERROR_CODE, &error_payload, 1);
}

static void send_continue(void)
{
    usb_controller_send(&g_usb_controller, CONTINUE_SEND_CODE, NULL, 0);
}

static void abort_download_common(void)
{
    lcd_usb_stream_enabled = lcd_stream_was_enabled;
    is_downloading = false;
}

static void abort_download_with_error(uint8_t error_type)
{
    abort_download_common();
    send_error(error_type);
}

/**
 * @brief 写入W25Q并回读验证，失败自动重试（最多WRITE_VERIFY_RETRY_MAX次）
 *        验证方式：写入后用 w25q_read_data 回读，与 dma_write_buf memcmp 对比
 */
#define DMA_WAIT_TIMEOUT_MS 500U
#define HOST_STATE_TIMEOUT_MS 500U
#define WRITE_VERIFY_RETRY_MAX 20U

static bool flash_write_and_verify(uint32_t addr, const uint8_t *data, uint32_t size)
{
    if (size == 0U) return true;

    for (uint32_t retry = 0U; retry < WRITE_VERIFY_RETRY_MAX; retry++)
    {
        // --- 等待DMA空闲 ---
        {
            uint32_t wait_start = HAL_GetTick();
            while (w25q_dma_is_busy())
            {
                w25q_dma_task();
                usb_controller_task(&g_usb_controller);
                if (HAL_GetTick() - wait_start >= DMA_WAIT_TIMEOUT_MS)
                {
                    send_error(0x0B);
                    return false;
                }
            }
        }

        // --- 拷贝数据到dma_write_buf ---
        memcpy(dma_write_buf, data, size);

        // --- 写入W25Q（优先DMA，TransmitReceive_DMA解决RX FIFO溢出） ---
        if (w25q_write_data_dma(addr, dma_write_buf, size))
        {
            // DMA 已启动，在后台运行，不等待也不验证
            // 下一块的函数调用会在顶部等待DMA完成，实现flash编程与USB传输重叠
            return true;
        }
        else
        {
            // DMA启动失败，回退同步写入+回读验证
            w25q_write_data(addr, dma_write_buf, size);
        }

        // --- 回读验证（仅同步路径执行） ---
        // 使用 FastRead + dummy byte + 1ms 延时，确保时序稳定
        HAL_Delay(1);
        w25q_fast_read_data(addr, rx_buffer, size);
        if (memcmp(rx_buffer, dma_write_buf, size) == 0)
        {
            return true; // 验证通过
        }
        // --- 不匹配 ---
    }


    return false; // 多次重试后仍失败
}
#pragma endregion

#pragma region 命令处理核心

static void process_host_command(void)
{
    if (host_payload_len < 2) return;
    if (!crc16_usb_packing(host_payload, FRAME_HDR_SIZE + host_payload_len, true))
    {
        send_error(0x01);
        return;
    }

    uint16_t actual_data_len = host_payload_len - 2;
    // ==================== 0x19: 删除文件 ====================
    if (host_cmd == 0x19)
    {
        if (host_payload_len < 4)
            return;
        uint8_t file_type_to_delete = host_payload[FRAME_HDR_SIZE];
        uint8_t file_index = host_payload[FRAME_HDR_SIZE + 1];

        if (file_type_to_delete == 0x11)
        {
            if (file_index >= global_fat.large_file_count || !global_fat.large_files[file_index].is_valid)
            {
                send_error(0x08);
                return;
            }
            // 擦除W25Q扇区并释放位图
            large_file_info_t *fi = &global_fat.large_files[file_index];
            erase_and_free_large_sectors(fi->start_sector, fi->sector_count);
            fi->is_valid = 0;
            storage_fat_save();
        }
        else if (file_type_to_delete == 0x45)
        {
            if (file_index >= global_fat.small_file_count || !global_fat.small_files[file_index].is_valid)
            {
                send_error(0x08);
                return;
            }
            global_fat.small_files[file_index].is_valid = 0; // sorry, no recycle for small file space.
            storage_fat_save();
        }
        else
        {
            send_error(0x02);
        }
        return;
    }
    // ==================== 0x14: 结束下载 ====================
    if (host_cmd == 0x14)
    {
        if (!is_downloading)
            return;

        memcpy(current_filename, (char *)&host_payload[FRAME_HDR_SIZE],
               (actual_data_len < MAX_FILENAME_LEN) ? actual_data_len : MAX_FILENAME_LEN);
        if (current_file_type == 0x11)
        {
            if (global_fat.large_file_count < MAX_LARGE_FILES)
            {
                large_file_info_t *fi = &global_fat.large_files[global_fat.large_file_count];
                fi->is_valid = 1;
                fi->file_type = current_file_type;
                fi->start_sector = current_start_sector;
                fi->size = current_file_size;
                fi->sector_count = current_sector_count;
                memcpy(fi->filename, current_filename, MAX_FILENAME_LEN);
                global_fat.large_file_count++;
                storage_fat_save();
            }
        }
        else if (current_file_type == 0x45)
        {
            if (global_fat.small_file_count < MAX_SMALL_FILES)
            {
                small_file_info_t *fi = &global_fat.small_files[global_fat.small_file_count];
                fi->is_valid = 1;
                fi->file_type = current_file_type;
                fi->start_address = small_file_start_addr;
                fi->size = current_file_size;
                memcpy(fi->filename, current_filename, MAX_FILENAME_LEN);
                global_fat.small_file_count++;
                storage_fat_save();
            }
        }
        lcd_usb_stream_enabled = lcd_stream_was_enabled;
        is_downloading = false;
        send_continue();
        return;
    }
    // ==================== 0x11 / 0x45: 下载数据 ====================
    if (host_cmd == 0x11 || host_cmd == 0x45)
    {
        uint32_t erase_start_sector = 0;
        uint32_t erase_end_sector = 0;
        bool need_erase = false;

        if (!is_downloading) // 新下载请求
        {
            is_downloading = true;
            lcd_stream_was_enabled = lcd_usb_stream_enabled;
            lcd_usb_stream_enabled = false;
            current_file_type = host_cmd;
            current_file_size = 0;
            current_allocated_size = 0;
            current_start_sector = 0;
            current_sector_count = 0;
            small_file_start_addr = 0;
            current_write_addr = 0;
            memset(current_filename, 0x00, sizeof(current_filename));

            if (host_cmd == 0x11)
            {
                // 大文件: 根据 host_total_file_size 预分配扇区
                uint32_t total_size = host_total_file_size;
                if (total_size == 0)
                {
                    // 如果没有总大小信息，至少分配1个扇区
                    total_size = W25Q_SECTOR_SIZE;
                }
                uint32_t required_sectors = (total_size + W25Q_SECTOR_SIZE - 1) / W25Q_SECTOR_SIZE;
                uint32_t allocated_first_sector = allocate_large_sectors(required_sectors);
                if (allocated_first_sector == 0xFFFFFFFF)
                {
                    abort_download_with_error(0x03);
                    return;
                }
                current_start_sector = allocated_first_sector;
                current_write_addr = allocated_first_sector * W25Q_SECTOR_SIZE;
                current_sector_count = required_sectors;
                current_allocated_size = required_sectors * W25Q_SECTOR_SIZE;
            }
            else
            {
                // 小文件: 保持原有线性分配逻辑
                uint32_t allocated_addr = allocate_small_space(W25Q_SECTOR_SIZE);
                if (allocated_addr == 0xFFFFFFFF)
                {
                    abort_download_with_error(0x04);
                    return;
                }
                small_file_start_addr = allocated_addr;
                current_write_addr = allocated_addr;
                current_allocated_size = W25Q_SECTOR_SIZE;
                erase_start_sector = allocated_addr / W25Q_SECTOR_SIZE;
                erase_end_sector = erase_start_sector + 1;
                need_erase = true;
            }
        }
        else
        {
            // 正在下载中，处理后续数据包
            if (current_file_type != host_cmd)
            {
                send_error(0x05);
                return;
            }

            if (host_cmd == 0x11)
            {
                // 大文件扩展：如果当前分配空间不足则扩展
                if (current_file_size + actual_data_len > current_allocated_size)
                {
                    uint32_t additional_bytes = (current_file_size + actual_data_len) - current_allocated_size;
                    uint32_t additional_sectors = (additional_bytes + W25Q_SECTOR_SIZE - 1) / W25Q_SECTOR_SIZE;
                    uint32_t new_total_sectors = current_sector_count + additional_sectors;
                    // 检查后续扇区是否可用
                    bool can_extend = true;
                    for (uint32_t i = 0; i < additional_sectors; i++)
                    {
                        uint32_t s = current_start_sector + current_sector_count + i;
                        if (s >= AREA_LARGE_START_SECTOR + AREA_LARGE_SECTORS || bitmap_test_used(s))
                        {
                            can_extend = false;
                            break;
                        }
                    }
                    if (can_extend)
                    {
                        bitmap_mark_block_used(current_start_sector + current_sector_count, additional_sectors);
                        current_sector_count = new_total_sectors;
                        current_allocated_size = current_sector_count * W25Q_SECTOR_SIZE;
                    }
                    else
                    {
                        abort_download_with_error(0x06);
                        return;
                    }
                }
            }
            // 小文件不预分配，直接按需擦除
            if (host_cmd == 0x45)
            {
                uint32_t needed_total = current_write_addr + actual_data_len - small_file_start_addr;
                if (needed_total > current_allocated_size)
                {
                    uint32_t additional_bytes = needed_total - current_allocated_size;
                    if (global_fat.small_next_addr + additional_bytes <= AREA_SMALL_END_ADDR)
                    {
                        global_fat.small_next_addr += additional_bytes;
                        current_allocated_size += additional_bytes;
                    }
                    else
                    {
                        abort_download_with_error(0x07);
                        return;
                    }
                }
                uint32_t new_end_sector = (small_file_start_addr + current_allocated_size - 1) / W25Q_SECTOR_SIZE;
                if (small_last_erased_sector < new_end_sector)
                {
                    erase_start_sector = small_last_erased_sector + 1;
                    erase_end_sector = new_end_sector + 1;
                    need_erase = true;
                }
            }
        }

        if (need_erase && host_cmd == 0x45)
        {
            for (uint32_t s = erase_start_sector; s < erase_end_sector; s++)
            {
                w25q_erase_sector(s * W25Q_SECTOR_SIZE);
                small_last_erased_sector = s;
            }
        }

        if (actual_data_len > 0)
        {
            if (!flash_write_and_verify(current_write_addr, &host_payload[FRAME_HDR_SIZE], actual_data_len))
            {
                abort_download_common();
                return;
            }
        }
        current_write_addr += actual_data_len;
        current_file_size += actual_data_len;
        send_continue();
        return;
    }

    // ==================== 0x20: ?????? (TLV??) ====================
    // ???: [entry_count(1B)] [slot_count(1B)] [slot_records...] [file_records...]
    // slot??: [rLen=10(1B)] [tag=0xFF(1B)] [start_sector(4B LE)] [sector_count(4B LE)]
    // ????: [rLen(1B)] [tag(1B)] [file_index(1B)] [name_len(1B)] [filename(NB)] [addr/sector(4B LE)] [size(4B LE)]
    //   ?????: sector_count(4B LE)
    //   small: rLen = 12 + name_len
    //   large: rLen = 16 + name_len
    if (host_cmd == 0x20)
    {
        static uint8_t file_list_buffer[2560];
        uint16_t idx = 0;
        uint8_t entry_count = 0;
        uint8_t slot_count = 0;

        idx = 2;

        {
            uint32_t s = AREA_LARGE_START_SECTOR;
            while (s < AREA_LARGE_START_SECTOR + AREA_LARGE_SECTORS)
            {
                if (bitmap_test_used(s))
                {
                    uint32_t block_start = s;
                    uint32_t block_count = 0;
                    while (s < AREA_LARGE_START_SECTOR + AREA_LARGE_SECTORS && bitmap_test_used(s))
                    {
                        block_count++;
                        s++;
                    }
                    // rLen(1) + tag(1) + start_sector(4) + sector_count(4) = 10
                    if (idx + 10 > sizeof(file_list_buffer)) break;
                    file_list_buffer[idx++] = 10;
                    file_list_buffer[idx++] = 0xFF;
                    memcpy(&file_list_buffer[idx], &block_start, 4);
                    idx += 4;
                    memcpy(&file_list_buffer[idx], &block_count, 4);
                    idx += 4;
                    slot_count++;
                }
                else
                {
                    s++;
                }
            }
        }

        for (uint16_t i = 0; i < global_fat.small_file_count; i++)
        {
            if (!global_fat.small_files[i].is_valid) continue;
            uint8_t namelen = (uint8_t)strlen(global_fat.small_files[i].filename);
            uint8_t record_len = 12 + namelen;
            if (idx + record_len > sizeof(file_list_buffer)) break;
            file_list_buffer[idx++] = record_len;
            file_list_buffer[idx++] = (0 << 7) | (global_fat.small_files[i].file_type & 0x7F);
            file_list_buffer[idx++] = i;
            file_list_buffer[idx++] = namelen;
            memcpy(&file_list_buffer[idx], global_fat.small_files[i].filename, namelen);
            idx += namelen;
            memcpy(&file_list_buffer[idx], &global_fat.small_files[i].start_address, 4);
            idx += 4;
            memcpy(&file_list_buffer[idx], &global_fat.small_files[i].size, 4);
            idx += 4;
            entry_count++;
        }

        for (uint16_t i = 0; i < global_fat.large_file_count; i++)
        {
            if (!global_fat.large_files[i].is_valid) continue;
            uint8_t namelen = (uint8_t)strlen(global_fat.large_files[i].filename);
            uint8_t record_len = 16 + namelen;  // 12?? + 4(sector_count) + namelen
            if (idx + record_len > sizeof(file_list_buffer)) break;
            file_list_buffer[idx++] = record_len;
            file_list_buffer[idx++] = (1 << 7) | (global_fat.large_files[i].file_type & 0x7F);
            file_list_buffer[idx++] = i;
            file_list_buffer[idx++] = namelen;
            memcpy(&file_list_buffer[idx], global_fat.large_files[i].filename, namelen);
            idx += namelen;
            memcpy(&file_list_buffer[idx], &global_fat.large_files[i].start_sector, 4);
            idx += 4;
            memcpy(&file_list_buffer[idx], &global_fat.large_files[i].size, 4);
            idx += 4;
            // sector_count
            memcpy(&file_list_buffer[idx], &global_fat.large_files[i].sector_count, 4);
            idx += 4;
            entry_count++;
        }

        file_list_buffer[0] = entry_count;
        file_list_buffer[1] = slot_count;
        usb_controller_send(&g_usb_controller, 0x20, file_list_buffer, idx);
        return;
    }

    // ==================== 0x21: 发送bitmap ====================
    if (host_cmd == 0x21)
    {
        usb_controller_send(&g_usb_controller, 0x21, global_fat.large_sector_bitmap, LARGE_BITMAP_SIZE);
        return;
    }

    // ==================== 0x10: LCD检测 ====================
    if (host_cmd == 0x10)
    {
        if (host_payload_len >= 3)
        {
            uint8_t sub_cmd = host_payload[FRAME_HDR_SIZE];
            if (sub_cmd == 0x01) lcd_usb_stream_enabled = true;
            else if (sub_cmd == 0x00) lcd_usb_stream_enabled = false;
        }
        uint8_t resp[1];
        resp[0] = lcd_usb_stream_enabled ? 0x01 : 0x00;
        usb_controller_send(&g_usb_controller, 0x10, resp, sizeof(resp));
        return;
    }

    // ==================== 未知指令 ====================
    send_error(0x09);
}

#pragma endregion

#pragma region 解码状态机

static void storage_manager_process_host_byte(uint8_t byte)
{
    switch (host_state)
    {
    case STATE_WAIT_HEAD0:
        if (byte == HOST_FRAME_HEAD_0)
        {
            host_payload[0] = byte;
            host_state = STATE_WAIT_HEAD1;
            host_state_tick = HAL_GetTick();
        }
        break;
    case STATE_WAIT_HEAD1:
        if (byte == HOST_FRAME_HEAD_1)
        {
            host_payload[1] = byte;
            host_state = STATE_WAIT_CMD;
            host_state_tick = HAL_GetTick();
        }
        else if (byte == HOST_FRAME_HEAD_0)
        {
            host_state = STATE_WAIT_HEAD1;
            host_state_tick = HAL_GetTick();
        }
        else
        {
            reset_host_state();
        }
        break;
    case STATE_WAIT_CMD:
        host_cmd = byte;
        host_payload[2] = byte;
        host_state = STATE_WAIT_TOTAL_SIZE_0;
        host_state_tick = HAL_GetTick();
        break;
    case STATE_WAIT_TOTAL_SIZE_0:
        host_payload[3] = byte;
        host_total_file_size = byte;
        host_state = STATE_WAIT_TOTAL_SIZE_1;
        host_state_tick = HAL_GetTick();
        break;
    case STATE_WAIT_TOTAL_SIZE_1:
        host_payload[4] = byte;
        host_total_file_size |= ((uint32_t)byte << 8);
        host_state = STATE_WAIT_TOTAL_SIZE_2;
        host_state_tick = HAL_GetTick();
        break;
    case STATE_WAIT_TOTAL_SIZE_2:
        host_payload[5] = byte;
        host_total_file_size |= ((uint32_t)byte << 16);
        host_state = STATE_WAIT_TOTAL_SIZE_3;
        host_state_tick = HAL_GetTick();
        break;
    case STATE_WAIT_TOTAL_SIZE_3:
        host_payload[6] = byte;
        host_total_file_size |= ((uint32_t)byte << 24);
        host_state = STATE_WAIT_LEN_L;
        host_state_tick = HAL_GetTick();
        break;
    case STATE_WAIT_LEN_L:
        host_payload_len = byte;
        host_payload[7] = byte;
        host_state = STATE_WAIT_LEN_H;
        host_state_tick = HAL_GetTick();
        break;
    case STATE_WAIT_LEN_H:
        host_payload_len |= ((uint16_t)byte << 8);
        host_payload[8] = byte;
        if (host_payload_len > sizeof(host_payload) - FRAME_HDR_SIZE || host_payload_len < 2)
        {
            reset_host_state();
        }
        else
        {
            host_payload_idx = 0;
            host_state = STATE_WAIT_PAYLOAD;
            host_state_tick = HAL_GetTick();
        }
        break;
    case STATE_WAIT_PAYLOAD:
        host_payload[host_payload_idx + FRAME_HDR_SIZE] = byte;
        host_payload_idx++;
        if (host_payload_idx >= host_payload_len)
        {
            process_host_command();
            reset_host_state();
        }
        break;
    }
}

#pragma endregion

#pragma region 外部接口

bool storage_manager_init(void)
{
    reset_host_state();
    bool fat_ok = storage_fat_load();
    small_last_erased_sector = (global_fat.small_next_addr > 0)
        ? ((global_fat.small_next_addr - 1) / W25Q_SECTOR_SIZE)
        : (AREA_SMALL_START_SECTOR - 1);
    return fat_ok;
}

void storage_manager_task(void)
{
    uint16_t rx_len = usb_controller_receive(&g_usb_controller, rx_buffer, sizeof(rx_buffer));
    for (uint16_t i = 0; i < rx_len;)
    {
        if (host_state == STATE_WAIT_PAYLOAD)
        {
            uint16_t need_len = host_payload_len - host_payload_idx;
            uint16_t valid_len = rx_len - i;
            uint16_t copy_len = (need_len < valid_len) ? need_len : valid_len;
            memcpy(&host_payload[host_payload_idx + FRAME_HDR_SIZE], &rx_buffer[i], copy_len);
            host_payload_idx += copy_len;
            i += copy_len;
            if (host_payload_idx >= host_payload_len)
            {
                process_host_command();
                reset_host_state();
            }
        }
        else
        {
            storage_manager_process_host_byte(rx_buffer[i]);
            i++;
        }
    }
    if (host_state != STATE_WAIT_HEAD0 && HAL_GetTick() - host_state_tick >= HOST_STATE_TIMEOUT_MS)
    {
        reset_host_state();
    }
}

bool storage_is_downloading(void)
{
    return is_downloading;
}

#pragma endregion
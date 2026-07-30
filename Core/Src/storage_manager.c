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

/* ============================================================================
 *  内部宏定义（集中查阅区）
 *  --------------------------------------------------------------------------
 *  W25Q 16MB 分区:
 *    [保留区]   Sector 0    ~ 1      (2 * 4KB  = 8KB)
 *    [小文件区] Sector 2    ~ 63     (62 * 4KB = 248KB)  线性挤压分配
 *    [大文件区] Sector 64   ~ 4031   (3968*4KB= 15.5MB)  位图管理
 *    [用户区]   Sector 4032 ~ 4095   (64 * 4KB = 256KB)
 * ============================================================================ */

/* ---- Flash 基础 ---- */
#define W25Q_SECTOR_SIZE 4096
#define W25Q_TOTAL_SECTORS 4096

/* ---- 分区: 保留区 ---- */
#define AREA_RESERVED_START_SECTOR 0
#define AREA_RESERVED_SECTORS 2
#define AREA_RESERVED_START_ADDR (AREA_RESERVED_START_SECTOR * W25Q_SECTOR_SIZE)

/* ---- 分区: 小文件区 ---- */
#define AREA_SMALL_START_SECTOR 2
#define AREA_SMALL_SECTORS 62
#define AREA_SMALL_START_ADDR (AREA_SMALL_START_SECTOR * W25Q_SECTOR_SIZE)
#define AREA_SMALL_END_ADDR ((AREA_SMALL_START_SECTOR + AREA_SMALL_SECTORS) * W25Q_SECTOR_SIZE)

/* ---- 分区: 大文件区 ---- */
#define AREA_LARGE_START_SECTOR 64
#define AREA_LARGE_SECTORS 3968
#define LARGE_BITMAP_SIZE ((AREA_LARGE_SECTORS + 7) / 8) /* 496 bytes */

/* ---- 分区: 用户自定义区 ---- */
#define AREA_USER_START_SECTOR 4032
#define AREA_USER_SECTORS 64

/* ---- FAT (存于 AT24C) ---- */
#define FAT_MAGIC_NUMBER 0x0D000722 /* bitmap-based FAT 版本号 */
#define FAT_STORAGE_ADDR 0x0000     /* FAT 在 AT24C 中的起始地址 */

/* ---- 大文件区位图辅助 ---- */
#define BITMAP_BYTE(b) ((b) >> 3)
#define BITMAP_MASK(b) (1 << ((b) & 7))

/* ---- Host 协议帧 ---- */
#define HOST_FRAME_HEAD_0 0xBBU
#define HOST_FRAME_HEAD_1 0x44U
#define FRAME_HDR_SIZE 9U           /* BB 44 CMD SIZE(4) LEN(2) */
#define HOST_PAYLOAD_DATA_MAX 2048U /* 单包 payload 最大数据字节数 */
#define HOST_CRC_SIZE 2U
#define HOST_FRAME_BUF_SIZE (FRAME_HDR_SIZE + HOST_PAYLOAD_DATA_MAX + HOST_CRC_SIZE)
#define RETRY_SEND_ERROR_CODE 0xE0U
#define CONTINUE_SEND_CODE 0xA1U

/* ---- 超时 / 重试 ---- */
#define DMA_WAIT_TIMEOUT_MS 100U
#define HOST_STATE_TIMEOUT_MS 500U
#define WRITE_VERIFY_RETRY_MAX 20U

#pragma region 文件系统与分配表实现

/* 存储在 AT24C 中的总分配表 (FAT) */
typedef struct
{
    uint32_t magic;
    /* 小文件分配器状态 (线性挤压式) */
    uint32_t small_next_addr; /* 下一个可分配地址，向上挤压，不回收碎片 */
    uint16_t small_file_count;
    small_file_info_t small_files[MAX_SMALL_FILES];
    /* 大文件位图: 每 bit 一个 sector, 1=已分配, 0=空闲 */
    uint8_t large_sector_bitmap[LARGE_BITMAP_SIZE];
    uint16_t large_file_count;
    large_file_info_t large_files[MAX_LARGE_FILES];
} storage_fat_t;

static storage_fat_t global_fat;

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

void clear_all_files_manual(void)
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

#pragma region 小文件区压缩

/* compact / 协议共用缓冲，必须在 compact_small_files 之前声明 */
static uint8_t dma_write_buf[HOST_PAYLOAD_DATA_MAX];
static uint8_t rx_buffer[HOST_FRAME_BUF_SIZE];
static uint32_t small_last_erased_sector = 0xFFFFFFFF;

static bool flash_write_and_verify(uint32_t addr, const uint8_t *data, uint32_t size);

/* 从 flash 读到 rx_buffer 再写到另一地址的 chunked copy（带回读验证） */
static bool flash_chunked_copy(uint32_t src, uint32_t dst, uint32_t size)
{
    while (size > 0)
    {
        uint32_t chunk = (size > sizeof(dma_write_buf)) ? sizeof(dma_write_buf) : size;
        w25q_fast_read_data(src, rx_buffer, chunk);
        if (!flash_write_and_verify(dst, rx_buffer, chunk))
        {
            return false;
        }
        src += chunk;
        dst += chunk;
        size -= chunk;
    }
    return true;
}

/**
 * @brief 小文件区垃圾回收压缩（分块式）
 *
 * 原理：按地址顺序逐文件处理。对于每个文件，检查它与哪些有效文件共享扇区，
 * 将这些文件组成一个"批"一起处理：
 *   ① 将整批文件拷贝到保留区(Sector 0~1, 8KB)暂存
 *   ② 擦除这批文件占用的所有扇区
 *   ③ 从保留区读回，按顺序紧凑写入小文件区起始位置
 *   ④ 更新所有被处理文件的 start_address
 *
 * 若单个文件大小超过保留区容量(8KB)则返回 false。
 * 压缩条件：小文件区剩余空间 < SMALL_FILE_COMPACT_THRESHOLD。
 *
 * @return true 压缩成功，false 失败
 */
bool compact_small_files(void)
{
    // ---- Step 1: 收集所有有效文件，按地址排序 ----
    uint16_t valid_list[MAX_SMALL_FILES];
    uint16_t valid_count = 0;
    for (uint16_t i = 0; i < global_fat.small_file_count; i++)
    {
        if (global_fat.small_files[i].is_valid)
        {
            valid_list[valid_count++] = i;
        }
    }

    // 没有有效文件 → 直接擦除小文件区并重置分配器
    if (valid_count == 0)
    {
        for (uint32_t s = AREA_SMALL_START_SECTOR; s < AREA_SMALL_START_SECTOR + AREA_SMALL_SECTORS; s++)
        {
            w25q_erase_sector(s * W25Q_SECTOR_SIZE);
        }
        global_fat.small_next_addr = AREA_SMALL_START_ADDR;
        small_last_erased_sector = AREA_SMALL_START_SECTOR - 1;
        storage_fat_save();
        return true;
    }

    // 按 start_address 升序排序（简单选择排序，n ≤ 32）
    for (uint16_t i = 0; i < valid_count; i++)
    {
        uint16_t min_idx = i;
        for (uint16_t j = i + 1; j < valid_count; j++)
        {
            if (global_fat.small_files[valid_list[j]].start_address <
                global_fat.small_files[valid_list[min_idx]].start_address)
            {
                min_idx = j;
            }
        }
        if (min_idx != i)
        {
            uint16_t tmp = valid_list[i];
            valid_list[i] = valid_list[min_idx];
            valid_list[min_idx] = tmp;
        }
    }

    // ---- Step 2: 分批处理 ----
    const uint32_t reserved_capacity = AREA_RESERVED_SECTORS * W25Q_SECTOR_SIZE; // 8192
    bool moved[MAX_SMALL_FILES];
    memset(moved, 0, sizeof(moved));

    uint32_t compact_dest = AREA_SMALL_START_ADDR;
    uint16_t vi = 0;

    while (vi < valid_count)
    {
        if (moved[valid_list[vi]])
        {
            vi++;
            continue;
        }

        // 2a. 构建当前批：找到与 valid_list[vi] 共享扇区的所有有效文件
        uint16_t batch_indices[MAX_SMALL_FILES];
        uint16_t batch_count = 0;
        uint32_t batch_size = 0;

        // 初始扇区范围由领头的文件决定
        small_file_info_t *leader = &global_fat.small_files[valid_list[vi]];
        uint32_t batch_start_sector = leader->start_address / W25Q_SECTOR_SIZE;
        uint32_t batch_end_sector = (leader->start_address + leader->size - 1) / W25Q_SECTOR_SIZE;

        // 迭代扫描：每次加入新文件后可能扩大扇区范围，再检查新范围内有无更多文件
        bool expanded;
        do
        {
            expanded = false;
            for (uint16_t j = vi; j < valid_count; j++)
            {
                uint16_t idx = valid_list[j];
                if (moved[idx])
                    continue;

                // 检查是否已在 batch 中
                bool already_in = false;
                for (uint16_t b = 0; b < batch_count; b++)
                {
                    if (batch_indices[b] == idx)
                    {
                        already_in = true;
                        break;
                    }
                }
                if (already_in)
                    continue;

                small_file_info_t *fj = &global_fat.small_files[idx];
                uint32_t js = fj->start_address / W25Q_SECTOR_SIZE;
                uint32_t je = (fj->start_address + fj->size - 1) / W25Q_SECTOR_SIZE;

                // 是否与当前扇区范围重叠
                if (js <= batch_end_sector && je >= batch_start_sector)
                {
                    // 加入批
                    batch_indices[batch_count++] = idx;
                    batch_size += fj->size;

                    // 扩展扇区范围
                    if (js < batch_start_sector)
                        batch_start_sector = js;
                    if (je > batch_end_sector)
                        batch_end_sector = je;
                    expanded = true;
                }
            }
        } while (expanded);

        // 2b. 检查批大小是否超过保留区容量
        if (batch_size > reserved_capacity)
        {
            return false; // 一个批放不下（文件太大），跳过本次压缩
        }

        // 2c. 擦除保留区 → 将整批文件拷贝到保留区
        {
            for (uint32_t s = AREA_RESERVED_START_SECTOR; s < AREA_RESERVED_START_SECTOR + AREA_RESERVED_SECTORS; s++)
            {
                w25q_erase_sector(s * W25Q_SECTOR_SIZE);
            }

            uint32_t reserved_off = 0;
            for (uint16_t b = 0; b < batch_count; b++)
            {
                small_file_info_t *fb = &global_fat.small_files[batch_indices[b]];
                if (!flash_chunked_copy(fb->start_address,
                                        AREA_RESERVED_START_ADDR + reserved_off,
                                        fb->size))
                {
                    return false;
                }
                reserved_off += fb->size;
            }
        }

        // 2d. 擦除这批文件占用的所有扇区
        for (uint32_t s = batch_start_sector; s <= batch_end_sector; s++)
        {
            w25q_erase_sector(s * W25Q_SECTOR_SIZE);
        }

        // 2e. 从保留区读回，紧凑写入小文件区
        {
            uint32_t reserved_off = 0;
            for (uint16_t b = 0; b < batch_count; b++)
            {
                small_file_info_t *fb = &global_fat.small_files[batch_indices[b]];
                if (!flash_chunked_copy(AREA_RESERVED_START_ADDR + reserved_off,
                                        compact_dest,
                                        fb->size))
                {
                    return false;
                }

                // 更新文件起始地址
                fb->start_address = compact_dest;
                compact_dest += fb->size;
                reserved_off += fb->size;
                moved[batch_indices[b]] = true;
            }

            // 每批处理完成后立即持久化 FAT，防止中途失败导致已处理批次的数据丢失
            storage_fat_save();
        }

        // 跳过已处理文件
        while (vi < valid_count && moved[valid_list[vi]])
            vi++;
    }

    // ---- Step 3: 末尾清理 — 擦除原数据区末尾剩余的扇区 ----
    {
        uint32_t old_last = (global_fat.small_next_addr > 0)
                                ? ((global_fat.small_next_addr - 1) / W25Q_SECTOR_SIZE)
                                : (AREA_SMALL_START_SECTOR - 1);
        uint32_t new_last = (compact_dest > 0)
                                ? ((compact_dest - 1) / W25Q_SECTOR_SIZE)
                                : (AREA_SMALL_START_SECTOR - 1);

        for (uint32_t s = new_last + 1; s <= old_last; s++)
        {
            w25q_erase_sector(s * W25Q_SECTOR_SIZE);
        }
    }

    // ---- Step 4: 更新分配器状态 ----
    global_fat.small_next_addr = compact_dest;
    small_last_erased_sector = (compact_dest > 0)
                                   ? ((compact_dest - 1) / W25Q_SECTOR_SIZE)
                                   : (AREA_SMALL_START_SECTOR - 1);

    // 保存 FAT
    storage_fat_save();
    return true;
}

#pragma endregion

#pragma region 协议常量与状态机

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
    STATE_WAIT_PAYLOAD /* 包括 crc16 在内的完整数据段 */
} host_cmd_state_t;

#pragma endregion

#pragma region 协议解析缓冲区与下载状态

static host_cmd_state_t host_state = STATE_WAIT_HEAD0;
static uint32_t host_state_tick = 0;
static uint8_t host_cmd;
/* host_total_file_size: 发送文件完整大小（仅第一包有效）
 * host_payload_len:     本包数据段长度(含CRC) */
static uint32_t host_total_file_size = 0;
static uint16_t host_payload_len;
static uint16_t host_payload_idx;
/* host_payload 布局: [0..8]=帧头9字节, [9..]=payload数据(含CRC16)
 * rx_buffer / dma_write_buf / small_last_erased_sector 见上方“小文件区压缩”区域 */
static uint8_t host_payload[HOST_FRAME_BUF_SIZE] = {0};

static bool is_downloading = false;
static bool lcd_stream_was_enabled = false;
static uint32_t current_write_addr = 0;
static uint32_t current_file_size = 0;
static uint32_t current_allocated_size = 0;
/* 首包声明的完整文件大小；后续包 total_size 字段为 0，不能复用 host_total_file_size */
static uint32_t expected_file_size = 0;
static uint8_t current_file_type = 0;
static char current_filename[MAX_FILENAME_LEN] = {0};
static uint32_t current_start_sector = 0;
static uint32_t current_sector_count = 0;
static uint32_t small_file_start_addr = 0;
static uint8_t error_payload = 0x00;

static uint32_t last_download_tick = 0; /* is_downloading 超时保护 */

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
    // 回滚已分配的大文件扇区（擦除+释放位图），避免空间泄漏
    if (current_allocated_size > 0 && current_file_type == 0x11)
    {
        erase_and_free_large_sectors(current_start_sector, current_sector_count);
    }
    /* 小文件区：不回退指针。线性分配指针保持前进，废弃的中间空间
     * 由 compact_small_files 在下一次删除触发时统一回收。
     * 直接回退会导致下次从同地址开始烧录，若新文件更短则残留旧数据。 */
    lcd_usb_stream_enabled = lcd_stream_was_enabled;
    is_downloading = false;
    expected_file_size = 0;
}

static void abort_download_with_error(uint8_t error_type)
{
    abort_download_common();
    send_error(error_type);
}

/**
 * @brief 等待 W25Q DMA 空闲（写/读状态机跑完）
 * @return true=空闲, false=超时
 */
static bool flash_wait_dma_idle(uint32_t timeout_ms)
{
    uint32_t wait_start = HAL_GetTick();
    while (w25q_dma_is_busy())
    {
        w25q_dma_task();
        usb_controller_task(&g_usb_controller);
        if ((uint32_t)(HAL_GetTick() - wait_start) >= timeout_ms)
        {
            return false;
        }
    }
    return !w25q_dma_is_error();
}

/**
 * @brief 写入W25Q并回读验证，失败自动重试（最多WRITE_VERIFY_RETRY_MAX次）
 *
 * 注意：DMA 路径必须等到本块编程完成再返回。
 * 若仅“启动 DMA 就 ACK”，最后一包可能在 0x14 收尾后仍在写，
 * 文件尾损坏 → 播放循环到末尾花屏（raw/MJPEG 都会出现）。
 */
static bool flash_write_and_verify(uint32_t addr, const uint8_t *data, uint32_t size)
{
    if (size == 0U)
        return true;

    for (uint32_t retry = 0U; retry < WRITE_VERIFY_RETRY_MAX; retry++)
    {
        if (!flash_wait_dma_idle(DMA_WAIT_TIMEOUT_MS))
        {
            return false;
        }

        // --- 拷贝数据到dma_write_buf ---
        memcpy(dma_write_buf, data, size);

        // --- 写入W25Q（优先DMA） ---
        if (w25q_write_data_dma(addr, dma_write_buf, size))
        {
            // 必须等本块写完；大块多页编程可能超过默认 100ms
            uint32_t prog_timeout = DMA_WAIT_TIMEOUT_MS + (size / 16U);
            if (prog_timeout < 500U)
            {
                prog_timeout = 500U;
            }
            if (!flash_wait_dma_idle(prog_timeout))
            {
                continue; // 重试
            }
        }
        else
        {
            // DMA启动失败，回退同步写入
            w25q_write_data(addr, dma_write_buf, size);
        }

        // --- 回读验证 ---
        HAL_Delay(1);
        w25q_fast_read_data(addr, rx_buffer, size);
        if (memcmp(rx_buffer, dma_write_buf, size) == 0)
        {
            return true;
        }
        // --- 不匹配，重试 ---
    }

    return false;
}
#pragma endregion

#pragma region 命令处理核心

static void process_host_command(void)
{
    if (host_payload_len < 2)
        return;
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
            global_fat.small_files[file_index].is_valid = 0;
            storage_fat_save();

            // 检查小文件区剩余空间，低于阈值则触发压缩回收
            uint32_t remaining_space = AREA_SMALL_END_ADDR - global_fat.small_next_addr;
            if (remaining_space < SMALL_FILE_COMPACT_THRESHOLD)
            {
                compact_small_files();
            }
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

        /* 最后一包可能仍在 DMA 编程；必须等写完再登记 FAT，否则文件尾损坏 */
        if (!flash_wait_dma_idle(2000U))
        {
            abort_download_with_error(0x0B);
            return;
        }

        /* 校验实际接收字节数与首包声明大小一致 */
        if (expected_file_size != 0U && current_file_size != expected_file_size)
        {
            abort_download_with_error(0x0B);
            return;
        }

        memcpy(current_filename, (char *)&host_payload[FRAME_HDR_SIZE],
               (actual_data_len < MAX_FILENAME_LEN) ? actual_data_len : MAX_FILENAME_LEN);
        if (current_file_type == 0x11)
        {
            // recycle deleted slot, fallback to append
            int16_t free_slot = -1;
            for (uint16_t i = 0; i < global_fat.large_file_count; i++)
            {
                if (!global_fat.large_files[i].is_valid)
                {
                    free_slot = (int16_t)i;
                    break;
                }
            }
            if (free_slot >= 0)
            {
                large_file_info_t *fi = &global_fat.large_files[free_slot];
                fi->is_valid = 1;
                fi->file_type = current_file_type;
                fi->start_sector = current_start_sector;
                fi->size = current_file_size;
                fi->sector_count = current_sector_count;
                memcpy(fi->filename, current_filename, MAX_FILENAME_LEN);
                storage_fat_save();
            }
            else if (global_fat.large_file_count < MAX_LARGE_FILES)
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
            else
            {
                // 大文件槽位已满，回滚已分配扇区并报错
                erase_and_free_large_sectors(current_start_sector, current_sector_count);
                abort_download_common();
                send_error(0x06);
                return;
            }
        }
        else if (current_file_type == 0x45)
        {
            // recycle deleted slot, fallback to append
            int16_t free_slot = -1;
            for (uint16_t i = 0; i < global_fat.small_file_count; i++)
            {
                if (!global_fat.small_files[i].is_valid)
                {
                    free_slot = (int16_t)i;
                    break;
                }
            }
            if (free_slot >= 0)
            {
                small_file_info_t *fi = &global_fat.small_files[free_slot];
                fi->is_valid = 1;
                fi->file_type = current_file_type;
                fi->start_address = small_file_start_addr;
                fi->size = current_file_size;
                memcpy(fi->filename, current_filename, MAX_FILENAME_LEN);
                storage_fat_save();
            }
            else if (global_fat.small_file_count < MAX_SMALL_FILES)
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
            else
            {
                // 小文件槽位已满，不回退指针（同 abort_download_common 策略）
                abort_download_common();
                send_error(0x07);
                return;
            }
        }
        lcd_usb_stream_enabled = lcd_stream_was_enabled;
        is_downloading = false;
        expected_file_size = 0;
        send_continue();
        return;
    }
    // ==================== 0x11 / 0x45: 下载数据 ====================
    if (host_cmd == 0x11 || host_cmd == 0x45)
    {
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
            expected_file_size = host_total_file_size; /* 仅首包有效 */
            memset(current_filename, 0x00, sizeof(current_filename));

            // 大文件与小文件均一致：根据 host_total_file_size 预分配全部空间
            uint32_t total_size = host_total_file_size;
            if (total_size == 0)
            {
                total_size = W25Q_SECTOR_SIZE;
            }

            if (host_cmd == 0x11)
            {
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
            else if (host_cmd == 0x45)
            {
                uint32_t required_size = total_size;
                uint32_t allocated_addr = allocate_small_space(required_size);
                if (allocated_addr == 0xFFFFFFFF)
                {
                    abort_download_with_error(0x04);
                    return;
                }
                small_file_start_addr = allocated_addr;
                current_write_addr = allocated_addr;
                current_allocated_size = required_size;
                small_last_erased_sector = (allocated_addr + required_size - 1) / W25Q_SECTOR_SIZE;
            }
            last_download_tick = HAL_GetTick();
        }
        else
        {
            // 正在下载中，处理后续数据包
            if (current_file_type != host_cmd)
            {
                send_error(0x05);
                return;
            }
            // 防御：若 host_total_file_size 非零说明是首包，但 is_downloading 已置位，
            // 表明上次下载未正常结束（上位机异常断开等极端情况），拒绝续传
            if (host_total_file_size != 0)
            {
                send_error(0x05);
                return;
            }
            last_download_tick = HAL_GetTick();
            // 无需额外分配或擦除——已在首包预分配+预擦除
        }

        if (actual_data_len > 0)
        {
            if (!flash_write_and_verify(current_write_addr, &host_payload[FRAME_HDR_SIZE], actual_data_len))
            {
                abort_download_with_error(0x0B);
                return;
            }
        }
        current_write_addr += actual_data_len;
        current_file_size += actual_data_len;
        send_continue();
        return;
    }

    // ==================== 0x20: 查询文件列表 (TLV 格式) ====================
    // 总布局: [entry_count(1B)] [slot_count(1B)] [slot_records...] [file_records...]
    // slot 记录: [rLen=10(1B)] [tag=0xFF(1B)] [start_sector(4B LE)] [sector_count(4B LE)]
    // 文件记录: [rLen(1B)] [tag(1B)] [file_index(1B)] [name_len(1B)] [filename(NB)]
    //           [addr/sector(4B LE)] [size(4B LE)]
    //   大文件额外: sector_count(4B LE)
    //   small: rLen = 12 + name_len, tag bit7=0
    //   large: rLen = 16 + name_len, tag bit7=1
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
                    if (idx + 10 > sizeof(file_list_buffer))
                        break;
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
            if (!global_fat.small_files[i].is_valid)
                continue;
            uint8_t namelen = (uint8_t)strlen(global_fat.small_files[i].filename);
            uint8_t record_len = 12 + namelen;
            if (idx + record_len > sizeof(file_list_buffer))
                break;
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
            if (!global_fat.large_files[i].is_valid)
                continue;
            uint8_t namelen = (uint8_t)strlen(global_fat.large_files[i].filename);
            uint8_t record_len = 16 + namelen; // 12 字节固定头 + 4(sector_count) + namelen
            if (idx + record_len > sizeof(file_list_buffer))
                break;
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
            if (sub_cmd == 0x01)
                lcd_usb_stream_enabled = true;
            else if (sub_cmd == 0x00)
                lcd_usb_stream_enabled = false;
        }
        uint8_t resp[1];
        resp[0] = lcd_usb_stream_enabled ? 0x01 : 0x00;
        usb_controller_send(&g_usb_controller, 0x10, resp, sizeof(resp));
        return;
    }

    // ==================== 0x15: 中止下载（上位机取消/超时通知MCU回滚） ====================
    if (host_cmd == 0x15)
    {
        if (is_downloading)
        {
            abort_download_common();
            send_continue();
        }
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
    small_last_erased_sector = (global_fat.small_next_addr > AREA_SMALL_START_ADDR)
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
    /* 下载超时保护：上位机异常断开/取消后，MCU 自动回滚并释放状态 */
    if (is_downloading && HAL_GetTick() - last_download_tick >= 5000U)
    {
        abort_download_common();
    }
}

bool storage_is_downloading(void)
{
    return is_downloading;
}

#pragma endregion
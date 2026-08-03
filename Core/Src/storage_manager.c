/*
 * storage_manager.c
 *
 *  Created on: 2026年4月27日
 *      Author: UnikoZera
 *
 *  ═══════════════════════════════════════════════════════════════════════
 *  Host Command 协议帧格式（上位机 → MCU）
 *  ═══════════════════════════════════════════════════════════════════════
 *    [0]~[1]:   帧头 0xBB 0x44               (2B)
 *    [2]:       命令码                        (1B)
 *    [3]~[6]:   文件总大小 uint32 LE          (4B) 非数据命令填 0，仅首包有效
 *    [7]~[8]:   本包长度 uint16 LE            (2B) = payload_len + 2(CRC16)
 *    [9]~:      Payload 数据
 *    [last-2]~[last-1]: CRC16                 (2B) 没有帧头 + payload（CRC 自身除外）
 *
 *  命令码：
 *    0x11  下载大文件数据                     payload: data + CRC16
 *    0x45  下载小文件数据                     payload: data + CRC16
 *    0x14  结束下载                           payload: 文件名(<=16B) + CRC16
 *    0x19  删除文件                           payload: 文件类型(1B) + 文件索引(1B) + CRC16
 *    0x20  查询文件列表 + 槽位信息            无 payload（仅 CRC16）
 *    0x21  发送大文件区位图                   无 payload
 *    0x10  LCD 流控制                         payload: 子命令(1B) + CRC16
 *    0x15  中止下载（上位机取消通知 MCU 回滚） 无 payload
 *
 *  ═══════════════════════════════════════════════════════════════════════
 *  W25Q 16MB 分区:
 *    [保留区]   Sector 0    ~ 3      (4 * 4KB  = 16KB)    压缩暂存区
 *    [小文件区] Sector 4    ~ 63     (60 * 4KB = 240KB)  线性挤压分配
 *    [大文件区] Sector 64   ~ 4031   (3968*4KB= 15.5MB)  位图管理
 *    [用户区]   Sector 4032 ~ 4095   (64 * 4KB = 256KB)
 *
 *  存储格式（CRC-16 校验块）：
 *    数据按 1022B 分块，每块末尾附 2B CRC-16（小端），物理块恒 1024B；
 *    尾块以 0xFF 补齐（大/小文件统一），4 块 = 4096B = 1 扇区，永不跨扇区。
 *
 *  ═══════════════════════════════════════════════════════════════════════
 *  文件结构索引：
 *    §1 配置与常量         §6 分配器核心          §11 辅助函数
 *    §2 FAT 数据结构       §7 清空               §12 命令处理核心
 *    §3 FAT 持久化         §8 Flash 读写辅助     §13 字节级接收状态机
 *    §4 文件查找与槽位     §9 小文件区压缩       §14 外部接口
 *    §5 大文件区位图       §10 协议接收状态
 *  ═══════════════════════════════════════════════════════════════════════
 */

#include "storage_manager.h"
#include "lcd.h"
#include "lcd_ui.h"

/* ============================================================================
 * §1 配置与常量
 * ============================================================================ */
#pragma region 配置与常量

/* ---- W25Q 分区 ---- */
#define W25Q_TOTAL_SECTORS 4096

/* 保留区（压缩暂存用）：4 扇区 = 16KB，支持单文件 ≤16KB 的整文件暂存回收 */
#define AREA_RESERVED_START_SECTOR 0
#define AREA_RESERVED_SECTORS 4
#define AREA_RESERVED_START_ADDR (AREA_RESERVED_START_SECTOR * W25Q_SECTOR_SIZE)

/* 小文件区：线性挤压分配 */
#define AREA_SMALL_START_SECTOR 4
#define AREA_SMALL_SECTORS 60
#define AREA_SMALL_START_ADDR (AREA_SMALL_START_SECTOR * W25Q_SECTOR_SIZE)
#define AREA_SMALL_END_ADDR ((AREA_SMALL_START_SECTOR + AREA_SMALL_SECTORS) * W25Q_SECTOR_SIZE)

/* 大文件区：位图管理 */
#define AREA_LARGE_START_SECTOR 64
#define AREA_LARGE_SECTORS 3968
#define LARGE_BITMAP_SIZE ((AREA_LARGE_SECTORS + 7) / 8) /* 496 bytes */

/* 用户自定义区 */
#define AREA_USER_START_SECTOR 4032
#define AREA_USER_SECTORS 64

/* ---- FAT（存于 AT24C） ---- */
#define FAT_MAGIC_NUMBER 0x0D000721 /* 分区调整（保留区 16KB）：版本号 +1 强制重新格式化 */
#define FAT_STORAGE_ADDR 0x0000

/* ---- 大文件区位图辅助 ---- */
#define BITMAP_BYTE(b) ((b) >> 3)
#define BITMAP_MASK(b) (1 << ((b) & 7))

/* ---- Host 协议帧 ---- */
#define HOST_FRAME_HEAD_0 0xBBU
#define HOST_FRAME_HEAD_1 0x44U
#define FRAME_HDR_SIZE 9U           /* BB 44 CMD SIZE(4) LEN(2) */
#define HOST_PAYLOAD_DATA_MAX 1024U /* 单包 payload 最大数据字节数 */
#define HOST_CRC_SIZE 2U
#define HOST_FRAME_BUF_SIZE (FRAME_HDR_SIZE + HOST_PAYLOAD_DATA_MAX + HOST_CRC_SIZE)
#define RETRY_SEND_ERROR_CODE 0xE0U
#define CONTINUE_SEND_CODE 0xA1U

/* ---- 命令码 ---- */
#define CMD_DOWNLOAD_LARGE     0x11U  /* 下载大文件数据 */
#define CMD_DOWNLOAD_SMALL     0x45U  /* 下载小文件数据 */
#define CMD_END_DOWNLOAD       0x14U  /* 结束下载 */
#define CMD_DELETE_FILE        0x19U  /* 删除文件 */
#define CMD_QUERY_FILE_LIST    0x20U  /* 查询文件列表 */
#define CMD_SEND_BITMAP        0x21U  /* 发送大文件区位图 */
#define CMD_LCD_STREAM         0x10U  /* LCD 流控制 */
#define CMD_ABORT_DOWNLOAD     0x15U  /* 中止下载 */

/* ---- LCD 流子命令 ---- */
#define LCD_SUBCMD_STOP        0x00U
#define LCD_SUBCMD_START       0x01U

/* ---- 超时 / 重试 ---- */
#define DMA_WAIT_TIMEOUT_MS 100U
#define HOST_STATE_TIMEOUT_MS 500U
#define WRITE_VERIFY_RETRY_MAX 10U
/* 下载无新包超时：与上位机 SEND_TIMEOUT(15s) 匹配，避免验证重试等慢包被误杀 */
#define HOST_DOWNLOAD_TIMEOUT_MS 15000U

#pragma endregion

/* ============================================================================
 * §2 FAT 数据结构
 * ============================================================================ */
#pragma region FAT 数据结构

/* 存储在 AT24C 中的总分配表 (FAT) */
typedef struct
{
    uint32_t magic;
    /* 小文件分配器状态（线性挤压式，不回收碎片，压缩时统一整理） */
    uint32_t small_next_addr;
    uint16_t small_file_count;
    small_file_info_t small_files[MAX_SMALL_FILES];
    /* 大文件位图: 每 bit 一个 sector, 1=已分配, 0=空闲 */
    uint8_t large_sector_bitmap[LARGE_BITMAP_SIZE];
    uint16_t large_file_count;
    large_file_info_t large_files[MAX_LARGE_FILES];
} storage_fat_t;

static storage_fat_t global_fat;

#pragma endregion

/* ============================================================================
 * §3 FAT 持久化
 * ============================================================================ */
#pragma region FAT 持久化

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

#pragma endregion

/* ============================================================================
 * §4 文件查找与槽位管理
 * ============================================================================ */
#pragma region 文件查找与槽位管理

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

/**
 * @brief 获取一个大文件空闲槽位：优先回收已删除槽位，否则追加新槽位。
 * @return 槽位指针；NULL=槽满（FAT 中无空闲且已达上限）。
 * @note 调用方负责填充内容；若返回的是追加槽位，需自行递增 large_file_count。
 */
static large_file_info_t *get_free_large_slot(void)
{
    for (uint16_t i = 0; i < global_fat.large_file_count; i++)
    {
        if (!global_fat.large_files[i].is_valid)
        {
            return &global_fat.large_files[i];
        }
    }
    if (global_fat.large_file_count < MAX_LARGE_FILES)
    {
        return &global_fat.large_files[global_fat.large_file_count];
    }
    return NULL;
}

/** @brief 同上：获取一个小文件空闲槽位。 */
static small_file_info_t *get_free_small_slot(void)
{
    for (uint16_t i = 0; i < global_fat.small_file_count; i++)
    {
        if (!global_fat.small_files[i].is_valid)
        {
            return &global_fat.small_files[i];
        }
    }
    if (global_fat.small_file_count < MAX_SMALL_FILES)
    {
        return &global_fat.small_files[global_fat.small_file_count];
    }
    return NULL;
}

#pragma endregion

/* ============================================================================
 * §5 大文件区位图
 * ============================================================================ */
#pragma region 大文件区位图

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
 * @brief 在大文件区寻找连续的 free_sectors 个空闲扇区。
 * @return 起始 sector 号；失败返回 0xFFFFFFFF。
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
    return 0xFFFFFFFF;
}

/** @brief 标记一段连续 sector 为已分配 */
static void bitmap_mark_block_used(uint32_t start_sector, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        bitmap_set_used(start_sector + i);
    }
}

/**
 * @brief 擦除并释放大文件占用的扇区（删除 / 下载回滚用）。
 *        逐扇区同步擦除（w25q_erase_sector 内部带超时兜底）。
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

/* ============================================================================
 * §6 分配器核心
 * ============================================================================ */
#pragma region 分配器核心

/**
 * @brief 小文件区线性挤压式分配。
 * @return 分配的起始地址；空间不足返回 0xFFFFFFFF。
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
 * @brief 大文件区位图分配：找到连续空闲扇区并标记。
 * @return 起始 sector 号；失败返回 0xFFFFFFFF。
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

/** @brief 计算文件 CRC 块化后的物理大小（尾块 0xFF 补齐，物理块恒 1024B）。 */
static inline uint32_t file_phys_size(uint32_t raw_size)
{
    return w25q_crc_phys_size_padded(raw_size);
}

#pragma endregion

/* ============================================================================
 * §7 清空
 * ============================================================================ */
#pragma region 清空

/** @brief 擦除大文件区所有已分配扇区 + 小文件区全部扇区，重置 FAT。 */
void clear_all_files(void)
{
    for (uint32_t i = AREA_LARGE_START_SECTOR; i < AREA_LARGE_START_SECTOR + AREA_LARGE_SECTORS; i++)
    {
        if (bitmap_test_used(i))
        {
            w25q_erase_sector(i * W25Q_SECTOR_SIZE);
        }
    }
    for (uint16_t i = AREA_SMALL_START_SECTOR; i < AREA_SMALL_START_SECTOR + AREA_SMALL_SECTORS; i++)
    {
        w25q_erase_sector(i * W25Q_SECTOR_SIZE);
    }
    storage_fat_init_default();
    storage_fat_save();
}

/** @brief 手动清空（用户触发，行为同 clear_all_files）。 */
void clear_all_files_manual(void)
{
    clear_all_files();
}

#pragma endregion

/* ============================================================================
 * §8 Flash 读写辅助
 * ============================================================================ */
#pragma region Flash 读写辅助

/* compact / 协议共用缓冲（协议缓冲必须 ≥ 最大帧长） */
static uint8_t dma_write_buf[HOST_PAYLOAD_DATA_MAX];
static uint8_t rx_buffer[HOST_FRAME_BUF_SIZE];
static uint32_t small_last_erased_sector = 0xFFFFFFFF;

static bool flash_write_and_verify(uint32_t addr, const uint8_t *data, uint32_t size);

/**
 * @brief 等待 W25Q DMA 空闲（写/读状态机跑完）。
 * @return true=空闲且无错误；false=超时或错误。
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
 * @brief 写入 W25Q 并回读验证，失败自动重试（最多 WRITE_VERIFY_RETRY_MAX 次）。
 *
 * ⚠ DMA 路径必须等到本块编程完成再返回：
 *   若仅"启动 DMA 就立即 ACK"，最后一包可能在收到 0x14（结束下载）后仍在写，
 *   文件尾损坏 → 播放时循环到末尾花屏（RAW / MJPEG 都会出现）。
 *
 * @param addr W25Q 目标地址
 * @param data 待写入数据
 * @param size 数据长度
 * @return true=写入并验证成功；false=失败
 */
static bool flash_write_and_verify(uint32_t addr, const uint8_t *data, uint32_t size)
{
    if (size == 0U)
    {
        return true;
    }

    for (uint32_t retry = 0U; retry < WRITE_VERIFY_RETRY_MAX; retry++)
    {
        if (!flash_wait_dma_idle(DMA_WAIT_TIMEOUT_MS))
        {
            return false;
        }

        /* 拷贝数据到 DMA 写缓冲 */
        memcpy(dma_write_buf, data, size);

        /* 尝试 DMA 写入；DMA 忙时自动排队等待 */
        if (w25q_write_data_dma(addr, dma_write_buf, size))
        {
            /* 大块多页编程可能超过默认 100ms，根据写入量估算超时 */
            uint32_t prog_timeout = DMA_WAIT_TIMEOUT_MS + (size / 16U);
            if (prog_timeout < 500U)
            {
                prog_timeout = 500U;
            }
            if (!flash_wait_dma_idle(prog_timeout))
            {
                continue; /* 重试 */
            }
        }
        else
        {
            /* DMA 启动失败，回退同步写入 */
            w25q_write_data(addr, dma_write_buf, size);
        }

        /* --- 回读验证（分块 ≤1024B：w25q_fast_read_data 单次上限） --- */
        {
            bool match = true;
            uint32_t v_off = 0;
            HAL_Delay(1);
            while (v_off < size)
            {
                uint32_t v_chunk = size - v_off;
                if (v_chunk > 1024U)
                {
                    v_chunk = 1024U;
                }
                w25q_fast_read_data(addr + v_off, rx_buffer, (uint16_t)v_chunk);
                if (memcmp(rx_buffer, dma_write_buf + v_off, v_chunk) != 0)
                {
                    match = false;
                    break;
                }
                v_off += v_chunk;
            }
            if (match)
            {
                return true;
            }
        }
        /* --- 不匹配，重试 --- */
    }

    return false;
}

/** @brief 从 flash 读到 rx_buffer 再写到另一地址的 chunked copy（带回读验证）。 */
static bool flash_chunked_copy(uint32_t src, uint32_t dst, uint32_t size)
{
    while (size > 0)
    {
        /* 单次 ≤1024B：w25q_fast_read_data 上限（READ_BUF_SIZE=1030） */
        uint32_t chunk = (size > 1024U) ? 1024U : size;
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


#pragma endregion

/* ============================================================================
 * §9 小文件区压缩
 * ============================================================================ */
#pragma region 小文件区压缩

/**
 * @brief 小文件区垃圾回收压缩（分块式）。
 *
 * 原理：按地址顺序逐文件处理。对于每个文件，检查它与哪些有效文件共享扇区，
 * 将这些文件组成一个"批"一起处理：
 *   ① 将整批文件拷贝到保留区(Sector 0~1, 8KB)暂存
 *   ② 擦除这批文件占用的所有扇区
 *   ③ 从保留区读回，按顺序紧凑写入小文件区起始位置
 *   ④ 更新所有被处理文件的 start_address
 *
 * 若单个批大小超过保留区容量(8KB)则返回 false。
 * 压缩条件：小文件区剩余空间 < SMALL_FILE_COMPACT_THRESHOLD。
 *
 * @return true=压缩成功；false=失败
 */
bool compact_small_files(void)
{
    /* ---- Step 1: 收集所有有效文件，按地址排序 ---- */
    uint16_t valid_list[MAX_SMALL_FILES];
    uint16_t valid_count = 0;
    for (uint16_t i = 0; i < global_fat.small_file_count; i++)
    {
        if (global_fat.small_files[i].is_valid)
        {
            valid_list[valid_count++] = i;
        }
    }

    /* 没有有效文件 → 直接擦除小文件区并重置分配器 */
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

    /* 按 start_address 升序排序（简单选择排序，n ≤ MAX_SMALL_FILES） */
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

    /* ---- Step 2: 分批处理 ---- */
    const uint32_t reserved_capacity = AREA_RESERVED_SECTORS * W25Q_SECTOR_SIZE; /* 8192 */
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

        /* 2a0. 单文件物理大小超保留区容量：无法搬移（保留区装不下）。
         *       compact_dest 跳过其空间（含与它共享扇区的文件，避免擦除误伤），
         *       这些文件留在原地不移动；其余文件照常分批紧凑，压缩不中断。 */
        {
            small_file_info_t *vi_file = &global_fat.small_files[valid_list[vi]];
            uint32_t vi_phys = file_phys_size(vi_file->size);
            if (vi_phys > reserved_capacity)
            {
                uint32_t skip_end = vi_file->start_address + vi_phys;
                for (uint16_t j = vi; j < valid_count; j++)
                {
                    small_file_info_t *fj = &global_fat.small_files[valid_list[j]];
                    uint32_t fj_end = fj->start_address + file_phys_size(fj->size);
                    if (fj->start_address < skip_end && fj_end > vi_file->start_address)
                    {
                        if (fj_end > skip_end)
                        {
                            skip_end = fj_end;
                        }
                        moved[valid_list[j]] = true; /* 留在原地 */
                    }
                }
                if (compact_dest < skip_end)
                {
                    compact_dest = skip_end;
                }
                vi++;
                continue;
            }
        }

        /* 2a. 拆批构建：按地址顺序（valid_list 已排序）贪心取文件，
         *      保证批的扇区范围内无未入批文件（擦除安全），累计 ≤ 保留区容量。
         *      与批扇区范围重叠的文件必须入批；不重叠则结束本批（剩余留待下一批）。 */
        uint16_t batch_indices[MAX_SMALL_FILES];
        uint16_t batch_count = 0;
        uint32_t batch_size = 0;
        bool in_batch[MAX_SMALL_FILES];
        memset(in_batch, 0, sizeof(in_batch));

        uint32_t batch_start_sector = 0;
        uint32_t batch_end_sector = 0;
        bool batch_has_range = false;

        for (uint16_t j = vi; j < valid_count; j++)
        {
            uint16_t idx = valid_list[j];
            if (moved[idx] || in_batch[idx])
            {
                continue;
            }

            small_file_info_t *fj = &global_fat.small_files[idx];
            uint32_t fj_phys = file_phys_size(fj->size);
            uint32_t js = fj->start_address / W25Q_SECTOR_SIZE;
            uint32_t je = (fj->start_address + fj_phys - 1) / W25Q_SECTOR_SIZE;

            bool overlaps = batch_has_range &&
                            (js <= batch_end_sector && je >= batch_start_sector);

            if (batch_count == 0 || overlaps)
            {
                /* 该文件与批共享扇区：必须入批（否则擦除会误伤其数据）。
                 * 若入批后超保留区容量 → 本批无法成立（保留区不足），压缩失败 */
                if (batch_size + fj_phys > reserved_capacity)
                {
                    return false;
                }
                batch_indices[batch_count++] = idx;
                in_batch[idx] = true;
                batch_size += fj_phys;

                if (!batch_has_range)
                {
                    batch_start_sector = js;
                    batch_end_sector = je;
                    batch_has_range = true;
                }
                else
                {
                    if (js < batch_start_sector)
                    {
                        batch_start_sector = js;
                    }
                    if (je > batch_end_sector)
                    {
                        batch_end_sector = je;
                    }
                }
            }
            else
            {
                /* 与批扇区范围不重叠：本批结束，剩余文件留待下一批 */
                break;
            }
        }

        /* 2c. 擦除保留区 → 整批文件拷贝到保留区暂存 */
        {
            for (uint32_t s = AREA_RESERVED_START_SECTOR; s < AREA_RESERVED_START_SECTOR + AREA_RESERVED_SECTORS; s++)
            {
                w25q_erase_sector(s * W25Q_SECTOR_SIZE);
            }

            uint32_t reserved_off = 0;
            for (uint16_t b = 0; b < batch_count; b++)
            {
                small_file_info_t *fb = &global_fat.small_files[batch_indices[b]];
                uint32_t fb_phys = file_phys_size(fb->size);
                if (!flash_chunked_copy(fb->start_address,
                                        AREA_RESERVED_START_ADDR + reserved_off,
                                        fb_phys))
                {
                    return false;
                }
                reserved_off += fb_phys;
            }
        }

        /* 2d. 擦除这批文件占用的所有扇区 */
        for (uint32_t s = batch_start_sector; s <= batch_end_sector; s++)
        {
            w25q_erase_sector(s * W25Q_SECTOR_SIZE);
        }

        /* 2e. 从保留区读回，紧凑写入小文件区 */
        {
            uint32_t reserved_off = 0;
            for (uint16_t b = 0; b < batch_count; b++)
            {
                small_file_info_t *fb = &global_fat.small_files[batch_indices[b]];
                uint32_t fb_phys = file_phys_size(fb->size);
                if (!flash_chunked_copy(AREA_RESERVED_START_ADDR + reserved_off,
                                        compact_dest,
                                        fb_phys))
                {
                    return false;
                }

                fb->start_address = compact_dest;
                compact_dest += fb_phys;
                reserved_off += fb_phys;
                moved[batch_indices[b]] = true;
            }

            /* 每批处理完立即持久化 FAT，防止中途失败丢失已处理批次 */
            storage_fat_save();
        }

        while (vi < valid_count && moved[valid_list[vi]])
        {
            vi++;
        }
    }

    /* ---- Step 3: 末尾清理 — 擦除原数据区末尾剩余的扇区 ---- */
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

    /* ---- Step 4: 更新分配器状态并保存 FAT ---- */
    global_fat.small_next_addr = compact_dest;
    small_last_erased_sector = (compact_dest > 0)
                                   ? ((compact_dest - 1) / W25Q_SECTOR_SIZE)
                                   : (AREA_SMALL_START_SECTOR - 1);
    storage_fat_save();
    return true;
}

#pragma endregion

/* ============================================================================
 * §10 协议接收状态
 * ============================================================================ */
#pragma region 协议接收状态

/**
 * @brief Host 协议接收状态机。
 * 解析流程：HEAD0 → HEAD1 → CMD → TOTAL_SIZE×4 → LEN_L → LEN_H → PAYLOAD → 处理 → HEAD0
 */
typedef enum
{
    STATE_WAIT_HEAD0,          /* 等待帧头第 1 字节 0xBB */
    STATE_WAIT_HEAD1,          /* 等待帧头第 2 字节 0x44 */
    STATE_WAIT_CMD,            /* 等待命令码 */
    STATE_WAIT_TOTAL_SIZE_0,   /* 文件总大小 byte 0 (LSB) */
    STATE_WAIT_TOTAL_SIZE_1,   /* 文件总大小 byte 1 */
    STATE_WAIT_TOTAL_SIZE_2,   /* 文件总大小 byte 2 */
    STATE_WAIT_TOTAL_SIZE_3,   /* 文件总大小 byte 3 (MSB) */
    STATE_WAIT_LEN_L,          /* 本包长度低字节 */
    STATE_WAIT_LEN_H,          /* 本包长度高字节 */
    STATE_WAIT_PAYLOAD         /* 等待 payload 数据 + CRC16 */
} host_cmd_state_t;

/* ---- 协议接收状态 ---- */
static host_cmd_state_t host_state = STATE_WAIT_HEAD0;
static uint32_t host_state_tick = 0;
static uint8_t host_cmd;
static uint32_t host_total_file_size = 0; /* 首包声明的文件完整大小 */
static uint16_t host_payload_len;         /* 本包数据段长度（含 CRC16） */
static uint16_t host_payload_idx;
/* host_payload 布局: [0..8]=帧头 9 字节, [9..]=payload 数据（含 CRC16） */
static uint8_t host_payload[HOST_FRAME_BUF_SIZE] = {0};

/* ---- 下载会话状态 ---- */
static bool is_downloading = false;
static bool lcd_stream_was_enabled = false;
static uint32_t current_write_addr = 0;     /* 当前写入地址（W25Q 绝对地址） */
static uint32_t current_file_size = 0;      /* 已接收原始字节数 */
static uint32_t current_allocated_size = 0; /* 预分配空间大小 */
static uint32_t expected_file_size = 0;     /* 首包声明的完整文件大小 */
static uint8_t current_file_type = 0;       /* CMD_DOWNLOAD_LARGE / CMD_DOWNLOAD_SMALL */
static char current_filename[MAX_FILENAME_LEN] = {0};
static uint32_t current_start_sector = 0;   /* 大文件起始扇区 */
static uint32_t current_sector_count = 0;   /* 大文件占用扇区数 */
static uint32_t small_file_start_addr = 0;  /* 小文件起始地址 */
static uint32_t last_download_tick = 0;     /* 下载超时保护 */
static uint8_t error_payload = 0x00;

#pragma endregion

/* ============================================================================
 * §11 辅助函数
 * ============================================================================ */
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

extern volatile bool lcd_usb_stream_enabled; /* from lcd.c */

static void send_error(uint8_t error_type)
{
    error_payload = error_type;
    usb_controller_send(&g_usb_controller, RETRY_SEND_ERROR_CODE, &error_payload, 1);
}

static void send_continue(void)
{
    /* 确认帧必须送达（删除完成/下载继续）：发送被瞬时拒绝时短暂重试。
     * DROPPED_PREVIOUS 表示已覆盖旧 pending 并接受本帧，同样视为成功。 */
    for (uint8_t i = 0U; i < 5U; i++)
    {
        usb_send_status_t st = usb_controller_send(&g_usb_controller, CONTINUE_SEND_CODE, NULL, 0U);
        if ((st == USB_SEND_OK) || (st == USB_SEND_QUEUED) || (st == USB_SEND_DROPPED_PREVIOUS))
        {
            return;
        }
        HAL_Delay(2);
    }
}

/**
 * @brief 中止下载：回滚已分配的大文件扇区并释放状态。
 * 小文件区不回退指针（线性分配指针保持前进，废弃空间由 compact 统一回收）。
 */
static void abort_download_common(void)
{
    if (current_allocated_size > 0 && current_file_type == CMD_DOWNLOAD_LARGE)
    {
        erase_and_free_large_sectors(current_start_sector, current_sector_count);
    }
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
 * @brief 从 W25Q 文件容器头读取图像尺寸（MJPEG / RAW5 14B 标准头，[6..9] 宽高 LE）。
 * 无有效容器头时输出 0xFFFF。
 */
static void read_file_dimensions(uint32_t data_addr, uint16_t *p_width, uint16_t *p_height)
{
    uint8_t hdr[14];
    *p_width = 0xFFFF;
    *p_height = 0xFFFF;
    memset(hdr, 0xFF, sizeof(hdr)); /* 读失败保持 0xFF（magic 不匹配 → 0xFFFF） */

    /* 存储为 CRC 块化格式，容器头位于文件起点；用无校验同步读（仅取前 14B 头部） */
    w25q_fast_read_data(data_addr, hdr, 14);

    bool is_mjpeg = (hdr[0] == 'M' && hdr[1] == 'J' && hdr[2] == 'P' && hdr[3] == 'G');
    bool is_raw5  = (hdr[0] == 'R' && hdr[1] == 'A' && hdr[2] == 'W' && hdr[3] == '5');

    if (is_mjpeg || is_raw5)
    {
        *p_width  = (uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8);
        *p_height = (uint16_t)hdr[8] | ((uint16_t)hdr[9] << 8);
    }
}

#pragma endregion

/* ============================================================================
 * §12 命令处理核心
 * ============================================================================ */
#pragma region 命令处理核心

static void process_host_command(void)
{
    if (host_payload_len < 2)
    {
        return;
    }
    /* CRC16 只覆盖 payload 数据（含末尾 2B CRC）：完整 1022B 数据包的 CRC
     * 即存储块 CRC（1022 数据 + 2 CRC），MCU 验证通过后可直接整包烧录，
     * 无需重新计算存储块 CRC。帧头完整性由 magic + 长度范围检查保证。 */
    if (!crc16_usb_packing(&host_payload[FRAME_HDR_SIZE], host_payload_len, true))
    {
        send_error(0x01);
        return;
    }

    uint16_t actual_data_len = host_payload_len - 2;

    switch (host_cmd)
    {
    /* ------------------------------------------------------------------
     *  CMD_DELETE_FILE (0x19): 删除文件
     * ------------------------------------------------------------------ */
    case CMD_DELETE_FILE:
    {
        if (host_payload_len < 4)
        {
            return;
        }
        uint8_t file_type_to_delete = host_payload[FRAME_HDR_SIZE];
        uint8_t file_index = host_payload[FRAME_HDR_SIZE + 1];

        if (file_type_to_delete == CMD_DOWNLOAD_LARGE)
        {
            if (file_index >= global_fat.large_file_count || !global_fat.large_files[file_index].is_valid)
            {
                send_error(0x08);
                return;
            }
            /* 擦除 W25Q 扇区并释放位图 */
            large_file_info_t *fi = &global_fat.large_files[file_index];
            erase_and_free_large_sectors(fi->start_sector, fi->sector_count);
            fi->is_valid = 0;
            storage_fat_save();
        }
        else if (file_type_to_delete == CMD_DOWNLOAD_SMALL)
        {
            if (file_index >= global_fat.small_file_count || !global_fat.small_files[file_index].is_valid)
            {
                send_error(0x08);
                return;
            }
            global_fat.small_files[file_index].is_valid = 0;
            storage_fat_save();

            /* 小文件区剩余空间低于阈值 → 触发压缩回收 */
            uint32_t remaining_space = AREA_SMALL_END_ADDR - global_fat.small_next_addr;
            if (remaining_space < SMALL_FILE_COMPACT_THRESHOLD)
            {
                compact_small_files();
            }
        }
        else
        {
            send_error(0x02);
            break;
        }
        send_continue(); /* 删除完成确认（上位机等待 0xA1 后刷新列表） */
        break;
    }

    /* ------------------------------------------------------------------
     *  CMD_END_DOWNLOAD (0x14): 结束下载
     * ------------------------------------------------------------------ */
    case CMD_END_DOWNLOAD:
    {
        if (!is_downloading)
        {
            return;
        }

        /* 最后一包可能仍在 DMA 编程；必须等写完再登记 FAT，否则文件尾损坏 */
        if (!flash_wait_dma_idle(2000U))
        {
            abort_download_with_error(0x0B);
            return;
        }

        /* 校验实际接收字节数与首包声明大小一致（尾块已在数据包阶段补齐写入） */
        if (expected_file_size != 0U && current_file_size != expected_file_size)
        {
            abort_download_with_error(0x0B);
            return;
        }

        /* 文件名：截断到 MAX_FILENAME_LEN-1 并强制 '\0' 终止——
         * 原实现拷满 16 字节时若无终止符，列表 strlen 会越界读 FAT 垃圾导致名字乱码 */
        {
            uint16_t name_len = (actual_data_len < MAX_FILENAME_LEN - 1)
                                    ? actual_data_len
                                    : (uint16_t)(MAX_FILENAME_LEN - 1);
            memcpy(current_filename, &host_payload[FRAME_HDR_SIZE], name_len);
            current_filename[name_len] = '\0';
        }

        /* 登记文件到 FAT：回收已删槽位，否则追加 */
        if (current_file_type == CMD_DOWNLOAD_LARGE)
        {
            large_file_info_t *fi = get_free_large_slot();
            if (fi == NULL)
            {
                /* 槽位已满：回滚已分配扇区并报错 */
                erase_and_free_large_sectors(current_start_sector, current_sector_count);
                abort_download_common();
                send_error(0x06);
                return;
            }
            if (fi == &global_fat.large_files[global_fat.large_file_count])
            {
                global_fat.large_file_count++; /* 追加槽位 */
            }
            fi->is_valid = 1;
            fi->file_type = current_file_type;
            fi->start_sector = current_start_sector;
            fi->size = current_file_size;
            fi->sector_count = current_sector_count;
            memcpy(fi->filename, current_filename, MAX_FILENAME_LEN);
            storage_fat_save();
        }
        else if (current_file_type == CMD_DOWNLOAD_SMALL)
        {
            small_file_info_t *fi = get_free_small_slot();
            if (fi == NULL)
            {
                /* 槽位已满：不回退指针（同 abort 策略），报错 */
                abort_download_common();
                send_error(0x07);
                return;
            }
            if (fi == &global_fat.small_files[global_fat.small_file_count])
            {
                global_fat.small_file_count++; /* 追加槽位 */
            }
            fi->is_valid = 1;
            fi->file_type = current_file_type;
            fi->start_address = small_file_start_addr;
            fi->size = current_file_size;
            memcpy(fi->filename, current_filename, MAX_FILENAME_LEN);
            storage_fat_save();
        }

        lcd_usb_stream_enabled = lcd_stream_was_enabled;
        is_downloading = false;
        expected_file_size = 0;
        /* 文件内容已变更：重置视频播放器，避免旧播放进度读新数据导致错位/黑屏重播 */
        lcd_ui_init();
        send_continue();
        return;
    }

    /* ------------------------------------------------------------------
     *  CMD_DOWNLOAD_LARGE / CMD_DOWNLOAD_SMALL (0x11 / 0x45): 下载数据
     * ------------------------------------------------------------------ */
    case CMD_DOWNLOAD_LARGE:
    case CMD_DOWNLOAD_SMALL:
    {
        if (!is_downloading) /* 新下载请求（首包） */
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

            /* 大/小文件统一：根据声明大小预分配全部空间 */
            uint32_t total_size = host_total_file_size;
            if (total_size == 0)
            {
                total_size = W25Q_SECTOR_SIZE;
            }

            if (host_cmd == CMD_DOWNLOAD_LARGE)
            {
                /* CRC 块化存储：物理大小 = ceil(raw/1022)*1024，4 块 = 4096B = 1 扇区恒对齐 */
                uint32_t required_sectors = (file_phys_size(total_size) + W25Q_SECTOR_SIZE - 1) / W25Q_SECTOR_SIZE;
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
            else /* CMD_DOWNLOAD_SMALL */
            {
                uint32_t required_size = file_phys_size(total_size);
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
        else /* 后续数据包 */
        {
            if (current_file_type != host_cmd)
            {
                send_error(0x05);
                return;
            }
            /* 防御：host_total_file_size 非零说明是首包但 is_downloading 已置位，
             * 表明上次下载未正常结束（上位机异常断开等极端情况），拒绝续传 */
            if (host_total_file_size != 0)
            {
                send_error(0x05);
                return;
            }
            last_download_tick = HAL_GetTick();
            /* 空间已在首包预分配，无需重复分配/擦除 */
        }

        /* 上位机按 CRC 块组包：完整包 = 1022B 数据 + 2B CRC（即存储块 CRC）；
         * 尾包（<1022B）= 实际数据 + CRC(实际数据)，由 MCU 补齐 0xFF 并重算 CRC。 */
        if (actual_data_len > 0)
        {
            if (actual_data_len == W25Q_CRC_BLOCK_DATA_SIZE)
            {
                /* 完整块：上位机 CRC 即存储块 CRC，验证通过后整包直接烧录 */
                if (!flash_write_and_verify(current_write_addr, &host_payload[FRAME_HDR_SIZE],
                                            W25Q_CRC_BLOCK_PHYS_SIZE))
                {
                    abort_download_with_error(0x0B);
                    return;
                }
                current_write_addr += W25Q_CRC_BLOCK_PHYS_SIZE;
            }
            else
            {
                /* 尾块（<1022B）：上位机 CRC 只覆盖实际数据；补齐 0xFF 后需重算存储块 CRC */
                memcpy(dma_write_buf, &host_payload[FRAME_HDR_SIZE], actual_data_len);
                memset(&dma_write_buf[actual_data_len], 0xFF,
                       W25Q_CRC_BLOCK_DATA_SIZE - actual_data_len);
                uint16_t crc = crc16_usb_calc(dma_write_buf, W25Q_CRC_BLOCK_DATA_SIZE);
                dma_write_buf[W25Q_CRC_BLOCK_DATA_SIZE]      = (uint8_t)(crc & 0xFFU);
                dma_write_buf[W25Q_CRC_BLOCK_DATA_SIZE + 1U] = (uint8_t)(crc >> 8);
                if (!flash_write_and_verify(current_write_addr, dma_write_buf,
                                            W25Q_CRC_BLOCK_PHYS_SIZE))
                {
                    abort_download_with_error(0x0B);
                    return;
                }
                current_write_addr += W25Q_CRC_BLOCK_PHYS_SIZE;
            }
        }
        current_file_size += actual_data_len;
        send_continue();
        return;
    }

    /* ------------------------------------------------------------------
     *  CMD_QUERY_FILE_LIST (0x20): 查询文件列表 (TLV 格式)
     *
     * 总布局: [entry_count(1B)] [slot_count(1B)] [slot_records...] [file_records...]
     *   slot 记录: [rLen=10(1B)] [tag=0xFF(1B)] [start_sector(4B LE)] [sector_count(4B LE)]
     *   文件记录: [rLen(1B)] [tag(1B)] [file_index(1B)] [name_len(1B)] [filename(NB)]
     *             [addr/sector(4B LE)] [size(4B LE)]
     *   大文件额外: sector_count(4B LE)
     *   通用额外: [width(2B LE)] [height(2B LE)] — 未知尺寸填 0xFFFF
     *   small: rLen = 16 + name_len, tag bit7=0
     *   large: rLen = 20 + name_len, tag bit7=1
     * ------------------------------------------------------------------ */
    case CMD_QUERY_FILE_LIST:
    {
        static uint8_t file_list_buffer[2560];
        uint16_t idx = 2;
        uint8_t entry_count = 0;
        uint8_t slot_count = 0;

        /* 大文件区已分配块（slot 记录） */
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
                    if (idx + 10 > sizeof(file_list_buffer))
                    {
                        break;
                    }
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

        /* 小文件记录 */
        for (uint16_t i = 0; i < global_fat.small_file_count; i++)
        {
            if (!global_fat.small_files[i].is_valid)
            {
                continue;
            }
            uint8_t namelen = (uint8_t)strlen(global_fat.small_files[i].filename);
            uint8_t record_len = 16 + namelen; /* 12(固定头) + 4(width/height) + namelen */
            if (idx + record_len > sizeof(file_list_buffer))
            {
                break;
            }

            uint16_t img_w, img_h;
            read_file_dimensions(global_fat.small_files[i].start_address, &img_w, &img_h);

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
            memcpy(&file_list_buffer[idx], &img_w, 2);
            idx += 2;
            memcpy(&file_list_buffer[idx], &img_h, 2);
            idx += 2;
            entry_count++;
        }

        /* 大文件记录 */
        for (uint16_t i = 0; i < global_fat.large_file_count; i++)
        {
            if (!global_fat.large_files[i].is_valid)
            {
                continue;
            }
            uint8_t namelen = (uint8_t)strlen(global_fat.large_files[i].filename);
            uint8_t record_len = 20 + namelen; /* 12(固定头) + 4(sector_count) + 4(width/height) + namelen */
            if (idx + record_len > sizeof(file_list_buffer))
            {
                break;
            }

            uint16_t img_w, img_h;
            read_file_dimensions(global_fat.large_files[i].start_sector * W25Q_SECTOR_SIZE, &img_w, &img_h);

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
            memcpy(&file_list_buffer[idx], &global_fat.large_files[i].sector_count, 4);
            idx += 4;
            memcpy(&file_list_buffer[idx], &img_w, 2);
            idx += 2;
            memcpy(&file_list_buffer[idx], &img_h, 2);
            idx += 2;
            entry_count++;
        }

        file_list_buffer[0] = entry_count;
        file_list_buffer[1] = slot_count;
        usb_controller_send(&g_usb_controller, CMD_QUERY_FILE_LIST, file_list_buffer, idx);
        return;
    }

    /* ------------------------------------------------------------------
     *  CMD_SEND_BITMAP (0x21): 发送大文件区位图
     * ------------------------------------------------------------------ */
    case CMD_SEND_BITMAP:
    {
        usb_controller_send(&g_usb_controller, CMD_SEND_BITMAP,
                            global_fat.large_sector_bitmap, LARGE_BITMAP_SIZE);
        return;
    }

    /* ------------------------------------------------------------------
     *  CMD_LCD_STREAM (0x10): LCD 流控制
     * ------------------------------------------------------------------ */
    case CMD_LCD_STREAM:
    {
        if (host_payload_len >= 3)
        {
            uint8_t sub_cmd = host_payload[FRAME_HDR_SIZE];
            if (sub_cmd == LCD_SUBCMD_START)
            {
                lcd_usb_stream_enabled = true;
            }
            else if (sub_cmd == LCD_SUBCMD_STOP)
            {
                lcd_usb_stream_enabled = false;
            }
        }
        uint8_t resp[1];
        resp[0] = lcd_usb_stream_enabled ? 0x01 : 0x00;
        usb_controller_send(&g_usb_controller, CMD_LCD_STREAM, resp, sizeof(resp));
        return;
    }

    /* ------------------------------------------------------------------
     *  CMD_ABORT_DOWNLOAD (0x15): 中止下载
     * ------------------------------------------------------------------ */
    case CMD_ABORT_DOWNLOAD:
    {
        if (is_downloading)
        {
            abort_download_common();
            send_continue();
        }
        return;
    }

    /* ------------------------------------------------------------------
     *  未知指令
     * ------------------------------------------------------------------ */
    default:
        send_error(0x09);
        break;
    }
}

#pragma endregion

/* ============================================================================
 * §13 字节级接收状态机
 * ============================================================================ */
#pragma region 字节级接收状态机

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

/* ============================================================================
 * §14 外部接口
 * ============================================================================ */
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
            /* 批量拷贝 payload（避免逐字节走状态机） */
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

    /* 帧头解析超时：半帧残留自动复位 */
    if (host_state != STATE_WAIT_HEAD0 && HAL_GetTick() - host_state_tick >= HOST_STATE_TIMEOUT_MS)
    {
        reset_host_state();
    }

    /* 下载超时保护：上位机异常断开/取消后自动回滚。
     * 阈值与上位机 SEND_TIMEOUT(15s) 匹配，避免验证重试等慢包被误杀；
     * 中止时回错误帧，上位机立即感知并停止，不会一直等 0xA1 等到超时。 */
    if (is_downloading && HAL_GetTick() - last_download_tick >= HOST_DOWNLOAD_TIMEOUT_MS)
    {
        abort_download_common();
        send_error(0x0B);
    }
}

bool storage_is_downloading(void)
{
    return is_downloading;
}

#pragma endregion

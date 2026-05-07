/*
 * storage_manager.c
 *
 *  Created on: 2026年4月27日
 *      Author: UnikoZera
 *
 *      可以产生发给单片机的测试代码：BB 44 11 04 00 31 32 89 C6
 *                              BB 44 20 02 00 2B BE
 *    这里说明一下usb主机与存储管理器之间的协议设计思路：
 * 1. 主机每次发送一个完整的命令帧，包含：帧头(2字节) + 命令(1字节) + 长度(2字节) + 数据(payload + CRC16(2字节))
 * 2. 数据全是uint8_t类型。别的其他均为小端格式。长度字段的值是数据部分的字节数加上CRC16的2字节。
 * 3. 关于参数：帧头主机端默认为 0xBB 0x44，长度的意思是 数据长度 + CRC16长度(2字节)，数据部分根据命令不同有不同的格式和意义， // !CRC16是帧头到crc16数据前的所有字节的校验和。
 * 4. 关于结束命令：一般来讲,大文件或者小文件的命令会分多次发送,主机端在发送完所有数据后会发送一个结束命令(0x14)，告知单片机当前文件传输完成，单片机收到结束命令后会进行必要的收尾处理（如注册文件信息到FAT表等）。如果在传输过程中发生错误，单片机会发送一个错误响应(0xE0)，主机端收到错误响应后可以选择重试或者放弃当前文件的传输。
 *
 *  协议命令列表：
 *    0x11 - 开始下载大文件(图片等)，payload: 数据 + CRC16
 *    0x45 - 开始下载小文件(文本等)，payload: 数据 + CRC16
 *    0x14 - 结束下载，payload: 文件的名字(不超过16B) +  CRC16
 *    0x19 - 删除文件，payload: 文件类型(1B) + 文件索引(1B) + CRC16
 *    0x20 - 查询文件列表，无payload(仅CRC16校验)
 */

#include "storage_manager.h"

#pragma region 文件系统与分配表实现

// ======================== 文件系统/分配表定义 ========================
#define FAT_MAGIC_NUMBER 0x0D000721
#define W25Q_SECTOR_SIZE 4096
#define W25Q_TOTAL_SECTORS 4096
// --- 分区映射表 ---
// [区段 1] 保留区: Sector 0 ~ 1 (8KB) 不做任何操作，纯空置
#define AREA_RESERVED_START_SECTOR 0
#define AREA_RESERVED_SECTORS 2
// [区段 2] 小文件区:紧凑字节级排列 (不推荐频繁删除的文件，删除后会有碎片但不影响使用)
// Sector 2 ~ 63 (共 62 个扇区 / 248KB)
#define AREA_SMALL_START_SECTOR 2
#define AREA_SMALL_SECTORS 62
#define AREA_SMALL_START_ADDR (AREA_SMALL_START_SECTOR * W25Q_SECTOR_SIZE)
#define AREA_SMALL_END_ADDR ((AREA_SMALL_START_SECTOR + AREA_SMALL_SECTORS) * W25Q_SECTOR_SIZE)
// [区段 3] 大文件区:按大块扇区对齐位图分配 (推荐频繁删除的文件，删除后通过位图清空对应扇区即可，不会有碎片问题)
// Sector 64 ~ 4031 (共 3968 个扇区 / 15.5MB)
#define AREA_LARGE_START_SECTOR 64
#define AREA_LARGE_SECTORS 3968
// [区段 4] 用户自定义区
// Sector 4032 ~ 4095 (共 64 个扇区 / 256KB)
#define AREA_USER_START_SECTOR 4032
#define AREA_USER_SECTORS 64
// 存放在 AT24C 中的总分配表 (FAT)
typedef struct
{
    uint32_t magic;
    // 小文件分配器状态 (线性挤压式)
    uint32_t small_next_addr; // 小文件区下一个可分配的地址，向上挤压分配，不回收碎片。初始值为 AREA_SMALL_START_ADDR
    uint16_t small_file_count;
    small_file_info_t small_files[MAX_SMALL_FILES];
    // 大文件分配器状态 (线性挤压式，以sector为单位)
    uint32_t large_next_sector; // 大文件区下一个可分配的起始扇区，向上挤压分配。初始值为 AREA_LARGE_START_SECTOR
    uint16_t large_file_count;
    large_file_info_t large_files[MAX_LARGE_FILES];
} storage_fat_t;

static storage_fat_t global_fat; // 全局分配表，启动时从AT24C加载，运行时保持更新，并定期或在关键操作后写回AT24C以持久化。

#define FAT_STORAGE_ADDR 0x0000  // FAT在AT24C中的存储地址

#pragma endregion

#pragma region FAT持久化与文件查找功能

static void storage_fat_init_default(void)
{
    memset(&global_fat, 0, sizeof(storage_fat_t));
    global_fat.magic = FAT_MAGIC_NUMBER;
    global_fat.small_next_addr = AREA_SMALL_START_ADDR;
    global_fat.small_file_count = 0;
    global_fat.large_next_sector = AREA_LARGE_START_SECTOR;
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

int16_t find_small_file_by_name(const char *name) // 返回小文件索引，找不到返回-1
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

int16_t find_large_file_by_name(const char *name) // 返回大文件索引，找不到返回-1
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

bool get_small_file_info(uint8_t file_id, small_file_info_t *info) // 返回小文件信息，找不到返回false
{
    if (file_id < global_fat.small_file_count && global_fat.small_files[file_id].is_valid)
    {
        memcpy(info, &global_fat.small_files[file_id], sizeof(small_file_info_t));
        return true;
    }
    return false;
}

bool get_large_file_info(uint8_t file_id, large_file_info_t *info) // 返回大文件信息，找不到返回false
{
    if (file_id < global_fat.large_file_count && global_fat.large_files[file_id].is_valid)
    {
        memcpy(info, &global_fat.large_files[file_id], sizeof(large_file_info_t));
        return true;
    }
    return false;
}

#pragma endregion

#pragma region 分配器核心

/**
 * @brief 对于小文件区，直接线性挤压式分配，返回下一个可用地址并推进指针。
 * 适合不频繁删除的小文件，删除后不回收空间。
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
 * @brief 对于大文件区，线性挤压式分配，以sector(4KB)为单位。
 * 适合大文件，删除后不回收空间，但以sector为单位方便擦除操作。
 */
static uint32_t allocate_large_sectors(uint32_t required_sectors)
{
    uint32_t sector = global_fat.large_next_sector;
    if (sector + required_sectors > AREA_LARGE_START_SECTOR + AREA_LARGE_SECTORS)
    {
        return 0xFFFFFFFF;
    }
    global_fat.large_next_sector += required_sectors;
    return sector;
}

#pragma endregion

#pragma region 协议常量与状态机

#define HOST_FRAME_HEAD_0 0xBBU
#define HOST_FRAME_HEAD_1 0x44U
#define FRAME_HDR_SIZE 5U // 帧头(2B) + 命令(1B) + 长度(2B)
#define RETRY_SEND_ERROR_CODE 0xE0U
#define CONTINUE_SEND_CODE 0xA1U

typedef enum
{
    STATE_WAIT_HEAD0,
    STATE_WAIT_HEAD1,
    STATE_WAIT_CMD,
    STATE_WAIT_LEN_L,
    STATE_WAIT_LEN_H,
    STATE_WAIT_PAYLOAD // 包括crc16在内的完整数据段
} host_cmd_state_t;

#pragma endregion

#pragma region 协议解析缓冲区与下载状态

static host_cmd_state_t host_state = STATE_WAIT_HEAD0;
static uint8_t host_cmd;
static uint16_t host_payload_len; // 数据部分的长度，包括CRC16的2字节，但不包括帧头、命令和长度字段本身
static uint16_t host_payload_idx; // 当前已接收的payload字节数，索引从0开始，对应host_payload[FRAME_HDR_SIZE]起始位置
// 帧格式：[帧头0][帧头1][命令][长度L][长度H][payload数据...][CRC16L][CRC16H]
// host_payload存储布局：[0..4]=帧头5字节，[5..]=payload数据(含CRC16)
static uint8_t host_payload[FRAME_HDR_SIZE + 512 + 2] = {0}; // 最大512字节数据 + 2字节CRC + 5字节帧头
static uint8_t rx_buffer[FRAME_HDR_SIZE + 512 + 2];          // USB接收缓冲，确保能容纳完整帧
// DMA写入缓冲区：用于在DMA传输期间保护host_payload不被新USB数据覆盖
static uint8_t dma_write_buf[512 + 2]; // 最大512字节数据 + 2字节CRC(预留)
// 下载任务的实时记录
static bool is_downloading = false;     // 当前是否处于下载状态机中
static uint32_t current_write_addr = 0; // 当前正在写入的物理地址
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
static inline void erase_sector(uint32_t sector)
{
    w25q_erase_sector(sector * W25Q_SECTOR_SIZE);
}
static void send_error(uint8_t error_type)
{
    error_payload = error_type;
    usb_controller_send(&g_usb_controller, RETRY_SEND_ERROR_CODE, &error_payload, 1); // 发送错误代码和一个字节的错误类型
}
static void send_continue(void)
{
    usb_controller_send(&g_usb_controller, CONTINUE_SEND_CODE, NULL, 0); // TODO: 可以在payload中返回当前已接收的文件大小等信息，供主机端显示进度
}
/**
 * @brief 以DMA方式写入flash数据，若DMA失败则回退到同步写入
 */
static bool flash_write_dma(uint32_t addr, const uint8_t *data, uint32_t size)
{
    while (w25q_dma_is_busy())
    {
        w25q_dma_task();
    }
    memcpy(dma_write_buf, data, size);
    if (w25q_write_data_dma(addr, dma_write_buf, size))
    {
        return true;
    }
    // DMA启动失败，回退到同步写入
    w25q_write_data(addr, dma_write_buf, size);
    return true;
}

#pragma endregion

#pragma region 命令处理核心

static void process_host_command(void)
{
    // CRC校验：对所有帧进行校验（帧长>=2时才可校验）
    if (host_payload_len >= 2)
    {
        if (!(crc16_usb_packing(host_payload, FRAME_HDR_SIZE + host_payload_len, true)))
        {
            send_error(0x01); // CRC错误
            return;
        }
    }
    else
    {
        return;
    }

    // CRC校验通过，继续处理命令
    uint16_t actual_data_len = host_payload_len - 2;
    // ==================== 0x19: 删除文件 ====================
    if (host_cmd == 0x19)
    {
        if (host_payload_len < 4)
            return; // 至少需要：文件类型(1B) + 文件索引(1B) + CRC16(2B)
        uint8_t file_type_to_delete = host_payload[FRAME_HDR_SIZE];
        uint8_t file_index = host_payload[FRAME_HDR_SIZE + 1];
        if (file_type_to_delete == 0x11)
        {
            if (file_index >= global_fat.large_file_count || !global_fat.large_files[file_index].is_valid)
            {
                send_error(0x08); // 文件索引无效
                return;
            }
            global_fat.large_files[file_index].is_valid = 0;
            storage_fat_save();
        }
        else if (file_type_to_delete == 0x45)
        {
            if (file_index >= global_fat.small_file_count || !global_fat.small_files[file_index].is_valid)
            {
                send_error(0x08); // 文件索引无效
                return;
            }
            global_fat.small_files[file_index].is_valid = 0;
            storage_fat_save();
        }
        else
        {
            send_error(0x02); // 未知文件类型
        }
        return;
    }
    // ==================== 0x14: 结束下载并且获取文件名字 ====================
    if (host_cmd == 0x14)
    {
        if (!is_downloading)
            return;
        // 等待DMA写入完成后再注册文件
        while (w25q_dma_is_busy())
        {
            w25q_dma_task();
        }
        if (actual_data_len < MAX_FILENAME_LEN)
        {
            memset(&host_payload[FRAME_HDR_SIZE + actual_data_len], 0x00, MAX_FILENAME_LEN - actual_data_len); // 确保文件名部分多余的字节被清零
        }

        memcpy(current_filename, (char *)&host_payload[FRAME_HDR_SIZE], MAX_FILENAME_LEN); // 如果超过了,也只取前16字节
        if (current_file_type == 0x11)                                                     // 大文件注册
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
        else if (current_file_type == 0x45) // 小文件注册
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
        is_downloading = false;
        return;
    }
    // ==================== 0x11 / 0x45: 下载数据 ====================
    if (host_cmd == 0x11 || host_cmd == 0x45)
    {
        // ---------- 首次进入下载：初始化状态 ----------
        if (!is_downloading)
        {
            is_downloading = true;
            current_file_type = host_cmd;
            current_file_size = 0;
            current_allocated_size = 0;
            current_start_sector = 0;
            current_sector_count = 0;
            small_file_start_addr = 0;
            current_write_addr = 0;
            memset(current_filename, 0x00, sizeof(current_filename));

            if (host_cmd == 0x11) // 大文件
            {
                uint32_t allocated_first_sector = allocate_large_sectors(1);
                if (allocated_first_sector == 0xFFFFFFFF)
                {
                    is_downloading = false;
                    send_error(0x03); // 扇区分配失败
                    return;
                }
                current_start_sector = allocated_first_sector;
                current_write_addr = allocated_first_sector * W25Q_SECTOR_SIZE;
                current_sector_count = 1;
                current_allocated_size = W25Q_SECTOR_SIZE;
                // 擦除首扇区，确保写入前扇区是干净的
                erase_sector(allocated_first_sector);
            }
            else if (host_cmd == 0x45) // 小文件
            {
                uint32_t allocated_addr = allocate_small_space(256);
                if (allocated_addr == 0xFFFFFFFF)
                {
                    is_downloading = false;
                    send_error(0x04); // 空间分配失败
                    return;
                }
                small_file_start_addr = allocated_addr;
                current_write_addr = allocated_addr;
                current_allocated_size = 256;
                uint32_t sector = allocated_addr / W25Q_SECTOR_SIZE;
                if (sector > small_last_erased_sector)
                {
                    erase_sector(sector);
                    small_last_erased_sector = sector;
                }
            }
            // 写入首段数据
            if (actual_data_len > 0)
            {
                flash_write_dma(current_write_addr, &host_payload[FRAME_HDR_SIZE], actual_data_len);
            }
            send_continue();
            current_write_addr += actual_data_len;
            current_file_size += actual_data_len;
            return;
        }
        // ---------- 下载中收到不同类型命令：冲突 ----------
        if (current_file_type != host_cmd)
        {
            is_downloading = false; // 直接退出下载状态，丢弃当前未完成的文件数据
            send_error(0x05);       // 命令类型冲突
            return;
        }
        // ---------- 下载中：续传数据 ----------
        if (host_cmd == 0x11) // 大文件扩展检查
        {
            uint32_t needed_size = current_write_addr + actual_data_len - (current_start_sector * W25Q_SECTOR_SIZE); // 注意的是,这里计算的是从当前大文件起始扇区到当前写入位置+新数据末尾的总需求大小,也就是目前所接受到的文件大小加上新数据的大小,与当前已分配的大小进行比较,如果超过了就需要扩展分配新的扇区
            if (needed_size > current_allocated_size)
            {
                uint32_t new_sectors_needed = (needed_size + W25Q_SECTOR_SIZE - 1) / W25Q_SECTOR_SIZE; // 这里向上取整计算需要的扇区数
                if (new_sectors_needed > current_sector_count)
                {
                    uint32_t extend_end = current_start_sector + new_sectors_needed; // 注意这里返回的是需要扩展到的扇区末尾,也就是所是下一个可用的扇区号,而不是需要扩展的扇区数.
                    if (extend_end > AREA_LARGE_START_SECTOR + AREA_LARGE_SECTORS)
                    {
                        is_downloading = false;
                        send_error(0x06); // 扇区超出范围
                        return;
                    }
                    // 擦除新增的扇区
                    for (uint32_t s = current_start_sector + current_sector_count; s < extend_end; s++)
                    {
                        erase_sector(s);
                    }
                    global_fat.large_next_sector = extend_end;
                    current_sector_count = new_sectors_needed;
                    current_allocated_size = new_sectors_needed * W25Q_SECTOR_SIZE;
                }
            }
        }
        else if (host_cmd == 0x45) // 小文件扩展检查
        {
            uint32_t needed_total = current_write_addr + actual_data_len - small_file_start_addr;
            if (needed_total > current_allocated_size)
            {
                uint32_t additional_bytes = needed_total - current_allocated_size;
                additional_bytes = ((additional_bytes + 255) / 256) * 256; // 按256字节对齐
                if (global_fat.small_next_addr + additional_bytes <= AREA_SMALL_END_ADDR)
                {
                    global_fat.small_next_addr += additional_bytes;
                    current_allocated_size += additional_bytes;
                }
                else
                {
                    is_downloading = false;
                    send_error(0x07); // 小文件空间不足
                    return;
                }
            }
            uint32_t new_end_sector = (small_file_start_addr + current_allocated_size - 1) / W25Q_SECTOR_SIZE;
            for (uint32_t s = small_last_erased_sector + 1; s <= new_end_sector; s++)
            {
                erase_sector(s);
                small_last_erased_sector = s;
            }
        }
        // 以DMA方式写入数据
        if (actual_data_len > 0)
        {
            flash_write_dma(current_write_addr, &host_payload[FRAME_HDR_SIZE], actual_data_len);
        }
        send_continue();
        // 推进游标
        current_write_addr += actual_data_len;
        current_file_size += actual_data_len;
        return;
    }
    // ==================== 0x20: 查询文件列表 ====================
    if (host_cmd == 0x20)
    {
        uint8_t file_list_buffer[256];
        uint16_t idx = 0;
        uint8_t valid_small_count = 0;
        uint8_t valid_large_count = 0;
        for (uint16_t i = 0; i < global_fat.small_file_count; i++)
        {
            if (global_fat.small_files[i].is_valid)
                valid_small_count++;
        }
        for (uint16_t i = 0; i < global_fat.large_file_count; i++)
        {
            if (global_fat.large_files[i].is_valid)
                valid_large_count++;
        }
        file_list_buffer[idx++] = valid_small_count;
        file_list_buffer[idx++] = valid_large_count;
        for (uint8_t i = 0; i < global_fat.small_file_count && idx < 240; i++)
        {
            if (global_fat.small_files[i].is_valid)
            {
                if (idx + 25 > sizeof(file_list_buffer))
                    break;
                file_list_buffer[idx++] = global_fat.small_files[i].file_type;
                memcpy(&file_list_buffer[idx], global_fat.small_files[i].filename, MAX_FILENAME_LEN);
                idx += MAX_FILENAME_LEN;
                memcpy(&file_list_buffer[idx], &global_fat.small_files[i].start_address, 4);
                idx += 4;
                memcpy(&file_list_buffer[idx], &global_fat.small_files[i].size, 4);
                idx += 4;
            }
        }
        for (uint8_t i = 0; i < global_fat.large_file_count && idx < 240; i++)
        {
            if (global_fat.large_files[i].is_valid)
            {
                if (idx + 25 > sizeof(file_list_buffer))
                    break;
                file_list_buffer[idx++] = global_fat.large_files[i].file_type;
                memcpy(&file_list_buffer[idx], global_fat.large_files[i].filename, MAX_FILENAME_LEN);
                idx += MAX_FILENAME_LEN;
                memcpy(&file_list_buffer[idx], &global_fat.large_files[i].start_sector, 4);
                idx += 4;
                memcpy(&file_list_buffer[idx], &global_fat.large_files[i].size, 4);
                idx += 4;
            }
        }
        usb_controller_send(&g_usb_controller, 0x20, file_list_buffer, idx);
        return;
    }
    // ==================== 未知命令 ====================
    send_error(0x09);
}

#pragma endregion

#pragma region 协议状态机(逐字节解析)

static void storage_manager_process_host_byte(uint8_t byte)
{
    switch (host_state)
    {
    case STATE_WAIT_HEAD0:
        if (byte == HOST_FRAME_HEAD_0)
        {
            host_payload[0] = byte;
            host_state = STATE_WAIT_HEAD1;
        }
        break;
    case STATE_WAIT_HEAD1:
        if (byte == HOST_FRAME_HEAD_1)
        {
            host_payload[1] = byte;
            host_state = STATE_WAIT_CMD;
        }
        else if (byte == HOST_FRAME_HEAD_0)
        {
            host_state = STATE_WAIT_HEAD1; // 应对重叠 0xBB 0xBB 0x44 的情况
        }
        else
        {
            host_state = STATE_WAIT_HEAD0;
            clear_host_payload();
        }
        break;
    case STATE_WAIT_CMD:
        host_cmd = byte;
        host_payload[2] = byte;
        host_state = STATE_WAIT_LEN_L;
        break;
    case STATE_WAIT_LEN_L:
        host_payload_len = byte;
        host_payload[3] = byte;
        host_state = STATE_WAIT_LEN_H;
        break;
    case STATE_WAIT_LEN_H:
        host_payload_len |= ((uint16_t)byte << 8);
        host_payload[4] = byte;
        if (host_payload_len > sizeof(host_payload) - FRAME_HDR_SIZE)
        {
            host_state = STATE_WAIT_HEAD0;
            clear_host_payload();
        }
        else if (host_payload_len < 2)
        {
            host_state = STATE_WAIT_HEAD0;
            clear_host_payload();
        }
        else
        {
            host_payload_idx = 0;
            host_state = STATE_WAIT_PAYLOAD;
        }
        break;
    case STATE_WAIT_PAYLOAD:
        host_payload[host_payload_idx + FRAME_HDR_SIZE] = byte;
        host_payload_idx++;
        if (host_payload_idx >= host_payload_len)
        {
            process_host_command();
            host_state = STATE_WAIT_HEAD0;
            clear_host_payload();
        }
        break;
    }
}
#pragma endregion

#pragma region 主任务接口

bool storage_manager_init(void)
{
    clear_host_payload();
    bool fat_ok = storage_fat_load();
    small_last_erased_sector = (global_fat.small_next_addr > 0) ? ((global_fat.small_next_addr - 1) / W25Q_SECTOR_SIZE) : (AREA_SMALL_START_ADDR / W25Q_SECTOR_SIZE - 1);
    if (!fat_ok)
    {
        uint32_t first_sector = global_fat.small_next_addr / W25Q_SECTOR_SIZE;
        erase_sector(first_sector);
        small_last_erased_sector = first_sector;
    }
    return fat_ok;
}

void storage_manager_task(void)
{
    uint16_t rx_len = usb_controller_receive(&g_usb_controller, rx_buffer, sizeof(rx_buffer));
    for (uint16_t i = 0; i < rx_len;)
    {
        if (host_state == STATE_WAIT_PAYLOAD)
        {
            // 批量拷贝模式：直接从rx_buffer批量拷贝payload数据到host_payload
            uint16_t need_len = host_payload_len - host_payload_idx;
            uint16_t valid_len = rx_len - i;
            uint16_t copy_len = (need_len < valid_len) ? need_len : valid_len;
            memcpy(&host_payload[host_payload_idx + FRAME_HDR_SIZE], &rx_buffer[i], copy_len);
            host_payload_idx += copy_len;
            i += copy_len;
            if (host_payload_idx >= host_payload_len)
            {
                process_host_command();
                host_state = STATE_WAIT_HEAD0;
                clear_host_payload();
            }
        }
        else
        {
            storage_manager_process_host_byte(rx_buffer[i]);
            i++;
        }
    }
}

#pragma endregion

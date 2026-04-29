/*
 * storage_manager.c
 *
 *  Created on: 2026年4月27日
 *      Author: UnikoZera
 *
 *      可以产生发给单片机的测试代码：BB 44 11 04 00 31 32 75 81 
 *    这里说明一下usb主机与存储管理器之间的协议设计思路：
 * 1. 主机每次发送一个完整的命令帧，包含：帧头(2字节) + 命令(1字节) + 长度(2字节) + 数据(payload) + CRC16(2字节)
 * 2. 数据全是uint8_t类型。别的其他均为小端格式。长度字段的值是数据部分的字节数加上CRC16的2字节。
 * 2. 关于参数：帧头主机端默认为 0xBB 0x44，长度的意思是 数据长度 + CRC16长度(2字节)，数据部分根据命令不同有不同的格式和意义，CRC16是帧头到数据末尾的所有字节的校验和。
 */

/* 这个文件在结构发生变动后，为了不影响其余代码导致错乱，在此进行整体修改与逻辑匹配！*/
// 目前功能比较简单，主要是为了后续可能的功能扩展和代码组织优化做准备的。现在暂时没有实现具体的功能。
#include "storage_manager.h"

#pragma region 文件系统与分配表实现
// ======================== 文件系统/分配表定义 ========================
#define FAT_MAGIC_NUMBER 0x0D000721 // okay, 我感觉这个魔数不错
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
#define MAX_SMALL_FILES 64

// [区段 3] 大文件区:按大块扇区对齐位图分配 (推荐频繁删除的文件，删除后通过位图清空对应扇区即可，不会有碎片问题)
// Sector 64 ~ 4031 (共 3968 个扇区 / 15.5MB)
#define AREA_LARGE_START_SECTOR 64
#define AREA_LARGE_SECTORS 3968
#define MAX_LARGE_FILES 32

// [区段 4] 用户自定义区
// Sector 4032 ~ 4095 (共 64 个扇区 / 256KB)
#define AREA_USER_START_SECTOR 4032
#define AREA_USER_SECTORS 64

// ---------------- 描述符定义 ----------------
typedef struct
{
    uint8_t is_valid;
    uint8_t file_type;      // 其实这里是用来保留的，目前没有实际意义的，后续可以用来区分不同类型的文件以便不同的处理逻辑
    uint32_t start_address; // 物理字节地址，紧挨着排列
    uint32_t size;
} small_file_info_t;

typedef struct
{
    uint8_t is_valid;
    uint8_t file_type;     // same.
    uint16_t sector_count; // 占用的扇区数量，向上取整
    uint32_t start_sector; // 物理扇区号
    uint32_t size;
    // 这里不储存脏页了，因为大文件区的分配是连续的，删除时直接清空对应位图即可，不需要记录哪些页被写过了。
} large_file_info_t;

// 存放在 AT24C 中的总分配表 (FAT)
typedef struct
{
    uint32_t magic;
    // 小文件分配器状态 (线性挤压式)
    uint32_t small_next_addr; // 小文件区下一个可分配的地址，向上挤压分配，不回收碎片。初始值为 AREA_SMALL_START_ADDR
    uint16_t small_file_count;
    small_file_info_t small_files[MAX_SMALL_FILES];
    // 大文件分配器状态 (扇区位图式)
    uint16_t large_file_count;
    large_file_info_t large_files[MAX_LARGE_FILES];
    uint8_t large_sector_bitmap[AREA_LARGE_SECTORS / 8]; // 仅对大文件区(3968区)维护脏页
} storage_fat_t;

static storage_fat_t global_fat; // 全局分配表，启动时从AT24C加载，运行时保持更新，并定期或在关键操作后写回AT24C以持久化。

#pragma endregion

#pragma region USB协议解析状态机与文件分配核心

#define HOST_FRAME_HEAD_0 0xBBU
#define HOST_FRAME_HEAD_1 0x44U

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

// ======================== 位图管理器与分配核心 ========================

/**
 * @brief 对于大文件区，必须分配连续的扇区以保证性能，因此需要一个函数来在位图中寻找足够大的连续空闲扇区，并标记它们为已占用。
 *
 * @param required_sectors 表示需要分配多少个连续扇区(一个扇区为4KB)，调用者需要根据即将写入的数据量计算出需要多少个扇区。
 * @return uint32_t w25q的物理扇区号，或者0xFFFFFFFF表示没有足够空间了。
 */
static uint32_t allocate_large_continuous_sectors(uint32_t required_sectors)
{
    uint32_t continuous_free_count = 0;
    uint32_t start_sector_candidate = 0;

    // 仅在大扇区位图中搜寻
    for (uint32_t i = 0; i < AREA_LARGE_SECTORS; i++)
    {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;

        if ((global_fat.large_sector_bitmap[byte_idx] & (1 << bit_idx)) == 0) // 此区空闲
        {
            if (continuous_free_count == 0)
                start_sector_candidate = i;
            continuous_free_count++;

            if (continuous_free_count >= required_sectors)
            {
                for (uint32_t j = start_sector_candidate; j <= i; j++)
                {
                    global_fat.large_sector_bitmap[j / 8] |= (1 << (j % 8));
                }
                // 加上大文件区的基准偏移量，返回真正的物理块
                return AREA_LARGE_START_SECTOR + start_sector_candidate;
            }
        }
        else
            continuous_free_count = 0;
    }
    return 0xFFFFFFFF; // 满了,或者没有足够连续空间
}

/**
 * @brief 对于小文件区，直接线性挤压式分配，返回下一个可用地址并推进指针。由于不回收碎片，所以删除文件后不会有空间回收，但也不会有碎片问题。适合不频繁删除的小文件。
 *
 * @param required_bytes 小文件需要的字节数，调用者需要根据即将写入的数据量计算出需要多少字节。
 * @return uint32_t 返回分配到的物理字节地址，或者0xFFFFFFFF表示没有足够空间了。
 */
static uint32_t allocate_small_space(uint32_t required_bytes)
{
    uint32_t addr = global_fat.small_next_addr;
    if (addr + required_bytes > AREA_SMALL_END_ADDR)
    {
        return 0xFFFFFFFF; // 小区满了
    }
    global_fat.small_next_addr += required_bytes;
    return addr;
}

// using when you delete a LARGE file. not fitable for small files.
static void free_sectors(uint32_t start_sector, uint32_t count)
{
    for (uint32_t j = start_sector; j < start_sector + count; j++)
    {
        global_fat.large_sector_bitmap[j / 8] &= ~(1 << (j % 8)); // 脏位置 0
    }
}

static host_cmd_state_t host_state = STATE_WAIT_HEAD0;
static uint8_t host_cmd;                      // 这个其实可以直接读取host_payload[2]的位置的，因为之前我们在状态机里就把它存进去了，但为了代码清晰我还是单独放了个变量来表示当前命令，避免后续维护时搞混了这个数组的用途。
static uint16_t host_payload_len;             // 这个是数据部分的长度，包括CRC16的2字节，但不包括帧头、命令和长度字段本身。调用者需要根据这个长度来知道数据部分有多长，以及CRC16在哪里。
static uint16_t host_payload_idx;             // 当前已经接收了多少数据字节了，用于知道下一个接收的字节应该存到host_payload的哪个位置。注意这个索引是针对数据部分的，所以它的起始位置是0，对应host_payload数组中第5个字节的位置，因为前面4个字节分别是帧头(2字节)和命令(1字节)以及长度(2字节)。
static uint8_t host_payload[512 + 2 + 2 + 1] = {0}; // 最大512字节数据 + 2字节CRC + 2字节长度 + 1字节命令
static uint8_t rx_buffer[512 + 2 + 2 + 1];    // 确保能够一次处理完整帧
static uint8_t tx_buffer[3200];               // 极端情况下也就只需要发送8次(全屏动画)

// 下载任务的实时记录
static bool is_downloading = false;
static uint32_t current_write_addr = 0;
static uint32_t current_file_size = 0;
static uint8_t current_file_type = 0;

static uint8_t count = 32; // debug....

static void clear_host_payload(void)
{
    memset(host_payload, 0x00, sizeof(host_payload));
}

static void process_host_command(void) // TODO：根据协议解析host_payload中的命令和数据，并执行相应的操作。这个函数是整个协议处理的核心，后续需要根据具体的协议设计来实现不同命令的处理逻辑。
{
    bool is_valid = false;

    if (host_cmd == 0x11 || host_cmd == 0x45 || host_cmd == 0x14 || host_cmd == 0x19 || host_cmd == 0x1A || host_cmd == 0x21) // 目前合法的命令
    {
        if (host_payload_len >= 2)
        {
            if (crc_packing(host_payload, 5 + host_payload_len, true))
            {
                is_valid = true;
            }
            else
            {
                usb_controller_send(&g_usb_controller, RETRY_SEND_ERROR_CODE, &count, 1);
            }
        }
    }
    else
    {
        is_valid = true;
    }

    if (is_valid) // good boy finally cames here.
    {
        if (host_cmd == 0x1A) // 删除文件命令
        {
            uint8_t file_type_to_delete = host_payload[5];
            uint8_t file_index = host_payload[6];

            if (file_type_to_delete == 0x11 || file_type_to_delete == 0x45) // 大文件
            {
                if (file_index < global_fat.large_file_count)
                {
                    large_file_info_t *fi = &global_fat.large_files[file_index];
                    if (fi->is_valid)
                    {
                        free_sectors(fi->start_sector, fi->sector_count);
                        fi->is_valid = 0;
                    }
                }
            }
            else if (file_type_to_delete == 0x14) // 小文件
            {
                if (file_index < global_fat.small_file_count)
                {
                    small_file_info_t *fi = &global_fat.small_files[file_index];
                    fi->is_valid = 0;
                }
            }
        }
        else if (host_cmd == 0x19)
        {
            // 接收到传输终止命令
            if (is_downloading)
            {
                // 将该文件参数写入全局分配表
                if (current_file_type == 0x11 || current_file_type == 0x45) // 大文件
                {
                    if (global_fat.large_file_count < MAX_LARGE_FILES)
                    {
                        large_file_info_t *fi = &global_fat.large_files[global_fat.large_file_count];
                        fi->is_valid = 1;
                        fi->file_type = current_file_type;
                        fi->start_sector = current_write_addr / W25Q_SECTOR_SIZE;
                        fi->size = current_file_size;
                        fi->sector_count = (current_file_size + W25Q_SECTOR_SIZE - 1) / W25Q_SECTOR_SIZE;

                        global_fat.large_file_count++;
                    }
                }
                else if (current_file_type == 0x14) // 小文件
                {
                    if (global_fat.small_file_count < MAX_SMALL_FILES)
                    {
                        small_file_info_t *fi = &global_fat.small_files[global_fat.small_file_count];
                        fi->is_valid = 1;
                        fi->file_type = current_file_type;
                        fi->start_address = current_write_addr;
                        fi->size = current_file_size;

                        global_fat.small_file_count++;
                    }
                }
                is_downloading = false; // 结束一次下载状态机
                usb_controller_send(&g_usb_controller, CONTINUE_SEND_CODE, NULL, 0);
            }
        }
        else if (host_cmd == 0x11 || host_cmd == 0x45 || host_cmd == 0x14) // 0x11图片等
        {
            uint16_t actual_data_len = host_payload_len - 2;
            if (!is_downloading) // 进入下载task,持续接受数据，直到收到0x19的结束命令
            {
                is_downloading = true;
                current_file_type = host_cmd;
                current_file_size = 0;

                if (host_cmd == 0x11 || host_cmd == 0x45) // 大文件
                {
                    uint32_t simulated_req_sectors = 100; // <- 这里您需要根据协议替换为实际需多大空间去脏页表占位！
                    uint32_t allocated_first_sector = allocate_large_continuous_sectors(simulated_req_sectors);
                    current_write_addr = allocated_first_sector * W25Q_SECTOR_SIZE;
                }
                else if (host_cmd == 0x14) // 小文件
                {
                    uint32_t simulated_req_bytes = 4096; // 小文件最大需求h
                    uint32_t allocated_addr = allocate_small_space(simulated_req_bytes);
                    current_write_addr = allocated_addr;
                }
            }

            uint32_t current_sector = current_write_addr / W25Q_SECTOR_SIZE;
            uint32_t next_sector = (current_write_addr + actual_data_len - 1) / W25Q_SECTOR_SIZE;

            if (next_sector > current_sector)
            {
                w25q_erase_sector(next_sector * W25Q_SECTOR_SIZE);
            }

            w25q_write_data(current_write_addr, host_payload, actual_data_len);

            // 推进游标
            current_write_addr += actual_data_len;
            current_file_size += actual_data_len;
        }
        else if (host_cmd == 0x20) // 查询文件列表
        {
            uint8_t file_list_buffer[128];
            uint8_t idx = 0;

            file_list_buffer[idx++] = global_fat.small_file_count;
            file_list_buffer[idx++] = global_fat.large_file_count;

            for (uint8_t i = 0; i < global_fat.small_file_count && idx < 120; i++)
            {
                file_list_buffer[idx++] = global_fat.small_files[i].file_type;
                file_list_buffer[idx++] = global_fat.small_files[i].is_valid;
                memcpy(&file_list_buffer[idx], &global_fat.small_files[i].start_address, 4);
                idx += 4;
                memcpy(&file_list_buffer[idx], &global_fat.small_files[i].size, 4);
                idx += 4;
            }

            for (uint8_t i = 0; i < global_fat.large_file_count && idx < 120; i++)
            {
                file_list_buffer[idx++] = global_fat.large_files[i].file_type;
                file_list_buffer[idx++] = global_fat.large_files[i].is_valid;
                memcpy(&file_list_buffer[idx], &global_fat.large_files[i].start_sector, 4);
                idx += 4;
                memcpy(&file_list_buffer[idx], &global_fat.large_files[i].size, 4);
                idx += 4;
            }

            usb_controller_send(&g_usb_controller, 0x20, file_list_buffer, idx);
        }
        else if (host_cmd == 0x21) // 读取文件数据
        {
            uint8_t file_type_to_read = host_payload[5];
            uint8_t file_index = host_payload[6];
            uint32_t read_offset = 0;
            uint32_t read_size = 0;

            if (file_type_to_read == 0x11 || file_type_to_read == 0x45) // 大文件
            {
                if (file_index < global_fat.large_file_count)
                {
                    large_file_info_t *fi = &global_fat.large_files[file_index];
                    if (fi->is_valid)
                    {
                        read_offset = fi->start_sector * W25Q_SECTOR_SIZE;
                        read_size = fi->size;
                        w25q_read_data(read_offset, tx_buffer, read_size);
                        usb_controller_send(&g_usb_controller, 0x21, tx_buffer, read_size);
                    }
                }
            }
            else if (file_type_to_read == 0x14) // 小文件
            {
                if (file_index < global_fat.small_file_count)
                {
                    small_file_info_t *fi = &global_fat.small_files[file_index];
                    if (fi->is_valid)
                    {
                        read_offset = fi->start_address;
                        read_size = fi->size;
                        w25q_read_data(read_offset, tx_buffer, read_size);
                        usb_controller_send(&g_usb_controller, 0x21, tx_buffer, read_size);
                    }
                }
            }
        }
    }
}

static void storage_manager_process_host_byte(uint8_t byte)
{
    switch (host_state)
    {
    case STATE_WAIT_HEAD0:
        if (byte == HOST_FRAME_HEAD_0)
        {
            host_payload[0] = byte; // 预存帧头0，方便后续构建完整帧进行CRC校验
            host_state = STATE_WAIT_HEAD1;
        }
        break;
    case STATE_WAIT_HEAD1:
        if (byte == HOST_FRAME_HEAD_1)
        {
            host_payload[1] = byte; // 预存帧头1，方便后续构建完整帧进行CRC校验
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
        host_payload[3] = byte; // 先暂存长度的低字节，等高字节来了再构建完整长度用于后续CRC校验
        host_state = STATE_WAIT_LEN_H;
        break;
    case STATE_WAIT_LEN_H:
        host_payload_len |= ((uint16_t)byte << 8);
        host_payload[4] = byte; // 现在长度的高字节也来了，暂存起来以便后续构建完整帧进行CRC校验 这样就不需要等数据都来了才去构建了，节省一点时间
        if (host_payload_len > sizeof(host_payload))
        {
            host_state = STATE_WAIT_HEAD0;
            clear_host_payload();
        }
        else if (host_payload_len == 0)
        {
            process_host_command();
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
        host_payload[host_payload_idx++] = byte;
        if (host_payload_idx >= host_payload_len)
        {
            process_host_command();
            host_state = STATE_WAIT_HEAD0;
            clear_host_payload();
        }
        break;
    }
}

void storage_manager_init(void)
{
    clear_host_payload();
    w25q_init();
    at24c_init();
}

void storage_manager_task(void)
{
    w25q_dma_task();

    uint16_t rx_len = usb_controller_receive(&g_usb_controller, rx_buffer, sizeof(rx_buffer));

    for (uint16_t i = 0; i < rx_len;)
    {
        if (host_state == STATE_WAIT_PAYLOAD)
        {
            uint16_t need_len = host_payload_len - host_payload_idx;           // 此包还需要多少字节
            uint16_t valid_len = rx_len - i;                                   // 当前数组还剩多少字节
            uint16_t copy_len = (need_len < valid_len) ? need_len : valid_len; // 取小值

            memcpy(&host_payload[host_payload_idx + 5], &rx_buffer[i], copy_len); // 全速拷贝

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

/*
 * storage_manager.h
 *
 *  Created on: 2026年4月27日
 *      Author: UnikoZera
 */
#ifndef INC_STORAGE_MANAGER_H_
#define INC_STORAGE_MANAGER_H_

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "crc16.h"
#include "at24c_controller.h"
#include "w25q_controller.h"
#include "usb_controller.h"

/* ============================================================================
 *  对外可调配置宏
 * ============================================================================ */

/* 文件名最大长度（含结尾 '\0' 的存储容量） */
#define MAX_FILENAME_LEN                 16

/* 小/大文件槽位上限 */
#define MAX_SMALL_FILES                  40
#define MAX_LARGE_FILES                  35

/* 小文件区剩余空间低于此值(字节)时触发压缩回收 */
#define SMALL_FILE_COMPACT_THRESHOLD     (1024 * 16)  /* 16KB */

/* ============================================================================
 *  文件信息结构
 * ============================================================================ */

typedef struct
{
    uint8_t is_valid;
    uint8_t file_type;
    uint32_t start_address;
    uint32_t size;
    char filename[MAX_FILENAME_LEN];
} small_file_info_t;

typedef struct
{
    uint8_t is_valid;
    uint8_t file_type;
    uint32_t sector_count;
    uint32_t start_sector;
    uint32_t size;
    char filename[MAX_FILENAME_LEN];
} large_file_info_t;

/* ============================================================================
 *  公共 API
 * ============================================================================ */

bool storage_manager_init(void);
void storage_manager_task(void);
bool storage_fat_load(void);
void storage_fat_save(void);
int16_t find_small_file_by_name(const char *name);
int16_t find_large_file_by_name(const char *name);
bool get_small_file_info(uint8_t file_id, small_file_info_t *info);
bool get_large_file_info(uint8_t file_id, large_file_info_t *info);
void clear_all_files_manual(void);
bool compact_small_files(void);
bool storage_is_downloading(void);

#endif /* INC_STORAGE_MANAGER_H_ */
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
#define MAX_FILENAME_LEN 16
#define MAX_SMALL_FILES 64
#define MAX_LARGE_FILES 32
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
    uint16_t sector_count;
    uint32_t start_sector;
    uint32_t size;
    char filename[MAX_FILENAME_LEN];
} large_file_info_t;

bool storage_manager_init(void);
void storage_manager_task(void);
bool storage_fat_load(void);
void storage_fat_save(void);
int16_t find_small_file_by_name(const char *name);
int16_t find_large_file_by_name(const char *name);
bool get_small_file_info(uint8_t file_id, small_file_info_t *info);
bool get_large_file_info(uint8_t file_id, large_file_info_t *info);

#endif /* INC_STORAGE_MANAGER_H_ */
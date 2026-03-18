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
#define W25Q_TIMEOUT 100 // SPI Timeout in ms


void w25q_check_busy(void);
void w25q_check_busy_nontimeout(void);
uint32_t w25q_read_id(void);
bool w25q_init(void);
void w25q_erase_sector(uint32_t address);
void w25q_erase_chip(void);
void w25q_page_program(uint32_t address, uint8_t *data, uint16_t size);
void w25q_write_data(uint32_t address, uint8_t *data, uint32_t size);
void w25q_read_data(uint32_t address, uint8_t *data, uint32_t size);
void w25q_fast_read_data(uint32_t address, uint8_t *data, uint32_t size);

#endif /* INC_W25Q_CONTROLLER_H_ */

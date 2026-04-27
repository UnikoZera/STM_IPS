/*
 * at24c_controller.h
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#ifndef INC_AT24C_CONTROLLER_H_
#define INC_AT24C_CONTROLLER_H_

#include <stdint.h>
#include <stdbool.h>
#include "i2c.h"

#define AT24C64_ADDRESS 0xA0 // AT24C64的I2C地址，注意根据实际接线可能需要调整
#define AT24C64_CAPACITY 65536U // 64Kb = 65536 bytes
#define AT24C64_PAGE_SIZE 32
#define AT24C_TIMEOUT 100

// *很遗憾的是,这里不需要任何的dma，因为AT24C64的写入速度非常慢，远低于DMA的启动和传输开销. 反而是需要在每次写入后等待EEPROM完成内部写入操作, 这通常需要几毫秒的时间, 远超过DMA传输的时间. 因此, 直接使用阻塞式的I2C读写函数更简单高效, 而不需要引入DMA的复杂性.
bool at24c_write_byte(uint16_t memAddress, uint8_t *data);
bool at24c_read_byte(uint16_t memAddress, uint8_t *data);
bool at24c_write_buffer(uint16_t memAddress, uint8_t *pData, uint16_t size);
bool at24c_read_buffer(uint16_t memAddress, uint8_t *pData, uint16_t size);

#endif /* INC_AT24C_CONTROLLER_H_ */

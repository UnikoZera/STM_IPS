/*
 * at24c_controller.h
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#ifndef INC_AT24C_CONTROLLER_H_
#define INC_AT24C_CONTROLLER_H_

#include <stdint.h>
#include "i2c.h"

#define AT24C64_ADDRESS 0xA0

void at24c_write_byte(uint16_t memAddress, uint8_t *data);
void at24c_read_byte(uint16_t memAddress, uint8_t *data);
void at24c_write_buffer(uint16_t memAddress, uint8_t *pData, uint16_t size);
void at24c_read_buffer(uint16_t memAddress, uint8_t *pData, uint16_t size);

#endif /* INC_AT24C_CONTROLLER_H_ */

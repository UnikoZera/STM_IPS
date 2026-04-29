/*
 * storage_controller.h
 *
 *  Created on: 2026年4月2日
 *      Author: UnikoZera
 */

#ifndef INC_CRC16_H_
#define INC_CRC16_H_

#include <stdint.h>
#include <stdbool.h>

uint16_t crc_packing(const uint8_t *data, uint32_t len, bool has_crc);

#endif /* INC_CRC16_H_ */

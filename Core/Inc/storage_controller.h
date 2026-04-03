/*
 * storage_controller.h
 *
 *  Created on: 2026年4月2日
 *      Author: UnikoZera
 */

#ifndef INC_STORAGE_CONTROLLER_H_
#define INC_STORAGE_CONTROLLER_H_

#include "w25q_controller.h"
#include "at24c_controller.h"

uint16_t crc_packing(const uint8_t *data, uint32_t len, bool has_crc);

#endif /* INC_STORAGE_CONTROLLER_H_ */

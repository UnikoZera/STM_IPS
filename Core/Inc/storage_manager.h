/*
 * storage_manager.h
 *
 *  Created on: 2026年4月27日
 *      Author: UnikoZera
 */

#ifndef INC_STORAGE_MANAGER_H_
#define INC_STORAGE_MANAGER_H_

#include <string.h> // 需要引入此库以支持 memcpy 高速拷贝
#include "crc16.h"
#include "at24c_controller.h"
#include "w25q_controller.h"
#include "usb_controller.h"


void storage_manager_init(void);
void storage_manager_task(void);

#endif /* INC_STORAGE_MANAGER_H_ */

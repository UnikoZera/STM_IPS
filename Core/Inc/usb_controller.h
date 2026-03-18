/*
 * usb_controller.h
 *
 *  Created on: 2026年3月18日
 *      Author: UnikoZera
 */

#ifndef INC_USB_CONTROLLER_H_
#define INC_USB_CONTROLLER_H_

#include <stdint.h>

uint8_t usb_transmit(uint8_t* buf, uint16_t len);
uint8_t usb_receive(uint8_t* buf, uint32_t len);

#endif /* INC_USB_CONTROLLER_H_ */

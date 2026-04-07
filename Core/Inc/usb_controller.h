/*
 * usb_controller.h
 *
 *  Created on: 2026年3月18日
 *      Author: UnikoZera
 */

#ifndef INC_USB_CONTROLLER_H_
#define INC_USB_CONTROLLER_H_

#include <stdint.h>
#include <stdbool.h>

#define USB_TIMEOUT_MS 100	// USB传输的超时时间，根据需要调整
#define USB_SEND_BYTES_PER_CALL 1024 // 根据USB设备的最大包大小调整
#define USB_RECEIVE_BUFFER_SIZE 1024 // 根据USB设备的最大包大小调整

typedef struct
{
	bool usb_enabled;
	bool usb_tx_active;
	bool usb_tx_done;
	bool usb_rx_active;
	bool usb_rx_done;

	uint32_t last_tx_tick;
	uint32_t last_rx_tick;

	uint8_t *tx_tail_ptr; // 记录当前发送数据的尾指针，方便分包发送
	uint8_t *rx_tail_ptr; // 记录当前接收数据的尾指针，方便分包接收
	uint16_t tx_remain_len;
	uint16_t rx_cached_len;


} usb_controller_t;

extern usb_controller_t g_usb_controller;

uint8_t usb_transmit(uint8_t* buf, uint16_t len);
uint8_t usb_receive(uint8_t* buf, uint32_t len);
uint16_t usb_controller_receive(usb_controller_t* controller, uint8_t* buf, uint16_t len);

void usb_controller_init(usb_controller_t* controller);
void usb_controller_task(usb_controller_t* controller);
void usb_controller_send(usb_controller_t* controller, uint8_t* data, uint16_t len);
void usb_controller_on_tx_complete(void);
void usb_controller_on_rx_received(uint8_t* buf, uint32_t len);

#endif /* INC_USB_CONTROLLER_H_ */

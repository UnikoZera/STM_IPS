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

#define USB_SEND_BYTES_PER_CALL 5120// 分片减小可提升LCD流开启时的命令响应稳定性
#define USB_RECEIVE_BUFFER_SIZE 1024 // 根据USB设备的最大包大小调整
#define USB_TX_RETRY_INTERVAL_MS 2U // busy时的最小重试间隔，避免高频空转
#define USB_TX_STUCK_TIMEOUT_MS 2000U // 发送活跃超过该时间仍无进展时执行软恢复

typedef enum {
    USB_SEND_OK = 0,
    USB_SEND_QUEUED,
    USB_SEND_DROPPED_PREVIOUS, // 覆盖了之前的pending数据
    USB_SEND_BUSY_REJECTED     // 槽位已满，拒绝当前发送
} usb_send_status_t;

typedef struct
{
	volatile bool usb_enabled;
	volatile bool usb_tx_active;
	volatile bool usb_tx_done;
	volatile bool usb_rx_active;
	volatile bool usb_rx_done;
	volatile uint16_t tx_remain_len;
	volatile uint16_t rx_head; // 写指针
	volatile uint16_t rx_tail; // 读指针
	volatile uint16_t tx_pending_len; // 单槽待发送长度，不复制payload以节省RAM

	volatile uint32_t last_tx_tick;
	volatile uint32_t last_rx_tick;

	const uint8_t *tx_tail_ptr; // 记录当前发送数据的尾指针，调用方需保证其生命周期覆盖整个发送过程
	const uint8_t *tx_pending_ptr; // 单槽待发送指针，调用方需保证其生命周期覆盖整个发送过程


} usb_controller_t;

extern usb_controller_t g_usb_controller;

uint8_t usb_transmit(const uint8_t* buf, uint16_t len);
uint8_t usb_receive(uint8_t* buf, uint32_t len);
uint16_t usb_controller_receive(usb_controller_t* controller, uint8_t* buf, uint16_t len);

void usb_controller_init(usb_controller_t* controller);
void usb_controller_task(usb_controller_t* controller);
usb_send_status_t usb_controller_send(usb_controller_t* controller, const uint8_t* data, uint16_t len);
void usb_controller_on_tx_complete(void);
void usb_controller_on_rx_received(uint8_t* buf, uint32_t len);

#endif /* INC_USB_CONTROLLER_H_ */

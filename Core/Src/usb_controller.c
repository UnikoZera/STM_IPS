/*
 * usb_controller.c
 *
 *  Created on: 2026年3月18日
 *      Author: UnikoZera
 */


#include "usb_controller.h"
#include "usbd_cdc_if.h"

bool usb_busy = false;

// 返回0表示成功，1表示USB忙碌，2表示其他错误
uint8_t usb_transmit(uint8_t* buf, uint16_t len)
{
    return CDC_Transmit_FS(buf, len);
}

// 返回0表示成功，1表示USB忙碌，2表示其他错误
uint8_t usb_receive(uint8_t* buf, uint32_t len)
{
    return CDC_ReceiveCallback(buf, &len);
}
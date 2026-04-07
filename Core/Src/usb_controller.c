/*
 * usb_controller.c
 *
 *  Created on: 2026年3月18日
 *      Author: UnikoZera
 */


#include "usb_controller.h"
#include "usbd_cdc_if.h"
#include "usb_device.h"
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

usb_controller_t g_usb_controller; // sadly we have to use a global variable here becauseUSB库的回调函数无法传递上下文指针
static uint8_t s_usb_rx_cache[USB_RECEIVE_BUFFER_SIZE];

static uint16_t usb_min_u16(uint16_t a, uint16_t b)
{
    return (a < b) ? a : b;
}

static void usb_clear_tx_queue(usb_controller_t* controller)
{
    controller->tx_tail_ptr = NULL;
    controller->tx_remain_len = 0U;
}

static bool usb_start_tx_chunk(usb_controller_t* controller, uint8_t* buf, uint16_t len)
{
    uint8_t result;

    if ((controller == NULL) || (buf == NULL) || (len == 0U))
    {
        return false;
    }

    result = usb_transmit(buf, len);
    if (result == USBD_OK)
    {
        controller->usb_tx_active = true;
        controller->usb_tx_done = false;
        controller->last_tx_tick = HAL_GetTick();
        return true;
    }

    if (result != USBD_BUSY)
    {
        controller->usb_tx_active = false;
        controller->usb_tx_done = true;
        usb_clear_tx_queue(controller);
    }

    return false;
}

// 返回0表示成功，1表示USB忙碌，2表示其他错误
uint8_t usb_transmit(uint8_t* buf, uint16_t len)
{
    if ((buf == NULL) || (len == 0U))
    {
        return USBD_FAIL;
    }

    return CDC_Transmit_FS(buf, len);
}

// 返回0表示成功，1表示USB忙碌，2表示其他错误
uint8_t usb_receive(uint8_t* buf, uint32_t len)
{
    uint16_t copied_len;

    if ((buf == NULL) || (len == 0U) || (len > UINT16_MAX))
    {
        return USBD_FAIL;
    }

    copied_len = usb_controller_receive(&g_usb_controller, buf, (uint16_t)len);
    return (copied_len > 0U) ? USBD_OK : USBD_BUSY;
}

uint16_t usb_controller_receive(usb_controller_t* controller, uint8_t* buf, uint16_t len)
{
    uint16_t copy_len;

    if ((controller == NULL) || !controller->usb_enabled || (buf == NULL) || (len == 0U))
    {
        return 0U;
    }

    if (controller->rx_cached_len == 0U)
    {
        controller->usb_rx_done = false;
        return 0U;
    }

    copy_len = usb_min_u16(len, controller->rx_cached_len);
    memcpy(buf, s_usb_rx_cache, copy_len);

    if (copy_len < controller->rx_cached_len)
    {
        memmove(s_usb_rx_cache, &s_usb_rx_cache[copy_len], controller->rx_cached_len - copy_len);
    }

    controller->rx_cached_len = (uint16_t)(controller->rx_cached_len - copy_len);
    controller->rx_tail_ptr = &s_usb_rx_cache[controller->rx_cached_len];
    controller->usb_rx_done = (controller->rx_cached_len > 0U);
    controller->last_rx_tick = HAL_GetTick();

    return copy_len;
}

void usb_controller_init(usb_controller_t* controller)
{
    if (controller == NULL)
    {
        return;
    }

    controller->usb_enabled = true;
    controller->usb_tx_active = false;
    controller->usb_tx_done = true;
    controller->usb_rx_active = false;
    controller->usb_rx_done = false;
    controller->last_tx_tick = 0;
    controller->last_rx_tick = 0;
    controller->tx_tail_ptr = NULL;
    controller->rx_tail_ptr = &s_usb_rx_cache[0];
    controller->tx_remain_len = 0U;
    controller->rx_cached_len = 0U;
}

/**
 * @brief 在主循环中调用此函数来处理USB传输状态和超时等逻辑
 * 
 * @param controller 
 */
void usb_controller_task(usb_controller_t* controller)
{
    if (controller == NULL || !controller->usb_enabled)
    {
        return;
    }

    uint32_t now = HAL_GetTick();

    // 这里可以添加一些USB状态监测和超时处理的逻辑
    if (controller->usb_tx_active && (now - controller->last_tx_tick > USB_TIMEOUT_MS))
    {
        controller->usb_tx_active = false; // 超时，认为传输失败
        controller->usb_tx_done = true;
        usb_clear_tx_queue(controller);
    }

    if (controller->usb_rx_active && (now - controller->last_rx_tick > USB_TIMEOUT_MS))
    {
        controller->usb_rx_active = false; // 超时，认为接收失败
    }

    if (!controller->usb_tx_active && controller->usb_tx_done && (controller->tx_remain_len > 0U) && (controller->tx_tail_ptr != NULL))
    {
        uint16_t tx_len = usb_min_u16(controller->tx_remain_len, USB_SEND_BYTES_PER_CALL);

        if (usb_start_tx_chunk(controller, controller->tx_tail_ptr, tx_len))
        {
            controller->tx_tail_ptr += tx_len;
            controller->tx_remain_len = (uint16_t)(controller->tx_remain_len - tx_len);

            if (controller->tx_remain_len == 0U)
            {
                controller->tx_tail_ptr = NULL;
            }
        }
    }


}

void usb_controller_send(usb_controller_t* controller, uint8_t* buf, uint16_t len)
{
    uint16_t tx_len;

    if ((controller == NULL) || !controller->usb_enabled || (buf == NULL) || (len == 0U))
    {
        return; // safe check.
    }

    if (controller->usb_tx_active || !controller->usb_tx_done || (controller->tx_remain_len > 0U))
    {
        return; // safe check.
    }

    tx_len = usb_min_u16(len, USB_SEND_BYTES_PER_CALL);

    if (len > tx_len)
    {
        controller->tx_tail_ptr = buf + tx_len;
        controller->tx_remain_len = (uint16_t)(len - tx_len);
    }
    else
    {
        controller->tx_tail_ptr = NULL;
        controller->tx_remain_len = 0U;
    }

    if (!usb_start_tx_chunk(controller, buf, tx_len))
    {
        usb_clear_tx_queue(controller);
    }
}

void usb_controller_on_tx_complete(void)
{
    g_usb_controller.usb_tx_active = false;
    g_usb_controller.usb_tx_done = true;
    g_usb_controller.last_tx_tick = HAL_GetTick();
}

// callback for rx
void usb_controller_on_rx_received(uint8_t* buf, uint32_t len)
{
    uint16_t free_len;
    uint16_t copy_len;

    if (!g_usb_controller.usb_enabled || (buf == NULL) || (len == 0U))
    {
        return;
    }

    g_usb_controller.usb_rx_active = true;
    g_usb_controller.last_rx_tick = HAL_GetTick();

    if (len > UINT16_MAX)
    {
        len = UINT16_MAX;
    }

    free_len = (uint16_t)(USB_RECEIVE_BUFFER_SIZE - g_usb_controller.rx_cached_len);
    copy_len = usb_min_u16((uint16_t)len, free_len);

    if (copy_len > 0U)
    {
        memcpy(&s_usb_rx_cache[g_usb_controller.rx_cached_len], buf, copy_len);
        g_usb_controller.rx_cached_len = (uint16_t)(g_usb_controller.rx_cached_len + copy_len);
        g_usb_controller.rx_tail_ptr = &s_usb_rx_cache[g_usb_controller.rx_cached_len];
        g_usb_controller.usb_rx_done = true;
    }

    g_usb_controller.usb_rx_active = false;
    g_usb_controller.last_rx_tick = HAL_GetTick();
}
/*
 * usb_controller.c
 *
 *  Created on: 2026年3月18日
 *      Author: UnikoZera
 */

#include "usb_controller.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"
#include "usb_device.h"
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

usb_controller_t g_usb_controller;
static uint8_t s_usb_rx_cache[USB_RECEIVE_BUFFER_SIZE];

static uint16_t usb_min_u16(uint16_t a, uint16_t b)
{
    return (a < b) ? a : b;
}

static void usb_clear_tx_protocol_payload(usb_controller_t *controller)
{
    controller->tx_protocol_payload_ptr = NULL;
    controller->tx_protocol_payload_len = 0U;
    controller->tx_protocol_payload_pending = false;
}

static void usb_clear_tx_queue(usb_controller_t *controller)
{
    controller->tx_tail_ptr = NULL;
    controller->tx_remain_len = 0U;
    usb_clear_tx_protocol_payload(controller);
}

static void usb_clear_tx_pending(usb_controller_t *controller)
{
    controller->tx_pending_ptr = NULL;
    controller->tx_pending_len = 0U;
    controller->tx_pending_cmd = 0U;
}

static bool usb_prepare_protocol_tx(usb_controller_t *controller, uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    if (controller == NULL)
    {
        return false;
    }

    controller->tx_header[0] = USB_PROTOCOL_HEAD_BYTE0;
    controller->tx_header[1] = USB_PROTOCOL_HEAD_BYTE1;
    controller->tx_header[2] = cmd;
    controller->tx_header[3] = (uint8_t)(payload_len & 0xFFU);
    controller->tx_header[4] = (uint8_t)(payload_len >> 8); // 小端长度字段

    controller->tx_tail_ptr = controller->tx_header;
    controller->tx_remain_len = USB_PROTOCOL_HEADER_SIZE;
    
    // 如果 len 为 0，不设置 payload 阶段指针即可
    controller->tx_protocol_payload_ptr = payload;
    controller->tx_protocol_payload_len = payload_len;
    controller->tx_protocol_payload_pending = (payload_len > 0U);

    return true;
}

static void usb_reset_tx_state(usb_controller_t *controller, bool clear_pending)
{
    controller->usb_tx_active = false;
    controller->usb_tx_done = true;
    usb_clear_tx_queue(controller);

    if (clear_pending)
    {
        usb_clear_tx_pending(controller);
    }
}

static void usb_reset_rx_state(usb_controller_t *controller)
{
    controller->usb_rx_active = false;
    controller->usb_rx_done = false;
}

static bool usb_load_pending_as_active(usb_controller_t *controller)
{
    if ((controller == NULL) || (controller->tx_pending_ptr == NULL) || (controller->tx_pending_len == 0U))
    {
        return false;
    }

    if (!usb_prepare_protocol_tx(controller, controller->tx_pending_cmd, controller->tx_pending_ptr, controller->tx_pending_len))
    {
        usb_clear_tx_pending(controller);
        return false;
    }

    usb_clear_tx_pending(controller);
    return true;
}

static bool usb_is_stack_configured(void)
{
    if ((hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) || (hUsbDeviceFS.pClassData == NULL))
    {
        return false;
    }

    return true;
}

static bool usb_is_stack_tx_idle(void)
{
    USBD_CDC_HandleTypeDef *hcdc;

    if (!usb_is_stack_configured())
    {
        return false;
    }

    hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    return (hcdc->TxState == 0U);
}

static uint8_t usb_start_tx_chunk(usb_controller_t *controller)
{
    uint8_t result;
    uint16_t tx_len;

    if ((controller == NULL) || (controller->tx_tail_ptr == NULL) || (controller->tx_remain_len == 0U))
    {
        return USBD_FAIL;
    }

    tx_len = usb_min_u16(controller->tx_remain_len, USB_SEND_BYTES_PER_CALL);
    result = usb_transmit(controller->tx_tail_ptr, tx_len);

    if (result == USBD_OK)
    {
        controller->usb_tx_active = true;
        controller->usb_tx_done = false;
        controller->last_tx_tick = HAL_GetTick();

        controller->tx_tail_ptr += tx_len;
        controller->tx_remain_len = (uint16_t)(controller->tx_remain_len - tx_len);
        if (controller->tx_remain_len == 0U)
        {
            if (controller->tx_protocol_payload_pending &&
                (controller->tx_protocol_payload_ptr != NULL) &&
                (controller->tx_protocol_payload_len > 0U))
            {
                controller->tx_tail_ptr = controller->tx_protocol_payload_ptr;
                controller->tx_remain_len = controller->tx_protocol_payload_len;
            }
            else
            {
                controller->tx_tail_ptr = NULL;
            }

            usb_clear_tx_protocol_payload(controller);
        }

        return USBD_OK;
    }

    controller->usb_tx_active = false;
    controller->usb_tx_done = true;

    if (result != USBD_BUSY)
    {
        return USBD_FAIL;
    }

    return USBD_BUSY;
}

// 返回0表示成功，1表示USB忙碌，2表示其他错误
uint8_t usb_transmit(const uint8_t *buf, uint16_t len)
{
    if ((buf == NULL) || (len == 0U))
    {
        return USBD_FAIL;
    }

    return CDC_Transmit_FS((uint8_t *)buf, len);
}

// 返回0表示成功，1表示USB忙碌，2表示其他错误
uint8_t usb_receive(uint8_t *buf, uint32_t len)
{
    uint16_t copied_len;

    if ((buf == NULL) || (len == 0U) || (len > UINT16_MAX))
    {
        return USBD_FAIL;
    }

    copied_len = usb_controller_receive(&g_usb_controller, buf, (uint16_t)len);
    return (copied_len > 0U) ? USBD_OK : USBD_BUSY;
}

uint16_t usb_controller_receive(usb_controller_t *controller, uint8_t *buf, uint16_t len)
{
    uint16_t copy_len = 0;
    uint32_t primask;

    if ((controller == NULL) || !controller->usb_enabled || (buf == NULL) || (len == 0U))
    {
        return 0U;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    uint16_t head = controller->rx_head;
    uint16_t tail = controller->rx_tail;
    if (primask == 0U)
    {
        __enable_irq();
    }

    // 计算可用数据量
    uint16_t available = (head >= tail) ? (head - tail) : (USB_RECEIVE_BUFFER_SIZE - tail + head);
    copy_len = usb_min_u16(len, available);

    if (copy_len > 0U)
    {
        uint16_t first_chunk = usb_min_u16(copy_len, USB_RECEIVE_BUFFER_SIZE - tail);
        memcpy(buf, &s_usb_rx_cache[tail], first_chunk);

        if (first_chunk < copy_len)
        {
            memcpy(buf + first_chunk, &s_usb_rx_cache[0], copy_len - first_chunk);
        }

        primask = __get_PRIMASK();
        __disable_irq();
        controller->rx_tail = (tail + copy_len) % USB_RECEIVE_BUFFER_SIZE;
        if (primask == 0U)
        {
            __enable_irq();
        }

        /* 腾出空间后，检查是否需要重新开启底层的接收 */
        // 但是这个函数已经在中断里调用了，所以理论上不应该有并发问题，暂时不加锁了。
        uint16_t new_free_space = usb_controller_get_rx_free_space();
        if (new_free_space >= 64U) // 以USB FS的最大包长为阈值，避免过早开启底层接收导致频繁NAK
        {
            USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
            if (hcdc != NULL && hcdc->RxState == 0) // 如果处于非接收状态，则恢复
            {
                USBD_CDC_ReceivePacket(&hUsbDeviceFS);
            }
        }
    }

    if (copy_len > 0U)
    {
        controller->usb_rx_done = (controller->rx_head != controller->rx_tail);
        controller->last_rx_tick = HAL_GetTick();
    }

    return copy_len;
}

uint16_t usb_controller_get_rx_free_space(void)
{
    uint16_t head = g_usb_controller.rx_head;
    uint16_t tail = g_usb_controller.rx_tail;
    return (tail > head) ? (tail - head - 1) : (USB_RECEIVE_BUFFER_SIZE - head + tail - 1);
}

void usb_controller_init(usb_controller_t *controller)
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
    controller->tx_remain_len = 0U;
    controller->tx_pending_ptr = NULL;
    controller->tx_pending_len = 0U;
    controller->tx_pending_cmd = 0U;
    controller->tx_protocol_payload_ptr = NULL;
    controller->tx_protocol_payload_len = 0U;
    controller->tx_protocol_payload_pending = false;
    controller->rx_head = 0U;
    controller->rx_tail = 0U;
}

/**
 * @brief 在主循环中调用此函数来处理USB传输状态和超时等逻辑
 *
 * @param controller USB控制器状态结构体指针
 * @note 该函数会根据当前USB状态自动处理发送队列、重试
 */
void usb_controller_task(usb_controller_t *controller)
{
    uint32_t now;

    if (controller == NULL || !controller->usb_enabled)
    {
        return;
    }

    // 仅在USB未配置时复位控制器状态，避免大流量发送下误超时导致TxState长期BUSY。
    if (!usb_is_stack_configured())
    {
        usb_reset_tx_state(controller, true);
        usb_reset_rx_state(controller);
        return;
    }

    now = HAL_GetTick();

    // 某些场景下可能丢失TX完成回调，这里依据底层TxState做自恢复。
    if (controller->usb_tx_active && usb_is_stack_tx_idle())
    {
        controller->usb_tx_active = false;
        controller->usb_tx_done = true;
        controller->last_tx_tick = now;
    }

    // 发送活跃过久仍无进展，丢弃当前发送任务避免整个链路长期卡死。
    if (controller->usb_tx_active && ((uint32_t)(now - controller->last_tx_tick) > USB_TX_STUCK_TIMEOUT_MS))
    {
        USBD_CDC_HandleTypeDef *hcdc;
        usb_reset_tx_state(controller, false);
        controller->last_tx_tick = now;

        // 软恢复底层端点状态
        hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
        if (hcdc != NULL)
        {
            USBD_LL_FlushEP(&hUsbDeviceFS, CDC_IN_EP);
            hcdc->TxState = 0;
        }
    }

    if (!controller->usb_tx_active && (controller->tx_remain_len == 0U) && (controller->tx_tail_ptr == NULL))
    {
        usb_load_pending_as_active(controller);
    }

    if (!controller->usb_tx_active && (controller->tx_remain_len > 0U) && (controller->tx_tail_ptr != NULL))
    {
        if ((uint32_t)(now - controller->last_tx_tick) < USB_TX_RETRY_INTERVAL_MS)
        {
            return;
        }

        uint8_t tx_result = usb_start_tx_chunk(controller);

        if (tx_result == USBD_BUSY)
        {
            controller->last_tx_tick = now;
        }

        if (tx_result == USBD_FAIL)
        {
            usb_clear_tx_queue(controller);
            controller->last_tx_tick = now;
        }
    }
}

usb_send_status_t usb_controller_send(usb_controller_t *controller, uint8_t cmd, const uint8_t *buf, uint16_t len)
{
    uint8_t tx_result;

    if ((controller == NULL) || !controller->usb_enabled)
    {
        return USB_SEND_BUSY_REJECTED; // safe check.
    }

    // 允许 len 为 0 (仅发送无负载协议头指令)
    if (len > 0U && buf == NULL)
    {
        return USB_SEND_BUSY_REJECTED; 
    }

    if (controller->usb_tx_active || (controller->tx_remain_len > 0U) || (controller->tx_tail_ptr != NULL))
    {
        if (controller->tx_pending_len == 0U)
        {
            controller->tx_pending_ptr = buf;
            controller->tx_pending_len = len;
            controller->tx_pending_cmd = cmd;
            return USB_SEND_QUEUED;
        }
        else if (len <= controller->tx_pending_len)
        {
            controller->tx_pending_ptr = buf;
            controller->tx_pending_len = len;
            controller->tx_pending_cmd = cmd;
            return USB_SEND_DROPPED_PREVIOUS;
        }
        return USB_SEND_BUSY_REJECTED; // safe check.
    }

    usb_clear_tx_pending(controller);
    if (!usb_prepare_protocol_tx(controller, cmd, buf, len))
    {
        return USB_SEND_BUSY_REJECTED;
    }

    tx_result = usb_start_tx_chunk(controller);
    if (tx_result == USBD_BUSY)
    {
        controller->last_tx_tick = HAL_GetTick();
    }

    if (tx_result == USBD_FAIL)
    {
        usb_clear_tx_queue(controller);
        return USB_SEND_BUSY_REJECTED;
    }

    return USB_SEND_OK;
}

// callback for tx complete
void usb_controller_on_tx_complete(void)
{
    g_usb_controller.usb_tx_active = false;
    g_usb_controller.usb_tx_done = true;
    g_usb_controller.last_tx_tick = HAL_GetTick();
}

// callback for rx
void usb_controller_on_rx_received(uint8_t *buf, uint32_t len)
{
    uint16_t free_len;
    uint16_t copy_len;
    uint16_t head;
    uint16_t tail;

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

    head = g_usb_controller.rx_head;
    tail = g_usb_controller.rx_tail;

    // 计算可用空间 (若等于则是空，差1是满)
    free_len = (tail > head) ? (tail - head - 1) : (USB_RECEIVE_BUFFER_SIZE - head + tail - 1);
    copy_len = usb_min_u16((uint16_t)len, free_len);

    if (copy_len > 0U)
    {
        uint16_t first_chunk = usb_min_u16(copy_len, USB_RECEIVE_BUFFER_SIZE - head);
        memcpy(&s_usb_rx_cache[head], buf, first_chunk);

        if (first_chunk < copy_len)
        {
            memcpy(&s_usb_rx_cache[0], buf + first_chunk, copy_len - first_chunk);
        }
        g_usb_controller.rx_head = (head + copy_len) % USB_RECEIVE_BUFFER_SIZE;
    }

    g_usb_controller.usb_rx_done = (g_usb_controller.rx_head != g_usb_controller.rx_tail);
    g_usb_controller.usb_rx_active = false;
    g_usb_controller.last_rx_tick = HAL_GetTick();
}

/*
 * lcd_mjpeg.c
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 *
 *  MJPEG 视频解码实现
 *
 *  依赖: lcd.h (lcd_write_ptr, lcd_dma_busy, LCD_W, LCD_H)
 *        w25q_controller.h (w25q_fast_read_data, w25q_dma_is_busy, w25q_dma_task)
 *        picojpeg.h (pjpeg_decode_init, pjpeg_decode_mcu, pjpeg_image_info_t)
 *
 *  工作流程:
 *    1. lcd_play_mjpeg_video() 每调用一次处理一帧
 *    2. 首次调用/位置变化时读取 14B 文件头, 初始化 picojpeg
 *    3. 读取 4B 帧大小前缀 + JPEG 数据
 *    4. 循环 pjpeg_decode_mcu() 解码所有 MCU
 *    5. 每解码一个 MCU，将 RGB 写到 lcd_write_ptr 缓冲区
 *    6. 播完最后一帧回到文件头循环
 */

#include "lcd.h"             /* lcd_write_ptr, lcd_dma_busy, LCD_W, LCD_H */
#include "lcd_mjpeg.h"       /* mjpeg_state_t, MJPEG_MAGIC, 错误码 */
#include "w25q_controller.h" /* w25q_fast_read_data, w25q_dma_* */
#include "picojpeg.h"        /* pjpeg_* */

#include <string.h> /* memcpy */

static mjpeg_state_t s_mjpeg = {0};

int8_t lcd_mjpeg_last_error(void)
{
    return s_mjpeg.last_error;
}

const mjpeg_state_t *lcd_mjpeg_get_state(void)
{
    return &s_mjpeg;
}

#define MJPEG_CACHE_SIZE 512
static uint8_t s_flash_cache[MJPEG_CACHE_SIZE];
static uint32_t s_cache_addr = 0xFFFFFFFF; // 当前缓存对应的 Flash 起始地址
static uint32_t s_cache_len = 0;           // 当前缓存的有效长度

static unsigned char mjpeg_read_cb(unsigned char *pBuf, unsigned char buf_size,
                                   unsigned char *pBytesActuallyRead,
                                   void *pCallback_data)
{
    uint32_t *pOffset = (uint32_t *)pCallback_data;
    uint32_t remaining = s_mjpeg.frame_size - *pOffset;
    uint32_t to_read = (buf_size > remaining) ? remaining : buf_size;

    if (to_read > 0)
    {
        uint32_t target_addr = s_mjpeg.frame_data_addr + *pOffset;

        // 检查请求的数据是否完全在 Cache 中
        if (s_cache_addr != 0xFFFFFFFF && target_addr >= s_cache_addr &&
            (target_addr + to_read) <= (s_cache_addr + s_cache_len))
        {
            uint32_t offset_in_cache = target_addr - s_cache_addr;
            memcpy(pBuf, &s_flash_cache[offset_in_cache], to_read);
        }
        else
        {
            // Cache Miss: 触发大块读取
            uint32_t chunk_to_read = remaining;
            if (chunk_to_read > MJPEG_CACHE_SIZE)
                chunk_to_read = MJPEG_CACHE_SIZE;

            w25q_fast_read_data(target_addr, s_flash_cache, (uint16_t)chunk_to_read);
            while (w25q_dma_is_busy())
                w25q_dma_task(); // 等待 DMA 完成

            s_cache_addr = target_addr;
            s_cache_len = chunk_to_read;

            memcpy(pBuf, s_flash_cache, to_read);
        }
        *pOffset += to_read;
    }

    *pBytesActuallyRead = (unsigned char)to_read;
    return 0;
}

/**
 * @brief 前进到下一帧
 *
 * 更新 cur_frame_idx，移动 current_frame_pos 到下一帧的 size prefix 位置。
 * 如果已播完最后一帧，则回到文件头循环。
 */
#define MJPEG_ADVANCE_FRAME()                                    \
    do                                                           \
    {                                                            \
        s_mjpeg.cur_frame_idx++;                                 \
        s_mjpeg.current_frame_pos =                              \
            s_mjpeg.frame_data_addr + s_mjpeg.frame_size;        \
        if (s_mjpeg.cur_frame_idx >= s_mjpeg.frame_count ||      \
            s_mjpeg.current_frame_pos >= s_mjpeg.end_addr)       \
        {                                                        \
            s_mjpeg.cur_frame_idx = 0;                           \
            s_mjpeg.current_frame_pos = s_mjpeg.start_addr + 14; \
        }                                                        \
    } while (0)

void lcd_play_mjpeg_video(int16_t x, int16_t y, int16_t width, int16_t height,
                          uint32_t w25q_start_addr, uint32_t w25q_end_addr)
{
    if (lcd_dma_busy)
    {
        s_mjpeg.last_error = MJPEG_ERR_DMA_BUSY;
        return;
    }

    /* ---- 判断是否为新文件/位置变化 ---- */
    bool is_new = (!s_mjpeg.active ||
                   s_mjpeg.lcd_x != x || s_mjpeg.lcd_y != y ||
                   s_mjpeg.start_addr != w25q_start_addr ||
                   s_mjpeg.end_addr != w25q_end_addr);

    /* ---- 初始化：读取文件头 ---- */
    if (is_new)
    {
        uint8_t hdr[14];
        w25q_fast_read_data(w25q_start_addr, hdr, 14);
        while (w25q_dma_is_busy())
            w25q_dma_task();

        uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);

        if (magic != MJPEG_MAGIC)
        {
            s_mjpeg.last_error = MJPEG_ERR_BAD_MAGIC;
            return;
        }

        s_mjpeg.active = true;
        s_mjpeg.lcd_x = x;
        s_mjpeg.lcd_y = y;
        s_mjpeg.start_addr = w25q_start_addr;
        s_mjpeg.end_addr = w25q_end_addr;
        s_mjpeg.frame_count = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);

        /* 用文件头中的尺寸覆盖传入参数 */
        {
            int16_t hdr_w = (int16_t)((uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8));
            int16_t hdr_h = (int16_t)((uint16_t)hdr[8] | ((uint16_t)hdr[9] << 8));
            if (hdr_w > 0 && hdr_h > 0)
            {
                if (hdr_w != width || hdr_h != height)
                {
                    // error handle.
                }
                s_mjpeg.width = hdr_w;
                s_mjpeg.height = hdr_h;
                /* 覆盖局部变量，后续裁剪使用正确尺寸 */
                width = hdr_w;
                height = hdr_h;
            }
            else
            {
                s_mjpeg.width = width;
                s_mjpeg.height = height;
            }
        }

        s_mjpeg.cur_frame_idx = 0;
        s_mjpeg.current_frame_pos = w25q_start_addr + 14;
        s_mjpeg.last_error = 0;
    }

    /* ---- 空帧检查 ---- */
    if (s_mjpeg.frame_count == 0)
    {
        s_mjpeg.last_error = MJPEG_ERR_ZERO_FRAMES;
        return;
    }

    /* ---- 读取当前帧的 4B 大小前缀 ---- */
    {
        uint8_t sz_buf[4];
        w25q_fast_read_data(s_mjpeg.current_frame_pos, sz_buf, 4);
        while (w25q_dma_is_busy())
            w25q_dma_task();

        uint32_t frame_size = (uint32_t)sz_buf[0] | ((uint32_t)sz_buf[1] << 8) | ((uint32_t)sz_buf[2] << 16) | ((uint32_t)sz_buf[3] << 24);

        /* 截断保护 */
        if (s_mjpeg.current_frame_pos + 4 + frame_size > s_mjpeg.end_addr)
        {
            uint32_t avail = s_mjpeg.end_addr - s_mjpeg.current_frame_pos - 4;
            if ((int32_t)avail < 0)
                avail = 0;
            frame_size = avail;
        }

        s_mjpeg.frame_size = frame_size;
        s_mjpeg.frame_data_addr = s_mjpeg.current_frame_pos + 4;

        if (frame_size == 0)
        {
            MJPEG_ADVANCE_FRAME();
            return;
        }

        /* 保存前 44 字节用于调试 */
        uint32_t dump_bytes = frame_size < 44 ? frame_size : 44;
        w25q_fast_read_data(s_mjpeg.frame_data_addr, s_mjpeg.frame_dump, (uint16_t)dump_bytes);
        while (w25q_dma_is_busy())
            w25q_dma_task();
    }

    /* ---- picojpeg 解码 ---- */
    pjpeg_image_info_t jinfo;
    uint32_t read_offset = 0;

    unsigned char ret = pjpeg_decode_init(&jinfo, mjpeg_read_cb,
                                          &read_offset, 0);
    s_mjpeg.pjpeg_ret = ret;

    if (ret != 0)
    {
        s_mjpeg.last_error = MJPEG_ERR_DECODE_INIT;
        MJPEG_ADVANCE_FRAME();
        return;
    }

    if (jinfo.m_comps != 3)
    {
        s_mjpeg.last_error = MJPEG_ERR_NOT_3_COMP;
        MJPEG_ADVANCE_FRAME();
        return;
    }

    /* ---- 逐 MCU 解码 ---- */
    int mcu_x = 0;
    int mcu_y = 0;
    const int mcu_w = jinfo.m_MCUWidth;  /* 8 or 16 */
    const int mcu_h = jinfo.m_MCUHeight; /* 8 or 16 */
    const int blocks_per_mcu = (mcu_w / 8) * (mcu_h / 8);
    const int img_w = s_mjpeg.width;
    const int img_h = s_mjpeg.height;

    while ((ret = pjpeg_decode_mcu()) == 0)
    {
        for (int blk = 0; blk < blocks_per_mcu; blk++)
        {
            int blk_col = blk % (mcu_w / 8);
            int blk_row = blk / (mcu_w / 8);
            int blk_origin_x = mcu_x + blk_col * 8;
            int blk_origin_y = mcu_y + blk_row * 8;

            for (int py = 0; py < 8; py++)
            {
                int img_y = blk_origin_y + py;
                if (img_y >= img_h)
                    continue;

                int16_t sy = s_mjpeg.lcd_y + img_y;
                if (sy < 0 || sy >= LCD_H)
                    continue;

                // 预计算该行的基础指针，省去内层循环的乘法开销
                uint16_t *row_ptr = &lcd_write_ptr[(uint32_t)sy * LCD_W];

                for (int px = 0; px < 8; px++)
                {
                    int img_x = blk_origin_x + px;
                    if (img_x >= img_w)
                        continue;

                    int16_t sx = s_mjpeg.lcd_x + img_x;
                    if (sx < 0 || sx >= LCD_W)
                        continue;

                    int idx = blk * 64 + py * 8 + px;
                    uint8_t r = jinfo.m_pMCUBufR[idx];
                    uint8_t g = jinfo.m_pMCUBufG[idx];
                    uint8_t b = jinfo.m_pMCUBufB[idx];

                    // 直接组装为适合 SPI DMA 的小端 RGB565 (低字节在前)
                    // 标准 RGB565: [R4 R3 R2 R1 R0 G5 G4 G3] [G2 G1 G0 B4 B3 B2 B1 B0]
                    uint8_t hi = (r & 0xF8) | (g >> 5);
                    uint8_t lo = ((g & 0x1C) << 3) | (b >> 3);

                    row_ptr[sx] = (uint16_t)((lo << 8) | hi);
                }
            }
        }

        /* 前进到下一个 MCU */
        mcu_x += mcu_w;
        if (mcu_x >= img_w)
        {
            mcu_x = 0;
            mcu_y += mcu_h;
        }
    }

    if (ret != PJPG_NO_MORE_BLOCKS)
    {
        s_mjpeg.pjpeg_ret = ret;
        /* 解码出错仍尝试前进到下一帧 */
    }

    /* ---- 前进到下一帧 ---- */
    MJPEG_ADVANCE_FRAME();
}

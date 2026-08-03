/*
 * lcd_mjpeg.c
 *
 *  Created on: 2026年4月5日
 *      Author: UnikoZera
 *
 *  MJPEG 视频解码实现 (优化版)
 *
 *  依赖: lcd.h (lcd_write_ptr, lcd_dma_busy, LCD_W, LCD_H)
 *        w25q_controller.h (w25q_fast_read_data, w25q_dma_busy, w25q_dma_task)
 *        picojpeg.h (pjpeg_decode_init, pjpeg_decode_mcu, pjpeg_image_info_t)
 *
 *  优化项:
 *    1. Flash 缓存 512B -> 1024B, 减少 DMA 读取次数
 *    2. MCU 解码热循环: 非裁剪快速路径, 跳过逐像素边界检查
 *    3. RGB565 内联函数, 编译器自动内联优化
 *    4. 本地指针缓存 MCU R/G/B buf, 避免重复解引用
 *    5. 移除每帧调试 dump 读取, 节省一次 Flash 访问
 *    6. 快速路径内层循环添加 img_x < img_w 裁剪, 支持任意分辨率
 */

#include "lcd.h"
#include "lcd_mjpeg.h"
#include "w25q_controller.h"
#include "picojpeg.h"

#include <string.h>

static mjpeg_state_t s_mjpeg = {0};

int8_t lcd_mjpeg_last_error(void)
{
    return s_mjpeg.last_error;
}

const mjpeg_state_t *lcd_mjpeg_get_state(void)
{
    return &s_mjpeg;
}

/* ---- Flash 块缓存：缓存一个已通过 CRC-16 校验的 1022B 数据块 ---- */
#define MJPEG_CACHE_SIZE W25Q_CRC_BLOCK_DATA_SIZE
#define MJPEG_FAIL_RETRY_INTERVAL_MS 500U /* 解码失败后降频重试间隔，避免文件损坏时全速空转 */
static uint8_t s_flash_cache[MJPEG_CACHE_SIZE];
static uint32_t s_cache_block = 0xFFFFFFFF; /* 当前缓存的原始块号 */

/**
 * @brief 重置 MJPEG 播放器状态（文件内容变更后调用，强制下次从第一帧重新初始化）
 */
void lcd_mjpeg_reset(void)
{
    s_mjpeg.active = 0;
    s_mjpeg.last_fail_tick = 0;
    s_mjpeg.fail_count = 0;
    s_cache_block = 0xFFFFFFFFUL; /* Flash 块缓存失效 */
}

static unsigned char mjpeg_read_cb(unsigned char *pBuf, unsigned char buf_size,
                                   unsigned char *pBytesActuallyRead,
                                   void *pCallback_data)
{
    uint32_t *pOffset   = (uint32_t *)pCallback_data;
    uint32_t  remaining = s_mjpeg.frame_size - *pOffset;
    if (remaining == 0)
    {
        *pBytesActuallyRead = 0;
        return 0;
    }

    uint32_t to_read = buf_size;
    if (to_read > remaining)
        to_read = remaining;
    uint32_t req = to_read;

    /* 帧数据区在文件内的原始偏移（帧数据区起点不一定是 1024 块边界） */
    uint32_t frame_file_off = s_mjpeg.frame_data_addr - s_mjpeg.start_addr;

    /* 按文件内原始偏移分块读取（每块读一次物理块 + CRC-16 校验） */
    uint32_t off = *pOffset;
    while (to_read > 0)
    {
        uint32_t file_off = frame_file_off + off;
        uint32_t blk   = file_off / W25Q_CRC_BLOCK_DATA_SIZE;
        uint32_t in    = file_off % W25Q_CRC_BLOCK_DATA_SIZE;
        uint32_t avail = W25Q_CRC_BLOCK_DATA_SIZE - in;
        uint32_t want  = (to_read > avail) ? avail : to_read;

        if (s_cache_block != blk)
        {
            if (!w25q_crc_read(s_mjpeg.start_addr,
                                blk * W25Q_CRC_BLOCK_DATA_SIZE,
                                s_flash_cache, W25Q_CRC_BLOCK_DATA_SIZE, 0U, true))
            {
                *pBytesActuallyRead = 0; /* CRC 校验失败：返回 0 让 picojpeg 安全结束 */
                return 0;
            }
            s_cache_block = blk;
        }

        memcpy(pBuf, &s_flash_cache[in], want);
        pBuf += want;
        off += want;
        to_read -= want;
    }

    *pOffset += req;
    *pBytesActuallyRead = (unsigned char)req;
    return 0;
}

/**
 * @brief 回绕到第一帧，重置 Flash 缓存
 */
#define MJPEG_REWIND()                                           \
    do                                                           \
    {                                                            \
        s_mjpeg.cur_frame_idx = 0;                               \
        s_mjpeg.current_frame_pos = s_mjpeg.start_addr + 14;     \
        s_cache_block = 0xFFFFFFFFUL;                            \
    } while (0)

/**
 * @brief 前进到下一帧；到达末端时自动回绕
 *
 * 更新 cur_frame_idx 和 current_frame_pos。
 * 边界条件检测：帧数到头 / 地址超过 end_addr / 剩余空间不足 4B 前缀 → 回绕。
 */
#define MJPEG_ADVANCE_FRAME()                                    \
    do                                                           \
    {                                                            \
        s_mjpeg.cur_frame_idx++;                                 \
        s_mjpeg.current_frame_pos =                              \
            s_mjpeg.frame_data_addr + s_mjpeg.frame_size;        \
        if (s_mjpeg.cur_frame_idx >= s_mjpeg.frame_count ||      \
            s_mjpeg.current_frame_pos >= s_mjpeg.end_addr ||     \
            (s_mjpeg.current_frame_pos + 4U) > s_mjpeg.end_addr) \
        {                                                        \
            MJPEG_REWIND();                                      \
        }                                                        \
    } while (0)

/**
 * @brief 将 RGB888 转换为 SPI DMA 字节序的 RGB565
 *
 * SPI DMA 发送 uint16_t 时低字节先发出。
 * LCD 需要的顺序是 [R4..R0 G5..G3] [G2..G0 B4..B4]，
 * 因此需要将标准 RGB565 的高低字节交换后存入 uint16_t。
 *
 * @param r 红色分量 (0-255)
 * @param g 绿色分量 (0-255)
 * @param b 蓝色分量 (0-255)
 * @return 字节交换后的 RGB565 值
 */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t hi = (r & 0xF8) | (g >> 5);
    uint8_t lo = ((g & 0x1C) << 3) | (b >> 3);
    return (uint16_t)((lo << 8) | hi);
}

void lcd_play_mjpeg_video(int16_t x, int16_t y, int16_t width, int16_t height,
                          uint32_t w25q_start_addr, uint32_t w25q_end_addr)
{
    if (lcd_dma_busy)
    {
        s_mjpeg.last_error = MJPEG_ERR_DMA_BUSY;
        return;
    }

    /* ---- 仅文件源变化才重置进度；x/y 动画移动不得打断播放 ---- */
    bool is_new = (!s_mjpeg.active ||
                   s_mjpeg.start_addr != w25q_start_addr ||
                   s_mjpeg.end_addr != w25q_end_addr);

    /* ---- 初始化: 读取文件头 ---- */
    if (is_new)
    {
        uint8_t hdr[14];
        if (!w25q_crc_read(w25q_start_addr, 0, hdr, 14, 0U, true))
        {
            s_mjpeg.last_error = MJPEG_ERR_BAD_MAGIC;
            return;
        }

        /* 校验 magic，避免脏读把错误容器当 MJPEG */
        if (!(hdr[0] == 'M' && hdr[1] == 'J' && hdr[2] == 'P' && hdr[3] == 'G'))
        {
            s_mjpeg.last_error = MJPEG_ERR_BAD_MAGIC;
            s_mjpeg.active = 0;
            return;
        }

        uint16_t hdr_count = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
        uint16_t hdr_w     = (uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8);
        uint16_t hdr_h     = (uint16_t)hdr[8] | ((uint16_t)hdr[9] << 8);

        s_mjpeg.active      = 1;
        s_mjpeg.start_addr  = w25q_start_addr;
        s_mjpeg.end_addr    = w25q_end_addr;
        s_mjpeg.frame_count = hdr_count;

        width  = hdr_w;
        height = hdr_h;

        if (hdr_w > 0 && hdr_h > 0)
        {
            s_mjpeg.width  = hdr_w;
            s_mjpeg.height = hdr_h;
        }
        else
        {
            s_mjpeg.width  = width;
            s_mjpeg.height = height;
        }

        s_mjpeg.cur_frame_idx     = 0;
        s_mjpeg.current_frame_pos = w25q_start_addr + 14;
        s_mjpeg.last_error        = 0;
        s_mjpeg.last_fail_tick    = 0; /* 新文件：清除失败降频 */
        s_mjpeg.fail_count        = 0;
        s_cache_block = 0xFFFFFFFFUL;
    }

    /* 失败降频：仅连续失败 ≥3 帧才降频（单次毛刺/坏帧不停顿，保证播放完整） */
    if (s_mjpeg.active && s_mjpeg.last_fail_tick != 0U &&
        s_mjpeg.fail_count >= 3U &&
        (uint32_t)(HAL_GetTick() - s_mjpeg.last_fail_tick) < MJPEG_FAIL_RETRY_INTERVAL_MS)
    {
        return;
    }

    /* 每帧更新绘制原点（动画改 x/y 时生效，不重置解码进度） */
    s_mjpeg.lcd_x = x;
    s_mjpeg.lcd_y = y;

    /* ---- 空帧检查 ---- */
    if (s_mjpeg.frame_count == 0)
    {
        s_mjpeg.last_error = MJPEG_ERR_ZERO_FRAMES;
        return;
    }

    /* ---- 读取当前帧的 4B 大小前缀 ---- */
    {
        /* 不足 4B 前缀：文件过小 / 已播完边界 → 回绕（下一帧从第一帧） */
        if ((s_mjpeg.current_frame_pos + 4U) > s_mjpeg.end_addr)
        {
            if (s_mjpeg.cur_frame_idx == 0)
            {
                s_mjpeg.last_error = MJPEG_ERR_BAD_MAGIC;
                s_mjpeg.fail_count++;
                s_mjpeg.last_fail_tick = HAL_GetTick();
                return; /* 文件过小，无帧可播 */
            }
            s_mjpeg.last_error = MJPEG_ERR_BAD_MAGIC;
            MJPEG_REWIND();
            return; /* 播完：下一帧从第一帧开始 */
        }

        uint8_t sz_buf[4];
        if (!w25q_crc_read(s_mjpeg.start_addr,
                        s_mjpeg.current_frame_pos - s_mjpeg.start_addr, sz_buf, 4, 0U, true))
        {
            s_mjpeg.last_error = MJPEG_ERR_BAD_MAGIC;
            s_mjpeg.fail_count++;
            s_mjpeg.last_fail_tick = HAL_GetTick();
            return;
        }

        uint32_t frame_size = (uint32_t)sz_buf[0] |
                              ((uint32_t)sz_buf[1] << 8) |
                              ((uint32_t)sz_buf[2] << 16) |
                              ((uint32_t)sz_buf[3] << 24);

        /*
         * 不完整帧 / 脏 size：禁止“截断后继续解码+按截断长度前进”。
         * 截断前进会把下一帧指针落在 JPEG 中部，造成持续花屏直到偶然对齐。
         */
        if (frame_size == 0U ||
            (s_mjpeg.current_frame_pos + 4U + frame_size) > s_mjpeg.end_addr)
        {
            s_mjpeg.last_error = MJPEG_ERR_BAD_MAGIC;
            if (s_mjpeg.cur_frame_idx == 0)
            {
                s_mjpeg.fail_count++;
                s_mjpeg.last_fail_tick = HAL_GetTick();
                return; /* 第一帧 size 脏：数据损坏 */
            }
            MJPEG_REWIND();
            return; /* 帧边界错乱：回绕，下一帧从第一帧重试 */
        }

        s_mjpeg.frame_size      = frame_size;
        s_mjpeg.frame_data_addr = s_mjpeg.current_frame_pos + 4;
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
        /* 解码初始化失败：跳过本帧继续（不回绕开头，保证播放完整） */
        s_mjpeg.fail_count++;
        s_mjpeg.last_fail_tick = HAL_GetTick();
        MJPEG_ADVANCE_FRAME();
        return;
    }

    if (jinfo.m_comps != 3)
    {
        s_mjpeg.last_error = MJPEG_ERR_NOT_3_COMP;
        s_mjpeg.fail_count++;
        s_mjpeg.last_fail_tick = HAL_GetTick();
        MJPEG_ADVANCE_FRAME();
        return;
    }

    /* ---- MCU 解码 (优化核心热循环) ---- */
    const int mcu_w          = jinfo.m_MCUWidth;   /* 8 or 16 */
    const int mcu_h          = jinfo.m_MCUHeight;  /* 8 or 16 */
    const int blocks_per_mcu = (mcu_w / 8) * (mcu_h / 8);
    const int mcu_w_blocks   = mcu_w / 8;
    const int img_w          = s_mjpeg.width;
    const int img_h          = s_mjpeg.height;
    const int16_t lcd_x      = s_mjpeg.lcd_x;
    const int16_t lcd_y      = s_mjpeg.lcd_y;

    /* LCD 映射范围（int32 避免 lcd_x + img_w 溢出 int16 导致 fast_path 误判） */
    const int32_t lcd_x_end = (int32_t)lcd_x + img_w;
    const int32_t lcd_y_end = (int32_t)lcd_y + img_h;

    /* 判断图像是否完全在 LCD 可见区域内 (快速路径) */
    const bool fast_path = (lcd_x >= 0 && lcd_y >= 0 &&
                            lcd_x_end <= LCD_W && lcd_y_end <= LCD_H &&
                            img_w > 0 && img_h > 0);

    /* MCU R/G/B 缓冲区指针 (局部变量, 避免重复解引用结构体) */
    const unsigned char *mcu_r = jinfo.m_pMCUBufR;
    const unsigned char *mcu_g = jinfo.m_pMCUBufG;
    const unsigned char *mcu_b = jinfo.m_pMCUBufB;

    int mcu_x = 0;
    int mcu_y = 0;

    if (fast_path)
    {
        /* ---- 快速路径: 无 LCD 裁剪, 仅需图像边界检查 ---- */
        while ((ret = pjpeg_decode_mcu()) == 0)
        {
            for (int blk = 0; blk < blocks_per_mcu; blk++)
            {
                int blk_origin_x = mcu_x + (blk % mcu_w_blocks) * 8;
                int blk_origin_y = mcu_y + (blk / mcu_w_blocks) * 8;
                int base_idx     = blk * 64;

                uint16_t *dst_row = &lcd_write_ptr[(uint32_t)(lcd_y + blk_origin_y) * LCD_W
                                                   + (uint32_t)(lcd_x + blk_origin_x)];

                for (int py = 0; py < 8; py++)
                {
                    int img_y = blk_origin_y + py;
                    if (img_y >= img_h)
                        continue;

                    uint16_t *dst = dst_row;
                    int idx = base_idx + py * 8;

                    for (int px = 0; px < 8; px++)
                    {
                        int img_x = blk_origin_x + px;
                        if (img_x < img_w)
                            dst[px] = rgb565(mcu_r[idx], mcu_g[idx], mcu_b[idx]);
                        idx++;
                    }

                    dst_row += LCD_W;
                }
            }

            mcu_x += mcu_w;
            if (mcu_x >= img_w)
            {
                mcu_x = 0;
                mcu_y += mcu_h;
            }
        }
    }
    else
    {
        /* ---- 通用路径: 完整边界检查 (LCD + 图像) ---- */
        while ((ret = pjpeg_decode_mcu()) == 0)
        {
            for (int blk = 0; blk < blocks_per_mcu; blk++)
            {
                int blk_origin_x = mcu_x + (blk % mcu_w_blocks) * 8;
                int blk_origin_y = mcu_y + (blk / mcu_w_blocks) * 8;
                int base_idx     = blk * 64;

                for (int py = 0; py < 8; py++)
                {
                    int img_y = blk_origin_y + py;
                    if (img_y >= img_h)
                        continue;

                    int16_t sy = lcd_y + img_y;
                    if (sy < 0 || sy >= LCD_H)
                        continue;

                    uint16_t *row_ptr = &lcd_write_ptr[(uint32_t)sy * LCD_W];
                    int row_idx = base_idx + py * 8;

                    for (int px = 0; px < 8; px++)
                    {
                        int img_x = blk_origin_x + px;
                        int16_t sx = lcd_x + img_x;
                        /* row_idx 必须随 px 每像素递增，与裁剪无关（否则 MCU 缓冲游标错位） */
                        if (img_x < img_w && sx >= 0 && sx < LCD_W)
                        {
                            row_ptr[sx] = rgb565(mcu_r[row_idx], mcu_g[row_idx], mcu_b[row_idx]);
                        }
                        row_idx++;
                    }
                }
            }

            mcu_x += mcu_w;
            if (mcu_x >= img_w)
            {
                mcu_x = 0;
                mcu_y += mcu_h;
            }
        }
    }

    if (ret != PJPG_NO_MORE_BLOCKS)
    {
        s_mjpeg.pjpeg_ret = ret;
        /* 帧数据读完（播放完成）或解码失败：本帧已画完，前进到下一帧。
         * ADVANCE 在到达文件尾时自动回绕第一帧，保证循环完整且无黑屏。
         * 不回绕重播第一帧——否则坏帧会导致循环卡在开头几帧（“只能播放前面几帧”）。 */
        s_mjpeg.last_error = MJPEG_ERR_DECODE_INIT;
        s_mjpeg.fail_count++;
        s_mjpeg.last_fail_tick = HAL_GetTick();
        MJPEG_ADVANCE_FRAME();
        return;
    }

    /* ---- 前进到下一帧（成功：清除失败计数） ---- */
    s_mjpeg.fail_count = 0;
    MJPEG_ADVANCE_FRAME();
}

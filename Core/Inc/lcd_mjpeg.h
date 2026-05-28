/*
 * lcd_mjpeg.h
 *
 *  Created on: 2026年4月5日
 *      Author: UnikoZera
 *
 *  MJPEG 视频解码（从 W25Q Flash 读取, 使用 picojpeg 软解 JPEG 帧）
 *
 *  文件格式:
 *     Header (14B):
 *       [0..3]  Magic "MJPG" (0x47504A4D LE)
 *       [4..5]  frame_count (uint16 LE)
 *       [6..7]  width        (uint16 LE)
 *       [8..9]  height       (uint16 LE)
 *       [10..13] reserved
 *     Body:
 *       [frame_size (uint32 LE)] [JPEG data (frame_size bytes)] ... 重复 frame_count 次
 */

#ifndef LCD_MJPEG_H
#define LCD_MJPEG_H

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────── Magic ────── */
#define MJPEG_MAGIC 0x47504A4DUL  /* "MJPG" as little-endian uint32 */

/* ────── 错误码 ────── */
#define MJPEG_ERR_DMA_BUSY      255
#define MJPEG_ERR_BAD_MAGIC     254
#define MJPEG_ERR_ZERO_FRAMES   253
#define MJPEG_ERR_DECODE_INIT   251
#define MJPEG_ERR_NOT_3_COMP    250

/* ────── 解码状态结构体 ────── */
typedef struct
{
    /* 运行状态 */
    uint8_t  active;                     /* 是否激活 */
    uint8_t  _pad0[5];                  /* 对齐填充 */
    int16_t  width;                      /* 图像宽度 */
    int16_t  height;                     /* 图像高度 */
    uint8_t  _pad1[2];                  /* 对齐填充 */

    /* 文件边界 */
    uint32_t start_addr;                 /* W25Q 中文件的起始地址 */
    uint32_t end_addr;                   /* W25Q 中文件的结束地址 */

    /* 帧遍历 */
    uint16_t frame_count;                /* 总帧数 */
    uint8_t  _pad2[2];                  /* 对齐 */
    uint32_t cur_frame_idx;              /* 当前帧序号 */
    uint32_t current_frame_pos;          /* 当前正在处理的数据指针 */
    uint32_t frame_size;                 /* 当前帧 JPEG 数据大小 */
    uint32_t frame_data_addr;            /* 当前帧 JPEG 数据的起始地址（跳过 size prefix）*/
    uint8_t  _pad3[4];                  /* 对齐填充 */

    /* LCD 位置 */
    int16_t  lcd_x;                      /* LCD x 偏移 */
    int16_t  lcd_y;                      /* LCD y 偏移 */

    /* 错误诊断 */
    int8_t   last_error;                 /* 最后的错误码 */
    uint8_t  pjpeg_ret;                  /* 最后的 picojpeg 返回值 */
} mjpeg_state_t;

/* ────── API ────── */

/**
 * @brief 获取最后的错误码
 * @return 错误码（0 = 无错误）
 */
int8_t lcd_mjpeg_last_error(void);

/**
 * @brief 获取解码器状态指针（用于 UI 诊断显示）
 * @return mjpeg_state_t 指针
 */
const mjpeg_state_t *lcd_mjpeg_get_state(void);

/**
 * @brief 从 W25Q Flash 播放 MJPEG 压缩视频
 *
 * 设计为可反复调用, 每次解码一帧。当播放到最后一帧后自动从头循环。
 *
 * @param x       LCD 左上角 x
 * @param y       LCD 左上角 y
 * @param width   图像宽度（会被文件头中的实际尺寸覆盖）
 * @param height  图像高度（会被文件头中的实际尺寸覆盖）
 * @param w25q_start_addr  W25Q 起始地址
 * @param w25q_end_addr    W25Q 结束地址
 */
void lcd_play_mjpeg_video(int16_t x, int16_t y, int16_t width, int16_t height,
                           uint32_t w25q_start_addr, uint32_t w25q_end_addr);

#ifdef __cplusplus
}
#endif

#endif /* LCD_MJPEG_H */
/*
 * lcd_driver.h
 *
 *  Created on: 2026年4月1日
 *      Author: UnikoZera
 */

#ifndef INC_LCD_DRIVER_H_
#define INC_LCD_DRIVER_H_

#include "lcd.h"
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include "math.h"

#define LCD_ANIM_MAX_COUNT 16		// TODO: 可以根据实际需求调整动画最大数量，过多会占用较多内存
#define LCD_LAYER_MAX_COUNT 16		// TODO: 可以根据实际需求调整图层最大数量，过多会占用较多内存

/**
 * @brief 动画缓动类型
 * @param LCD_ANIM_EASE_LINEAR: 线性缓动，匀速动画
 * @param LCD_ANIM_EASE_IN_QUAD: 二次方缓入，动画开始慢，逐渐加速
 * @param LCD_ANIM_EASE_OUT_QUAD: 二次方缓出，动画开始快，逐渐减速
 * @param LCD_ANIM_EASE_IN_OUT_QUAD: 二次方缓入缓出，动画开始慢，中间加速，结束慢
 * @param LCD_ANIM_EASE_IN_SINE: 正弦缓入，动画开始慢，逐渐加速
 * @param LCD_ANIM_EASE_OUT_SINE: 正弦缓出，动画开始快，逐渐减速
 * @param LCD_ANIM_EASE_IN_OUT_SINE: 正弦缓入缓出，动画开始慢，中间加速，结束慢
 * @param LCD_ANIM_EASE_IN_EXPO: 指数缓入，动画开始非常慢，逐渐加速
 * @param LCD_ANIM_EASE_OUT_EXPO: 指数缓出，动画开始非常快，逐渐减速
 * @param LCD_ANIM_EASE_IN_OUT_EXPO: 指数缓入缓出，动画开始非常慢，中间加速，结束非常慢
 * @param LCD_ANIM_EASE_IN_CIRC: 圆形缓入，动画开始慢，逐渐加速
 * @param LCD_ANIM_EASE_OUT_CIRC: 圆形缓出，动画开始快，逐渐减速
 * @param LCD_ANIM_EASE_IN_OUT_CIRC: 圆形缓入缓出，动画开始慢，中间加速，结束慢
 * @param LCD_ANIM_EASE_IN_OUT_BACK: 回弹缓入缓出，动画开始慢，中间加速，结束时超过目标值再回弹到目标值
 * @param LCD_ANIM_EASE_OUT_ELASTIC: 弹性缓出，动画结束时超过目标值再回弹到目标值
 */
typedef enum
{
	LCD_ANIM_EASE_LINEAR = 0,
	LCD_ANIM_EASE_IN_QUAD,
	LCD_ANIM_EASE_OUT_QUAD,
	LCD_ANIM_EASE_IN_OUT_QUAD,
	LCD_ANIM_EASE_IN_OUT_SINE,
	LCD_ANIM_EASE_IN_SINE,
	LCD_ANIM_EASE_OUT_SINE,
	LCD_ANIM_EASE_IN_EXPO,
	LCD_ANIM_EASE_OUT_EXPO,
	LCD_ANIM_EASE_IN_OUT_EXPO,
	LCD_ANIM_EASE_IN_CIRC,
	LCD_ANIM_EASE_OUT_CIRC,
	LCD_ANIM_EASE_IN_OUT_CIRC,
	LCD_ANIM_EASE_IN_OUT_BACK,
	LCD_ANIM_EASE_OUT_ELASTIC
} lcd_anim_ease_t;

// 下面是一些函数指针类型定义，分别用于动画路径计算、动画执行和完成回调，以及图层绘制回调
typedef int32_t (*lcd_anim_path_cb_t)(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
typedef void (*lcd_anim_exec_cb_t)(void *target, int32_t value);
typedef void (*lcd_anim_done_cb_t)(void *target);
typedef void (*lcd_layer_draw_cb_t)(void *ctx);

/**
 * @brief 动画配置结构体
 * @param target: 动画目标
 * @param start_value: 起始值
 * @param end_value: 结束值
 * @param duration_ms: 持续时间（毫秒）
 * @param delay_ms: 延迟时间（毫秒）
 * @param repeat: 是否重复
 * @param yoyo: 是否往返
 * @param exec_cb: 执行回调函数
 * @param done_cb: 完成回调函数
 * @param path_cb: 路径回调函数，如果为NULL则使用线性路径
 */
typedef struct
{
	void *target;
	int32_t start_value;
	int32_t end_value;
	uint32_t duration_ms;
	uint32_t delay_ms;
	bool repeat;
	bool yoyo;
	lcd_anim_exec_cb_t exec_cb;
	lcd_anim_done_cb_t done_cb;
	lcd_anim_path_cb_t path_cb;
} lcd_anim_config_t;

/**
 * @brief 方形层结构体
 * @param x: X坐标
 * @param y: Y坐标
 * @param w: 宽度
 * @param h: 高度
 * @param color: 颜色
 */
typedef struct
{
	int16_t x;
	int16_t y;
	int16_t w;
	int16_t h;
	uint16_t color;
} lcd_rect_t;

/**
 * @brief 圆形层结构体
 * @param x: X坐标
 * @param y: Y坐标
 * @param radius: 半径
 * @param color: 颜色
 */
typedef struct
{
	int16_t x;
	int16_t y;
	uint8_t radius;
	uint16_t color;
} lcd_circle_t;

/**
 * @brief 标签层结构体
 * @param x: X坐标
 * @param y: Y坐标
 * @param fg_color: 前景色
 * @param bg_color: 背景色
 * @param size: 字体大小
 * @param text: 显示文本
 * 
 */
typedef struct
{
	int16_t x;
	int16_t y;
	uint16_t fg_color;
	uint16_t bg_color;
	uint8_t size;
	const char *text;
} lcd_label_t;

typedef struct
{
	int16_t x;
	int16_t y;
	int16_t width;
	int16_t height;
	const uint16_t *data;
} lcd_picture_t;


void lcd_anim_manager_init(void);
void lcd_anim_manager_set_bg(uint16_t color);
void lcd_anim_manager_task(void);
void lcd_anim_manager_render(void);

int8_t lcd_anim_manager_add_layer(void *ctx, lcd_layer_draw_cb_t draw_cb);
bool lcd_anim_manager_remove_layer(int8_t layer_id);
void lcd_anim_manager_clear_layers(void);

int8_t lcd_anim_start(const lcd_anim_config_t *config);
bool lcd_anim_stop(int8_t anim_id);
void lcd_anim_stop_all(void);

lcd_anim_path_cb_t lcd_anim_get_path(lcd_anim_ease_t ease);
int32_t lcd_anim_path_linear(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_in_quad(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_out_quad(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_in_out_quad(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_in_sine(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_out_sine(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_in_out_sine(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_in_expo(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_out_expo(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_in_out_expo(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_in_circ(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_out_circ(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_in_out_circ(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_in_out_back(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
int32_t lcd_anim_path_ease_out_elastic(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);

void lcd_dma_draw_pixel(int16_t x, int16_t y, uint16_t color);
void lcd_dma_draw_filled_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void lcd_dma_draw_circle(int16_t x0, int16_t y0, uint8_t r, uint16_t color);
void lcd_dma_draw_label(const lcd_label_t *label);
void lcd_dma_draw_picture(int16_t x, int16_t y, int16_t width, int16_t height, const uint16_t *data);

void lcd_draw_rect_layer(void *ctx);
void lcd_draw_circle_layer(void *ctx);
void lcd_draw_label_layer(void *ctx);
void lcd_draw_picture_layer(void *ctx);

void lcd_anim_exec_set_i16(void *target, int32_t value);
void lcd_anim_exec_set_u16(void *target, int32_t value);
void lcd_anim_exec_set_u8(void *target, int32_t value);

#endif /* INC_LCD_DRIVER_H_ */

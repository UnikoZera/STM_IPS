/*
 * lcd_driver.h
 *
 *  Created on: 2026年4月1日
 *      Author: UnikoZera
 */

#ifndef INC_LCD_DRIVER_H_
#define INC_LCD_DRIVER_H_

#include "lcd.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LCD_ANIM_MAX_COUNT 16
#define LCD_LAYER_MAX_COUNT 16

typedef enum
{
	LCD_ANIM_EASE_LINEAR = 0,
	LCD_ANIM_EASE_IN_QUAD,
	LCD_ANIM_EASE_OUT_QUAD,
	LCD_ANIM_EASE_IN_OUT_QUAD
} lcd_anim_ease_t;

typedef int32_t (*lcd_anim_path_cb_t)(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration);
typedef void (*lcd_anim_exec_cb_t)(void *target, int32_t value);
typedef void (*lcd_anim_done_cb_t)(void *target);
typedef void (*lcd_layer_draw_cb_t)(void *ctx);

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

typedef struct
{
	int16_t x;
	int16_t y;
	int16_t w;
	int16_t h;
	uint16_t color;
} lcd_rect_t;

typedef struct
{
	int16_t x;
	int16_t y;
	uint8_t radius;
	uint16_t color;
} lcd_circle_t;

typedef struct
{
	int16_t x;
	int16_t y;
	uint16_t fg_color;
	uint16_t bg_color;
	uint8_t size;
	const char *text;
} lcd_label_t;

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

void lcd_dma_draw_pixel(int16_t x, int16_t y, uint16_t color);
void lcd_dma_draw_filled_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void lcd_dma_draw_circle(int16_t x0, int16_t y0, uint8_t r, uint16_t color);
void lcd_dma_draw_label(const lcd_label_t *label);

void lcd_draw_rect_layer(void *ctx);
void lcd_draw_circle_layer(void *ctx);
void lcd_draw_label_layer(void *ctx);

void lcd_anim_exec_set_i16(void *target, int32_t value);
void lcd_anim_exec_set_u16(void *target, int32_t value);
void lcd_anim_exec_set_u8(void *target, int32_t value);

#ifdef __cplusplus
}
#endif

#endif /* INC_LCD_DRIVER_H_ */

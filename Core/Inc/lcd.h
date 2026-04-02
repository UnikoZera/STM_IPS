/*
 * ips_basic.h
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#ifndef LCD_H_
#define LCD_H_

#include "spi.h"
#include "gpio.h"
#include "tim.h"
#include "lcd_font.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define LCD_RES_Clr() HAL_GPIO_WritePin(IPS_RES_GPIO_Port, IPS_RES_Pin, GPIO_PIN_RESET)
#define LCD_RES_Set() HAL_GPIO_WritePin(IPS_RES_GPIO_Port, IPS_RES_Pin, GPIO_PIN_SET)

#define LCD_DC_Clr() HAL_GPIO_WritePin(IPS_DC_GPIO_Port, IPS_DC_Pin, GPIO_PIN_RESET)
#define LCD_DC_Set() HAL_GPIO_WritePin(IPS_DC_GPIO_Port, IPS_DC_Pin, GPIO_PIN_SET)

#define LCD_CS_Clr() HAL_GPIO_WritePin(IPS_CS_GPIO_Port, IPS_CS_Pin, GPIO_PIN_RESET)
#define LCD_CS_Set() HAL_GPIO_WritePin(IPS_CS_GPIO_Port, IPS_CS_Pin, GPIO_PIN_SET)

#define USE_HORIZONTAL 2 // 设置横屏或者竖屏显示 0或1为竖屏 2或3为横屏

#if USE_HORIZONTAL == 0 || USE_HORIZONTAL == 1
#define LCD_W 80
#define LCD_H 160
#define LCD_OFFSET_X 26
#define LCD_OFFSET_Y 1

#else
#define LCD_W 160
#define LCD_H 80
#define LCD_OFFSET_X 1
#define LCD_OFFSET_Y 26
#endif

#pragma region 颜色定义
#define WHITE 0xFFFF
#define BLACK 0x0000
#define BLUE 0x001F
#define BRED 0XF81F
#define GRED 0XFFE0
#define GBLUE 0X07FF
#define RED 0xF800
#define MAGENTA 0xF81F
#define GREEN 0x07E0
#define CYAN 0x7FFF
#define YELLOW 0xFFE0
#define BROWN 0XBC40      // 棕色
#define BRRED 0XFC07      // 棕红色
#define GRAY 0X8430       // 灰色
#define DARKBLUE 0X01CF   // 深蓝色
#define LIGHTBLUE 0X7D7C  // 浅蓝色
#define GRAYBLUE 0X5458   // 灰蓝色
#define LIGHTGREEN 0X841F // 浅绿色
#define LGRAY 0XC618      // 浅灰色(PANNEL),窗体背景色
#define LGRAYBLUE 0XA651  // 浅灰蓝色(中间层颜色)
#define LBBLUE 0X2B12     // 浅棕蓝色(选择条目的反色)
#pragma endregion

void lcd_init(void);
void lcd_fill_screen(uint16_t color);
void lcd_set_address(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void set_lcd_brightness(uint8_t brightness);

void lcd_draw_point(uint16_t x,uint16_t y,uint16_t color);
void lcd_draw_line(uint16_t x1,uint16_t y1,uint16_t x2,uint16_t y2,uint16_t color);
void lcd_draw_rectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_circle(uint16_t x0,uint16_t y0,uint8_t r,uint16_t color);

void lcd_calculate_fps();

// dma相关函数
void lcd_screen_update_dma();
void lcd_fill_screen_dma(uint16_t color);
void lcd_draw_point_dma(int16_t x, int16_t y, uint16_t color);
void lcd_draw_point_dma_swapped(int16_t x, int16_t y, uint16_t swapped_color);
void lcd_draw_line_dma(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t thickness, uint16_t color);
void lcd_draw_char(int16_t x, int16_t y, uint16_t fc, uint16_t bc, uint8_t sizey, char ch);
void lcd_draw_string(int16_t x,int16_t y, uint16_t fc, uint16_t bc, uint8_t sizey, const char *p);
void lcd_set_area_color(int16_t start_x, int16_t start_y, int16_t end_x, int16_t end_y, uint16_t color);

extern volatile bool lcd_dma_busy;
// 可能不需要暴露这两个缓冲区指针，外部只需要调用 lcd_screen_update_dma() 来更新屏幕即可，直接操作指针可能会导致数据竞争和不一致问题
// extern uint16_t lcd_front_buf[LCD_W * LCD_H];
// extern uint16_t lcd_back_buf[LCD_W * LCD_H];
extern uint16_t *lcd_frame_ptr;
extern uint16_t *lcd_write_ptr;
extern uint16_t lcd_fps;

#endif /* LCD_H_ */

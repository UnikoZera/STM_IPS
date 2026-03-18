/*
 * ips_basic.c
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#include "lcd.h"

bool dma_busy = false;

uint16_t lcd_send_buf[LCD_W * LCD_H];
uint16_t lcd_back_buf[LCD_W * LCD_H];
uint16_t lcd_fps = 0;

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
	if (hspi == &hspi1)
	{
		LCD_CS_Set();
		dma_busy = false;
	}
}

static void lcd_write_cmd(uint8_t cmd) // not using dma
{
	LCD_DC_Clr();
	LCD_CS_Clr();
	HAL_SPI_Transmit(&hspi1, &cmd, 1, 100);
	LCD_CS_Set();
}

static void lcd_write_data(uint8_t data) // not using dma
{
	LCD_DC_Set();
	LCD_CS_Clr();
	HAL_SPI_Transmit(&hspi1, &data, 1, 100);
	LCD_CS_Set();
}

static void lcd_write_data16(uint16_t data) // not using dma
{
	uint8_t buf[2];
	buf[0] = data >> 8;
	buf[1] = data & 0xFF;
	LCD_DC_Set();
	LCD_CS_Clr();
	HAL_SPI_Transmit(&hspi1, buf, 2, 100);
	LCD_CS_Set();
}

static inline uint16_t swap_uint16_builtin(uint16_t x) 
{
    return __builtin_bswap16(x);
}

// 为什么不使用dma来写命令和数据是因为每次写命令或数据都需要等待dma完成，效率反而更低
void lcd_set_address(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
	if (USE_HORIZONTAL == 0)
	{
		lcd_write_cmd(0x2a); // 列地址设置
		lcd_write_data16(x1 + LCD_OFFSET_X);
		lcd_write_data16(x2 + LCD_OFFSET_X);
		lcd_write_cmd(0x2b); // 行地址设置
		lcd_write_data16(y1 + LCD_OFFSET_Y);
		lcd_write_data16(y2 + LCD_OFFSET_Y);
		lcd_write_cmd(0x2c); // 储存器写
	}
	else if (USE_HORIZONTAL == 1)
	{
		lcd_write_cmd(0x2a); // 列地址设置
		lcd_write_data16(x1 + LCD_OFFSET_X);
		lcd_write_data16(x2 + LCD_OFFSET_X);
		lcd_write_cmd(0x2b); // 行地址设置
		lcd_write_data16(y1 + LCD_OFFSET_Y);
		lcd_write_data16(y2 + LCD_OFFSET_Y);
		lcd_write_cmd(0x2c); // 储存器写
	}
	else if (USE_HORIZONTAL == 2)
	{
		lcd_write_cmd(0x2a); // 列地址设置
		lcd_write_data16(x1 + LCD_OFFSET_X);
		lcd_write_data16(x2 + LCD_OFFSET_X);
		lcd_write_cmd(0x2b); // 行地址设置
		lcd_write_data16(y1 + LCD_OFFSET_Y);
		lcd_write_data16(y2 + LCD_OFFSET_Y);
		lcd_write_cmd(0x2c); // 储存器写
	}
	else if (USE_HORIZONTAL == 3)
	{
		lcd_write_cmd(0x2a); // 列地址设置
		lcd_write_data16(x1 + LCD_OFFSET_X);
		lcd_write_data16(x2 + LCD_OFFSET_X);
		lcd_write_cmd(0x2b); // 行地址设置
		lcd_write_data16(y1 + LCD_OFFSET_Y);
		lcd_write_data16(y2 + LCD_OFFSET_Y);
		lcd_write_cmd(0x2c); // 储存器写
	}
}

void lcd_init(void)
{
	LCD_RES_Clr();
	HAL_Delay(50);
	LCD_RES_Set();
	HAL_Delay(100);
	lcd_write_cmd(0x01);
	HAL_Delay(150);
	lcd_write_cmd(0x11);
	HAL_Delay(120);

	// 展开后的配置指令
	lcd_write_cmd(0xB1);
	lcd_write_data(0x05);
	lcd_write_data(0x3C);
	lcd_write_data(0x3C);

	lcd_write_cmd(0xB2);
	lcd_write_data(0x05);
	lcd_write_data(0x3C);
	lcd_write_data(0x3C);

	lcd_write_cmd(0xB3);
	lcd_write_data(0x05);
	lcd_write_data(0x3C);
	lcd_write_data(0x3C);
	lcd_write_data(0x05);
	lcd_write_data(0x3C);
	lcd_write_data(0x3C);

	lcd_write_cmd(0xB4);
	lcd_write_data(0x03);

	lcd_write_cmd(0xC0);
	lcd_write_data(0x28);
	lcd_write_data(0x08);
	lcd_write_data(0x04);

	lcd_write_cmd(0xC1);
	lcd_write_data(0xC0);

	lcd_write_cmd(0xC2);
	lcd_write_data(0x0D);
	lcd_write_data(0x00);

	lcd_write_cmd(0xC3);
	lcd_write_data(0x8D);
	lcd_write_data(0x2A);

	lcd_write_cmd(0xC4);
	lcd_write_data(0x8D);
	lcd_write_data(0xEE);

	lcd_write_cmd(0xC5);
	lcd_write_data(0x1A);

	lcd_write_cmd(0x36);
	if (USE_HORIZONTAL == 0)
		lcd_write_data(0x08); // 竖屏
	else if (USE_HORIZONTAL == 1)
		lcd_write_data(0xC8); // 竖屏 + BGR
	else if (USE_HORIZONTAL == 2)
		lcd_write_data(0x68); // 横屏
	else if (USE_HORIZONTAL == 3)
		lcd_write_data(0xA8); // 横屏 + BGR

	lcd_write_cmd(0x3A); // 16位色彩
	lcd_write_data(0x05);

	lcd_write_cmd(0x21);
	lcd_write_cmd(0x29);

	lcd_write_cmd(0x2A); // Set Column Address
	lcd_write_data(0x00);
	lcd_write_data(0x1A); // 26
	lcd_write_data(0x00);
	lcd_write_data(0x69); // 105

	lcd_write_cmd(0x2B); // Set Page Address
	lcd_write_data(0x00);
	lcd_write_data(0x01); // 1
	lcd_write_data(0x00);
	lcd_write_data(0xA0); // 160

	lcd_write_cmd(0x2C);
}

void set_lcd_brightness(uint8_t brightness)
{
	if (brightness > 100)
		brightness = 100; // 限制亮度范围
	__HAL_TIM_SetCompare(&htim9, TIM_CHANNEL_2, brightness);
}

#pragma region no dma drawing functions
void lcd_fill_screen(uint16_t color)
{
	lcd_set_address(0, 0, LCD_W - 1, LCD_H - 1);
	for (uint32_t i = 0; i < LCD_W * LCD_H; i++)
	{
		lcd_write_data16(color);
	}
}

void lcd_draw_point(uint16_t x, uint16_t y, uint16_t color)
{
	lcd_set_address(x, y, x, y); // 设置光标位置
	lcd_write_data16(color);
}

void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
	uint16_t t;
	int xerr = 0, yerr = 0, delta_x, delta_y, distance;
	int incx, incy, uRow, uCol;
	delta_x = x2 - x1; // 计算坐标增量
	delta_y = y2 - y1;
	uRow = x1; // 画线起点坐标
	uCol = y1;
	if (delta_x > 0)
		incx = 1; // 设置单步方向
	else if (delta_x == 0)
		incx = 0; // 垂直线
	else
	{
		incx = -1;
		delta_x = -delta_x;
	}
	if (delta_y > 0)
		incy = 1;
	else if (delta_y == 0)
		incy = 0; // 水平线
	else
	{
		incy = -1;
		delta_y = -delta_y;
	}
	if (delta_x > delta_y)
		distance = delta_x; // 选取基本增量坐标轴
	else
		distance = delta_y;
	for (t = 0; t < distance + 1; t++)
	{
		lcd_draw_point(uRow, uCol, color); // 画点
		xerr += delta_x;
		yerr += delta_y;
		if (xerr > distance)
		{
			xerr -= distance;
			uRow += incx;
		}
		if (yerr > distance)
		{
			yerr -= distance;
			uCol += incy;
		}
	}
}

void lcd_draw_rectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
	lcd_draw_line(x1, y1, x2, y1, color);
	lcd_draw_line(x1, y1, x1, y2, color);
	lcd_draw_line(x1, y2, x2, y2, color);
	lcd_draw_line(x2, y1, x2, y2, color);
}

void lcd_draw_circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color)
{
	int a, b;
	a = 0;
	b = r;
	while (a <= b)
	{
		lcd_draw_point(x0 - b, y0 - a, color); // 3
		lcd_draw_point(x0 + b, y0 - a, color); // 0
		lcd_draw_point(x0 - a, y0 + b, color); // 1
		lcd_draw_point(x0 - a, y0 - b, color); // 2
		lcd_draw_point(x0 + b, y0 + a, color); // 4
		lcd_draw_point(x0 + a, y0 - b, color); // 5
		lcd_draw_point(x0 + a, y0 + b, color); // 6
		lcd_draw_point(x0 - b, y0 + a, color); // 7
		a++;
		if ((a * a + b * b) > (r * r)) // 判断要画的点是否过远
		{
			b--;
		}
	}
}

#pragma endregion


// ! 注意，在每次改变显示图像时候都要调用 lcd_screen_update_dma() 来更新屏幕，否则屏幕不会刷新
void lcd_screen_update_dma()
{
	if (dma_busy)
		return; // DMA忙，直接返回

	memcpy(lcd_send_buf, lcd_back_buf, LCD_W * LCD_H * 2);
	lcd_set_address(0, 0, LCD_W - 1, LCD_H - 1);
	LCD_DC_Set();
	LCD_CS_Clr();
	HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)lcd_send_buf, LCD_W * LCD_H * 2); 
	memset(lcd_back_buf, 0x00, LCD_W * LCD_H * 2);
	dma_busy = true;
}

void lcd_fill_screen_dma(uint16_t color)
{
	uint16_t swapped_color = swap_uint16_builtin(color);
	for (uint32_t i = 0; i < LCD_W * LCD_H; i++)
	{
		lcd_back_buf[i] = swapped_color;
	}
}

void lcd_draw_char(int16_t x, int16_t y, uint16_t fc, uint16_t bc, uint8_t sizey, char ch)
{
    uint8_t temp, t1, t, sizex;
	if (sizey == 8)
    {
        sizex = 6;
    }
    else if (sizey == 16)
    {
        sizex = 12;
    }
    else
    {
        return;
    }

    if (x >= LCD_W || y >= LCD_H || x + sizex <= 0 || y + sizey <= 0) return;
    
    ch = ch - ' '; // 计算偏移地址
    
    uint16_t swapped_fc = swap_uint16_builtin(fc);
    uint16_t swapped_bc = swap_uint16_builtin(bc);
    
    if (sizey == 8)
    {
        for (t = 0; t < sizex; t++)
        {
            temp = LCD_FONT_6x8[ch * sizex + t];
            
            for (t1 = 0; t1 < 8; t1++)
            {
                int16_t draw_x = x + t;
                int16_t draw_y = y + t1;
                if (draw_x >= 0 && draw_x < LCD_W && draw_y >= 0 && draw_y < LCD_H)
                {
                    if (temp & 0x01)
                    {
                        lcd_back_buf[draw_y * LCD_W + draw_x] = swapped_fc;
                    }
                    else
                    {
                        lcd_back_buf[draw_y * LCD_W + draw_x] = swapped_bc;
                    }
                }
                temp >>= 1;
            }
        }
    }
    else if (sizey == 16)
    {
        uint16_t char_offset = ch * (sizex * 2); // 24个字节

        for (t = 0; t < sizex; t++)
        {
            for (uint8_t half = 0; half < 2; half++)
            {
                temp = LCD_FONT_12x16[char_offset + t * 2 + half];
                
                for (t1 = 0; t1 < 8; t1++) 
                {
                    int16_t draw_x = x + t;
                    int16_t draw_y = y + t1 + (half * 8);
                    
                    if (draw_x >= 0 && draw_x < LCD_W && draw_y >= 0 && draw_y < LCD_H)
                    {
                        if (temp & 0x01)
                        {
                            lcd_back_buf[draw_y * LCD_W + draw_x] = swapped_fc;
                        }
                        else
                        {
                            lcd_back_buf[draw_y * LCD_W + draw_x] = swapped_bc;
                        }
                    }
                    temp >>= 1;
                }
            }
        }
    }
}

// sizey 只能是8或16，其他值会错误
void lcd_draw_string(int16_t x, int16_t y, uint16_t fc, uint16_t bc, uint8_t sizey, const char *p)
{
	while (*p != '\0')
	{
		lcd_draw_char(x, y, fc, bc, sizey, *p);
		if (sizey == 8)
			x += 6;
		else if (sizey == 16)
			x += 12;
		p++;
	}
}

void lcd_calculate_fps()
{
	static uint32_t last_time = 0;
	static uint32_t frame_count = 0;

	uint32_t current_time = HAL_GetTick();
	frame_count++;

	if (current_time - last_time >= 1000) // 每秒更新一次FPS
	{
		lcd_fps = (uint16_t)(frame_count * 1000.0f / (current_time - last_time));
		last_time = current_time;
		frame_count = 0;
	}
}


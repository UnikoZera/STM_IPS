/*
 * ips_basic.c
 *
 *  Created on: 2026年3月15日
 *      Author: UnikoZera
 */

#include "lcd.h"
#include "storage_manager.h"
#include "w25q_controller.h"

#define LCD_PIC_CHUNK_SIZE 2048

volatile bool lcd_dma_busy = false;
volatile bool lcd_usb_stream_enabled = false;

/*
 * 帧缓冲：单缓冲方案
 *   lcd_frame_ptr → 当前显示帧
 *   lcd_write_ptr → 正在写入的位置（可移植到双缓冲方案）
 */
static uint16_t lcd_frame_buffer[LCD_W * LCD_H + SEND_TAIL];
uint16_t *lcd_frame_ptr = lcd_frame_buffer;
uint16_t *lcd_write_ptr = lcd_frame_buffer;

/* ---- USB 流节流 ---- */
static uint32_t s_lcd_last_usb_stream_tick = 0U;

/* ---- FPS / CPU 使用率统计 ---- */
uint16_t lcd_fps = 0;
static uint32_t s_dwt_last_cycle = 0;
static uint32_t s_cycle_window = 0;
static uint32_t s_call_window = 0;
static uint32_t s_best_cycle_per_call = 0;
static bool s_dwt_ready = false;
uint8_t cpu_usage_percent = 0;

/* ============================================================================
 *  SPI 底层写函数（同步方式）
 * ============================================================================ */

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
    HAL_TIM_PWM_Start(&htim9, TIM_CHANNEL_2);

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
    set_lcd_brightness(95);
}

void set_lcd_brightness(uint8_t brightness)
{
    brightness = brightness > 100 ? 100 : brightness;   // 限制亮度在0-100范围内
    brightness = brightness < 5 ? 5 : brightness;       // 最小亮度限制在5%，过低可能导致屏幕完全看不见
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



/* ============================================================================
 *  DMA 绘图函数
 * ============================================================================ */

#pragma region dma drawing functions

/*
 * @brief 在DMA模式下绘制一个点（颜色已交换）
 * @param x: 点的X坐标
 * @param y: 点的Y坐标
 * @attention 这里的x和y是相对于屏幕坐标的，范围是0到LCD_W-1和0到LCD_H-1，超出范围的点会被忽略
 * @param swapped_color: 交换后的颜色值
 */
void lcd_draw_point_dma_swapped(int16_t x, int16_t y, uint16_t swapped_color)
{
	if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H)
	{
		return;
	}
	
	lcd_write_ptr[(uint32_t)y * LCD_W + (uint32_t)x] = swapped_color;
}

void lcd_draw_point_dma(int16_t x, int16_t y, uint16_t color)
{
	lcd_draw_point_dma_swapped(x, y, swap_uint16_builtin(color));
}

/**
 * @brief 将帧缓冲通过 SPI DMA 发送到 LCD 刷新显示
 *
 * ⚠ 每次修改绘制内容后必须调用此函数，屏幕才会更新。
 *    应在所有绘图操作完成后最后调用。
 *
 * 若 USB 流模式已启用且不在下载中，同时通过 USB 发送帧数据供上位机预览。
 */
void lcd_screen_update_dma()
{
	if (lcd_dma_busy)
	{
		return;
	}

	lcd_set_address(0, 0, LCD_W - 1, LCD_H - 1);
	LCD_DC_Set();
	LCD_CS_Clr();
	if (HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)lcd_write_ptr, LCD_W * LCD_H * 2) != HAL_OK)
	{
		LCD_CS_Set();
		return;
	}

	lcd_frame_ptr = lcd_write_ptr;
	lcd_dma_busy = true;

	if (lcd_usb_stream_enabled && !storage_is_downloading())
	{
		uint32_t now_tick = HAL_GetTick();
		if ((uint32_t)(now_tick - s_lcd_last_usb_stream_tick) < LCD_USB_STREAM_MIN_INTERVAL_MS)
		{
			return;
		}

		// usb_controller_send要求发送的数据必须在lcd_frame_ptr指向的内存区域末尾添加SEND_TAIL字节的尾部数据，以便接收端正确识别帧结束
		memcpy(lcd_frame_ptr + LCD_W * LCD_H, LCD_FRAME_TAIL, SEND_TAIL);
		if (!g_usb_controller.usb_tx_active &&
			(g_usb_controller.tx_remain_len == 0U) &&
			(g_usb_controller.tx_pending_len == 0U) &&
			(g_usb_controller.tx_protocol_payload_pending == false))
		{
			usb_controller_send(&g_usb_controller, 0xA0U, (uint8_t *)lcd_frame_ptr, LCD_W * LCD_H * 2 + SEND_TAIL);
			s_lcd_last_usb_stream_tick = now_tick;
		}
	}
}

/**
 * @brief 用指定颜色填充整个帧缓冲（清屏）
 *
 * 使用 32-bit 双像素写入，循环次数减半，速度约 2x 于逐 halfword 写入。
 */
void lcd_fill_screen_dma(uint16_t color)
{
	uint16_t swapped_color = swap_uint16_builtin(color);
	uint32_t *draw_buf32 = (uint32_t *)lcd_write_ptr;
	uint32_t dual_pixel = ((uint32_t)swapped_color << 16) | swapped_color;
	uint32_t count = (LCD_W * LCD_H) / 2;

	for (uint32_t i = 0; i < count; i++)
	{
		draw_buf32[i] = dual_pixel;
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
	uint16_t *draw_buf = lcd_write_ptr;
    
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
	                        draw_buf[draw_y * LCD_W + draw_x] = swapped_fc;
                    }
                    else
                    {
	                        draw_buf[draw_y * LCD_W + draw_x] = swapped_bc;
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
	                            draw_buf[draw_y * LCD_W + draw_x] = swapped_fc;
                        }
                        else
                        {
	                            draw_buf[draw_y * LCD_W + draw_x] = swapped_bc;
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

/* ============================================================================
 *  FPS / CPU 使用率统计
 * ============================================================================ */

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

/**
 * @brief 在主循环中调用此函数来估算CPU使用率
 * @attention 这是基于DWT周期计数的主循环负载估算，不是RTOS级别的精确CPU占用率
 */
void lcd_calculate_usage()
{
	static uint32_t last_time = 0;

	if (!s_dwt_ready)
	{
		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		DWT->CYCCNT = 0;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
		s_dwt_last_cycle = DWT->CYCCNT;
		s_dwt_ready = true;
		last_time = HAL_GetTick();
		return;
	}

	uint32_t current_cycle = DWT->CYCCNT;
	uint32_t delta_cycle = current_cycle - s_dwt_last_cycle;
	s_dwt_last_cycle = current_cycle;
	s_cycle_window += delta_cycle;
	s_call_window++;

	uint32_t current_time = HAL_GetTick();

	if (current_time - last_time >= 1000)
	{
		if (s_call_window == 0)
		{
			cpu_usage_percent = 0;
		}
		else
		{
			uint32_t avg_cycle_per_call = s_cycle_window / s_call_window;
			if (s_best_cycle_per_call == 0 || avg_cycle_per_call < s_best_cycle_per_call)
			{
				s_best_cycle_per_call = avg_cycle_per_call;
			}

			if (s_best_cycle_per_call == 0 || avg_cycle_per_call == 0)
			{
				cpu_usage_percent = 0;
			}
			else if (avg_cycle_per_call <= s_best_cycle_per_call)
			{
				cpu_usage_percent = 0;
			}
			else
			{
				uint32_t usage = 100U - ((s_best_cycle_per_call * 100U) / avg_cycle_per_call);
				if (usage > 100U)
				{
					usage = 100U;
				}
				cpu_usage_percent = (uint8_t)usage;
			}
		}

		last_time = current_time;
		s_cycle_window = 0;
		s_call_window = 0;
	}
}

void lcd_set_area_color(int16_t start_x, int16_t start_y, int16_t end_x, int16_t end_y, uint16_t color)
{
	if (start_x < 0) start_x = 0;
	if (start_y < 0) start_y = 0;
	if (end_x >= LCD_W) end_x = LCD_W - 1;
	if (end_y >= LCD_H) end_y = LCD_H - 1;

	uint16_t swapped_color = swap_uint16_builtin(color);
	uint16_t *draw_buf = lcd_write_ptr;

	for (int16_t y = start_y; y <= end_y; y++)
	{
		for (int16_t x = start_x; x <= end_x; x++)
		{
			draw_buf[y * LCD_W + x] = swapped_color;
		}
	}
}

void lcd_draw_line_dma(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t thickness, uint16_t color)
{
	if (thickness == 0)
	{
		thickness = 1;
	}

	uint16_t swapped_color = swap_uint16_builtin(color);
	uint16_t *draw_buf = lcd_write_ptr;
	int16_t half = (int16_t)(thickness / 2);

	int16_t dx = (x2 >= x1) ? (x2 - x1) : (x1 - x2);
	int16_t sx = (x1 < x2) ? 1 : -1;
	int16_t dy = (y2 >= y1) ? (y1 - y2) : (y2 - y1);
	int16_t sy = (y1 < y2) ? 1 : -1;
	int16_t err = dx + dy;

	while (1)
	{
		int16_t start_x = x1 - half;
		int16_t start_y = y1 - half;
		int16_t end_x = x1 + half;
		int16_t end_y = y1 + half;

		if (start_x < 0)
		{
			start_x = 0;
		}
		if (start_y < 0)
		{
			start_y = 0;
		}
		if (end_x >= LCD_W)
		{
			end_x = LCD_W - 1;
		}
		if (end_y >= LCD_H)
		{
			end_y = LCD_H - 1;
		}

		for (int16_t yy = start_y; yy <= end_y; yy++)
		{
			uint32_t index = (uint32_t)yy * LCD_W + (uint32_t)start_x;
			for (int16_t xx = start_x; xx <= end_x; xx++)
			{
				draw_buf[index++] = swapped_color;
			}
		}

		if (x1 == x2 && y1 == y2)
		{
			break;
		}

		int16_t e2 = (int16_t)(2 * err);
		if (e2 >= dy)
		{
			err += dy;
			x1 += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			y1 += sy;
		}
	}
}

// 注意这个函数需要ram地址的data参数，不能直接传入flash地址的图片数据，否则会因为访问权限问题导致硬件错误(需要先把flash地址的数据读到ram中，再传入这个函数) //!(not used anymore, but keep it here for reference)
void lcd_draw_picture_dma(int16_t x, int16_t y, int16_t width, int16_t height, const uint16_t *data)
{
    if (lcd_dma_busy) return;
    // 计算源偏移和目标偏移
    int16_t src_x = 0, src_y = 0;
    int16_t draw_x = x, draw_y = y;
    if (x < 0) { src_x = -x; draw_x = 0; }
    if (y < 0) { src_y = -y; draw_y = 0; }
    int16_t clip_w = width - src_x;
    int16_t clip_h = height - src_y;
    if (draw_x + clip_w > LCD_W) clip_w = LCD_W - draw_x;
    if (draw_y + clip_h > LCD_H) clip_h = LCD_H - draw_y;
    if (clip_w <= 0 || clip_h <= 0) return;
    uint16_t *draw_buf = lcd_write_ptr;
    for (int16_t j = 0; j < clip_h; j++)
    {
        for (int16_t i = 0; i < clip_w; i++)
        {
            draw_buf[(draw_y + j) * LCD_W + (draw_x + i)] =
                swap_uint16_builtin(data[(src_y + j) * width + (src_x + i)]);
        }
    }
}

/*
 * RAW5 container (mirrors MJPEG 14B header layout):
 *   [0..3]  "RAW5"
 *   [4..5]  frame_count (uint16 LE)
 *   [6..7]  width       (uint16 LE)
 *   [8..9]  height      (uint16 LE)
 *   [10..13] reserved
 *   Body: concatenated big-endian RGB565 frames, each width*height*2 bytes
 */
#define RAW5_HEADER_SIZE 14U

static bool raw5_is_magic(const uint8_t *m)
{
    return (m[0] == 'R' && m[1] == 'A' && m[2] == 'W' && m[3] == '5');
}

/* 把 flash 中的 BE RGB565 字节写入 LCD 缓冲（SPI DMA 需要 LE halfword）
 * file_base = 文件物理起点；data_raw_off = 数据区在文件内的原始偏移（无头=0，RAW5=14）
 * @return true=全部块读取成功；false=至少一个块 CRC 校验失败（调用方据此降频重试） */
static bool raw_blit_frame_from_w25q(int16_t x, int16_t y, int16_t width, int16_t height,
                                     uint32_t file_base, uint32_t data_raw_off, uint32_t frame_bytes)
{
    /* 栈仅 1KB（_Min_Stack_Size=0x400）：2048B 缓冲必须 static，否则每次调用栈溢出 */
    static uint8_t chunk[LCD_PIC_CHUNK_SIZE];
    uint32_t done = 0;
    bool all_ok = true;
    while (done < frame_bytes)
    {
        uint32_t to_read = frame_bytes - done;
        if (to_read > LCD_PIC_CHUNK_SIZE)
            to_read = LCD_PIC_CHUNK_SIZE;

        /* CRC-16 校验读取：失败则跳过本块，避免把脏数据画到帧缓冲导致整屏花 */
        if (!w25q_crc_read(file_base, data_raw_off + done, chunk, to_read, 0U, true))
        {
            all_ok = false;
            done += to_read;
            continue;
        }

        uint32_t pixels = to_read / 2;
        /* 计算当前 chunk 的起始行列（一次除法/取模，而非每像素重复） */
        uint32_t base_idx = done / 2;
        uint16_t img_row = (uint16_t)(base_idx / (uint32_t)width);
        uint16_t img_col = (uint16_t)(base_idx % (uint32_t)width);

        for (uint32_t p = 0; p < pixels; p++)
        {
            int16_t screen_x = x + (int16_t)img_col;
            int16_t screen_y = y + (int16_t)img_row;
            if (screen_x >= 0 && screen_x < LCD_W && screen_y >= 0 && screen_y < LCD_H)
            {
                /* flash [hi,lo] BE -> LE halfword for SPI DMA */
                uint16_t pixel_le = ((uint16_t)chunk[p * 2 + 1] << 8) | chunk[p * 2];
                lcd_write_ptr[(uint32_t)screen_y * LCD_W + (uint32_t)screen_x] = pixel_le;
            }
            /* 递增列；列到尾行后换行——纯递增运算，无除法 */
            img_col++;
            if (img_col >= (uint16_t)width)
            {
                img_col = 0;
                img_row++;
            }
        }
        done += to_read;
    }
    return all_ok;
}

// 分块从W25Q读取图片：MJPEG / RAW5 / 无头 raw RGB565
void lcd_draw_picture_from_w25q(int16_t x, int16_t y, int16_t width, int16_t height, uint32_t w25q_addr)
{
    if (lcd_dma_busy) return;

    uint8_t hdr[RAW5_HEADER_SIZE];
    if (!w25q_crc_read(w25q_addr, 0, hdr, RAW5_HEADER_SIZE, 0U, true))
    {
        return;
    }

    if (hdr[0] == 'M' && hdr[1] == 'J' && hdr[2] == 'P' && hdr[3] == 'G')
    {
        uint16_t frame_count = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);

        /* Scan frame size prefixes to compute total end address */
        uint32_t end_addr = w25q_addr + 14;
        for (uint16_t i = 0; i < frame_count; i++)
        {
            uint8_t sz[4];
            if (!w25q_crc_read(w25q_addr, end_addr - w25q_addr, sz, 4, 0U, true))
            {
                return;
            }
            uint32_t frame_size = (uint32_t)sz[0] | ((uint32_t)sz[1] << 8) |
                                  ((uint32_t)sz[2] << 16) | ((uint32_t)sz[3] << 24);
            end_addr += 4 + frame_size;
        }

        lcd_play_mjpeg_video(x, y, width, height, w25q_addr, end_addr);
        return;
    }

    uint32_t data_raw_off = 0; /* 数据区在文件内的原始偏移（无头=0，RAW5=14） */
    int16_t draw_w = width;
    int16_t draw_h = height;

    if (raw5_is_magic(hdr))
    {
        uint16_t hdr_w = (uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8);
        uint16_t hdr_h = (uint16_t)hdr[8] | ((uint16_t)hdr[9] << 8);
        if (hdr_w > 0U && hdr_h > 0U)
        {
            draw_w = (int16_t)hdr_w;
            draw_h = (int16_t)hdr_h;
        }
        data_raw_off = RAW5_HEADER_SIZE;
    }

    if (draw_w <= 0 || draw_h <= 0) return;
    raw_blit_frame_from_w25q(x, y, draw_w, draw_h, w25q_addr, data_raw_off,
                             (uint32_t)draw_w * (uint32_t)draw_h * 2U);
}

static struct video_ctx_t {
    bool active;
    bool has_header;          /* true: RAW5, body starts after 14B header */
    int16_t x, y;
    int16_t width, height;
    uint32_t start_addr;      /* file start (may include header) */
    uint32_t end_addr;        /* exclusive end of payload */
    uint32_t body_start;      /* first frame byte address */
    uint32_t current_addr;    /* next frame start */
    uint32_t frame_bytes;
    uint32_t last_fail_tick;  /* 读取失败时间戳（降频重试，防文件损坏/被删时高 CPU） */
    uint8_t  fail_count;      /* 连续失败计数（≥3 才降频，单次失败不停顿） */
} s_video_ctx = {0};

void lcd_play_video_from_w25q(int16_t x, int16_t y, int16_t width, int16_t height, uint32_t w25q_start_addr, uint32_t w25q_end_addr)
{
    if (lcd_dma_busy) return;

    /* 失败降频：仅连续失败 ≥3 帧才降频（单次毛刺/坏帧不停顿，保证播放完整） */
    if (s_video_ctx.active && s_video_ctx.last_fail_tick != 0U &&
        s_video_ctx.fail_count >= 3U &&
        (uint32_t)(HAL_GetTick() - s_video_ctx.last_fail_tick) < 500U)
    {
        return;
    }

    /* auto-detect MJPEG / RAW5 container */
    uint8_t m[RAW5_HEADER_SIZE];
    if (!w25q_crc_read(w25q_start_addr, 0, m, RAW5_HEADER_SIZE, 0U, true))
    {
        return;
    }

    if (m[0] == 'M' && m[1] == 'J' && m[2] == 'P' && m[3] == 'G')
    {
        lcd_play_mjpeg_video(x, y, width, height, w25q_start_addr, w25q_end_addr);
        return;
    }

    bool is_raw5 = raw5_is_magic(m);
    int16_t play_w = width;
    int16_t play_h = height;
    uint32_t body_start = w25q_start_addr;

    if (is_raw5)
    {
        uint16_t hdr_w = (uint16_t)m[6] | ((uint16_t)m[7] << 8);
        uint16_t hdr_h = (uint16_t)m[8] | ((uint16_t)m[9] << 8);
        if (hdr_w > 0U && hdr_h > 0U)
        {
            play_w = (int16_t)hdr_w;
            play_h = (int16_t)hdr_h;
        }
        body_start = w25q_start_addr + RAW5_HEADER_SIZE;
    }

    uint32_t frame_bytes = (uint32_t)play_w * (uint32_t)play_h * 2U;
    if (frame_bytes == 0U || play_w <= 0 || play_h <= 0) return;
    if (body_start >= w25q_end_addr) return;

    /* 仅源地址/分辨率变化才重置播放进度；x/y 由动画驱动时不应从头重播 */
    if (!s_video_ctx.active ||
        s_video_ctx.width != play_w || s_video_ctx.height != play_h ||
        s_video_ctx.start_addr != w25q_start_addr ||
        s_video_ctx.end_addr != w25q_end_addr)
    {
        s_video_ctx.width = play_w;
        s_video_ctx.height = play_h;
        s_video_ctx.start_addr = w25q_start_addr;
        s_video_ctx.end_addr = w25q_end_addr;
        s_video_ctx.body_start = body_start;
        s_video_ctx.current_addr = body_start;
        s_video_ctx.frame_bytes = frame_bytes;
        s_video_ctx.has_header = is_raw5;
        s_video_ctx.active = true;
        s_video_ctx.last_fail_tick = 0; /* 新文件：清除失败降频 */
        s_video_ctx.fail_count = 0;
    }
    s_video_ctx.x = x;
    s_video_ctx.y = y;

    /* 不足一帧则回绕，避免半帧花屏 */
    if ((s_video_ctx.current_addr + s_video_ctx.frame_bytes) > s_video_ctx.end_addr)
    {
        s_video_ctx.current_addr = s_video_ctx.body_start;
    }

    if (!raw_blit_frame_from_w25q(s_video_ctx.x, s_video_ctx.y,
                                   s_video_ctx.width, s_video_ctx.height,
                                   s_video_ctx.start_addr,
                                   s_video_ctx.current_addr - s_video_ctx.start_addr,
                                   s_video_ctx.frame_bytes))
    {
        s_video_ctx.fail_count++;
        s_video_ctx.last_fail_tick = HAL_GetTick(); /* 读取失败：连续失败才降频 */
    }
    else
    {
        s_video_ctx.fail_count = 0;
    }

    s_video_ctx.current_addr += s_video_ctx.frame_bytes;
    if ((s_video_ctx.current_addr + s_video_ctx.frame_bytes) > s_video_ctx.end_addr)
        s_video_ctx.current_addr = s_video_ctx.body_start;
}

#pragma endregion
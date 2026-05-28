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
#define BL_MAGIC0 'B'
#define BL_MAGIC1 'L'

volatile bool lcd_dma_busy = false;
volatile bool lcd_usb_stream_enabled = false;

static uint16_t lcd_frame_buffer[LCD_W * LCD_H + SEND_TAIL]; // 直接使用单个缓冲区，lcd_frame_ptr指向当前帧数据，lcd_write_ptr指向正在写入的数据位置 可以轻松移植到双缓冲方案
uint16_t *lcd_frame_ptr = lcd_frame_buffer;
uint16_t *lcd_write_ptr = lcd_frame_buffer;
// USB帧流时间节流相关变量
static uint32_t s_lcd_last_usb_stream_tick = 0U;
uint16_t lcd_fps = 0;
static uint32_t s_dwt_last_cycle = 0;
static uint32_t s_cycle_window = 0;
static uint32_t s_call_window = 0;
static uint32_t s_best_cycle_per_call = 0;
static bool s_dwt_ready = false;
uint8_t cpu_usage_percent = 0;

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
    set_lcd_brightness(75);
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

// ! 注意，在每次改变显示图像时候都要调用 lcd_screen_update_dma() 来更新屏幕，否则屏幕不会刷新(最后调用!)
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

// 这里其实就算是清除画面的函数
void lcd_fill_screen_dma(uint16_t color)
{
	uint16_t swapped_color = swap_uint16_builtin(color);
	uint16_t *draw_buf = lcd_write_ptr;
	for (uint32_t i = 0; i < LCD_W * LCD_H; i++)
	{
		draw_buf[i] = swapped_color;
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

/* Forward declaration for BL decompression helper */
static inline void bl_put_pixel(int16_t x, int16_t y,
                                 int16_t img_x, int16_t img_y,
                                 uint8_t hi, uint8_t lo,
                                 uint32_t *done);

// 这个函数会分块从W25Q读取图片数据到RAM，然后再写入lcd_write_ptr，最后调用lcd_screen_update_dma()来刷新屏幕 (更加节省内存、但是会频繁调用W25Q的DMA读取函数，可能会有性能影响，适合大图片显示)
void lcd_draw_picture_from_w25q(int16_t x, int16_t y, int16_t width, int16_t height, uint32_t w25q_addr)
{
    if (lcd_dma_busy) return;

    /* Read 6-byte BL header to determine compression type */
    uint8_t hdr[6];
    w25q_fast_read_data_dma(w25q_addr, hdr, 6);
    while (w25q_dma_is_busy()) w25q_dma_task();

    /* Check for MJPEG magic */
    if (hdr[0] == 'M' && hdr[1] == 'J' && hdr[2] == 'P' && hdr[3] == 'G')
    {
        /* frame_count already in hdr[4..5] from the 6-byte read */
        uint16_t frame_count = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);

        /* Scan frame size prefixes to compute total end address */
        uint32_t end_addr = w25q_addr + 14;
        for (uint16_t i = 0; i < frame_count; i++)
        {
            uint8_t sz[4];
            w25q_fast_read_data_dma(end_addr, sz, 4);
            while (w25q_dma_is_busy()) w25q_dma_task();
            uint32_t frame_size = (uint32_t)sz[0] | ((uint32_t)sz[1] << 8) |
                                  ((uint32_t)sz[2] << 16) | ((uint32_t)sz[3] << 24);
            end_addr += 4 + frame_size;
        }

        lcd_play_mjpeg_video(x, y, width, height, w25q_addr, end_addr);
        return;
    }

    bool is_bl = (hdr[0] == BL_MAGIC0 && hdr[1] == BL_MAGIC1);

    if (is_bl && hdr[4] != 0)
    {
        /* ---- BL compressed path (lvl > 0) ---- */
        uint32_t frame_pixels = (uint32_t)width * height;
        int blk_lvl = (int)hdr[4];
        int bw = (blk_lvl == 1) ? 2 : (blk_lvl == 4 ? 4 : (blk_lvl == 3 ? 8 : 4));
        int bh = (blk_lvl == 1 || blk_lvl == 4) ? 2 : 4;
        int npix = bw * bh;
        int idx_bytes = (npix <= 4) ? 1 : (npix <= 8 ? 2 : (npix <= 16 ? 4 : 8));
        int blk_bytes = 4 + idx_bytes;
        int blocks_per_row = (width + bw - 1) / bw;
        uint32_t addr = w25q_addr + 6;
        uint32_t done = 0;
        uint32_t block_index = 0;

        while (done < frame_pixels)
        {
            uint8_t blk[12];
            w25q_fast_read_data_dma(addr, blk, blk_bytes);
            while (w25q_dma_is_busy()) w25q_dma_task();
            addr += blk_bytes;

            /* Decode 4-colour palette (matches _compress_block server-side) */
            uint8_t c0h = blk[0], c0l = blk[1];
            uint8_t c1h = blk[2], c1l = blk[3];
            uint16_t c0v = (uint16_t)((c0h << 8) | c0l);
            uint16_t c1v = (uint16_t)((c1h << 8) | c1l);
            uint8_t c0r = (c0v >> 11) & 0x1F, c0g = (c0v >> 5) & 0x3F, c0b = c0v & 0x1F;
            uint8_t c1r = (c1v >> 11) & 0x1F, c1g = (c1v >> 5) & 0x3F, c1b = c1v & 0x1F;
            uint8_t c2r = (c0r * 2 + c1r) / 3, c2g = (c0g * 2 + c1g) / 3, c2b = (c0b * 2 + c1b) / 3;
            uint8_t c3r = (c0r + c1r * 2) / 3, c3g = (c0g + c1g * 2) / 3, c3b = (c0b + c1b * 2) / 3;
            uint16_t c2v = (uint16_t)((c2r << 11) | (c2g << 5) | c2b);
            uint16_t c3v = (uint16_t)((c3r << 11) | (c3g << 5) | c3b);
            uint8_t col_hi[4] = {c0h, c1h, (uint8_t)((c2v >> 8) & 0xFF), (uint8_t)((c3v >> 8) & 0xFF)};
            uint8_t col_lo[4] = {c0l, c1l, (uint8_t)(c2v & 0xFF), (uint8_t)(c3v & 0xFF)};

            /* Read 2-bit indices */
            uint8_t idx_buf[8];
            for (int b = 0; b < idx_bytes; b++)
                idx_buf[b] = blk[4 + b];

            /* Use explicit block_index (not done/npix) so encoder edge-padding
             * blocks are fully traversed. Skip pixels outside image bounds. */
            int bx = (int)(block_index % (uint32_t)blocks_per_row);
            int by = (int)(block_index / (uint32_t)blocks_per_row);

            for (int pi = 0; pi < npix; pi++)
            {
                int16_t img_x = (int16_t)(bx * bw + (pi % bw));
                int16_t img_y = (int16_t)(by * bh + (pi / bw));

                /* Encoder pads blocks at edges with zeros — skip them */
                if (img_x >= width || img_y >= height)
                    continue;

                int bit_pos = 2 * (npix - 1 - pi);
                int byte_idx = idx_bytes - 1 - (bit_pos / 8);
                int bit_in_byte = bit_pos % 8;
                int ci = (int)((idx_buf[byte_idx] >> bit_in_byte) & 3);

                bl_put_pixel(x, y, img_x, img_y, col_hi[ci], col_lo[ci], &done);
            }
            block_index++;
        }
    }
    else
    {
        /* ---- Raw passthrough path (no BL / lvl == 0) ---- */
        uint32_t data_addr = w25q_addr + (is_bl ? 6U : 0U);

        uint8_t chunk[LCD_PIC_CHUNK_SIZE];
        uint32_t total_bytes = (uint32_t)width * height * 2;
        uint32_t done = 0;
        while (done < total_bytes)
        {
            uint32_t to_read = total_bytes - done;
            if (to_read > LCD_PIC_CHUNK_SIZE)
                to_read = LCD_PIC_CHUNK_SIZE;
            w25q_fast_read_data_dma(data_addr + done, chunk, to_read);
            while (w25q_dma_is_busy())
                w25q_dma_task();
            uint32_t pixels = to_read / 2;
            for (uint32_t p = 0; p < pixels; p++)
            {
                uint32_t g_idx = done / 2 + p;
                uint16_t img_col = (uint16_t)(g_idx % (uint32_t)width);
                uint16_t img_row = (uint16_t)(g_idx / (uint32_t)width);
                int16_t screen_x = x + img_col;
                int16_t screen_y = y + img_row;
                if (screen_x < 0 || screen_x >= LCD_W || screen_y < 0 || screen_y >= LCD_H)
                    continue;
                uint16_t pixel_le = (uint16_t)chunk[p * 2 + 1] << 8 | chunk[p * 2];
                lcd_write_ptr[(uint32_t)screen_y * LCD_W + (uint32_t)screen_x] =
                    pixel_le;
            }
            done += to_read;
        }
    }
}

static struct video_ctx_t {
    bool active;
    int16_t x, y;
    int16_t width, height;
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t current_addr;
    uint32_t frame_bytes;
    // compressed format fields
    uint32_t compressed_frame_addr; // current read position in compressed stream
    uint8_t bpp;                    // 0=raw, 4/8/16=compressed
    uint8_t rle_chunk[256];         // rle read buffer
    uint16_t rle_chunk_len;
    uint16_t rle_chunk_pos;
} s_video_ctx = {0};

void lcd_play_video_from_w25q(int16_t x, int16_t y, int16_t width, int16_t height, uint32_t w25q_start_addr, uint32_t w25q_end_addr)
{
    if (lcd_dma_busy) return;

    /* auto-detect BL compressed data and redirect */
    {
        uint8_t m[4];
        w25q_fast_read_data_dma(w25q_start_addr, m, 4);
        while (w25q_dma_is_busy()) w25q_dma_task();
        if (m[0] == BL_MAGIC0 && m[1] == BL_MAGIC1)
        {
            lcd_play_compressed_video_from_w25q(x, y, width, height, w25q_start_addr, w25q_end_addr);
            return;
        }
        else if (m[0] == 'M' && m[1] == 'J' && m[2] == 'P' && m[3] == 'G')
        {
            lcd_play_mjpeg_video(x, y, width, height, w25q_start_addr, w25q_end_addr);
            return;
        }
        /* 既不是 BL 也不是 MJPEG，继续下方原始 RGB565 播放 */
    }

    uint32_t frame_bytes = (uint32_t)width * height * 2;
    if (frame_bytes == 0) return;

    if (!s_video_ctx.active ||
        s_video_ctx.x != x || s_video_ctx.y != y ||
        s_video_ctx.width != width || s_video_ctx.height != height ||
        s_video_ctx.start_addr != w25q_start_addr ||
        s_video_ctx.end_addr != w25q_end_addr)
    {
        s_video_ctx.x = x;
        s_video_ctx.y = y;
        s_video_ctx.width = width;
        s_video_ctx.height = height;
        s_video_ctx.start_addr = w25q_start_addr;
        s_video_ctx.end_addr = w25q_end_addr;
        s_video_ctx.current_addr = w25q_start_addr;
        s_video_ctx.frame_bytes = frame_bytes;
        s_video_ctx.active = true;
    }

    uint32_t done = 0;
    while (done < s_video_ctx.frame_bytes)
    {
        uint8_t chunk[LCD_PIC_CHUNK_SIZE];
        uint32_t to_read = s_video_ctx.frame_bytes - done;
        if (to_read > LCD_PIC_CHUNK_SIZE)
            to_read = LCD_PIC_CHUNK_SIZE;
        w25q_fast_read_data_dma(s_video_ctx.current_addr + done, chunk, to_read);
        while (w25q_dma_is_busy())
            w25q_dma_task();
        uint32_t pixels = to_read / 2;
        for (uint32_t p = 0; p < pixels; p++)
        {
            uint32_t g_idx = done / 2 + p;
            uint16_t img_col = (uint16_t)(g_idx % (uint32_t)s_video_ctx.width);
            uint16_t img_row = (uint16_t)(g_idx / (uint32_t)s_video_ctx.width);
            int16_t screen_x = s_video_ctx.x + img_col;
            int16_t screen_y = s_video_ctx.y + img_row;
            if (screen_x < 0 || screen_x >= LCD_W || screen_y < 0 || screen_y >= LCD_H)
                continue;
            uint16_t pixel_le = (uint16_t)chunk[p * 2 + 1] << 8 | chunk[p * 2];
            lcd_write_ptr[(uint32_t)screen_y * LCD_W + (uint32_t)screen_x] =
                pixel_le;
        }
        done += to_read;
    }

    s_video_ctx.current_addr += s_video_ctx.frame_bytes;
    if (s_video_ctx.current_addr >= s_video_ctx.end_addr)
        s_video_ctx.current_addr = s_video_ctx.start_addr;
}

#pragma endregion

#pragma region 压缩视频解码 (BL块格式)

/* BL_MAGIC0/1 and BLOCK_SIZE defined above */
#define BLOCK_SIZE 4

/**
 * @brief DMA缓冲读取6B BL块，带自动回填
 */
static inline void bl_read_block(uint32_t *addr, uint32_t limit,
                                  uint8_t out[6])
{
    while (s_video_ctx.rle_chunk_pos >= s_video_ctx.rle_chunk_len)
    {
        uint32_t remaining = limit - *addr;
        if (remaining == 0) return;
        uint32_t to_read = remaining > sizeof(s_video_ctx.rle_chunk)
                               ? sizeof(s_video_ctx.rle_chunk) : remaining;
        if (to_read < 6) to_read = 6;
        w25q_fast_read_data_dma(*addr, s_video_ctx.rle_chunk, to_read);
        while (w25q_dma_is_busy())
            w25q_dma_task();
        s_video_ctx.rle_chunk_len = (uint16_t)to_read;
        s_video_ctx.rle_chunk_pos = 0;
    }
    for (int i = 0; i < 6; i++)
        out[i] = s_video_ctx.rle_chunk[s_video_ctx.rle_chunk_pos++];
    *addr += 6;
}

static inline void bl_read_block_var(uint32_t *addr, uint32_t limit,
                                      uint8_t out[], int blk_bytes)
{
    if (s_video_ctx.rle_chunk_pos + blk_bytes > s_video_ctx.rle_chunk_len)
    {
        uint32_t remaining = limit - *addr;
        uint32_t to_read = remaining > 64 ? 64 : remaining;
        if (to_read < (uint32_t)blk_bytes) to_read = (uint32_t)blk_bytes;
        if (remaining == 0) return;
        w25q_fast_read_data_dma(*addr, s_video_ctx.rle_chunk, to_read);
        while (w25q_dma_is_busy())
            w25q_dma_task();
        s_video_ctx.rle_chunk_len = (uint16_t)to_read;
        s_video_ctx.rle_chunk_pos = 0;
    }
    for (int i = 0; i < blk_bytes; i++)
        out[i] = s_video_ctx.rle_chunk[s_video_ctx.rle_chunk_pos++];
    *addr += blk_bytes;
}

/**
 * @brief 写入RGB565像素(大端)到LCD写缓冲区
 */
static inline void bl_put_pixel(int16_t x, int16_t y,
                                 int16_t img_x, int16_t img_y,
                                 uint8_t hi, uint8_t lo,
                                 uint32_t *done)
{
    int16_t sx = x + img_x;
    int16_t sy = y + img_y;
    if (sx >= 0 && sx < LCD_W && sy >= 0 && sy < LCD_H)
    {
        /* Store pixel in LE memory order for 8-bit SPI DMA.
         * Palette format in W25Q: [hi, lo] (big-endian RGB565).
         * DMA sends LE bytes, so (lo<<8)|hi gives LE bytes[hi, lo]
         * -> wire: hi, lo -> ST7735: (hi<<8)|lo, matching fill_screen. */
        uint16_t px_le = (uint16_t)(lo << 8) | hi;
        lcd_write_ptr[(uint32_t)sy * LCD_W + (uint32_t)sx] =
            px_le;
    }
    (*done)++;
}

/**
 * @brief 从W25Q播放BL格式压缩视频/图片
 *
 * 数据格式: [6B header] + [block数据]
 * 每个block: color0(2B) + color1(2B) + bitmap(2B) = 6B, 16像素
 * 视频多帧连续拼接, 按像素计数分割帧边界。
 */
void lcd_play_compressed_video_from_w25q(int16_t x, int16_t y, int16_t width, int16_t height,
                                          uint32_t w25q_start_addr, uint32_t w25q_end_addr)
{
    if (lcd_dma_busy) return;

    uint32_t frame_pixels = (uint32_t)width * height;
    if (frame_pixels == 0) return;

    bool is_new = (!s_video_ctx.active ||
                   s_video_ctx.x != x || s_video_ctx.y != y ||
                   s_video_ctx.width != width || s_video_ctx.height != height ||
                   s_video_ctx.start_addr != w25q_start_addr ||
                   s_video_ctx.end_addr != w25q_end_addr);

    if (is_new)
    {
        uint8_t hdr[6];
        w25q_fast_read_data_dma(w25q_start_addr, hdr, 6);
        while (w25q_dma_is_busy())
            w25q_dma_task();

        if (hdr[0] != BL_MAGIC0 || hdr[1] != BL_MAGIC1)
        {
            lcd_play_video_from_w25q(x, y, width, height, w25q_start_addr, w25q_end_addr);
            return;
        }

        s_video_ctx.x = x;
        s_video_ctx.y = y;
        s_video_ctx.width = width;
        s_video_ctx.height = height;
        s_video_ctx.start_addr = w25q_start_addr;
        s_video_ctx.end_addr = w25q_end_addr;
        s_video_ctx.active = true;
        s_video_ctx.bpp = hdr[4];  /* block level (1/2/3) */
        s_video_ctx.compressed_frame_addr = w25q_start_addr + 6;
        s_video_ctx.rle_chunk_len = 0;
        s_video_ctx.rle_chunk_pos = 0;
    }

    // 逐块解码一帧 (支持可变块大小)
    uint32_t done = 0;
    uint32_t addr = s_video_ctx.compressed_frame_addr;
    int blk_lvl = (int)s_video_ctx.bpp;

    if (blk_lvl == 0)
    {
        /* lvl=0: raw passthrough — read RGB565 pixels directly */
        uint8_t chunk[LCD_PIC_CHUNK_SIZE];
        while (done < (uint32_t)frame_pixels)
        {
            uint32_t remain = (uint32_t)frame_pixels - done;
            uint32_t to_read = remain * 2;
            if (to_read > sizeof(chunk)) to_read = sizeof(chunk);
            if (addr + to_read > w25q_end_addr) to_read = w25q_end_addr - addr;
            if (to_read == 0) break;
            w25q_fast_read_data_dma(addr, chunk, to_read);
            while (w25q_dma_is_busy()) w25q_dma_task();
            uint32_t px_in_chunk = to_read / 2;
            for (uint32_t p = 0; p < px_in_chunk && done < (uint32_t)frame_pixels; p++)
            {
                uint32_t gx = done % (uint32_t)width;
                uint32_t gy = done / (uint32_t)width;
                int16_t img_x = (int16_t)gx;
                int16_t img_y = (int16_t)gy;
                uint8_t hi = chunk[p * 2];
                uint8_t lo = chunk[p * 2 + 1];
                int16_t sx = x + img_x;
                int16_t sy = y + img_y;
                if (sx >= 0 && sx < LCD_W && sy >= 0 && sy < LCD_H)
                {
                    uint16_t px_le = (uint16_t)(lo << 8) | hi;
                    lcd_write_ptr[(uint32_t)sy * LCD_W + (uint32_t)sx] =
                        px_le;
                }
                done++;
            }
            addr += to_read;
        }
    }
    else
    {
        int bw = (blk_lvl == 1) ? 2 : (blk_lvl == 4 ? 4 : (blk_lvl == 3 ? 8 : 4));
        int bh = (blk_lvl == 1 || blk_lvl == 4) ? 2 : 4;
        int npix = bw * bh;
        /* 4-colour indices: 2-bit per pixel */
        int idx_bytes = (npix <= 4) ? 1 : (npix <= 8 ? 2 : (npix <= 16 ? 4 : 8));
        int blk_bytes = 4 + idx_bytes;
        uint8_t blk[12];

        while (done < (uint32_t)frame_pixels)
        {

            bl_read_block_var(&addr, w25q_end_addr, blk, blk_bytes);
            uint8_t c0h = blk[0], c0l = blk[1];
            uint8_t c1h = blk[2], c1l = blk[3];

            /* interpolate c2, c3 */
            uint16_t c0v = (uint16_t)((c0h << 8) | c0l);
            uint16_t c1v = (uint16_t)((c1h << 8) | c1l);
            uint8_t c0r = (c0v >> 11) & 0x1F, c0g = (c0v >> 5) & 0x3F, c0b = c0v & 0x1F;
            uint8_t c1r = (c1v >> 11) & 0x1F, c1g = (c1v >> 5) & 0x3F, c1b = c1v & 0x1F;
            uint8_t c2r = (c0r * 2 + c1r) / 3, c2g = (c0g * 2 + c1g) / 3, c2b = (c0b * 2 + c1b) / 3;
            uint8_t c3r = (c0r + c1r * 2) / 3, c3g = (c0g + c1g * 2) / 3, c3b = (c0b + c1b * 2) / 3;
            uint16_t c2v = (uint16_t)((c2r << 11) | (c2g << 5) | c2b);
            uint16_t c3v = (uint16_t)((c3r << 11) | (c3g << 5) | c3b);
            uint8_t col_hi[4] = {c0h, c1h, (uint8_t)((c2v >> 8) & 0xFF), (uint8_t)((c3v >> 8) & 0xFF)};
            uint8_t col_lo[4] = {c0l, c1l, (uint8_t)(c2v & 0xFF), (uint8_t)(c3v & 0xFF)};

            uint8_t idx_buf[8];
            for (int b = 0; b < idx_bytes; b++)
                idx_buf[b] = blk[4 + b];

            int blocks_per_row = (width + bw - 1) / bw;
            for (int pi = 0; pi < npix && done < (uint32_t)frame_pixels; pi++)
            {
                /* Compute pixel position from block grid, not linear frame order. */
                uint32_t block_index = done / npix;
                uint32_t pixel_in_block = done % npix;
                int16_t img_x = (int16_t)((block_index % (uint32_t)blocks_per_row) * bw
                                          + (pixel_in_block % bw));
                int16_t img_y = (int16_t)((block_index / (uint32_t)blocks_per_row) * bh
                                          + (pixel_in_block / bw));
                int bit_pos = 2 * (npix - 1 - pi);
                int byte_idx = idx_bytes - 1 - (bit_pos / 8);
                int bit_in_byte = bit_pos % 8;
                int ci = (int)((idx_buf[byte_idx] >> bit_in_byte) & 3);
                bl_put_pixel(x, y, img_x, img_y, col_hi[ci], col_lo[ci], &done);
            }
        }
    }

    /* fallback: if no pixels decoded (shouldn't happen without end_addr check), show checkerboard */
    if (done == 0)
    {
        for (uint32_t py = 0; py < (uint32_t)height && (uint32_t)(y + py) < LCD_H; py++)
            for (uint32_t px = 0; px < (uint32_t)width && (uint32_t)(x + px) < LCD_W; px++)
                lcd_write_ptr[(uint32_t)(y + py) * LCD_W + (uint32_t)(x + px)] =
                    swap_uint16_builtin(((px + py) & 1) ? 0xF800 : 0x001F);
    }

    /* Reset DMA chunk buffer so next frame's bl_read_block_var does a fresh read. */
    s_video_ctx.rle_chunk_len = 0;
    s_video_ctx.rle_chunk_pos = 0;

    /* Skip inter-frame BL header (6 bytes) between concatenated frames.
     * Each frame in the W25Q stream starts with:
     *   [BL_MAGIC 2B][VER 1B][QUALITY 1B][LVL 1B][unused 1B]
     * Init skips the first frame's header; subsequent frames need the same. */
    if (addr + 6 <= w25q_end_addr)
    {
        uint8_t peek[2];
        w25q_fast_read_data_dma(addr, peek, 2);
        while (w25q_dma_is_busy()) w25q_dma_task();
        if (peek[0] == BL_MAGIC0 && peek[1] == BL_MAGIC1)
            addr += 6;
    }

    s_video_ctx.compressed_frame_addr = addr;
    if (s_video_ctx.compressed_frame_addr >= w25q_end_addr)
    {
        s_video_ctx.compressed_frame_addr = w25q_start_addr + 6;
    }
}

#pragma endregion
#include "lcd_driver.h"

/**
 * @brief 动画槽结构体
 * @param used: 是否被占用
 * @param started: 是否已经开始动画
 * @param direction: 当前动画段的方向，1表示从start_value到end_value，-1表示反向
 * @param created_tick: 动画创建的系统时钟tick值
 * @param segment_tick: 当前动画段开始的系统时钟tick值
 * @param completed_segments: 已完成的动画段数量，用于repeat和yoyo逻辑
 * @param cfg: 动画配置
 */
typedef struct
{
	bool used;
	bool started;
	int8_t direction;
	uint32_t created_tick;
	uint32_t segment_tick;
	uint32_t completed_segments;
	lcd_anim_config_t cfg;
} lcd_anim_slot_t;

/**
 * @brief 层槽结构体
 * @param used: 是否被占用
 * @param ctx: 上下文指针
 * @param draw_cb: 绘制回调函数
 */
typedef struct
{
	bool used;
	void *ctx;
	lcd_layer_draw_cb_t draw_cb;
} lcd_layer_slot_t;

static lcd_anim_slot_t s_anim_slots[LCD_ANIM_MAX_COUNT];
static lcd_layer_slot_t s_layer_slots[LCD_LAYER_MAX_COUNT];
static uint16_t s_bg_color = BLACK;

// 作用是将elapsed限制在duration范围内，避免动画路径函数计算出超出预期的值
static inline uint32_t lcd_anim_clamp_elapsed(uint32_t elapsed, uint32_t duration)
{
	return (elapsed > duration) ? duration : elapsed;
}

static inline int32_t lcd_anim_mix_q10(int32_t start, int32_t end, uint32_t progress_q10)
{
	int64_t delta = (int64_t)end - (int64_t)start;
	return (int32_t)((int64_t)start + (delta * progress_q10) / 1024);
}

static inline void lcd_dma_plot_point(int16_t x, int16_t y, uint16_t color)
{
	lcd_draw_point_dma(x, y, color);
}

#pragma region animation manager

void lcd_anim_manager_init(void)
{
	memset(s_anim_slots, 0, sizeof(s_anim_slots));
	memset(s_layer_slots, 0, sizeof(s_layer_slots));
	s_bg_color = BLACK;
}

void lcd_anim_manager_set_bg(uint16_t color)
{
	s_bg_color = color;
}

void lcd_anim_manager_task(void)
{
	uint32_t now = HAL_GetTick();

	for (uint32_t i = 0; i < LCD_ANIM_MAX_COUNT; i++)
	{
		lcd_anim_slot_t *slot = &s_anim_slots[i];
		if (!slot->used)
		{
			continue;
		}

		if (!slot->started)
		{
			if ((uint32_t)(now - slot->created_tick) < slot->cfg.delay_ms)
			{
				continue;
			}
			slot->started = true;
			slot->segment_tick = now;
		}

		uint32_t elapsed = lcd_anim_clamp_elapsed((uint32_t)(now - slot->segment_tick), slot->cfg.duration_ms);
		int32_t segment_start = (slot->direction > 0) ? slot->cfg.start_value : slot->cfg.end_value;
		int32_t segment_end = (slot->direction > 0) ? slot->cfg.end_value : slot->cfg.start_value;

		int32_t value = slot->cfg.path_cb(segment_start, segment_end, elapsed, slot->cfg.duration_ms);
		slot->cfg.exec_cb(slot->cfg.target, value);

		if (elapsed >= slot->cfg.duration_ms)
		{
			slot->completed_segments++;

			bool should_continue = slot->cfg.repeat;
			if (!should_continue)
			{
				uint32_t required_segments = slot->cfg.yoyo ? 2U : 1U;
				should_continue = (slot->completed_segments < required_segments);
			}

			if (!should_continue)
			{
				if (slot->cfg.done_cb != NULL)
				{
					slot->cfg.done_cb(slot->cfg.target);
				}
				slot->used = false;
				continue;
			}

			slot->direction = slot->cfg.yoyo ? (int8_t)(-slot->direction) : 1;
			slot->segment_tick = now;
		}
	}
}

void lcd_anim_manager_render(void)
{
	if (lcd_dma_busy)
	{
		return;
	}

#if LCD_USB_STREAM_ENABLE
	// 单缓冲+USB流模式下，必须等待USB发送完全空闲后再重绘，避免发送中的帧被改写造成撕裂。
	if (g_usb_controller.usb_tx_active ||
		(g_usb_controller.tx_remain_len > 0U) ||
		(g_usb_controller.tx_pending_len > 0U))
	{
		return;
	}
#endif

	lcd_fill_screen_dma(s_bg_color);

	for (uint32_t i = 0; i < LCD_LAYER_MAX_COUNT; i++)
	{
		if (s_layer_slots[i].used && s_layer_slots[i].draw_cb != NULL)
		{
			s_layer_slots[i].draw_cb(s_layer_slots[i].ctx);
		}
	}

	lcd_screen_update_dma();
}

/**
 * @brief 添加图层
 * @param ctx: 上下文指针
 * @param draw_cb: 绘制回调函数
 * @return 图层ID，失败返回-1
 */
int8_t lcd_anim_manager_add_layer(void *ctx, lcd_layer_draw_cb_t draw_cb)
{
	if (draw_cb == NULL)
	{
		return -1; // 画了个啥呀你
	}

	for (uint32_t i = 0; i < LCD_LAYER_MAX_COUNT; i++)
	{
		if (!s_layer_slots[i].used)
		{
			s_layer_slots[i].used = true;
			s_layer_slots[i].ctx = ctx;
			s_layer_slots[i].draw_cb = draw_cb;
			return (int8_t)i;
		}
	}

	return -1;
}

/**
 * @brief 移除图层
 * @param layer_id: 图层ID
 * @return true: 成功, false: 失败
 */
bool lcd_anim_manager_remove_layer(int8_t layer_id)
{
	if (layer_id < 0 || layer_id >= LCD_LAYER_MAX_COUNT)
	{
		return false;
	}

	s_layer_slots[layer_id].used = false;
	s_layer_slots[layer_id].ctx = NULL;
	s_layer_slots[layer_id].draw_cb = NULL;
	return true;
}

/**
 * @brief 清除所有图层
 */
void lcd_anim_manager_clear_layers(void)
{
	memset(s_layer_slots, 0, sizeof(s_layer_slots));
}

/**
 * @brief 启动动画
 * @param config: 动画配置
 * @return 动画ID，失败返回-1
 */
int8_t lcd_anim_start(const lcd_anim_config_t *config)
{
	if (config == NULL || config->exec_cb == NULL || config->duration_ms == 0)
	{
		return -1;
	}

	for (uint32_t i = 0; i < LCD_ANIM_MAX_COUNT; i++)
	{
		if (!s_anim_slots[i].used)
		{
			s_anim_slots[i].used = true;
			s_anim_slots[i].started = false;
			s_anim_slots[i].direction = 1;
			s_anim_slots[i].created_tick = HAL_GetTick();
			s_anim_slots[i].segment_tick = 0;
			s_anim_slots[i].completed_segments = 0;
			s_anim_slots[i].cfg = *config;

			if (s_anim_slots[i].cfg.path_cb == NULL)
			{
				s_anim_slots[i].cfg.path_cb = lcd_anim_path_linear; // default.
			}

			return (int8_t)i;
		}
	}

	return -1;
}

/**
 * @brief 停止动画
 * @param anim_id: 动画ID
 * @return true: 成功, false: 失败
 */
bool lcd_anim_stop(int8_t anim_id)
{
	if (anim_id < 0 || anim_id >= LCD_ANIM_MAX_COUNT)
	{
		return false;
	}

	if (!s_anim_slots[anim_id].used)
	{
		return false;
	}

	s_anim_slots[anim_id].used = false;
	return true;
}

/**
 * @brief 停止所有动画
 */
void lcd_anim_stop_all(void)
{
	memset(s_anim_slots, 0, sizeof(s_anim_slots));
}

#pragma endregion

#pragma region path functions

lcd_anim_path_cb_t lcd_anim_get_path(lcd_anim_ease_t ease)
{
	switch (ease)
	{
	case LCD_ANIM_EASE_IN_QUAD:
		return lcd_anim_path_ease_in_quad;
	case LCD_ANIM_EASE_OUT_QUAD:
		return lcd_anim_path_ease_out_quad;
	case LCD_ANIM_EASE_IN_OUT_QUAD:
		return lcd_anim_path_ease_in_out_quad;
	case LCD_ANIM_EASE_LINEAR:
		return lcd_anim_path_linear;
	case LCD_ANIM_EASE_IN_OUT_SINE:
		return lcd_anim_path_ease_in_out_sine;
	case LCD_ANIM_EASE_IN_SINE:
		return lcd_anim_path_ease_in_sine;
	case LCD_ANIM_EASE_OUT_SINE:
		return lcd_anim_path_ease_out_sine;
	case LCD_ANIM_EASE_IN_OUT_EXPO:
		return lcd_anim_path_ease_in_out_expo;
	case LCD_ANIM_EASE_IN_EXPO:
		return lcd_anim_path_ease_in_expo;
	case LCD_ANIM_EASE_OUT_EXPO:
		return lcd_anim_path_ease_out_expo;
	case LCD_ANIM_EASE_IN_OUT_CIRC:
		return lcd_anim_path_ease_in_out_circ;
	case LCD_ANIM_EASE_IN_CIRC:
		return lcd_anim_path_ease_in_circ;
	case LCD_ANIM_EASE_OUT_CIRC:
		return lcd_anim_path_ease_out_circ;
	case LCD_ANIM_EASE_IN_OUT_BACK:
		return lcd_anim_path_ease_in_out_back;
	case LCD_ANIM_EASE_OUT_ELASTIC:
		return lcd_anim_path_ease_out_elastic;
	default:
		return lcd_anim_path_linear;
	}
}

int32_t lcd_anim_path_linear(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	uint32_t progress_q10 = (t * 1024U) / duration;
	return lcd_anim_mix_q10(start, end, progress_q10);
}

int32_t lcd_anim_path_ease_in_quad(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	uint32_t linear_q10 = (t * 1024U) / duration;
	uint32_t progress_q10 = (linear_q10 * linear_q10) / 1024U;
	return lcd_anim_mix_q10(start, end, progress_q10);
}

int32_t lcd_anim_path_ease_out_quad(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	uint32_t linear_q10 = (t * 1024U) / duration;
	uint32_t progress_q10 = (linear_q10 * (2048U - linear_q10)) / 1024U;
	return lcd_anim_mix_q10(start, end, progress_q10);
}

int32_t lcd_anim_path_ease_in_out_quad(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	uint32_t linear_q10 = (t * 1024U) / duration;
	uint32_t progress_q10;

	if (linear_q10 < 512U)
	{
		progress_q10 = (2U * linear_q10 * linear_q10) / 1024U;
	}
	else
	{
		uint32_t inv = 1024U - linear_q10;
		progress_q10 = 1024U - (2U * inv * inv) / 1024U;
	}

	return lcd_anim_mix_q10(start, end, progress_q10);
}

int32_t lcd_anim_path_ease_in_out_sine(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased = 0.5 * (1 - cos(progress * M_PI));
	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_in_sine(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased = 1 - cos((progress * M_PI) / 2);
	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_out_sine(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased = sin((progress * M_PI) / 2);
	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_in_out_expo(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased;

	if (progress < 0.5)
	{
		eased = 0.5 * pow(2, 10 * (2 * progress - 1));
	}
	else
	{
		eased = 0.5 * (2 - pow(2, -10 * (2 * progress - 1)));
	}

	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_in_expo(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased = (progress == 0) ? 0 : pow(2, 10 * (progress - 1));
	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_out_expo(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased = (progress == 1) ? 1 : 1 - pow(2, -10 * progress);
	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_in_out_circ(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased;

	if (progress < 0.5)
	{
		eased = 0.5 * (1 - sqrt(1 - 4 * progress * progress));
	}
	else
	{
		double inv = 2 * progress - 2;
		eased = 0.5 * (sqrt(1 - inv * inv) + 1);
	}

	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_in_circ(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased = 1 - sqrt(1 - progress * progress);
	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_out_circ(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double inv = progress - 1;
	double eased = sqrt(1 - inv * inv);
	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_in_out_back(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased;

	const double c1 = 1.70158;
	const double c2 = c1 * 1.525;

	if (progress < 0.5)
	{
		eased = 0.5 * (pow(2 * progress, 2) * ((c2 + 1) * 2 * progress - c2));
	}
	else
	{
		double inv = 2 * progress - 2;
		eased = 0.5 * (pow(inv, 2) * ((c2 + 1) * inv + c2) + 2);
	}

	return (int32_t)(start + (end - start) * eased);
}

int32_t lcd_anim_path_ease_out_elastic(int32_t start, int32_t end, uint32_t elapsed, uint32_t duration)
{
	if (duration == 0)
	{
		return end;
	}

	uint32_t t = lcd_anim_clamp_elapsed(elapsed, duration);
	double progress = (double)t / (double)duration;
	double eased = sin(-13 * M_PI_2 * (progress + 1)) * pow(2, -10 * progress) + 1;
	return (int32_t)(start + (end - start) * eased);
}

#pragma endregion

#pragma region draw functions

void lcd_dma_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
	lcd_dma_plot_point(x, y, color);
}

void lcd_dma_draw_filled_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
	if (w <= 0 || h <= 0)
	{
		return;
	}

	int32_t x1 = x;
	int32_t y1 = y;
	int32_t x2 = (int32_t)x + (int32_t)w - 1;
	int32_t y2 = (int32_t)y + (int32_t)h - 1;

	if (x2 < 0 || y2 < 0 || x1 >= LCD_W || y1 >= LCD_H)
	{
		return;
	}

	if (x1 < 0)
	{
		x1 = 0;
	}
	if (y1 < 0)
	{
		y1 = 0;
	}
	if (x2 >= LCD_W)
	{
		x2 = LCD_W - 1;
	}
	if (y2 >= LCD_H)
	{
		y2 = LCD_H - 1;
	}

	// 单次调用完成大块填充，比逐点调用更高效。
	lcd_set_area_color((int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2, color);
}

void lcd_dma_draw_circle(int16_t x0, int16_t y0, uint8_t r, uint16_t color)
{
	int16_t x = r;
	int16_t y = 0;
	int16_t error = 1 - (int16_t)r;
	uint16_t swapped_color = __builtin_bswap16(color);

	while (x >= y)
	{
		lcd_draw_point_dma_swapped(x0 + x, y0 + y, swapped_color);
		lcd_draw_point_dma_swapped(x0 + y, y0 + x, swapped_color);
		lcd_draw_point_dma_swapped(x0 - y, y0 + x, swapped_color);
		lcd_draw_point_dma_swapped(x0 - x, y0 + y, swapped_color);
		lcd_draw_point_dma_swapped(x0 - x, y0 - y, swapped_color);
		lcd_draw_point_dma_swapped(x0 - y, y0 - x, swapped_color);
		lcd_draw_point_dma_swapped(x0 + y, y0 - x, swapped_color);
		lcd_draw_point_dma_swapped(x0 + x, y0 - y, swapped_color);

		y++;
		if (error < 0)
		{
			error += 2 * y + 1;
		}
		else
		{
			x--;
			error += 2 * (y - x) + 1;
		}
	}
}

void lcd_dma_draw_label(const lcd_label_t *label)
{
	if (label == NULL || label->text == NULL)
	{
		return;
	}

	lcd_draw_string(label->x, label->y, label->fg_color, label->bg_color, label->size, label->text);
}

//! 注意这个函数会要求你把照片数据放在 RAM 中，如果照片较大可能会占用较多内存，适合小图标等使用(如果内存不够可以直接调用lcd.c下的原生函数)
void lcd_dma_draw_picture(int16_t x, int16_t y, int16_t width, int16_t height, const uint16_t *data)
{
	if (data == NULL || width <= 0 || height <= 0)
	{
		return;
	}

	lcd_draw_picture_dma(x, y, width, height, data);
}



void lcd_draw_rect_layer(void *ctx)
{
	lcd_rect_t *rect = (lcd_rect_t *)ctx;
	if (rect == NULL)
	{
		return;
	}

	lcd_dma_draw_filled_rect(rect->x, rect->y, rect->w, rect->h, rect->color);
}

void lcd_draw_circle_layer(void *ctx)
{
	lcd_circle_t *circle = (lcd_circle_t *)ctx;
	if (circle == NULL)
	{
		return;
	}

	lcd_dma_draw_circle(circle->x, circle->y, circle->radius, circle->color);
}

void lcd_draw_label_layer(void *ctx)
{
	lcd_label_t *label = (lcd_label_t *)ctx;
	lcd_dma_draw_label(label);
}

void lcd_draw_picture_layer(void *ctx)
{
	lcd_picture_t *pic = (lcd_picture_t *)ctx;
	if (pic == NULL || pic->data == NULL)
	{
		return;
	}

	lcd_draw_picture_dma(pic->x, pic->y, pic->width, pic->height, pic->data);
}

#pragma endregion

#pragma region exec functions	

void lcd_anim_exec_set_i16(void *target, int32_t value)
{
	if (target == NULL)
	{
		return;
	}

	if (value > INT16_MAX)
	{
		value = INT16_MAX;
	}
	if (value < INT16_MIN)
	{
		value = INT16_MIN;
	}

	*((int16_t *)target) = (int16_t)value;
}

void lcd_anim_exec_set_u16(void *target, int32_t value)
{
	if (target == NULL)
	{
		return;
	}

	if (value < 0)
	{
		value = 0;
	}
	if (value > UINT16_MAX)
	{
		value = UINT16_MAX;
	}

	*((uint16_t *)target) = (uint16_t)value;
}

void lcd_anim_exec_set_u8(void *target, int32_t value)
{
	if (target == NULL)
	{
		return;
	}

	if (value < 0)
	{
		value = 0;
	}
	if (value > UINT8_MAX)
	{
		value = UINT8_MAX;
	}

	*((uint8_t *)target) = (uint8_t)value;
}

#pragma endregion

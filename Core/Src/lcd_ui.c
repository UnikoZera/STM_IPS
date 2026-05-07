/*
 * lcd_ui.c
 *
 *  Created on: 2026年4月2日
 *      Author: UnikoZera
 * tips: 在每次你想要加入新的元素ui的时候通过不同的region来组织代码，方便后续维护和查找。
 *       每次初始化的时候都要确保你的元素在每个region的定义区被正确初始化，并且在lcd_ui_init函数中被正确添加到动画管理器里。
 */

#include "lcd_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include "math.h"

#pragma region UI元素定义

static lcd_rect_t g_rect = {0, 24, 28, 20, RED};
static lcd_circle_t g_circle = {20, 58, 10, YELLOW};
static lcd_label_t g_label = {6, 4, WHITE, BLACK, 8, "DMA ANIM"};
static lcd_label_t g_label2 = {12, 14, WHITE, BLACK, 8, "TEST"};
static lcd_label_t g_label3 = {12, 14, WHITE, BLACK, 8, "TEST"};
static lcd_circle_t g_circle2 = {60, 58, 10, CYAN};
static lcd_rect_t g_rect2 = {50, 24, 35, 30, MAGENTA};

#pragma endregion

void lcd_ui_init(void)
{
    lcd_anim_manager_init();
    lcd_anim_manager_set_bg(BLACK);

    #pragma region 添加元素到动画管理器

    lcd_anim_manager_add_layer(&g_label, lcd_draw_label_layer);
    lcd_anim_manager_add_layer(&g_rect, lcd_draw_rect_layer);
    lcd_anim_manager_add_layer(&g_circle, lcd_draw_circle_layer);
    lcd_anim_manager_add_layer(&g_circle2, lcd_draw_circle_layer);
    lcd_anim_manager_add_layer(&g_label2, lcd_draw_label_layer);
    lcd_anim_manager_add_layer(&g_rect2, lcd_draw_rect_layer);
    lcd_anim_manager_add_layer(&g_label3, lcd_draw_label_layer);

    #pragma endregion

    #pragma region 定义动画并启动(可选,不定义动画元素也会被正常渲染，但定义动画后元素会动起来)

    lcd_anim_config_t rect_anim_x = {
      .target = &g_rect.x,
      .start_value = 0,
      .end_value = LCD_W - g_rect.w,
      .duration_ms = 1300,
      .delay_ms = 0,
      .repeat = true,
      .yoyo = true,
      .exec_cb = lcd_anim_exec_set_i16,
      .done_cb = NULL,
      .path_cb = lcd_anim_get_path(LCD_ANIM_EASE_IN_OUT_SINE),
    };
    lcd_anim_start(&rect_anim_x);

    lcd_anim_config_t circle_anim_y = {
      .target = &g_circle.y,
      .start_value = 20,
      .end_value = LCD_H - 20,
      .duration_ms = 900,
      .delay_ms = 0,
      .repeat = true,
      .yoyo = true,
      .exec_cb = lcd_anim_exec_set_i16,
      .done_cb = NULL,
      .path_cb = lcd_anim_get_path(LCD_ANIM_EASE_OUT_ELASTIC),
    };
    lcd_anim_start(&circle_anim_y);

    lcd_anim_config_t circle2_anim_y = {
      .target = &g_circle2.y,
      .start_value = 20,
      .end_value = LCD_H - 20,
      .duration_ms = 900,
      .delay_ms = 0,
      .repeat = true,
      .yoyo = false,
      .exec_cb = lcd_anim_exec_set_i16,
      .done_cb = NULL,
      .path_cb = lcd_anim_get_path(LCD_ANIM_EASE_OUT_CIRC),
    };
    lcd_anim_start(&circle2_anim_y);

    lcd_anim_config_t rect2_anim_x = {
      .target = &g_rect2.x,
      .start_value = 50,
      .end_value = LCD_W - g_rect2.w - 50,
      .duration_ms = 1100,
      .delay_ms = 0,
      .repeat = true,
      .yoyo = true,
      .exec_cb = lcd_anim_exec_set_i16,
      .done_cb = NULL,
      .path_cb = lcd_anim_get_path(LCD_ANIM_EASE_IN_OUT_BACK),
    };
    lcd_anim_start(&rect2_anim_x);

    lcd_anim_config_t label_anim_color = {
      .target = &g_label.fg_color,
      .start_value = RED,
      .end_value = WHITE,
      .duration_ms = 1000,
      .delay_ms = 0,
      .repeat = true,
      .yoyo = true,
      .exec_cb = lcd_anim_exec_set_u16,
      .done_cb = NULL,
      .path_cb = lcd_anim_get_path(LCD_ANIM_EASE_LINEAR),
    };
    lcd_anim_start(&label_anim_color);

    lcd_anim_config_t label2_anim_color = {
      .target = &g_label2.fg_color,
      .start_value = CYAN,
      .end_value = MAGENTA,
      .duration_ms = 3000,
      .delay_ms = 0,
      .repeat = true,
      .yoyo = true,
      .exec_cb = lcd_anim_exec_set_u16,
      .done_cb = NULL,
      .path_cb = lcd_anim_get_path(LCD_ANIM_EASE_IN_OUT_SINE),
    };
    lcd_anim_start(&label2_anim_color);

    lcd_anim_config_t label3_anim_color = {
      .target = &g_label3.x,
      .start_value = 0,
      .end_value = 60,
      .duration_ms = 3000,
      .delay_ms = 300,
      .repeat = true,
      .yoyo = true,
      .exec_cb = lcd_anim_exec_set_u16,
      .done_cb = NULL,
      .path_cb = lcd_anim_get_path(LCD_ANIM_EASE_IN_OUT_SINE),
    };
    lcd_anim_start(&label3_anim_color);

    lcd_anim_config_t label3_anim_color2 = {
      .target = &g_label3.y,
      .start_value = 14,
      .end_value = 40,
      .duration_ms = 1000,
      .delay_ms = 0,
      .repeat = true,
      .yoyo = true,
      .exec_cb = lcd_anim_exec_set_u16,
      .done_cb = NULL,
      .path_cb = lcd_anim_get_path(LCD_ANIM_EASE_OUT_ELASTIC),
    };
    lcd_anim_start(&label3_anim_color2);

    #pragma endregion

    lcd_anim_manager_render();
}

/**
 * @brief 在这个函数里面更新UI状态
 * 
 */
void lcd_ui_change(void)
{
  lcd_calculate_usage();
  static char str_buf[32];
  static char fps_buf[16];
  snprintf(fps_buf, sizeof(fps_buf), "FPS:%u", lcd_fps);
  g_label3.text = fps_buf;
  static char last_str_buf[8];
  snprintf(str_buf, sizeof(str_buf), "usage percent:%u %%", cpu_usage_percent);
  g_label.text = str_buf;
  g_rect.color = BLUE;
  g_circle.color = GREEN;

  for (uint8_t i = 0; i < sizeof(last_str_buf); i++)
  {
    last_str_buf[i] = 32 + (rand() % 95);
  }
    g_label2.text = last_str_buf;

    lcd_calculate_fps();
}

/**
 * @brief 调用这个函数来更新UI，通常在主循环里调用。它会处理动画状态并重新渲染UI。
 * @attention 先更新tasker再render，确保动画状态被正确更新后再渲染到屏幕上。
 * 
 */
void lcd_ui_updater(void)
{
    lcd_ui_change();            // 这个是更新UI状态的，你可以在这个函数里修改元素属性来改变UI显示的内容和样式
    lcd_anim_manager_task();    // 这个是更新动画管理器的状态，计算动画进度并调用exec_cb更新元素属性的
    lcd_anim_manager_render();  // 这个是更新屏幕ui渲染状态的
}

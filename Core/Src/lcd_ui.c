/*
 * lcd_ui.c
 *
 *  Created on: 2026年4月2日
 *      Author: UnikoZera
 */

#include "lcd_ui.h"

/**
 * @brief UI元素的全局变量，根据需要添加更多的元素和状态变量
 * 
 */
static lcd_rect_t g_rect = {0, 24, 28, 20, RED};
static lcd_circle_t g_circle = {20, 58, 10, YELLOW};
static lcd_label_t g_label = {6, 4, WHITE, BLACK, 8, "DMA ANIM"};

/**
 * @brief 初始化UI，设置动画和图层。在这里添加更多的图层和动画配置。
 * 
 */
void lcd_ui_init(void)
{
    lcd_anim_manager_init();
    lcd_anim_manager_set_bg(BLACK);

    lcd_anim_manager_add_layer(&g_label, lcd_draw_label_layer);
    lcd_anim_manager_add_layer(&g_rect, lcd_draw_rect_layer);
    lcd_anim_manager_add_layer(&g_circle, lcd_draw_circle_layer);

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

    lcd_anim_manager_render();
}

/**
 * @brief 在这个函数里面更新UI状态
 * 
 */
void lcd_ui_change(void)
{
    g_label.text = "CHANGED!";
    g_rect.color = BLUE;
    g_circle.color = GREEN;
}

/**
 * @brief 调用这个函数来更新UI，通常在主循环里调用。它会处理动画状态并重新渲染UI。
 * 
 */
void lcd_ui_updater(void)
{
    lcd_anim_manager_task();
    lcd_anim_manager_render();
}
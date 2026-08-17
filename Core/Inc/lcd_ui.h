/*
 * lcd_ui.h
 *
 *  Created on: 2026年4月2日
 *      Author: UnikoZera
 *
 *  UI 层接口：基于「页面状态机 + 组件」组织。
 *
 *  - 每个页面(screen)负责自己的图层注册、内容更新与退出清理。
 *  - 组件(如状态栏)可跨页面复用，由页面在 enter 时挂载。
 *  - 新增页面：在 lcd_screen_id_t 末尾(COUNT 之前)加枚举，并在 lcd_ui.c 的
 *    s_screens 表里注册对应的 enter/update/exit 实现。
 */

#ifndef INC_LCD_UI_H_
#define INC_LCD_UI_H_

#include "lcd_driver.h"

/**
 * @brief 页面 ID
 * @attention LCD_SCREEN_COUNT 必须始终位于枚举末尾，新增页面加在它之前。
 */
typedef enum
{
    LCD_SCREEN_HOME = 0,   /* 主界面：视频 / 图片内容展示 */
    LCD_SCREEN_SETTINGS,   /* 系统诊断页 */
    LCD_SCREEN_COUNT       /* 页面总数（哨兵，勿在其后加页面） */
} lcd_screen_id_t;

void lcd_ui_init(void);
void lcd_ui_change(void);
void lcd_ui_updater(void);

/* ────── 页面状态机 ────── */

/**
 * @brief 切换到指定页面。
 *        会先调用旧页面的 exit、停止所有动画、清空图层，再挂载状态栏并
 *        进入新页面。切换后立即渲染一帧。
 * @param screen 目标页面 ID（越界或与当前相同则忽略）
 */
void lcd_ui_switch_screen(lcd_screen_id_t screen);

/**
 * @brief 获取当前页面 ID
 */
lcd_screen_id_t lcd_ui_current_screen(void);

#endif /* INC_LCD_UI_H_ */

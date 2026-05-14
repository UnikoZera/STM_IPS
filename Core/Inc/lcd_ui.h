/*
 * lcd_ui.h
 *
 *  Created on: 2026年4月2日
 *      Author: UnikoZera
 */

#ifndef INC_LCD_UI_H_
#define INC_LCD_UI_H_

#include "lcd_driver.h"

void lcd_ui_init(void);
void lcd_ui_change(void);
void lcd_ui_updater(void);
void lcd_ui_set_picture(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *ram_buf);

#endif /* INC_LCD_UI_H_ */

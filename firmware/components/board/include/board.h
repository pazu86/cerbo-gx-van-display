#pragma once
/*
 * Board support for the Waveshare ESP32-S3-Touch-LCD-7B.
 *
 * Owns the shared I2C bus, the CH32V003 IO expander, the 1024x600 RGB panel,
 * the GT911 touch controller and the LVGL adapter. After board_init() the
 * LVGL task is running; wrap every LVGL call from other tasks in
 * board_lvgl_lock()/board_lvgl_unlock().
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600

/* Bring up I2C, expander, panel, touch and LVGL. Backlight is left on. */
esp_err_t board_init(void);

lv_display_t *board_display(void);

bool board_lvgl_lock(int timeout_ms);
void board_lvgl_unlock(void);

/* Backlight brightness 0-100 %. 0 switches the backlight off entirely. */
void board_backlight_set(uint8_t percent);
uint8_t board_backlight_get(void);

/* Battery (PH2.0 LiPo connector) voltage in volts, read via the expander ADC. */
float board_battery_voltage(void);

#ifdef __cplusplus
}
#endif

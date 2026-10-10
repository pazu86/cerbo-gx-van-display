#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";

/* ---- I2C ---- */
#define I2C_PORT        I2C_NUM_0
#define I2C_SDA         GPIO_NUM_8
#define I2C_SCL         GPIO_NUM_9
#define I2C_FREQ_HZ     (400 * 1000)
#define I2C_TIMEOUT_MS  100

/* ---- CH32V003 IO expander (Waveshare firmware) ---- */
#define EXP_ADDR        0x24
#define EXP_REG_MODE    0x02  /* bit=1 -> output */
#define EXP_REG_OUTPUT  0x03
#define EXP_REG_INPUT   0x04
#define EXP_REG_PWM     0x05  /* backlight PWM, inverted: 0 = brightest */
#define EXP_REG_ADC     0x06  /* 10-bit battery ADC, little endian */

#define EXIO_TOUCH_RST  1
#define EXIO_BACKLIGHT  2
#define EXIO_LCD_RST    3
#define EXIO_SD_CS      4
#define EXIO_USB_CAN    5     /* 0 = USB on the Type-C, 1 = CAN transceiver */
#define EXIO_LCD_VDD_EN 6     /* panel power (LCD_VDD_EN) - must be high or the LCD stays black */

/* The expander firmware misbehaves near 100 % duty, Waveshare caps it at 97 */
#define EXP_PWM_MAX_PCT 97

/* ---- RGB panel ---- */
#define LCD_PCLK_HZ     (24 * 1000 * 1000)
/* 20 lines x2 buffers = 80 kB of internal RAM. 10 lines tore while map
 * tiles were downloading/decoding (heavy PSRAM traffic). */
#define LCD_BOUNCE_PX   (BOARD_LCD_H_RES * 20)
#define TOUCH_INT       GPIO_NUM_4

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_exp;
static SemaphoreHandle_t s_exp_lock;
static uint8_t s_exp_out;
static uint8_t s_backlight_pct;
static lv_display_t *s_disp;

/* ------------------------------------------------------------------ */
/* IO expander                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t exp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_exp, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t exp_set(uint8_t pin, bool level)
{
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    if (level) {
        s_exp_out |= (1 << pin);
    } else {
        s_exp_out &= ~(1 << pin);
    }
    esp_err_t err = exp_write(EXP_REG_OUTPUT, s_exp_out);
    xSemaphoreGive(s_exp_lock);
    return err;
}

static esp_err_t exp_init(void)
{
    s_exp_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_exp_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXP_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_exp), TAG, "add expander");
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_MODE, 0xFF), TAG, "expander not responding");

    /* Waveshare's default is all-high; we differ only in starting with the
     * backlight off and keeping the Type-C on USB rather than CAN. */
    s_exp_out = 0xFF & ~(1 << EXIO_BACKLIGHT) & ~(1 << EXIO_USB_CAN);
    return exp_write(EXP_REG_OUTPUT, s_exp_out);
}

void board_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_backlight_pct = percent;
    if (percent == 0) {
        exp_set(EXIO_BACKLIGHT, false);
        return;
    }
    uint8_t inverted = 100 - percent;
    if (inverted > EXP_PWM_MAX_PCT) {
        inverted = EXP_PWM_MAX_PCT;
    }
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    exp_write(EXP_REG_PWM, (uint8_t)(inverted * 255 / 100));
    xSemaphoreGive(s_exp_lock);
    exp_set(EXIO_BACKLIGHT, true);
}

uint8_t board_backlight_get(void)
{
    return s_backlight_pct;
}

float board_battery_voltage(void)
{
    uint8_t reg = EXP_REG_ADC;
    uint8_t data[2] = {0};
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit_receive(s_exp, &reg, 1, data, 2, I2C_TIMEOUT_MS);
    xSemaphoreGive(s_exp_lock);
    if (err != ESP_OK) {
        return 0.0f;
    }
    uint16_t raw = (data[1] << 8) | data[0];
    /* 10-bit ADC, 3.3 V reference, 3:1 divider */
    return raw * 3.0f * 3.3f / 1023.0f;
}

/* ------------------------------------------------------------------ */
/* Panel + touch                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t panel_init(uint8_t num_fbs, esp_lcd_panel_handle_t *out)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 162,
            .hsync_back_porch = 152,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 45,
            .vsync_back_porch = 13,
            .vsync_front_porch = 3,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = num_fbs,
        .bounce_buffer_size_px = LCD_BOUNCE_PX,
        .dma_burst_size = 64,
        .hsync_gpio_num = GPIO_NUM_46,
        .vsync_gpio_num = GPIO_NUM_3,
        .de_gpio_num = GPIO_NUM_5,
        .pclk_gpio_num = GPIO_NUM_7,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = {
            /* B3-B7 */ GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_10,
            /* G2-G7 */ GPIO_NUM_39, GPIO_NUM_0, GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21,
            /* R3-R7 */ GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40,
        },
        .flags.fb_in_psram = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, out), TAG, "new rgb panel");
    return esp_lcd_panel_init(*out);
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *out)
{
    /* GT911 address select: INT held low while reset is released -> 0x5D */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << TOUCH_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    exp_set(EXIO_TOUCH_RST, false);
    gpio_set_level(TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    exp_set(EXIO_TOUCH_RST, true);
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &tp_io), TAG, "touch io");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = TOUCH_INT,
    };
    return esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, out);
}

/* ------------------------------------------------------------------ */

esp_err_t board_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_LOGI(TAG, "init: i2c + expander (int free %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus");
    ESP_RETURN_ON_ERROR(exp_init(), TAG, "io expander");

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;

    ESP_LOGI(TAG, "init: rgb panel (int free %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(panel_init(esp_lv_adapter_get_required_frame_buffer_count(tear, rotation), &panel),
                        TAG, "panel");

    ESP_LOGI(TAG, "init: touch (int free %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    esp_lcd_touch_handle_t touch = NULL;
    esp_err_t err = touch_init(&touch);
    if (err != ESP_OK) {
        /* Keep going without touch so the screen is still useful */
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(err));
        touch = NULL;
    }

    ESP_LOGI(TAG, "init: lvgl (int free %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    esp_lv_adapter_config_t lv_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    lv_cfg.task_stack_size = 12 * 1024;
    lv_cfg.stack_in_psram = true;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&lv_cfg), TAG, "lvgl adapter");

    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel, NULL, BOARD_LCD_H_RES, BOARD_LCD_V_RES, rotation);
    disp_cfg.profile.use_psram = true;
    /* In triple-partial mode the adapter keeps this draw buffer in internal RAM
     * regardless of use_psram: 50 lines (default) = 100 kB. 20 lines = 40 kB. */
    disp_cfg.profile.buffer_height = 20;
    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "register display");

    if (touch) {
        esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, touch);
        ESP_RETURN_ON_FALSE(esp_lv_adapter_register_touch(&touch_cfg), ESP_FAIL, TAG, "register touch");
    }

    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "lvgl start");

    board_backlight_set(80);
    ESP_LOGI(TAG, "board ready (touch %s, int free %u, largest %u)", touch ? "ok" : "MISSING",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

lv_display_t *board_display(void)
{
    return s_disp;
}

bool board_lvgl_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void board_lvgl_unlock(void)
{
    esp_lv_adapter_unlock();
}

#include "bed_display.h"
#include "bed_ui.h"
#include "projector.h"
#include "projector_logic.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define LCD_SPI SPI3_HOST
#define LED_PIN 3
#define LED_MAX_DUTY 750

static const char *TAG = "bed-display";
static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_panel_handle_t lcd_panel;
static bool provisioning;
static char ap_ssid[33];
static char ap_password[17];

static void set_pwm(uint8_t brightness)
{
    uint32_t duty = ((uint32_t)brightness * LED_MAX_DUTY + 50) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void apply_orientation(unsigned rotation, bool mirror)
{
    bool swap_xy, mirror_x, mirror_y;
    projector_orientation_panel(rotation, mirror, &swap_xy, &mirror_x, &mirror_y);
    esp_lcd_panel_swap_xy(lcd_panel, swap_xy);
    esp_lcd_panel_mirror(lcd_panel, mirror_x, mirror_y);
    lv_obj_invalidate(lv_screen_active());
}

static void display_task(void *argument)
{
    (void)argument;
    bool last_power = false;
    uint8_t last_brightness = 0;
    int last_orientation = -1;
    while (true) {
        projector_tick();
        /* About 2.5 KB: static keeps it off this task's stack. */
        static projector_snapshot_t snapshot;
        projector_snapshot(&snapshot);
        uint8_t target = snapshot.calibration.active && !provisioning ?
            (snapshot.brightness < 30 ? 30 : snapshot.brightness) :
            snapshot.power ? snapshot.brightness : 0;
        if (target != last_brightness || snapshot.power != last_power) {
            set_pwm(target);
            last_brightness = target;
            last_power = snapshot.power;
        }
        if (lvgl_port_lock(1000)) {
            int orientation = snapshot.calibration.rotation | (snapshot.calibration.mirror ? 4 : 0);
            if (orientation != last_orientation) {
                apply_orientation(snapshot.calibration.rotation, snapshot.calibration.mirror);
                last_orientation = orientation;
            }
            bed_ui_render(&snapshot, time(NULL), provisioning ? ap_ssid : NULL, ap_password);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t bed_display_init(void)
{
    ledc_timer_config_t timer = {.speed_mode=LEDC_LOW_SPEED_MODE, .timer_num=LEDC_TIMER_0,
        .duty_resolution=LEDC_TIMER_10_BIT, .freq_hz=200, .clk_cfg=LEDC_AUTO_CLK};
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "PWM timer");
    ledc_channel_config_t channel = {.speed_mode=LEDC_LOW_SPEED_MODE, .channel=LEDC_CHANNEL_0,
        .timer_sel=LEDC_TIMER_0, .intr_type=LEDC_INTR_DISABLE, .gpio_num=LED_PIN, .duty=0, .hpoint=0};
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "PWM channel");
    spi_bus_config_t bus = {.sclk_io_num=13, .mosi_io_num=10, .miso_io_num=GPIO_NUM_NC,
        .quadwp_io_num=GPIO_NUM_NC, .quadhd_io_num=GPIO_NUM_NC,
        .max_transfer_sz=128 * 8 * sizeof(uint16_t)};
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");
    esp_lcd_panel_io_spi_config_t io = {.dc_gpio_num=12, .cs_gpio_num=11,
        .pclk_hz=40 * 1000 * 1000, .lcd_cmd_bits=8, .lcd_param_bits=8,
        .spi_mode=0, .trans_queue_depth=10};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI, &io, &lcd_io), TAG, "LCD IO");
    esp_lcd_panel_dev_config_t panel = {.reset_gpio_num=9, .color_space=ESP_LCD_COLOR_SPACE_BGR,
        .bits_per_pixel=16};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7735(lcd_io, &panel, &lcd_panel), TAG, "LCD panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(lcd_panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(lcd_panel), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(lcd_panel, true, true), TAG, "LCD mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), TAG, "LCD on");
    lvgl_port_cfg_t lvconfig = {.task_priority=8, .task_stack=8192,
        .task_affinity=1, .task_max_sleep_ms=100, .timer_period_ms=5};
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvconfig), TAG, "LVGL init");
    lvgl_port_display_cfg_t disp = {.io_handle=lcd_io, .panel_handle=lcd_panel,
        .buffer_size=128 * 8 * sizeof(uint16_t), .double_buffer=true,
        .hres=128, .vres=128, .monochrome=false,
        .rotation={.swap_xy=false, .mirror_x=true, .mirror_y=true},
        .flags={.buff_dma=true, .full_refresh=false, .buff_spiram=false, .swap_bytes=true}};
    lv_display_t *lvdisplay = lvgl_port_add_disp(&disp);
    if (!lvdisplay) return ESP_ERR_NO_MEM;
    lv_display_set_rotation(lvdisplay, LV_DISPLAY_ROTATION_90);
    if (!lvgl_port_lock(1000)) return ESP_ERR_TIMEOUT;
    bed_ui_create(lv_screen_active());
    lvgl_port_unlock();
    return ESP_OK;
}

void bed_display_start(void)
{
    /* Rendering runs newlib snprintf/strftime and LVGL text layout: 4 KB overflowed
     * silently (past the canary) once HA data made the status row format a number. */
    xTaskCreatePinnedToCore(display_task, "projector-display", 8192, NULL, 5, NULL, 1);
}

void bed_display_provisioning(const char *ssid, const char *password)
{
    snprintf(ap_ssid, sizeof(ap_ssid), "%s", ssid);
    snprintf(ap_password, sizeof(ap_password), "%s", password);
    provisioning = true;
}

void bed_display_provisioning_done(void)
{
    provisioning = false;
}

#include "bed_display.h"
#include "projector.h"
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
static lv_obj_t *projection;
static lv_obj_t *calibration_ring;
static lv_obj_t *calibration_horizontal;
static lv_obj_t *calibration_vertical;
static lv_obj_t *header;
static lv_obj_t *main_label;
static lv_obj_t *lines[3];
static lv_obj_t *door_icon;
static lv_obj_t *alarm_icon;
static lv_obj_t *door_count;
static lv_obj_t *alarm_text;
static bool provisioning;
static char ap_ssid[33];
static char ap_password[17];

static int scaled(int size, int reference)
{
    return (size * reference + 64) / 128;
}

/* Fonts from largest to smallest; labels take the largest one that fits and
 * fall back to dots only when even the smallest one is too wide. */
static const lv_font_t *const time_fonts[] = {&lv_font_montserrat_36, &lv_font_montserrat_28,
    &lv_font_montserrat_24, &lv_font_montserrat_20, &lv_font_montserrat_14, &lv_font_montserrat_12};
static const lv_font_t *const text_fonts[] = {&lv_font_montserrat_14, &lv_font_montserrat_12,
    &lv_font_montserrat_10, &lv_font_montserrat_8};
#define ARRAY_COUNT(items) ((int)(sizeof(items) / sizeof((items)[0])))

typedef struct {
    const char *state, *short_label, *label;
    uint32_t color;
} alarm_mode_t;

static const alarm_mode_t alarm_modes[] = {
    {"disarmed", "OFF", "Disinserito", 0x607080},
    {"armed_home", "CASA", "In casa", 0xffd040},
    {"armed_away", "FUORI", "Fuori casa", 0xffa000},
    {"armed_night", "NOTTE", "Notte", 0x8090ff},
    {"armed_vacation", "VACANZA", "Vacanza", 0xffa000},
    {"armed_custom_bypass", "PERS.", "Personalizzato", 0xffa000},
    {"arming", "INS...", "Inserimento...", 0xffd040},
    {"pending", "ATTESA", "In attesa", 0xff6020},
    {"triggered", "SCATTATO", "SCATTATO", 0xff3030},
};
static const alarm_mode_t alarm_unknown = {"unknown", "?", "?", 0x607080};

static const alarm_mode_t *alarm_mode(const projector_snapshot_t *s)
{
    if (s->ha_fresh)
        for (int i = 0; i < ARRAY_COUNT(alarm_modes); ++i)
            if (!strcmp(s->alarm, alarm_modes[i].state)) return &alarm_modes[i];
    return &alarm_unknown;
}

static int text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* Half of the widest horizontal box at rows y..y+height-1 that stays inside the circle. */
static int circle_half_width(int diameter, int y, int height)
{
    int radius = diameter / 2;
    int top_distance = y > radius ? y - radius : radius - y;
    int bottom = y + height - 1;
    int bottom_distance = bottom > radius ? bottom - radius : radius - bottom;
    int distance = top_distance > bottom_distance ? top_distance : bottom_distance;
    int remaining = radius * radius - distance * distance;
    int half_width = 0;
    while ((half_width + 1) * (half_width + 1) <= remaining) ++half_width;
    half_width -= 2;
    return half_width < 1 ? 1 : half_width;
}

/* Keep the complete label box inside the circular projection without LVGL masks.
 * Rounded child clipping needs LV_DRAW_SW_COMPLEX, which is disabled in this build. */
static void place_circle_label(lv_obj_t *obj, int diameter, int y, int height)
{
    int half_width = circle_half_width(diameter, y, height);
    lv_obj_set_pos(obj, diameter / 2 - half_width, y);
    lv_obj_set_size(obj, half_width * 2, height);
}

/* Set text with the largest font (no taller than slot) whose line fits the circle,
 * vertically centred in the slot. */
static void fit_circle_label(lv_obj_t *obj, const char *text, int diameter, int y, int slot,
                             const lv_font_t *const *fonts, int count)
{
    const lv_font_t *font = fonts[count - 1];
    for (int i = 0; i < count; ++i) {
        int height = lv_font_get_line_height(fonts[i]);
        if (height > slot) continue;
        int top = y + (slot - height) / 2;
        if (text_width(text, fonts[i]) <= circle_half_width(diameter, top, height) * 2) {
            font = fonts[i];
            break;
        }
    }
    int height = lv_font_get_line_height(font);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_label_set_text(obj, text);
    place_circle_label(obj, diameter, y + (slot - height) / 2, height);
}

static void show(lv_obj_t *obj, bool visible)
{
    bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (visible && hidden) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else if (!visible && !hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_pwm(uint8_t brightness)
{
    uint32_t duty = ((uint32_t)brightness * LED_MAX_DUTY + 50) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void text_widget(const char *name, const projector_snapshot_t *s,
                        const projector_page_t *page, char *out, size_t size)
{
    if (!strcmp(name, "openings")) {
        if (s->ha_fresh && s->openings_known) snprintf(out, size, "Porte/finestre: %u", s->openings);
        else snprintf(out, size, "Porte/finestre: ?");
    } else if (!strcmp(name, "alarm"))
        snprintf(out, size, "Allarme: %s", alarm_mode(s)->label);
    else if (!strcmp(name, "weather"))
        snprintf(out, size, "%s %s", s->ha_fresh && s->weather[0] ? s->weather : "Meteo ?",
                 s->ha_fresh ? s->temperature : "");
    else if (!strcmp(name, "date")) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        if (now > 1577836800) strftime(out, size, "%d/%m/%Y", &local);
        else snprintf(out, size, "Ora in attesa di NTP");
    } else if (!strcmp(name, "text")) snprintf(out, size, "%s", page->text);
    else if (!strncmp(name, "entity", 6) && name[6] >= '1' && name[6] <= '4') {
        int index = name[6] - '1';
        snprintf(out, size, "%s", s->ha_fresh && s->extras[index][0] ? s->extras[index] : "Dato HA ?");
    } else out[0] = 0;
}

/* Clock page bottom row: [door] count  (alarm) mode, centred and shrunk as one group. */
static void render_status_row(const projector_snapshot_t *s, int diameter)
{
    enum { DOOR_W = 10, DOOR_H = 13, DOT = 10, PAD = 3, GAP = 8 };
    char count[4] = "?";
    if (s->ha_fresh && s->openings_known) snprintf(count, sizeof(count), "%u", s->openings);
    const alarm_mode_t *alarm = alarm_mode(s);
    int y = scaled(diameter, 67), slot = 16;
    int first = diameter >= 104 ? 0 : 1;
    int available = circle_half_width(diameter, y, slot) * 2;
    const lv_font_t *font = text_fonts[first];
    for (int i = first; i < ARRAY_COUNT(text_fonts); ++i) {
        font = text_fonts[i];
        int total = DOOR_W + PAD + text_width(count, font) + GAP + DOT + PAD +
                    text_width(alarm->short_label, font);
        if (total <= available) break;
    }
    int height = lv_font_get_line_height(font);
    int count_width = text_width(count, font);
    int fixed = DOOR_W + PAD + count_width + GAP + DOT + PAD;
    int alarm_width = text_width(alarm->short_label, font);
    if (fixed + alarm_width > available) alarm_width = available - fixed > 1 ? available - fixed : 1;
    int x = diameter / 2 - (fixed + alarm_width) / 2;
    int middle = y + slot / 2;
    lv_obj_set_pos(door_icon, x, middle - DOOR_H / 2);
    x += DOOR_W + PAD;
    lv_obj_set_style_text_font(door_count, font, 0);
    lv_label_set_text(door_count, count);
    lv_obj_set_pos(door_count, x, middle - height / 2);
    x += count_width + GAP;
    lv_obj_set_pos(alarm_icon, x, middle - DOT / 2);
    x += DOT + PAD;
    lv_obj_set_style_text_font(alarm_text, font, 0);
    lv_label_set_text(alarm_text, alarm->short_label);
    lv_obj_set_pos(alarm_text, x, middle - height / 2);
    lv_obj_set_size(alarm_text, alarm_width, height);
    lv_obj_set_style_bg_color(alarm_icon, lv_color_hex(alarm->color), 0);
    lv_obj_set_style_border_color(door_icon,
        lv_color_hex(!s->ha_fresh || !s->openings_known ? 0x607080 :
                     s->openings ? 0xffa000 : 0x80c080), 0);
    show(door_icon, true); show(alarm_icon, true);
    show(door_count, true); show(alarm_text, true);
}

static void render(const projector_snapshot_t *s)
{
    int diameter = s->calibration.diameter;
    int left = s->calibration.center_x - diameter / 2;
    int top = s->calibration.center_y - diameter / 2;
    bool calibrating = s->calibration.active && !provisioning;
    lv_obj_set_pos(projection, left, top);
    lv_obj_set_size(projection, diameter, diameter);
    show(projection, !calibrating);
    show(calibration_ring, calibrating);
    show(calibration_horizontal, calibrating);
    show(calibration_vertical, calibrating);
    if (calibrating) {
        lv_obj_set_pos(calibration_ring, left, top);
        lv_obj_set_size(calibration_ring, diameter, diameter);
        lv_obj_set_pos(calibration_horizontal, left + 4, s->calibration.center_y - 1);
        lv_obj_set_size(calibration_horizontal, diameter - 8, 2);
        lv_obj_set_pos(calibration_vertical, s->calibration.center_x - 1, top + 4);
        lv_obj_set_size(calibration_vertical, 2, diameter - 8);
        return;
    }
    const projector_page_t *page = &s->pages[s->page_index];
    if (provisioning) {
        show(header, true);
        show(main_label, false);
        fit_circle_label(header, "Wi-Fi AP", diameter, scaled(diameter, 18), 16,
                         text_fonts, ARRAY_COUNT(text_fonts));
        char ap_lines[3][33];
        snprintf(ap_lines[0], sizeof(ap_lines[0]), "%s", ap_ssid);
        snprintf(ap_lines[1], sizeof(ap_lines[1]), "%.8s", ap_password);
        snprintf(ap_lines[2], sizeof(ap_lines[2]), "%s", ap_password + 8);
        show(door_icon, false); show(alarm_icon, false);
        show(door_count, false); show(alarm_text, false);
        for (int i = 0; i < 3; ++i) {
            lv_obj_set_style_text_align(lines[i], LV_TEXT_ALIGN_CENTER, 0);
            fit_circle_label(lines[i], ap_lines[i], diameter, scaled(diameter, 45 + i * 22), 15,
                             text_fonts + 1, ARRAY_COUNT(text_fonts) - 1);
            show(lines[i], true);
        }
        return;
    }
    bool clock = !strcmp(page->layout, "clock");
    bool weather_layout = !strcmp(page->layout, "weather");
    show(main_label, clock);
    show(header, !clock);
    if (clock) {
        time_t now = time(NULL);
        char time_text[8] = "--:--";
        if (now > 1577836800) {
            struct tm local;
            localtime_r(&now, &local);
            snprintf(time_text, sizeof(time_text), "%02d:%02d", local.tm_hour, local.tm_min);
        }
        fit_circle_label(main_label, time_text, diameter, scaled(diameter, 30),
                         diameter >= 120 ? 42 : diameter >= 104 ? 32 : 18,
                         time_fonts, ARRAY_COUNT(time_fonts));
        render_status_row(s, diameter);
    } else {
        fit_circle_label(header, page->name, diameter, scaled(diameter, 18), 16,
                         text_fonts, ARRAY_COUNT(text_fonts));
        show(door_icon, false); show(alarm_icon, false);
        show(door_count, false); show(alarm_text, false);
    }
    int row = 0;
    for (int i = 0; i < PROJECTOR_MAX_WIDGETS; ++i) {
        const char *widget = page->widgets[i];
        if (!widget[0] || (clock && (!strcmp(widget, "openings") || !strcmp(widget, "alarm")))) continue;
        if (row >= 3) break;
        char value[64];
        text_widget(widget, s, page, value, sizeof(value));
        lv_obj_set_style_text_align(lines[row], clock ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
        int first_font = diameter >= 112 && weather_layout && row == 0 ? 0 : 1;
        fit_circle_label(lines[row], value, diameter, scaled(diameter,
            clock ? 89 : weather_layout ? 34 + row * 27 : 35 + row * 27), 16,
            text_fonts + first_font, ARRAY_COUNT(text_fonts) - first_font);
        show(lines[row], true);
        ++row;
    }
    for (; row < 3; ++row) show(lines[row], false);
}

static void display_task(void *argument)
{
    (void)argument;
    bool last_power = false;
    uint8_t last_brightness = 0;
    while (true) {
        projector_tick();
        projector_snapshot_t snapshot;
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
            render(&snapshot);
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
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    projection = lv_obj_create(screen);
    lv_obj_remove_style_all(projection);
    lv_obj_set_size(projection, 112, 112);
    lv_obj_set_pos(projection, 8, 8);
    lv_obj_set_style_bg_opa(projection, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(projection, lv_color_white(), 0);
    lv_obj_remove_flag(projection, LV_OBJ_FLAG_SCROLLABLE);
    header = lv_label_create(projection);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_size(header, 116, 18);
    lv_label_set_long_mode(header, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(header, 6, 5);
    main_label = lv_label_create(projection);
    lv_obj_set_style_text_font(main_label, &lv_font_montserrat_36, 0);
    lv_label_set_long_mode(main_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(main_label, LV_TEXT_ALIGN_CENTER, 0);
    door_icon = lv_obj_create(projection);
    lv_obj_set_size(door_icon, 10, 13);
    lv_obj_set_pos(door_icon, 10, 74);
    lv_obj_set_style_bg_opa(door_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(door_icon, 2, 0);
    lv_obj_set_style_radius(door_icon, 0, 0);
    alarm_icon = lv_obj_create(projection);
    lv_obj_set_size(alarm_icon, 10, 10);
    lv_obj_set_pos(alarm_icon, 64, 76);
    lv_obj_set_style_radius(alarm_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(alarm_icon, 0, 0);
    door_count = lv_label_create(projection);
    lv_obj_set_pos(door_count, 25, 73);
    alarm_text = lv_label_create(projection);
    lv_label_set_long_mode(alarm_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(alarm_text, 80, 73);
    for (int i = 0; i < 3; ++i) {
        lines[i] = lv_label_create(projection);
        lv_obj_set_size(lines[i], 120, 20);
        lv_label_set_long_mode(lines[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(lines[i], &lv_font_montserrat_12, 0);
        lv_obj_set_pos(lines[i], 5, 30 + i * 28);
    }
    calibration_ring = lv_obj_create(screen);
    lv_obj_remove_style_all(calibration_ring);
    lv_obj_set_style_radius(calibration_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(calibration_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(calibration_ring, lv_color_white(), 0);
    lv_obj_set_style_border_width(calibration_ring, 2, 0);
    lv_obj_remove_flag(calibration_ring, LV_OBJ_FLAG_SCROLLABLE);
    calibration_horizontal = lv_obj_create(screen);
    lv_obj_remove_style_all(calibration_horizontal);
    lv_obj_set_style_bg_color(calibration_horizontal, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(calibration_horizontal, LV_OPA_COVER, 0);
    calibration_vertical = lv_obj_create(screen);
    lv_obj_remove_style_all(calibration_vertical);
    lv_obj_set_style_bg_color(calibration_vertical, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(calibration_vertical, LV_OPA_COVER, 0);
    show(calibration_ring, false);
    show(calibration_horizontal, false);
    show(calibration_vertical, false);
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

#include "bed_display.h"
#include "bed_icons.h"
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
static lv_obj_t *projection;
static lv_obj_t *calibration_ring;
static lv_obj_t *calibration_horizontal;
static lv_obj_t *calibration_vertical;
static lv_obj_t *calibration_top;
static lv_obj_t *header;
static lv_obj_t *main_label;
static lv_obj_t *lines[3];
static lv_obj_t *weather_icon;
static lv_obj_t *temp_high;
static lv_obj_t *temp_low;
static lv_obj_t *door_icon;
static lv_obj_t *door_count;
static lv_obj_t *alarm_icon;
static bool provisioning;
/* Rounded borders need LV_DRAW_SW_COMPLEX, so the calibration ring is drawn pixel by pixel. */
static uint8_t ring_buffer[LV_CANVAS_BUF_SIZE(128, 128, 16, LV_DRAW_BUF_STRIDE_ALIGN)];
static int ring_diameter;
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
static const lv_font_t *const icon_fonts[] = {&bed_icons_22, &bed_icons_18, &bed_icons_14};
#define ARRAY_COUNT(items) ((int)(sizeof(items) / sizeof((items)[0])))

/* Icons match Home Assistant's alarm_control_panel state icons. */
typedef struct {
    const char *state, *icon, *label;
    uint32_t color;
} alarm_mode_t;

static const alarm_mode_t alarm_modes[] = {
    {"disarmed", BED_ICON_SHIELD_OFF, "Disinserito", 0x607080},
    {"armed_home", BED_ICON_SHIELD_HOME, "In casa", 0xffd040},
    {"armed_away", BED_ICON_SHIELD_LOCK, "Fuori casa", 0xffa000},
    {"armed_night", BED_ICON_SHIELD_MOON, "Notte", 0x8090ff},
    {"armed_vacation", BED_ICON_SHIELD_AIRPLANE, "Vacanza", 0xffa000},
    {"armed_custom_bypass", BED_ICON_SECURITY, "Personalizzato", 0xffa000},
    {"arming", BED_ICON_SHIELD, "Inserimento...", 0xffd040},
    {"pending", BED_ICON_SHIELD_OUTLINE, "In attesa", 0xff6020},
    {"triggered", BED_ICON_BELL_RING, "SCATTATO", 0xff3030},
};
static const alarm_mode_t alarm_unknown = {"unknown", BED_ICON_SHIELD, "?", 0x404850};

/* Icons match the Home Assistant frontend's weather condition icons. */
static const struct {
    const char *condition, *icon;
    uint32_t color;
} weather_icons[] = {
    {"clear-night", BED_ICON_WEATHER_NIGHT, 0x8090ff},
    {"cloudy", BED_ICON_WEATHER_CLOUDY, 0xc0c8d0},
    {"exceptional", BED_ICON_ALERT_CIRCLE_OUTLINE, 0xff6020},
    {"fog", BED_ICON_WEATHER_FOG, 0xc0c8d0},
    {"hail", BED_ICON_WEATHER_HAIL, 0xa0d0ff},
    {"lightning", BED_ICON_WEATHER_LIGHTNING, 0xffd040},
    {"lightning-rainy", BED_ICON_WEATHER_LIGHTNING_RAINY, 0xffd040},
    {"partlycloudy", BED_ICON_WEATHER_PARTLY_CLOUDY, 0xffe080},
    {"pouring", BED_ICON_WEATHER_POURING, 0x60a0ff},
    {"rainy", BED_ICON_WEATHER_RAINY, 0x80b0ff},
    {"snowy", BED_ICON_WEATHER_SNOWY, 0xffffff},
    {"snowy-rainy", BED_ICON_WEATHER_SNOWY_RAINY, 0xa0d0ff},
    {"sunny", BED_ICON_WEATHER_SUNNY, 0xffd040},
    {"windy", BED_ICON_WEATHER_WINDY, 0xc0c8d0},
    {"windy-variant", BED_ICON_WEATHER_WINDY_VARIANT, 0xc0c8d0},
};

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

/* 2-pixel ring touching the edge of a diameter x diameter box, using doubled
 * coordinates so the centre falls between pixels for even diameters. */
static void draw_calibration_ring(int diameter)
{
    if (diameter == ring_diameter) return;
    ring_diameter = diameter;
    lv_canvas_set_buffer(calibration_ring, lv_draw_buf_align(ring_buffer, LV_COLOR_FORMAT_RGB565),
                         diameter, diameter, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(calibration_ring, lv_color_black(), LV_OPA_COVER);
    int outer = diameter * diameter, inner = (diameter - 4) * (diameter - 4);
    for (int y = 0; y < diameter; ++y)
        for (int x = 0; x < diameter; ++x) {
            int dx = 2 * x + 1 - diameter, dy = 2 * y + 1 - diameter;
            int distance = dx * dx + dy * dy;
            if (distance <= outer && distance >= inner)
                lv_canvas_set_px(calibration_ring, x, y, lv_color_white(), LV_OPA_COVER);
        }
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

/* One item of a centred clock-page row: an icon glyph or a short text. */
typedef struct {
    lv_obj_t *obj;
    const char *text;
    bool icon;
    uint32_t color;
    int gap; /* space before the item */
} row_item_t;

static void hide_clock_rows(void)
{
    lv_obj_t *const objects[] = {weather_icon, temp_high, temp_low, door_icon, door_count, alarm_icon};
    for (int i = 0; i < ARRAY_COUNT(objects); ++i) show(objects[i], false);
}

/* Centre the items in the circle, shrinking icons and text together until the row fits. */
static void layout_row(const row_item_t *items, int count, int diameter, int y, int slot)
{
    int available = circle_half_width(diameter, y, slot) * 2;
    int first_icon = slot >= 22 ? 0 : slot >= 18 ? 1 : 2;
    int first_text = diameter >= 104 ? 1 : 2;
    int levels = ARRAY_COUNT(text_fonts) - first_text;
    if (ARRAY_COUNT(icon_fonts) - first_icon > levels) levels = ARRAY_COUNT(icon_fonts) - first_icon;
    const lv_font_t *icon_font = NULL, *text_font = NULL;
    int total = 0;
    for (int level = 0; level < levels; ++level) {
        int icon = first_icon + level, text = first_text + level;
        icon_font = icon_fonts[icon < ARRAY_COUNT(icon_fonts) ? icon : ARRAY_COUNT(icon_fonts) - 1];
        text_font = text_fonts[text < ARRAY_COUNT(text_fonts) ? text : ARRAY_COUNT(text_fonts) - 1];
        total = 0;
        for (int i = 0; i < count; ++i)
            total += (i ? items[i].gap : 0) + text_width(items[i].text, items[i].icon ? icon_font : text_font);
        if (total <= available) break;
    }
    int x = diameter / 2 - total / 2, middle = y + slot / 2;
    for (int i = 0; i < count; ++i) {
        const lv_font_t *font = items[i].icon ? icon_font : text_font;
        int width = text_width(items[i].text, font), height = lv_font_get_line_height(font);
        if (i) x += items[i].gap;
        lv_obj_set_style_text_font(items[i].obj, font, 0);
        lv_obj_set_style_text_color(items[i].obj, lv_color_hex(items[i].color), 0);
        lv_label_set_text(items[i].obj, items[i].text);
        lv_obj_set_pos(items[i].obj, x, middle - height / 2);
        lv_obj_set_size(items[i].obj, width, height);
        show(items[i].obj, true);
        x += width;
    }
}

static const char *weather_icon_of(const projector_snapshot_t *s, uint32_t *color)
{
    for (int i = 0; s->ha_fresh && i < ARRAY_COUNT(weather_icons); ++i)
        if (!strcmp(s->condition, weather_icons[i].condition)) {
            *color = weather_icons[i].color;
            return weather_icons[i].icon;
        }
    return NULL;
}

/* Clock page top row: today's forecast icon with maximum and minimum temperature. */
static void render_weather_row(const projector_snapshot_t *s, int diameter, int y, int slot)
{
    uint32_t color = 0;
    const char *icon = weather_icon_of(s, &color);
    show(weather_icon, false); show(temp_high, false); show(temp_low, false);
    if (!icon) return;
    char high[12], low[12];
    snprintf(high, sizeof(high), "%d\xC2\xB0", s->temp_high);
    snprintf(low, sizeof(low), "%d\xC2\xB0", s->temp_low);
    row_item_t items[3] = {{weather_icon, icon, true, color, 0}};
    int count = 1;
    if (s->temp_high_known) items[count++] = (row_item_t){temp_high, high, false, 0xffb070, 3};
    if (s->temp_low_known) items[count++] = (row_item_t){temp_low, low, false, 0x80b0ff, 4};
    layout_row(items, count, diameter, y, slot);
}

/* An unusable count must never look like "all closed", so it shows as a grey "?". */
static bool doors_visible(const projector_snapshot_t *s)
{
    return !(s->ha_fresh && s->openings_known) || s->openings;
}

/* A disarmed alarm is the normal state, so its icon is left out. */
static bool alarm_visible(const projector_snapshot_t *s)
{
    return strcmp(alarm_mode(s)->state, "disarmed");
}

/* Clock page row under the time: open doors (only when any is open) and the alarm
 * icon (only when not disarmed). */
static void render_status_row(const projector_snapshot_t *s, int diameter, int y, int slot)
{
    bool known = s->ha_fresh && s->openings_known;
    char count[4] = "?";
    if (known) snprintf(count, sizeof(count), "%u", s->openings);
    const alarm_mode_t *alarm = alarm_mode(s);
    row_item_t items[3];
    int n = 0;
    show(door_icon, false); show(door_count, false); show(alarm_icon, false);
    if (doors_visible(s)) {
        uint32_t color = known ? 0xffa000 : 0x607080;
        items[n++] = (row_item_t){door_icon, BED_ICON_DOOR_OPEN, true, color, 0};
        items[n++] = (row_item_t){door_count, count, false, color, 2};
    }
    if (alarm_visible(s)) {
        int gap = n ? 10 : 0;
        items[n++] = (row_item_t){alarm_icon, alarm->icon, true, alarm->color, gap};
    }
    if (n) layout_row(items, n, diameter, y, slot);
}

static void render(const projector_snapshot_t *s)
{
    /* The panel is rotated/mirrored in hardware, so the saved center is mapped
     * to keep the circle on the same physical pixels in every orientation. */
    int diameter = s->calibration.diameter;
    int center_x, center_y;
    projector_orientation_center(s->calibration.rotation, s->calibration.mirror,
                                 s->calibration.center_x, s->calibration.center_y,
                                 diameter, &center_x, &center_y);
    int left = center_x - diameter / 2;
    int top = center_y - diameter / 2;
    bool calibrating = s->calibration.active && !provisioning;
    lv_obj_set_pos(projection, left, top);
    lv_obj_set_size(projection, diameter, diameter);
    show(projection, !calibrating);
    show(calibration_ring, calibrating);
    show(calibration_horizontal, calibrating);
    show(calibration_vertical, calibrating);
    show(calibration_top, calibrating);
    if (calibrating) {
        lv_obj_set_pos(calibration_ring, left, top);
        draw_calibration_ring(diameter);
        lv_obj_set_pos(calibration_horizontal, left + 4, center_y - 1);
        lv_obj_set_size(calibration_horizontal, diameter - 8, 2);
        lv_obj_set_pos(calibration_vertical, center_x - 1, top + 4);
        lv_obj_set_size(calibration_vertical, 2, diameter - 8);
        /* Text shows both the rotation and the mirroring of the content. */
        lv_obj_set_pos(calibration_top, center_x + 6, top + diameter / 4 - 7);
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
        hide_clock_rows();
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
    const char *widgets[3];
    int count = 0;
    for (int i = 0; i < PROJECTOR_MAX_WIDGETS && count < 3; ++i) {
        const char *widget = page->widgets[i];
        if (!widget[0] || (clock && (!strcmp(widget, "openings") || !strcmp(widget, "alarm")))) continue;
        widgets[count++] = widget;
    }
    int line_y[3];
    if (clock) {
        /* Stack the rows top to bottom and centre the stack vertically, so the page
         * stays balanced whether or not the weather and status rows are shown. */
        uint32_t unused;
        bool weather = weather_icon_of(s, &unused) != NULL;
        bool status = doors_visible(s) || alarm_visible(s);
        int weather_slot = scaled(diameter, 20);
        int time_slot = diameter >= 120 ? 42 : diameter >= 104 ? 32 : 18;
        int status_slot = diameter >= 112 ? 18 : 14;
        /* The time font has empty space under the digits, so the next row may overlap it. */
        int time_trim = time_slot / 8;
        int below = status + count;
        int total = (weather ? weather_slot + 2 : 0) + time_slot + (status ? status_slot : 0) +
                    count * 16 + (below ? 2 * (below - 1) - time_trim : 0);
        int y = (diameter - total) / 2;
        if (y < 0) y = 0;
        render_weather_row(s, diameter, y, weather_slot);
        if (weather) y += weather_slot + 2;
        time_t now = time(NULL);
        char time_text[8] = "--:--";
        if (now > 1577836800) {
            struct tm local;
            localtime_r(&now, &local);
            snprintf(time_text, sizeof(time_text), "%02d:%02d", local.tm_hour, local.tm_min);
        }
        fit_circle_label(main_label, time_text, diameter, y, time_slot,
                         time_fonts, ARRAY_COUNT(time_fonts));
        y += time_slot - time_trim;
        render_status_row(s, diameter, y, status_slot);
        if (status) y += status_slot + 2;
        for (int row = 0; row < count; ++row, y += 18) line_y[row] = y;
    } else {
        fit_circle_label(header, page->name, diameter, scaled(diameter, 18), 16,
                         text_fonts, ARRAY_COUNT(text_fonts));
        hide_clock_rows();
        for (int row = 0; row < count; ++row)
            line_y[row] = scaled(diameter, weather_layout ? 34 + row * 27 : 35 + row * 27);
    }
    int row = 0;
    for (; row < count; ++row) {
        char value[64];
        text_widget(widgets[row], s, page, value, sizeof(value));
        lv_obj_set_style_text_align(lines[row], clock ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
        int first_font = diameter >= 112 && weather_layout && row == 0 ? 0 : 1;
        fit_circle_label(lines[row], value, diameter, line_y[row], 16,
            text_fonts + first_font, ARRAY_COUNT(text_fonts) - first_font);
        show(lines[row], true);
    }
    for (; row < 3; ++row) show(lines[row], false);
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
            int orientation = snapshot.calibration.rotation | (snapshot.calibration.mirror ? 4 : 0);
            if (orientation != last_orientation) {
                apply_orientation(snapshot.calibration.rotation, snapshot.calibration.mirror);
                last_orientation = orientation;
            }
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
    lv_obj_t **row_labels[] = {&weather_icon, &temp_high, &temp_low, &door_icon, &door_count, &alarm_icon};
    for (int i = 0; i < ARRAY_COUNT(row_labels); ++i) {
        *row_labels[i] = lv_label_create(projection);
        lv_label_set_long_mode(*row_labels[i], LV_LABEL_LONG_MODE_CLIP);
        show(*row_labels[i], false);
    }
    for (int i = 0; i < 3; ++i) {
        lines[i] = lv_label_create(projection);
        lv_obj_set_size(lines[i], 120, 20);
        lv_label_set_long_mode(lines[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(lines[i], &lv_font_montserrat_12, 0);
        lv_obj_set_pos(lines[i], 5, 30 + i * 28);
    }
    calibration_ring = lv_canvas_create(screen);
    calibration_horizontal = lv_obj_create(screen);
    lv_obj_remove_style_all(calibration_horizontal);
    lv_obj_set_style_bg_color(calibration_horizontal, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(calibration_horizontal, LV_OPA_COVER, 0);
    calibration_vertical = lv_obj_create(screen);
    lv_obj_remove_style_all(calibration_vertical);
    lv_obj_set_style_bg_color(calibration_vertical, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(calibration_vertical, LV_OPA_COVER, 0);
    calibration_top = lv_label_create(screen);
    lv_obj_set_style_text_font(calibration_top, &lv_font_montserrat_12, 0);
    lv_label_set_text(calibration_top, "SU");
    show(calibration_ring, false);
    show(calibration_horizontal, false);
    show(calibration_vertical, false);
    show(calibration_top, false);
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

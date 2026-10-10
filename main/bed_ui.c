#include "bed_ui.h"
#include "bed_fonts.h"
#include "bed_icons.h"
#include "projector_logic.h"
#include <stdio.h>
#include <string.h>

#define ARRAY_COUNT(items) ((int)(sizeof(items) / sizeof((items)[0])))
/* Clocks before 2020 have not been set by NTP yet. */
#define VALID_TIME 1577836800
/* Marquee speed in pixels per second: slow enough to read lying in bed. */
#define MARQUEE_SPEED 16
#define RECOLOR(color, text) "#" color " " text "#"

/* Palette for a projection on a dark ceiling: dim colours fade first, so even
 * secondary text stays above ~40% brightness. */
#define COLOR_TEXT 0xf2f4f8
#define COLOR_MUTED 0xa8b2c0
#define COLOR_DIM 0x6a7482
#define COLOR_LINE 0x3a4250
#define COLOR_WARM 0xffb36b
#define COLOR_COOL 0x7db8ff
#define COLOR_OPEN 0xffa040
#define COLOR_LIGHT 0xffd54a
#define COLOR_OK 0x6fd08c
#define COLOR_RAIN 0x6fb0ff

static const lv_font_t *const clock_fonts[] = {&bed_font_clock_46, &bed_font_clock_40,
    &bed_font_clock_34, &bed_font_clock_28};
static const lv_font_t *const bold_fonts[] = {&bed_font_bold_26, &bed_font_bold_20,
    &bed_font_bold_16, &bed_font_bold_14};
static const lv_font_t *const text_fonts[] = {&bed_font_text_14, &bed_font_text_12,
    &bed_font_text_10};
static const lv_font_t *const icon_fonts[] = {&bed_icons_30, &bed_icons_22, &bed_icons_18,
    &bed_icons_14, &bed_icons_12};

static const char *const weekdays[] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};
static const char *const months[] = {"Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio", "Giugno",
    "Luglio", "Agosto", "Settembre", "Ottobre", "Novembre", "Dicembre"};

/* Icons match Home Assistant's alarm_control_panel state icons. */
typedef struct {
    const char *state, *icon, *label;
    uint32_t color;
} alarm_mode_t;

static const alarm_mode_t alarm_modes[] = {
    {"disarmed", BED_ICON_SHIELD_OFF, "Disinserito", 0x8a96a6},
    {"armed_home", BED_ICON_SHIELD_HOME, "In casa", 0xffd040},
    {"armed_away", BED_ICON_SHIELD_LOCK, "Fuori casa", 0xffa000},
    {"armed_night", BED_ICON_SHIELD_MOON, "Notte", 0x8c9cff},
    {"armed_vacation", BED_ICON_SHIELD_AIRPLANE, "Vacanza", 0xffa000},
    {"armed_custom_bypass", BED_ICON_SECURITY, "Personalizzato", 0xffa000},
    {"arming", BED_ICON_SHIELD, "Inserimento\xE2\x80\xA6", 0xffd040},
    {"pending", BED_ICON_SHIELD_OUTLINE, "In attesa", 0xff6020},
    {"triggered", BED_ICON_BELL_RING, "SCATTATO", 0xff3030},
};
static const alarm_mode_t alarm_unknown = {"unknown", BED_ICON_SHIELD, "Allarme ?", 0x5a6472};

/* Icons match the Home Assistant frontend's weather condition icons. */
static const struct {
    const char *condition, *icon, *label;
    uint32_t color;
} weather_icons[] = {
    {"clear-night", BED_ICON_WEATHER_NIGHT, "Sereno", 0x9aa8ff},
    {"cloudy", BED_ICON_WEATHER_CLOUDY, "Nuvoloso", 0xc8d0da},
    {"exceptional", BED_ICON_ALERT_CIRCLE_OUTLINE, "Eccezionale", 0xff6020},
    {"fog", BED_ICON_WEATHER_FOG, "Nebbia", 0xc8d0da},
    {"hail", BED_ICON_WEATHER_HAIL, "Grandine", 0xa0d0ff},
    {"lightning", BED_ICON_WEATHER_LIGHTNING, "Temporale", 0xffd040},
    {"lightning-rainy", BED_ICON_WEATHER_LIGHTNING_RAINY, "Temporale", 0xffd040},
    {"partlycloudy", BED_ICON_WEATHER_PARTLY_CLOUDY, "Poco nuvoloso", 0xffe080},
    {"pouring", BED_ICON_WEATHER_POURING, "Rovesci", 0x60a0ff},
    {"rainy", BED_ICON_WEATHER_RAINY, "Pioggia", 0x80b0ff},
    {"snowy", BED_ICON_WEATHER_SNOWY, "Neve", 0xffffff},
    {"snowy-rainy", BED_ICON_WEATHER_SNOWY_RAINY, "Nevischio", 0xa0d0ff},
    {"sunny", BED_ICON_WEATHER_SUNNY, "Sereno", 0xffd040},
    {"windy", BED_ICON_WEATHER_WINDY, "Ventoso", 0xc8d0da},
    {"windy-variant", BED_ICON_WEATHER_WINDY_VARIANT, "Ventoso", 0xc8d0da},
};

typedef struct {
    const char *icon, *label;
    uint32_t color;
} weather_look_t;

static lv_obj_t *projection;
static lv_obj_t *calibration_ring, *calibration_horizontal, *calibration_vertical, *calibration_top;
/* Rounded borders need LV_DRAW_SW_COMPLEX, so the calibration ring is drawn pixel by pixel. */
static uint8_t ring_buffer[LV_CANVAS_BUF_SIZE(128, 128, 16, LV_DRAW_BUF_STRIDE_ALIGN)];
static int ring_diameter;

static struct {
    lv_obj_t *page, *weather_icon, *high, *low, *time, *door_icon, *door_count, *light_icon,
        *light_count, *alarm_icon, *date;
} clock_ui;

static struct {
    lv_obj_t *page, *alarm_icon, *alarm, *indoor_icon, *indoor, *outdoor_icon, *outdoor, *divider,
        *door_icon, *doors, *light_icon, *lights;
} home_ui;

static struct {
    lv_obj_t *page, *icon, *temperature, *summary, *rule_left, *rule_right, *title, *day_label,
        *day_icon, *day_temp, *evening_label, *evening_icon, *evening_temp, *outlook;
} weather_ui;

static struct {
    lv_obj_t *page, *header, *lines[3];
} list_ui;

/* ---- Label helpers. Setters skip unchanged values: any text or style change
 * re-lays a label out and would restart its marquee from the beginning. ---- */

static void show(lv_obj_t *obj, bool visible)
{
    bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (visible && hidden) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else if (!visible && !hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_text(lv_obj_t *obj, const char *text)
{
    if (strcmp(lv_label_get_text(obj), text)) lv_label_set_text(obj, text);
}

static void set_font(lv_obj_t *obj, const lv_font_t *font)
{
    if (lv_obj_get_style_text_font(obj, 0) != font) lv_obj_set_style_text_font(obj, font, 0);
}

static void set_color(lv_obj_t *obj, uint32_t color)
{
    lv_color_t value = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, 0), value))
        lv_obj_set_style_text_color(obj, value, 0);
}

static void set_area(lv_obj_t *obj, int x, int y, int width, int height)
{
    if (lv_obj_get_style_x(obj, 0) != x || lv_obj_get_style_y(obj, 0) != y) lv_obj_set_pos(obj, x, y);
    if (lv_obj_get_style_width(obj, 0) != width || lv_obj_get_style_height(obj, 0) != height)
        lv_obj_set_size(obj, width, height);
}

static void set_label(lv_obj_t *obj, const char *text, const lv_font_t *font, uint32_t color)
{
    set_font(obj, font);
    set_color(obj, color);
    set_text(obj, text);
    show(obj, true);
}

static int text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_RECOLOR);
    return size.x;
}

/* Vertical extent of the visible strokes, as offsets from the top of a label:
 * digits for text fonts, the 20/24 drawing area of Material Design Icons. */
typedef struct {
    int top, bottom;
} ink_t;

static ink_t ink_of(const lv_font_t *font)
{
    int line = lv_font_get_line_height(font), baseline = line - font->base_line;
    lv_font_glyph_dsc_t glyph;
    if (lv_font_get_glyph_dsc(font, &glyph, '0', 0))
        return (ink_t){baseline - glyph.ofs_y - glyph.box_h, baseline - glyph.ofs_y};
    return (ink_t){line / 12, line - line / 12};
}

static int ink_height(const lv_font_t *font)
{
    ink_t ink = ink_of(font);
    return ink.bottom - ink.top;
}

/* Place a single-line label so the middle of its ink sits on row y + height/2. */
static void place_on_row(lv_obj_t *obj, const lv_font_t *font, int x, int width, int y, int height)
{
    ink_t ink = ink_of(font);
    int top = y + height / 2 - (ink.top + ink.bottom) / 2;
    set_area(obj, x, top, width, lv_font_get_line_height(font));
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

/* Marquee: scrolls in a loop only when the text is wider than the label. The
 * duration is set explicitly because LVGL's default speed caps a loop at 10 s. */
static void set_marquee(lv_obj_t *obj, const char *text, const lv_font_t *font, uint32_t color)
{
    set_label(obj, text, font, color);
    uint32_t duration = (uint32_t)(text_width(text, font) + 3 * text_width(" ", font)) * 1000 /
                        MARQUEE_SPEED;
    if (lv_obj_get_style_anim_duration(obj, 0) != duration)
        lv_obj_set_style_anim_duration(obj, duration, 0);
}

/* Centre a marquee in the circle; it scrolls only when the row is too narrow. The
 * box gets 2 spare pixels so a text that just fits is never clipped or scrolled. */
/* The next smaller text font when it lets text fit in width; short labels shrink
 * rather than scroll. */
static const lv_font_t *fitting_font(const char *text, const lv_font_t *font, int width)
{
    if (text_width(text, font) + 2 <= width) return font;
    for (int i = 0; i < ARRAY_COUNT(text_fonts) - 1; ++i)
        if (text_fonts[i] == font && text_width(text, text_fonts[i + 1]) + 2 <= width)
            return text_fonts[i + 1];
    return font;
}

static void place_marquee(lv_obj_t *obj, const char *text, const lv_font_t *font, uint32_t color,
                          int diameter, int y, int height)
{
    int available = circle_half_width(diameter, y, height) * 2;
    font = fitting_font(text, font, available);
    int width = text_width(text, font) + 2;
    if (width > available) width = available;
    set_marquee(obj, text, font, color);
    place_on_row(obj, font, diameter / 2 - width / 2, width, y, height);
}

static lv_obj_t *new_label(lv_obj_t *parent, lv_label_long_mode_t mode)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_long_mode(label, mode);
    lv_label_set_recolor(label, true);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, &bed_font_text_12, 0);
    show(label, false);
    return label;
}

static lv_obj_t *new_line(lv_obj_t *parent)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_LINE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    show(line, false);
    return line;
}

static lv_obj_t *new_page(void)
{
    lv_obj_t *page = lv_obj_create(projection);
    lv_obj_remove_style_all(page);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(page, lv_color_hex(COLOR_TEXT), 0);
    return page;
}

/* ---- Data helpers ---- */

static const alarm_mode_t *alarm_mode(const projector_snapshot_t *s)
{
    if (s->ha_fresh)
        for (int i = 0; i < ARRAY_COUNT(alarm_modes); ++i)
            if (!strcmp(s->alarm, alarm_modes[i].state)) return &alarm_modes[i];
    return &alarm_unknown;
}

/* Evening periods show the night variants HA's frontend uses after sunset. */
static bool weather_look(const char *condition, bool night, weather_look_t *out)
{
    for (int i = 0; condition[0] && i < ARRAY_COUNT(weather_icons); ++i)
        if (!strcmp(condition, weather_icons[i].condition)) {
            *out = (weather_look_t){weather_icons[i].icon, weather_icons[i].label, weather_icons[i].color};
            if (night && !strcmp(condition, "sunny"))
                *out = (weather_look_t){BED_ICON_WEATHER_NIGHT, "Sereno", 0x9aa8ff};
            else if (night && !strcmp(condition, "partlycloudy"))
                *out = (weather_look_t){BED_ICON_WEATHER_NIGHT_PARTLY_CLOUDY, "Poco nuvoloso", 0xb8c4e8};
            return true;
        }
    return false;
}

static const projector_day_t *forecast_day(const projector_snapshot_t *s, const struct tm *local,
                                           bool valid_time, int days_after)
{
    if (!s->ha_fresh) return NULL;
    if (!valid_time) return days_after < PROJECTOR_FORECAST_DAYS && s->days[days_after].date[0] ?
        &s->days[days_after] : NULL;
    char date[11];
    projector_date_after(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday, days_after, date);
    for (int i = 0; i < PROJECTOR_FORECAST_DAYS; ++i)
        if (!strcmp(s->days[i].date, date)) return &s->days[i];
    return NULL;
}

static void degrees(char *out, size_t size, bool known, int value)
{
    if (known) snprintf(out, size, "%d\xC2\xB0", value);
    else snprintf(out, size, "--\xC2\xB0");
}

/* "Finestra Sala · Finestra Cucina +2", or a plain count when HA sent no names. */
static void join_names(char *out, size_t size, char names[][32], int count, int total,
                       const char *noun)
{
    out[0] = 0;
    if (!count) {
        snprintf(out, size, "%d %s", total, noun);
        return;
    }
    size_t used = 0;
    for (int i = 0; i < count && used < size; ++i)
        used += snprintf(out + used, size - used, "%s%s", i ? "  \xC2\xB7  " : "", names[i]);
    if (total > count && used < size) snprintf(out + used, size - used, "  +%d", total - count);
}

/* ---- Rows ---- */

typedef struct {
    lv_obj_t *obj;
    const char *text;
    const lv_font_t *const *fonts;
    int font_count;
    int level; /* Index of the preferred font. */
    uint32_t color;
    int gap;   /* Space before the item. */
} row_item_t;

static const lv_font_t *item_font(const row_item_t *item, int step)
{
    int index = item->level + step;
    return item->fonts[index < item->font_count ? index : item->font_count - 1];
}

/* Height of the tallest ink in a row at its preferred fonts. */
static int row_height(const row_item_t *items, int count)
{
    int height = 0;
    for (int i = 0; i < count; ++i) {
        int ink = ink_height(item_font(&items[i], 0));
        if (ink > height) height = ink;
    }
    return height;
}

/* Centre the items on one line, stepping all fonts down together until it fits the circle. */
static void layout_row(const row_item_t *items, int count, int diameter, int y, int height)
{
    int available = circle_half_width(diameter, y, height) * 2, total = 0, step = 0;
    for (;; ++step) {
        total = 0;
        bool smallest = true;
        for (int i = 0; i < count; ++i) {
            total += (i ? items[i].gap : 0) + text_width(items[i].text, item_font(&items[i], step));
            if (items[i].level + step < items[i].font_count - 1) smallest = false;
        }
        if (total <= available || smallest) break;
    }
    int x = diameter / 2 - total / 2;
    for (int i = 0; i < count; ++i) {
        const lv_font_t *font = item_font(&items[i], step);
        int width = text_width(items[i].text, font);
        if (i) x += items[i].gap;
        set_label(items[i].obj, items[i].text, font, items[i].color);
        place_on_row(items[i].obj, font, x, width, y, height);
        x += width;
    }
}

/* A fixed icon followed by text that scrolls when the circle is too narrow for it. */
static void layout_icon_text(lv_obj_t *icon_obj, const char *icon, uint32_t icon_color,
                             const lv_font_t *icon_font, lv_obj_t *text_obj, const char *text,
                             uint32_t color, const lv_font_t *font, int diameter, int y, int height)
{
    int available = circle_half_width(diameter, y, height) * 2;
    int icon_width = text_width(icon, icon_font), gap = 4;
    font = fitting_font(text, font, available - icon_width - gap);
    int width = text_width(text, font) + 2;
    int total = icon_width + gap + width;
    int left = total <= available ? diameter / 2 - total / 2 : diameter / 2 - available / 2;
    int text_box = total <= available ? width : available - icon_width - gap;
    set_label(icon_obj, icon, icon_font, icon_color);
    place_on_row(icon_obj, icon_font, left, icon_width, y, height);
    set_marquee(text_obj, text, font, color);
    place_on_row(text_obj, font, left + icon_width + gap, text_box, y, height);
}

/* Vertical stack of rows centred in the circle with even gaps between inks. */
static int stack_top(int diameter, const int *heights, int count, int gap)
{
    int total = 0, rows = 0;
    for (int i = 0; i < count; ++i)
        if (heights[i]) {
            total += heights[i];
            ++rows;
        }
    total += rows > 1 ? gap * (rows - 1) : 0;
    int top = (diameter - total) / 2;
    return top < 0 ? 0 : top;
}

/* 0 for the largest circles, 3 for the smallest: picks the preferred font sizes. */
static int size_level(int diameter)
{
    return diameter >= 122 ? 0 : diameter >= 106 ? 1 : diameter >= 94 ? 2 : 3;
}

/* ---- Clock page: forecast, time, conditional house status, scrolling date. ---- */

static void render_clock(const projector_snapshot_t *s, const struct tm *local, bool valid_time,
                         int diameter)
{
    int level = size_level(diameter);
    /* Until 21:00 today's forecast, then tomorrow's. */
    bool tomorrow = valid_time && projector_shows_tomorrow(local->tm_hour);
    const projector_day_t *day = forecast_day(s, local, valid_time, tomorrow ? 1 : 0);
    weather_look_t look;
    bool high_known = false, low_known = false, weather = false;
    int high = 0, low = 0;
    if (day && weather_look(day->condition, false, &look)) {
        weather = true;
        high_known = day->high_known, high = day->high;
        low_known = day->low_known, low = day->low;
    } else if (!tomorrow && s->ha_fresh && weather_look(s->condition, false, &look)) {
        /* Integrations older than the per-day forecast send only today's range. */
        weather = true;
        high_known = s->temp_high_known, high = s->temp_high;
        low_known = s->temp_low_known, low = s->temp_low;
    }
    char high_text[12], low_text[12];
    degrees(high_text, sizeof(high_text), high_known, high);
    degrees(low_text, sizeof(low_text), low_known, low);
    row_item_t weather_row[3] = {
        {clock_ui.weather_icon, weather ? look.icon : "", icon_fonts, ARRAY_COUNT(icon_fonts),
         level + 1, weather ? look.color : 0, 0},
        {clock_ui.high, high_text, bold_fonts, ARRAY_COUNT(bold_fonts), level + 1, COLOR_WARM, 5},
        {clock_ui.low, low_text, bold_fonts, ARRAY_COUNT(bold_fonts), level + 1, COLOR_COOL, 6},
    };

    char time_text[8] = "--:--";
    if (valid_time) snprintf(time_text, sizeof(time_text), "%02d:%02d", local->tm_hour, local->tm_min);
    row_item_t time_row[1] = {{clock_ui.time, time_text, clock_fonts, ARRAY_COUNT(clock_fonts), level,
                               COLOR_TEXT, 0}};

    /* Status icons appear only when something needs attention. An unusable door
     * count must never look like "all closed", so it shows as a grey "?". */
    bool doors_known = s->ha_fresh && s->openings_known;
    bool doors = !doors_known || s->openings;
    bool lights = s->ha_fresh && s->lights_known && s->lights;
    const alarm_mode_t *alarm = alarm_mode(s);
    bool alarm_visible = strcmp(alarm->state, "disarmed");
    char door_count[4] = "?", light_count[4];
    if (doors_known) snprintf(door_count, sizeof(door_count), "%u", s->openings);
    snprintf(light_count, sizeof(light_count), "%u", s->lights);
    row_item_t status_row[5];
    int status = 0;
    int status_level = level + 2;
    if (doors) {
        uint32_t color = doors_known ? COLOR_OPEN : COLOR_DIM;
        status_row[status++] = (row_item_t){clock_ui.door_icon, BED_ICON_DOOR_OPEN, icon_fonts,
            ARRAY_COUNT(icon_fonts), status_level, color, 0};
        status_row[status++] = (row_item_t){clock_ui.door_count, door_count, bold_fonts,
            ARRAY_COUNT(bold_fonts), status_level + 1, color, 1};
    }
    if (lights) {
        status_row[status++] = (row_item_t){clock_ui.light_icon, BED_ICON_LIGHTBULB_ON, icon_fonts,
            ARRAY_COUNT(icon_fonts), status_level, COLOR_LIGHT, status ? 10 : 0};
        status_row[status++] = (row_item_t){clock_ui.light_count, light_count, bold_fonts,
            ARRAY_COUNT(bold_fonts), status_level + 1, COLOR_LIGHT, 1};
    }
    if (alarm_visible)
        status_row[status++] = (row_item_t){clock_ui.alarm_icon, alarm->icon, icon_fonts,
            ARRAY_COUNT(icon_fonts), status_level, alarm->color, status ? 10 : 0};

    char date[96] = "In attesa dell'ora";
    const projector_day_t *today = forecast_day(s, local, valid_time, 0);
    if (valid_time) {
        int used = snprintf(date, sizeof(date), "%s %d %s", weekdays[local->tm_wday], local->tm_mday,
                            months[local->tm_mon]);
        if (today && today->sunrise[0] && today->sunset[0])
            snprintf(date + used, sizeof(date) - used,
                     "   " RECOLOR("ffc860", BED_ICON_SUNRISE) " " RECOLOR("a8b2c0", "%s")
                     "   " RECOLOR("ff8c60", BED_ICON_SUNSET) " " RECOLOR("a8b2c0", "%s"),
                     today->sunrise, today->sunset);
    }
    const lv_font_t *date_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;
    /* The date row ink includes the inline icons, which reach above the digits. */
    int date_height = lv_font_get_line_height(date_font) - 4;

    int heights[4] = {weather ? row_height(weather_row, 3) : 0, row_height(time_row, 1),
                      status ? row_height(status_row, status) : 0, date_height};
    int gap = level >= 2 ? 5 : 7;
    int y = stack_top(diameter, heights, 4, gap);
    show(clock_ui.weather_icon, weather);
    show(clock_ui.high, weather);
    show(clock_ui.low, weather);
    if (weather) {
        layout_row(weather_row, 3, diameter, y, heights[0]);
        y += heights[0] + gap;
    }
    layout_row(time_row, 1, diameter, y, heights[1]);
    y += heights[1] + gap;
    lv_obj_t *const status_objects[] = {clock_ui.door_icon, clock_ui.door_count, clock_ui.light_icon,
        clock_ui.light_count, clock_ui.alarm_icon};
    for (int i = 0; i < ARRAY_COUNT(status_objects); ++i) show(status_objects[i], false);
    if (status) {
        layout_row(status_row, status, diameter, y, heights[2]);
        y += heights[2] + gap;
    }
    place_marquee(clock_ui.date, date, date_font, COLOR_TEXT, diameter, y, date_height);
}

/* ---- Home page: alarm, bedroom and outdoor temperature, open doors, lights. ---- */

static void render_home(const projector_snapshot_t *s, int diameter)
{
    int level = size_level(diameter);
    const lv_font_t *text_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;
    const lv_font_t *row_icon = level >= 2 ? &bed_icons_12 : &bed_icons_14;
    const lv_font_t *temp_font = level == 0 ? &bed_font_bold_16 : &bed_font_bold_14;
    const lv_font_t *temp_icon = level == 0 ? &bed_icons_14 : &bed_icons_12;

    /* Rows hug the centre, where the circle is widest: alarm, temperatures,
     * open doors and windows, lights. */
    int row = ink_height(row_icon) > ink_height(text_font) ? ink_height(row_icon) : ink_height(text_font);
    int temp_height = ink_height(temp_icon) > ink_height(temp_font) ?
        ink_height(temp_icon) : ink_height(temp_font);
    int gap = level >= 2 ? 5 : 7;
    int heights[4] = {row, temp_height, row, row};
    int y = stack_top(diameter, heights, 4, gap);

    const alarm_mode_t *alarm = alarm_mode(s);
    layout_icon_text(home_ui.alarm_icon, alarm->icon, alarm->color, row_icon, home_ui.alarm,
                     alarm->label, alarm->color, text_font, diameter, y, row);
    y += row + gap;

    /* Bedroom left and outdoors right of a hairline, each an icon and a value. */
    const char *indoor = s->ha_fresh && s->indoor[0] ? s->indoor : "--\xC2\xB0";
    const char *outdoor = s->ha_fresh && s->outdoor[0] ? s->outdoor : "--\xC2\xB0";
    struct {
        lv_obj_t *icon, *value;
        const char *glyph, *text;
        uint32_t color;
    } columns[2] = {
        {home_ui.indoor_icon, home_ui.indoor, BED_ICON_BED, indoor, 0xc8a8ff},
        {home_ui.outdoor_icon, home_ui.outdoor, BED_ICON_PINE_TREE, outdoor, COLOR_OK},
    };
    int half_gap = level >= 2 ? 4 : 5;
    /* Small circles lose the icons rather than clip a value. */
    bool icons = true;
    for (int i = 0; i < 2; ++i)
        if (half_gap + text_width(columns[i].glyph, temp_icon) + 2 +
            text_width(columns[i].text, temp_font) > circle_half_width(diameter, y, temp_height) + 2)
            icons = false; /* + 2: the half width already keeps 2 pixels off the edge. */
    for (int i = 0; i < 2; ++i) {
        show(columns[i].icon, icons);
        int icon_width = icons ? text_width(columns[i].glyph, temp_icon) : -2;
        int value_width = text_width(columns[i].text, temp_font);
        int width = icon_width + 2 + value_width;
        int left = i == 0 ? diameter / 2 - half_gap - width : diameter / 2 + half_gap + 1;
        if (icons) {
            set_label(columns[i].icon, columns[i].glyph, temp_icon, columns[i].color);
            place_on_row(columns[i].icon, temp_icon, left, icon_width, y, temp_height);
        }
        set_label(columns[i].value, columns[i].text, temp_font, icons ? COLOR_TEXT : columns[i].color);
        place_on_row(columns[i].value, temp_font, left + icon_width + 2, value_width, y, temp_height);
    }
    set_area(home_ui.divider, diameter / 2, y - 2, 1, temp_height + 4);
    show(home_ui.divider, true);
    y += temp_height + gap;

    char text[300];
    if (!(s->ha_fresh && s->openings_known))
        layout_icon_text(home_ui.door_icon, BED_ICON_DOOR_OPEN, COLOR_DIM, row_icon, home_ui.doors,
                         "Aperture ?", COLOR_DIM, text_font, diameter, y, row);
    else if (!s->openings)
        layout_icon_text(home_ui.door_icon, BED_ICON_DOOR_CLOSED, COLOR_OK, row_icon, home_ui.doors,
                         "Tutto chiuso", COLOR_MUTED, text_font, diameter, y, row);
    else {
        char names[PROJECTOR_MAX_NAMES][32];
        memcpy(names, s->opening_names, sizeof(names));
        join_names(text, sizeof(text), names, s->opening_name_count, s->openings,
                   s->openings == 1 ? "aperta" : "aperte");
        layout_icon_text(home_ui.door_icon, BED_ICON_DOOR_OPEN, COLOR_OPEN, row_icon, home_ui.doors,
                         text, COLOR_TEXT, text_font, diameter, y, row);
    }
    y += row + gap;

    if (!(s->ha_fresh && s->lights_known))
        layout_icon_text(home_ui.light_icon, BED_ICON_LIGHTBULB_OUTLINE, COLOR_DIM, row_icon,
                         home_ui.lights, "Luci ?", COLOR_DIM, text_font, diameter, y, row);
    else if (!s->lights)
        layout_icon_text(home_ui.light_icon, BED_ICON_LIGHTBULB_OUTLINE, COLOR_MUTED, row_icon,
                         home_ui.lights, "Luci spente", COLOR_MUTED, text_font, diameter, y, row);
    else {
        char names[PROJECTOR_MAX_NAMES][32];
        memcpy(names, s->light_names, sizeof(names));
        join_names(text, sizeof(text), names, s->light_name_count, s->lights,
                   s->lights == 1 ? "accesa" : "accese");
        layout_icon_text(home_ui.light_icon, BED_ICON_LIGHTBULB_ON, COLOR_LIGHT, row_icon,
                         home_ui.lights, text, COLOR_TEXT, text_font, diameter, y, row);
    }
}

/* ---- Weather page: now, then the day ahead split into daytime and evening. ---- */

typedef struct {
    lv_obj_t *label, *icon, *temp;
} period_ui_t;

/* One column of the day ahead: name above icon and temperature. */
static void render_period(const period_ui_t *ui, const projector_period_t *period, bool night,
                          const char *name, int center, int y, int height,
                          const lv_font_t *icon_font, const lv_font_t *temp_font)
{
    const lv_font_t *small = &bed_font_text_10;
    weather_look_t look;
    bool known = period && weather_look(period->condition, night, &look);
    char temp[12];
    degrees(temp, sizeof(temp), known && period->temperature_known, known ? period->temperature : 0);
    int label_width = text_width(name, small);
    set_label(ui->label, name, small, COLOR_MUTED);
    place_on_row(ui->label, small, center - label_width / 2, label_width + 1, y, ink_height(small));
    y += ink_height(small) + 4;
    const char *icon = known ? look.icon : BED_ICON_HELP_CIRCLE_OUTLINE;
    int icon_width = text_width(icon, icon_font);
    int temp_width = text_width(temp, temp_font), gap = 2;
    int left = center - (icon_width + gap + temp_width) / 2;
    set_label(ui->icon, icon, icon_font, known ? look.color : COLOR_DIM);
    place_on_row(ui->icon, icon_font, left, icon_width, y, height);
    set_label(ui->temp, temp, temp_font, known ? COLOR_TEXT : COLOR_DIM);
    place_on_row(ui->temp, temp_font, left + icon_width + gap, temp_width, y, height);
}

static bool rainy(const projector_period_t *period)
{
    return period->condition[0] && period->precipitation >= 10;
}

static void render_weather(const projector_snapshot_t *s, const struct tm *local, bool valid_time,
                           int diameter)
{
    int level = size_level(diameter);
    const lv_font_t *small = &bed_font_text_10;
    const lv_font_t *text_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;

    /* Now: condition icon and temperature, then a scrolling line of details. */
    weather_look_t look;
    bool now = s->ha_fresh && weather_look(s->weather, false, &look);
    char temperature[12], details[160];
    degrees(temperature, sizeof(temperature), s->ha_fresh && s->temp_now_known, s->temp_now);
    row_item_t now_row[2] = {
        {weather_ui.icon, now ? look.icon : BED_ICON_HELP_CIRCLE_OUTLINE, icon_fonts,
         ARRAY_COUNT(icon_fonts), level, now ? look.color : COLOR_DIM, 0},
        {weather_ui.temperature, temperature, bold_fonts, ARRAY_COUNT(bold_fonts), level, COLOR_TEXT, 5},
    };
    int used = snprintf(details, sizeof(details), "%s", now ? look.label : "Meteo ?");
    if (s->ha_fresh && s->feels[0] && used < (int)sizeof(details))
        used += snprintf(details + used, sizeof(details) - used, "   " RECOLOR("a8b2c0", "%s %s"),
                         BED_ICON_THERMOMETER, s->feels);
    if (s->ha_fresh && s->humidity >= 0 && used < (int)sizeof(details))
        used += snprintf(details + used, sizeof(details) - used, "   " RECOLOR("6fb0ff", "%s") " %d%%",
                         BED_ICON_WATER_PERCENT, s->humidity);
    if (s->ha_fresh && s->wind[0] && used < (int)sizeof(details))
        snprintf(details + used, sizeof(details) - used, "   " RECOLOR("c8d0da", "%s") " %s",
                 BED_ICON_WEATHER_WINDY, s->wind);

    /* The day ahead: today until 21:00, then tomorrow, titled inside the rule. */
    bool tomorrow = valid_time && projector_shows_tomorrow(local->tm_hour);
    const projector_day_t *day = forecast_day(s, local, valid_time, tomorrow ? 1 : 0);
    const char *title = tomorrow ? "DOMANI" : "OGGI";
    /* A name and a rain chance are too wide for half the circle, so the rain
     * chances lead the scrolling outlook instead. */
    char outlook[160] = "";
    int length = 0;
    if (day && (rainy(&day->day) || rainy(&day->evening))) {
        length = snprintf(outlook, sizeof(outlook), RECOLOR("6fb0ff", "%s"), BED_ICON_UMBRELLA);
        if (rainy(&day->day))
            length += snprintf(outlook + length, sizeof(outlook) - length, " Giorno " RECOLOR("6fb0ff", "%d%%"),
                               day->day.precipitation);
        if (rainy(&day->evening) && length < (int)sizeof(outlook))
            length += snprintf(outlook + length, sizeof(outlook) - length, " Sera " RECOLOR("6fb0ff", "%d%%"),
                               day->evening.precipitation);
    }
    if (day && day->summary[0] && length < (int)sizeof(outlook))
        snprintf(outlook + length, sizeof(outlook) - length, "%s%s", length ? "   " : "", day->summary);
    const lv_font_t *period_icon = level >= 2 ? &bed_icons_18 : &bed_icons_22;
    const lv_font_t *period_temp = level >= 2 ? &bed_font_bold_14 : &bed_font_bold_16;
    int period_height = ink_height(period_icon) > ink_height(period_temp) ?
        ink_height(period_icon) : ink_height(period_temp);
    int periods = ink_height(small) + 4 + period_height;

    int heights[5] = {row_height(now_row, 2), ink_height(text_font), ink_height(small), periods,
                      outlook[0] ? ink_height(text_font) : 0};
    int gap = level >= 2 ? 4 : 5;
    int y = stack_top(diameter, heights, 5, gap);
    layout_row(now_row, 2, diameter, y, heights[0]);
    y += heights[0] + gap;
    place_marquee(weather_ui.summary, details, text_font, COLOR_TEXT, diameter, y, heights[1]);
    y += heights[1] + gap;
    int title_width = text_width(title, small);
    int rule = circle_half_width(diameter, y, heights[2]) * 2 * 3 / 4;
    int rule_width = (rule - title_width) / 2 - 5;
    set_label(weather_ui.title, title, small, COLOR_MUTED);
    place_on_row(weather_ui.title, small, diameter / 2 - title_width / 2, title_width + 1, y, heights[2]);
    set_area(weather_ui.rule_left, diameter / 2 - rule / 2, y + heights[2] / 2, rule_width, 1);
    set_area(weather_ui.rule_right, diameter / 2 + rule / 2 - rule_width, y + heights[2] / 2, rule_width, 1);
    show(weather_ui.rule_left, rule_width > 4);
    show(weather_ui.rule_right, rule_width > 4);
    y += heights[2] + gap;
    int offset = diameter * 6 / 25;
    const period_ui_t day_ui = {weather_ui.day_label, weather_ui.day_icon, weather_ui.day_temp};
    const period_ui_t evening_ui = {weather_ui.evening_label, weather_ui.evening_icon,
                                    weather_ui.evening_temp};
    render_period(&day_ui, day ? &day->day : NULL, false, "Giorno", diameter / 2 - offset, y,
                  period_height, period_icon, period_temp);
    render_period(&evening_ui, day ? &day->evening : NULL, true, "Sera", diameter / 2 + offset, y,
                  period_height, period_icon, period_temp);
    y += heights[3] + gap;
    show(weather_ui.outlook, outlook[0]);
    if (outlook[0]) place_marquee(weather_ui.outlook, outlook, text_font, COLOR_MUTED, diameter, y, heights[4]);
}

/* ---- Generic page of widget lines, also used for the access point screen. ---- */

static void text_widget(const char *name, const projector_snapshot_t *s, const projector_page_t *page,
                        const struct tm *local, bool valid_time, char *out, size_t size)
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
        if (valid_time) snprintf(out, size, "%s %d %s", weekdays[local->tm_wday], local->tm_mday,
                                 months[local->tm_mon]);
        else snprintf(out, size, "In attesa dell'ora");
    } else if (!strcmp(name, "text")) snprintf(out, size, "%s", page->text);
    else if (!strncmp(name, "entity", 6) && name[6] >= '1' && name[6] <= '4') {
        int index = name[6] - '1';
        snprintf(out, size, "%s", s->ha_fresh && s->extras[index][0] ? s->extras[index] : "Dato HA ?");
    } else out[0] = 0;
}

static void render_lines(const char *header, const char *const *lines, int count, int diameter)
{
    const lv_font_t *font = size_level(diameter) >= 2 ? &bed_font_text_10 : &bed_font_text_12;
    int heights[4] = {ink_height(&bed_font_text_10), 0, 0, 0};
    for (int i = 0; i < count; ++i) heights[i + 1] = ink_height(font);
    int gap = diameter >= 104 ? 12 : 9;
    int y = stack_top(diameter, heights, 4, gap);
    int width = text_width(header, &bed_font_text_10);
    set_label(list_ui.header, header, &bed_font_text_10, COLOR_MUTED);
    place_on_row(list_ui.header, &bed_font_text_10, diameter / 2 - width / 2, width, y, heights[0]);
    y += heights[0] + gap;
    for (int i = 0; i < 3; ++i) {
        show(list_ui.lines[i], i < count);
        if (i >= count) continue;
        place_marquee(list_ui.lines[i], lines[i], font, COLOR_TEXT, diameter, y, heights[i + 1]);
        y += heights[i + 1] + gap;
    }
}

/* ---- Calibration overlay ---- */

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

static lv_obj_t *new_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    return bar;
}

void bed_ui_create(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    projection = lv_obj_create(screen);
    lv_obj_remove_style_all(projection);
    lv_obj_remove_flag(projection, LV_OBJ_FLAG_SCROLLABLE);

    clock_ui.page = new_page();
    lv_obj_t **clock_labels[] = {&clock_ui.weather_icon, &clock_ui.high, &clock_ui.low, &clock_ui.time,
        &clock_ui.door_icon, &clock_ui.door_count, &clock_ui.light_icon, &clock_ui.light_count,
        &clock_ui.alarm_icon};
    for (int i = 0; i < ARRAY_COUNT(clock_labels); ++i)
        *clock_labels[i] = new_label(clock_ui.page, LV_LABEL_LONG_MODE_CLIP);
    clock_ui.date = new_label(clock_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    home_ui.page = new_page();
    lv_obj_t **home_labels[] = {&home_ui.alarm_icon, &home_ui.indoor_icon, &home_ui.indoor,
        &home_ui.outdoor_icon, &home_ui.outdoor, &home_ui.door_icon, &home_ui.light_icon};
    for (int i = 0; i < ARRAY_COUNT(home_labels); ++i)
        *home_labels[i] = new_label(home_ui.page, LV_LABEL_LONG_MODE_CLIP);
    home_ui.alarm = new_label(home_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    home_ui.doors = new_label(home_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    home_ui.lights = new_label(home_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    home_ui.divider = new_line(home_ui.page);

    weather_ui.page = new_page();
    lv_obj_t **weather_labels[] = {&weather_ui.icon, &weather_ui.temperature, &weather_ui.title,
        &weather_ui.day_icon, &weather_ui.day_temp, &weather_ui.day_label, &weather_ui.evening_icon,
        &weather_ui.evening_temp, &weather_ui.evening_label};
    for (int i = 0; i < ARRAY_COUNT(weather_labels); ++i)
        *weather_labels[i] = new_label(weather_ui.page, LV_LABEL_LONG_MODE_CLIP);
    weather_ui.summary = new_label(weather_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    weather_ui.outlook = new_label(weather_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    weather_ui.rule_left = new_line(weather_ui.page);
    weather_ui.rule_right = new_line(weather_ui.page);

    list_ui.page = new_page();
    list_ui.header = new_label(list_ui.page, LV_LABEL_LONG_MODE_CLIP);
    for (int i = 0; i < 3; ++i) list_ui.lines[i] = new_label(list_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    calibration_ring = lv_canvas_create(screen);
    calibration_horizontal = new_bar(screen);
    calibration_vertical = new_bar(screen);
    calibration_top = lv_label_create(screen);
    lv_obj_set_style_text_font(calibration_top, &bed_font_text_12, 0);
    lv_label_set_text(calibration_top, "SU");
    show(calibration_ring, false);
    show(calibration_horizontal, false);
    show(calibration_vertical, false);
    show(calibration_top, false);
}

void bed_ui_render(const projector_snapshot_t *s, time_t now, const char *ap_ssid,
                   const char *ap_password)
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
    bool calibrating = s->calibration.active && !ap_ssid;
    set_area(projection, left, top, diameter, diameter);
    show(projection, !calibrating);
    show(calibration_ring, calibrating);
    show(calibration_horizontal, calibrating);
    show(calibration_vertical, calibrating);
    show(calibration_top, calibrating);
    if (calibrating) {
        lv_obj_set_pos(calibration_ring, left, top);
        draw_calibration_ring(diameter);
        set_area(calibration_horizontal, left + 4, center_y - 1, diameter - 8, 2);
        set_area(calibration_vertical, center_x - 1, top + 4, 2, diameter - 8);
        /* Text shows both the rotation and the mirroring of the content. */
        lv_obj_set_pos(calibration_top, center_x + 6, top + diameter / 4 - 7);
        return;
    }

    struct tm local;
    localtime_r(&now, &local);
    bool valid_time = now > VALID_TIME;
    const projector_page_t *page = &s->pages[s->page_index];
    lv_obj_t *active = list_ui.page;
    if (ap_ssid) {
        char password_a[9], password_b[9];
        snprintf(password_a, sizeof(password_a), "%.8s", ap_password);
        snprintf(password_b, sizeof(password_b), "%s", strlen(ap_password) > 8 ? ap_password + 8 : "");
        const char *lines[3] = {ap_ssid, password_a, password_b};
        render_lines("WI-FI AP", lines, 3, diameter);
    } else if (!strcmp(page->layout, "clock")) {
        active = clock_ui.page;
        render_clock(s, &local, valid_time, diameter);
    } else if (!strcmp(page->layout, "home")) {
        active = home_ui.page;
        render_home(s, diameter);
    } else if (!strcmp(page->layout, "weather")) {
        active = weather_ui.page;
        render_weather(s, &local, valid_time, diameter);
    } else {
        char values[3][64];
        const char *lines[3];
        int count = 0;
        for (int i = 0; i < PROJECTOR_MAX_WIDGETS; ++i)
            if (page->widgets[i][0]) {
                text_widget(page->widgets[i], s, page, &local, valid_time, values[count],
                            sizeof(values[count]));
                lines[count] = values[count];
                ++count;
            }
        char header[17];
        snprintf(header, sizeof(header), "%s", page->name);
        for (char *c = header; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 'a' - 'A';
        render_lines(header, lines, count, diameter);
    }
    lv_obj_t *const pages[] = {clock_ui.page, home_ui.page, weather_ui.page, list_ui.page};
    for (int i = 0; i < ARRAY_COUNT(pages); ++i) {
        set_area(pages[i], 0, 0, diameter, diameter);
        show(pages[i], pages[i] == active);
    }
}

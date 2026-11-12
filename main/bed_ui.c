#include "bed_ui.h"
#include "bed_fonts.h"
#include "bed_icons.h"
#include "projector_logic.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(items) ((int)(sizeof(items) / sizeof((items)[0])))
/* Clocks before 2020 have not been set by NTP yet. */
#define VALID_TIME 1577836800
/* Marquee speed in pixels per second: slow enough to read lying in bed. */
#define MARQUEE_SPEED 16

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
#define COLOR_WIND 0xc8d0da
#define COLOR_SUNRISE 0xffc860
#define COLOR_SUNSET 0xff8c60
#define COLOR_ALERT 0xff3030
#define COLOR_ALERT_TEXT 0xff8a80

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

/* Message icon names accepted by projector.c, mapped to their glyphs. */
static const struct {
    const char *name, *icon;
} message_icons[] = {
    {"bell", BED_ICON_BELL},
    {"information", BED_ICON_INFORMATION},
    {"alert", BED_ICON_ALERT},
    {"washing-machine", BED_ICON_WASHING_MACHINE},
    {"package-variant", BED_ICON_PACKAGE},
    {"door-open", BED_ICON_DOOR_OPEN},
    {"lightbulb-on", BED_ICON_LIGHTBULB_ON},
    {"phone", BED_ICON_PHONE},
    {"alarm", BED_ICON_ALARM},
    {"calendar", BED_ICON_CALENDAR},
};

/* Indexed by projector_moon_phase(). */
static const char *const moon_phases[] = {"Luna nuova", "Luna crescente", "Primo quarto",
    "Gibbosa crescente", "Luna piena", "Gibbosa calante", "Ultimo quarto", "Luna calante"};

static lv_obj_t *projection;
static lv_obj_t *calibration_ring, *calibration_horizontal, *calibration_vertical, *calibration_top;
/* Arcs and rounded borders need LV_DRAW_SW_COMPLEX, so the calibration ring and the thermostat
 * dial are drawn pixel by pixel. They are never visible together and share one 32 KB buffer:
 * canvas_owner is the canvas whose drawing it holds. */
static uint8_t canvas_buffer[LV_CANVAS_BUF_SIZE(128, 128, 16, LV_DRAW_BUF_STRIDE_ALIGN)];
static lv_obj_t *canvas_owner;
static int ring_diameter;

static struct {
    lv_obj_t *page, *weather_icon, *high, *low, *time, *door_icon, *door_count, *light_icon,
        *light_count, *alarm_icon, *wake_icon, *wake_text, *date;
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
    lv_obj_t *page, *wake_icon, *wake_time, *wake_day, *divider, *events[PROJECTOR_MAX_EVENTS];
} agenda_ui;

static struct {
    lv_obj_t *page, *dial, *inner, *status_icon, *status, *target, *current, *humidity;
} climate_ui;

static struct {
    lv_obj_t *page, *header, *icons[PROJECTOR_MAX_WIDGETS], *lines[PROJECTOR_MAX_WIDGETS];
} list_ui;

static struct {
    lv_obj_t *page, *icon, *lines[2];
} message_ui;

static struct {
    lv_obj_t *page, *dial, *inner, *time, *icons[SLOT_COUNT], *values[SLOT_COUNT];
} face_ui;

static struct {
    lv_obj_t *page, *title, *time, *weather_icon, *high, *low, *event;
} wake_ui;

/* Palette of the frame being drawn: every colour goes through paint(). */
static projector_theme_t theme = {.palette = PALETTE_COLOR};

static uint32_t paint(uint32_t color)
{
    return projector_tint(color, theme.palette, theme.tint);
}

/* "rrggbb" of a painted colour for "#rrggbb text#" markup. Each call returns its own buffer
 * of a small ring, so one snprintf can take several. */
static const char *hex(uint32_t color)
{
    static char buffers[8][7];
    static int next;
    char *out = buffers[next++ % 8];
    snprintf(out, 7, "%06lx", (unsigned long)paint(color));
    return out;
}

/* Hairlines, repainted when the palette changes. */
static lv_obj_t *hairlines[4];
static int hairline_count;

/* One square per page, shown on secondary pages. */
static lv_obj_t *page_dots[PROJECTOR_MAX_PAGES];
#define DOT_SIZE 3
#define DOT_SPACING 4
/* Pixels kept free at the bottom of the circle while the page dots are shown. */
static int bottom_reserve;

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
    lv_color_t value = lv_color_hex(paint(color));
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, 0), value))
        lv_obj_set_style_text_color(obj, value, 0);
}

static void set_background(lv_obj_t *obj, uint32_t color)
{
    lv_color_t value = lv_color_hex(paint(color));
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, 0), value)) lv_obj_set_style_bg_color(obj, value, 0);
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
    int half_width = remaining > 0 ? (int)sqrtf((float)remaining) : 0;
    while (half_width * half_width > remaining) --half_width; /* Guard float rounding. */
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

/* Centre a marquee in the circle; it scrolls only when the row is too narrow. The
 * box gets 2 spare pixels so a text that just fits is never clipped or scrolled. */
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
    hairlines[hairline_count++] = line;
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
            if (!strcmp(s->ha.alarm, alarm_modes[i].state)) return &alarm_modes[i];
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
    if (!valid_time) return days_after < PROJECTOR_FORECAST_DAYS && s->ha.days[days_after].date[0] ?
        &s->ha.days[days_after] : NULL;
    char date[11];
    projector_date_after(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday, days_after, date);
    for (int i = 0; i < PROJECTOR_FORECAST_DAYS; ++i)
        if (!strcmp(s->ha.days[i].date, date)) return &s->ha.days[i];
    return NULL;
}

static void degrees(char *out, size_t size, bool known, int value)
{
    if (known) snprintf(out, size, "%d\xC2\xB0", value);
    else snprintf(out, size, "--\xC2\xB0");
}

/* "Finestra Sala · Finestra Cucina +2", or a plain count when HA sent no names. */
static void join_names(char *out, size_t size, const char (*names)[32], int count, int total,
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

/* "Oggi", "Domani" or the weekday of an HA date in the coming week; "dd/mm" otherwise. */
static void day_name(const char *date, const struct tm *local, bool valid_time, char *out, size_t size)
{
    int days = valid_time ? projector_relative_day(local->tm_year + 1900, local->tm_mon + 1,
                                                   local->tm_mday, date) : -1;
    if (days == 0) snprintf(out, size, "Oggi");
    else if (days == 1) snprintf(out, size, "Domani");
    else if (days > 1) snprintf(out, size, "%s", weekdays[(local->tm_wday + days) % 7]);
    else if (strlen(date) == 10) snprintf(out, size, "%.2s/%.2s", date + 8, date + 5);
    else snprintf(out, size, "?");
}

/* "Oggi 18:30", "Domani" (all day) or "Mer 9:00": when an event starts. */
static void event_when(const projector_event_t *event, const struct tm *local, bool valid_time,
                       char *out, size_t size)
{
    char day[12];
    day_name(event->date, local, valid_time, day, sizeof(day));
    if (!event->time[0]) snprintf(out, size, "%s", day);
    else if (!strcmp(day, "Oggi")) snprintf(out, size, "%s", event->time);
    else snprintf(out, size, "%s %s", day, event->time);
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

/* The font step that fits the items in available, or the smallest; total is their width. */
static int fit_row(const row_item_t *items, int count, int available, int *total)
{
    for (int step = 0;; ++step) {
        *total = 0;
        bool smallest = true;
        for (int i = 0; i < count; ++i) {
            *total += (i ? items[i].gap : 0) + text_width(items[i].text, item_font(&items[i], step));
            if (items[i].level + step < items[i].font_count - 1) smallest = false;
        }
        if (*total <= available || smallest) return step;
    }
}

static bool row_fits(const row_item_t *items, int count, int diameter, int y, int height)
{
    int available = circle_half_width(diameter, y, height) * 2, total;
    fit_row(items, count, available, &total);
    return total <= available;
}

/* Centre the items on one line, stepping all fonts down together until it fits the circle. */
static void layout_row(const row_item_t *items, int count, int diameter, int y, int height)
{
    int total, step = fit_row(items, count, circle_half_width(diameter, y, height) * 2, &total);
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

/* An icon and a label as a LABELED_* mode shows them: both, the label alone or the icon alone.
 * LABELED_OFF hides both. */
static void layout_labeled(int mode, lv_obj_t *icon_obj, const char *icon, uint32_t icon_color,
                           const lv_font_t *icon_font, lv_obj_t *text_obj, const char *text, uint32_t color,
                           const lv_font_t *font, int diameter, int y, int height)
{
    show(icon_obj, mode == LABELED_ICON_LABEL || mode == LABELED_ICON);
    show(text_obj, mode == LABELED_ICON_LABEL || mode == LABELED_LABEL);
    if (mode == LABELED_ICON_LABEL)
        layout_icon_text(icon_obj, icon, icon_color, icon_font, text_obj, text, color, font, diameter, y,
                         height);
    else if (mode == LABELED_LABEL)
        place_marquee(text_obj, text, font, color, diameter, y, height);
    else if (mode == LABELED_ICON) {
        int width = text_width(icon, icon_font);
        set_label(icon_obj, icon, icon_font, icon_color);
        place_on_row(icon_obj, icon_font, diameter / 2 - width / 2, width, y, height);
    }
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
    /* Rise only as much as needed to clear the page dots. */
    if (bottom_reserve && top + total > diameter - bottom_reserve) top = diameter - bottom_reserve - total;
    return top < 0 ? 0 : top;
}

/* 0 for the largest circles, 3 for the smallest: picks the preferred font sizes. */
static int size_level(int diameter)
{
    return diameter >= 122 ? 0 : diameter >= 106 ? 1 : diameter >= 94 ? 2 : 3;
}

/* ---- Clock page: forecast, time, conditional house status, scrolling date. ---- */

static void render_clock(const projector_snapshot_t *s, const projector_page_t *page, const struct tm *local,
                         bool valid_time, int diameter)
{
    int level = projector_size_level(size_level(diameter), page->size);
    /* Until tomorrow_from (21:00 by default) today's forecast, then tomorrow's. */
    bool tomorrow = valid_time && projector_shows_tomorrow(local->tm_hour, page->tomorrow_from);
    const projector_day_t *day = forecast_day(s, local, valid_time, tomorrow ? 1 : 0);
    weather_look_t look;
    int forecast_mode = page->show[CLOCK_FORECAST];
    bool weather = forecast_mode != FORECAST_OFF && day && weather_look(day->condition, false, &look);
    bool high_known = weather && day->high_known, low_known = weather && day->low_known;
    int high = weather ? day->high : 0, low = weather ? day->low : 0;
    char high_text[12], low_text[12];
    degrees(high_text, sizeof(high_text), high_known, high);
    degrees(low_text, sizeof(low_text), low_known, low);
    row_item_t weather_items[3] = {
        {clock_ui.weather_icon, weather ? look.icon : "", icon_fonts, ARRAY_COUNT(icon_fonts),
         level + 1, weather ? look.color : 0, 0},
        {clock_ui.high, high_text, bold_fonts, ARRAY_COUNT(bold_fonts), level + 1, COLOR_WARM, 5},
        {clock_ui.low, low_text, bold_fonts, ARRAY_COUNT(bold_fonts), level + 1, COLOR_COOL, 6},
    };
    /* The forecast row: icon and temperatures, the temperatures alone or the icon alone. */
    row_item_t *weather_row = weather_items + (forecast_mode == FORECAST_TEMPS ? 1 : 0);
    int weather_count = forecast_mode == FORECAST_TEMPS ? 2 : forecast_mode == FORECAST_ICON ? 1 : 3;

    char time_text[8] = "--:--";
    if (valid_time) snprintf(time_text, sizeof(time_text), "%02d:%02d", local->tm_hour, local->tm_min);
    row_item_t time_row[1] = {{clock_ui.time, time_text, clock_fonts, ARRAY_COUNT(clock_fonts), level,
                               COLOR_TEXT, 0}};

    /* Status icons appear only when something needs attention. An unusable door
     * count must never look like "all closed", so it shows as a grey "?", even when
     * the page shows icons alone. The morning alarm is the exception: a known "none"
     * shows as a dim crossed bell, while an unknown one is left out rather than
     * passing for either. */
    int doors_mode = page->show[CLOCK_OPENINGS], lights_mode = page->show[CLOCK_LIGHTS];
    int wake_mode = page->show[CLOCK_WAKE];
    bool doors_known = s->ha_fresh && s->ha.openings_known;
    bool doors = doors_mode != STATUS_OFF && (!doors_known || s->ha.openings);
    bool lights = lights_mode != STATUS_OFF && s->ha_fresh && s->ha.lights_known && s->ha.lights;
    const alarm_mode_t *alarm = alarm_mode(s);
    bool alarm_visible = page->show[CLOCK_ALARM] == SHOW_ON && strcmp(alarm->state, "disarmed");
    char door_count[4] = "?", light_count[4];
    if (doors_known) snprintf(door_count, sizeof(door_count), "%u", s->ha.openings);
    snprintf(light_count, sizeof(light_count), "%u", s->ha.lights);
    bool wake_known = valid_time && s->ha_fresh && s->ha.wake_known && wake_mode != WAKE_OFF;
    int wake_day = wake_known && s->ha.wake_date[0] ?
        projector_relative_day(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday,
                               s->ha.wake_date) : -1;
    const char *colon = strchr(s->ha.wake_time, ':');
    int wake_hour = atoi(s->ha.wake_time), wake_minute = colon ? atoi(colon + 1) : 0;
    bool wake = wake_day >= 0 && projector_wake_next_morning(local->tm_hour, wake_day, wake_hour);
    /* Beside an orange bell, optionally the alarm time or the time left until it. */
    char wake_text[12] = "";
    if (wake && wake_mode == WAKE_TIME)
        snprintf(wake_text, sizeof(wake_text), "%s", s->ha.wake_time);
    else if (wake && wake_mode == WAKE_COUNTDOWN)
        projector_format_countdown(local->tm_hour, local->tm_min, wake_day, wake_hour, wake_minute,
                                   wake_text, sizeof(wake_text));
    row_item_t status_row[7];
    int status = 0;
    int status_level = level + 2;
    /* Narrow circles need tighter groups to fit all four. */
    int group_gap = level >= 2 ? 6 : 10;
    if (doors) {
        uint32_t color = doors_known ? COLOR_OPEN : COLOR_DIM;
        status_row[status++] = (row_item_t){clock_ui.door_icon, BED_ICON_DOOR_OPEN, icon_fonts,
            ARRAY_COUNT(icon_fonts), status_level, color, 0};
        if (doors_mode == STATUS_COUNT || !doors_known)
            status_row[status++] = (row_item_t){clock_ui.door_count, door_count, bold_fonts,
                ARRAY_COUNT(bold_fonts), status_level + 1, color, 1};
    }
    if (lights) {
        status_row[status++] = (row_item_t){clock_ui.light_icon, BED_ICON_LIGHTBULB_ON, icon_fonts,
            ARRAY_COUNT(icon_fonts), status_level, COLOR_LIGHT, status ? group_gap : 0};
        if (lights_mode == STATUS_COUNT)
            status_row[status++] = (row_item_t){clock_ui.light_count, light_count, bold_fonts,
                ARRAY_COUNT(bold_fonts), status_level + 1, COLOR_LIGHT, 1};
    }
    if (alarm_visible)
        status_row[status++] = (row_item_t){clock_ui.alarm_icon, alarm->icon, icon_fonts,
            ARRAY_COUNT(icon_fonts), status_level, alarm->color, status ? group_gap : 0};
    int wake_start = status;
    if (wake_known)
        status_row[status++] = (row_item_t){clock_ui.wake_icon, wake ? BED_ICON_ALARM : BED_ICON_ALARM_OFF,
            icon_fonts, ARRAY_COUNT(icon_fonts), status_level, wake ? COLOR_WARM : COLOR_DIM,
            status ? group_gap : 0};
    if (wake_text[0])
        /* The bold faces hold only digits; the narrower text face also has "h" and "m". */
        status_row[status++] = (row_item_t){clock_ui.wake_text, wake_text, text_fonts,
            ARRAY_COUNT(text_fonts), level >= 2 ? 2 : 1, COLOR_WARM, 2};

    /* The date line: the date in full or short, then sunrise and sunset. */
    int date_mode = page->show[CLOCK_DATE];
    char date[128] = "";
    const projector_day_t *today = forecast_day(s, local, valid_time, 0);
    if (!valid_time) {
        if (date_mode != DATE_OFF) snprintf(date, sizeof(date), "In attesa dell'ora");
    } else {
        int used = 0;
        if (date_mode == DATE_FULL)
            used = snprintf(date, sizeof(date), "%s %d %s", weekdays[local->tm_wday], local->tm_mday,
                            months[local->tm_mon]);
        else if (date_mode == DATE_SHORT)
            used = snprintf(date, sizeof(date), "%s %d", weekdays[local->tm_wday], local->tm_mday);
        if (page->show[CLOCK_SUN] == SHOW_ON && today && today->sunrise[0] && today->sunset[0])
            snprintf(date + used, sizeof(date) - used,
                     "%s#%s " BED_ICON_SUNRISE "# #%s %s#   #%s " BED_ICON_SUNSET "# #%s %s#",
                     used ? "   " : "", hex(COLOR_SUNRISE), hex(COLOR_MUTED), today->sunrise, hex(COLOR_SUNSET), hex(COLOR_MUTED),
                     today->sunset);
    }
    const lv_font_t *date_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;
    /* The date row ink includes the inline icons, which reach above the digits. */
    int date_height = date[0] ? lv_font_get_line_height(date_font) - 4 : 0;

    int heights[5] = {weather ? row_height(weather_row, weather_count) : 0, row_height(time_row, 1),
                      status ? row_height(status_row, status) : 0, 0, date_height};
    int gap = level >= 2 ? 5 : 7;
    int y = stack_top(diameter, heights, 5, gap);
    /* The wake alarm with its text moves to a row of its own when the status row overflows;
     * a smaller clock and tighter gaps make room for it. */
    row_item_t *wake_row = status_row + wake_start;
    int wake_count = 0;
    if (wake_text[0] && wake_start) {
        int status_y = y + (weather ? heights[0] + gap : 0) + heights[1] + gap;
        if (!row_fits(status_row, status, diameter, status_y, heights[2])) {
            wake_count = status - wake_start;
            status = wake_start;
            wake_row[0].gap = 0;
            ++time_row[0].level;
            gap = level >= 2 ? 3 : 4;
            heights[1] = row_height(time_row, 1);
            heights[2] = row_height(status_row, status);
            heights[3] = row_height(wake_row, wake_count);
            y = stack_top(diameter, heights, 5, gap);
        }
    }
    for (int i = 0; i < 3; ++i) show(weather_items[i].obj, false);
    if (weather) {
        layout_row(weather_row, weather_count, diameter, y, heights[0]);
        y += heights[0] + gap;
    }
    layout_row(time_row, 1, diameter, y, heights[1]);
    y += heights[1] + gap;
    lv_obj_t *const status_objects[] = {clock_ui.door_icon, clock_ui.door_count, clock_ui.light_icon,
        clock_ui.light_count, clock_ui.alarm_icon, clock_ui.wake_icon, clock_ui.wake_text};
    for (int i = 0; i < ARRAY_COUNT(status_objects); ++i) show(status_objects[i], false);
    if (status) {
        layout_row(status_row, status, diameter, y, heights[2]);
        y += heights[2] + gap;
    }
    if (wake_count) {
        layout_row(wake_row, wake_count, diameter, y, heights[3]);
        y += heights[3] + gap;
    }
    show(clock_ui.date, date[0]);
    if (date[0]) place_marquee(clock_ui.date, date, date_font, COLOR_TEXT, diameter, y, date_height);
}

/* ---- Home page: alarm, bedroom and outdoor temperature, open doors, lights. ---- */

static void render_home(const projector_snapshot_t *s, const projector_page_t *page, int diameter)
{
    int level = projector_size_level(size_level(diameter), page->size);
    const lv_font_t *text_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;
    const lv_font_t *row_icon = level >= 2 ? &bed_icons_12 : &bed_icons_14;
    const lv_font_t *temp_font = level == 0 ? &bed_font_bold_16 : &bed_font_bold_14;
    const lv_font_t *temp_icon = level == 0 ? &bed_icons_14 : &bed_icons_12;
    int alarm_mode_shown = page->show[HOME_ALARM], temps_mode = page->show[HOME_TEMPERATURES];
    int doors_mode = page->show[HOME_OPENINGS], lights_mode = page->show[HOME_LIGHTS];

    /* Rows hug the centre, where the circle is widest: alarm, temperatures,
     * open doors and windows, lights. */
    int row = ink_height(row_icon) > ink_height(text_font) ? ink_height(row_icon) : ink_height(text_font);
    int temp_height = ink_height(temp_icon) > ink_height(temp_font) ?
        ink_height(temp_icon) : ink_height(temp_font);
    int gap = level >= 2 ? 5 : 7;
    int heights[4] = {alarm_mode_shown != LABELED_OFF ? row : 0, temps_mode != TEMPS_OFF ? temp_height : 0,
                      doors_mode != NAMES_OFF ? row : 0, lights_mode != NAMES_OFF ? row : 0};
    int y = stack_top(diameter, heights, 4, gap);

    const alarm_mode_t *alarm = alarm_mode(s);
    layout_labeled(alarm_mode_shown, home_ui.alarm_icon, alarm->icon, alarm->color, row_icon, home_ui.alarm,
                   alarm->label, alarm->color, text_font, diameter, y, row);
    if (heights[0]) y += row + gap;

    /* Bedroom left and outdoors right of a hairline, each an icon and a value. */
    const char *indoor = s->ha_fresh && s->ha.indoor[0] ? s->ha.indoor : "--\xC2\xB0";
    const char *outdoor = s->ha_fresh && s->ha.outdoor[0] ? s->ha.outdoor : "--\xC2\xB0";
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
    bool icons = temps_mode == TEMPS_ICONS;
    for (int i = 0; i < 2; ++i)
        if (half_gap + text_width(columns[i].glyph, temp_icon) + 2 +
            text_width(columns[i].text, temp_font) > circle_half_width(diameter, y, temp_height) + 2)
            icons = false; /* + 2: the half width already keeps 2 pixels off the edge. */
    show(home_ui.divider, temps_mode != TEMPS_OFF);
    for (int i = 0; i < 2; ++i) {
        show(columns[i].icon, icons);
        show(columns[i].value, temps_mode != TEMPS_OFF);
        if (temps_mode == TEMPS_OFF) continue;
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
    if (temps_mode != TEMPS_OFF) {
        set_area(home_ui.divider, diameter / 2, y - 2, 1, temp_height + 4);
        y += temp_height + gap;
    }

    /* Names of what is open or on, or only how many. */
    char text[300];
    show(home_ui.door_icon, doors_mode != NAMES_OFF);
    show(home_ui.doors, doors_mode != NAMES_OFF);
    if (doors_mode == NAMES_OFF) {
        /* Hidden above. */
    } else if (!(s->ha_fresh && s->ha.openings_known))
        layout_icon_text(home_ui.door_icon, BED_ICON_DOOR_OPEN, COLOR_DIM, row_icon, home_ui.doors,
                         "Aperture ?", COLOR_DIM, text_font, diameter, y, row);
    else if (!s->ha.openings)
        layout_icon_text(home_ui.door_icon, BED_ICON_DOOR_CLOSED, COLOR_OK, row_icon, home_ui.doors,
                         "Tutto chiuso", COLOR_MUTED, text_font, diameter, y, row);
    else {
        int names = doors_mode == NAMES_ALL ? s->ha.opening_name_count : 0;
        join_names(text, sizeof(text), s->ha.opening_names, names, s->ha.openings,
                   s->ha.openings == 1 ? "aperta" : "aperte");
        layout_icon_text(home_ui.door_icon, BED_ICON_DOOR_OPEN, COLOR_OPEN, row_icon, home_ui.doors,
                         text, COLOR_TEXT, text_font, diameter, y, row);
    }
    if (heights[2]) y += row + gap;

    show(home_ui.light_icon, lights_mode != NAMES_OFF);
    show(home_ui.lights, lights_mode != NAMES_OFF);
    if (lights_mode == NAMES_OFF) {
        /* Hidden above. */
    } else if (!(s->ha_fresh && s->ha.lights_known))
        layout_icon_text(home_ui.light_icon, BED_ICON_LIGHTBULB_OUTLINE, COLOR_DIM, row_icon,
                         home_ui.lights, "Luci ?", COLOR_DIM, text_font, diameter, y, row);
    else if (!s->ha.lights)
        layout_icon_text(home_ui.light_icon, BED_ICON_LIGHTBULB_OUTLINE, COLOR_MUTED, row_icon,
                         home_ui.lights, "Luci spente", COLOR_MUTED, text_font, diameter, y, row);
    else {
        int names = lights_mode == NAMES_ALL ? s->ha.light_name_count : 0;
        join_names(text, sizeof(text), s->ha.light_names, names, s->ha.lights,
                   s->ha.lights == 1 ? "accesa" : "accese");
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

static void render_weather(const projector_snapshot_t *s, const projector_page_t *page, const struct tm *local,
                           bool valid_time, int diameter)
{
    int level = projector_size_level(size_level(diameter), page->size);
    int now_mode = page->show[WEATHER_NOW], details_mode = page->show[WEATHER_DETAILS];
    bool periods_shown = page->show[WEATHER_PERIODS] == SHOW_ON;
    const lv_font_t *small = &bed_font_text_10;
    const lv_font_t *text_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;

    /* Now: condition icon and temperature, then a scrolling line of details. */
    weather_look_t look;
    bool now = s->ha_fresh && weather_look(s->ha.weather, false, &look);
    char temperature[12];
    degrees(temperature, sizeof(temperature), s->ha_fresh && s->ha.temp_now_known, s->ha.temp_now);
    row_item_t now_items[2] = {
        {weather_ui.icon, now ? look.icon : BED_ICON_HELP_CIRCLE_OUTLINE, icon_fonts,
         ARRAY_COUNT(icon_fonts), level, now ? look.color : COLOR_DIM, 0},
        {weather_ui.temperature, temperature, bold_fonts, ARRAY_COUNT(bold_fonts), level, COLOR_TEXT, 5},
    };
    /* Icon and temperature, the temperature alone or the icon alone. */
    row_item_t *now_row = now_items + (now_mode == NOW_TEMP ? 1 : 0);
    int now_count = now_mode == NOW_ICON_TEMP ? 2 : 1;
    char details[160] = "";
    int used = details_mode == DETAILS_OFF ? 0 :
        snprintf(details, sizeof(details), "%s", now ? look.label : "Meteo ?");
    bool full = details_mode == DETAILS_FULL;
    if (full && s->ha_fresh && s->ha.feels[0] && used < (int)sizeof(details))
        used += snprintf(details + used, sizeof(details) - used, "   #%s %s %s#",
                         hex(COLOR_MUTED), BED_ICON_THERMOMETER, s->ha.feels);
    if (full && s->ha_fresh && s->ha.humidity >= 0 && used < (int)sizeof(details))
        used += snprintf(details + used, sizeof(details) - used, "   #%s %s# %d%%",
                         hex(COLOR_RAIN), BED_ICON_WATER_PERCENT, s->ha.humidity);
    if (full && s->ha_fresh && s->ha.wind[0] && used < (int)sizeof(details))
        snprintf(details + used, sizeof(details) - used, "   #%s %s# %s",
                 hex(COLOR_WIND), BED_ICON_WEATHER_WINDY, s->ha.wind);

    /* The day ahead: today until tomorrow_from, then tomorrow, titled inside the rule. */
    bool tomorrow = valid_time && projector_shows_tomorrow(local->tm_hour, page->tomorrow_from);
    const projector_day_t *day = forecast_day(s, local, valid_time, tomorrow ? 1 : 0);
    const char *title = tomorrow ? "DOMANI" : "OGGI";
    /* A name and a rain chance are too wide for half the circle, so the rain
     * chances lead the scrolling outlook instead. */
    char outlook[160] = "";
    int length = 0;
    if (page->show[WEATHER_RAIN] == SHOW_ON && day && (rainy(&day->day) || rainy(&day->evening))) {
        length = snprintf(outlook, sizeof(outlook), "#%s %s#", hex(COLOR_RAIN), BED_ICON_UMBRELLA);
        if (rainy(&day->day))
            length += snprintf(outlook + length, sizeof(outlook) - length, " Giorno #%s %d%%#",
                               hex(COLOR_RAIN), day->day.precipitation);
        if (rainy(&day->evening) && length < (int)sizeof(outlook))
            length += snprintf(outlook + length, sizeof(outlook) - length, " Sera #%s %d%%#",
                               hex(COLOR_RAIN), day->evening.precipitation);
    }
    if (page->show[WEATHER_SUMMARY] == SHOW_ON && day && day->summary[0] && length < (int)sizeof(outlook))
        snprintf(outlook + length, sizeof(outlook) - length, "%s%s", length ? "   " : "", day->summary);
    const lv_font_t *period_icon = level >= 2 ? &bed_icons_18 : &bed_icons_22;
    const lv_font_t *period_temp = level >= 2 ? &bed_font_bold_14 : &bed_font_bold_16;
    int period_height = ink_height(period_icon) > ink_height(period_temp) ?
        ink_height(period_icon) : ink_height(period_temp);
    int periods = ink_height(small) + 4 + period_height;

    int heights[5] = {row_height(now_row, now_count), details[0] ? ink_height(text_font) : 0,
                      periods_shown ? ink_height(small) : 0, periods_shown ? periods : 0,
                      outlook[0] ? ink_height(text_font) : 0};
    int gap = level >= 2 ? 4 : 5;
    int y = stack_top(diameter, heights, 5, gap);
    show(weather_ui.icon, false);
    show(weather_ui.temperature, false);
    layout_row(now_row, now_count, diameter, y, heights[0]);
    y += heights[0] + gap;
    show(weather_ui.summary, details[0]);
    if (details[0]) {
        place_marquee(weather_ui.summary, details, text_font, COLOR_TEXT, diameter, y, heights[1]);
        y += heights[1] + gap;
    }
    lv_obj_t *const period_objects[] = {weather_ui.title, weather_ui.rule_left, weather_ui.rule_right,
        weather_ui.day_label, weather_ui.day_icon, weather_ui.day_temp, weather_ui.evening_label,
        weather_ui.evening_icon, weather_ui.evening_temp};
    if (!periods_shown)
        for (int i = 0; i < ARRAY_COUNT(period_objects); ++i) show(period_objects[i], false);
    else {
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
    }
    show(weather_ui.outlook, outlook[0]);
    if (outlook[0]) place_marquee(weather_ui.outlook, outlook, text_font, COLOR_MUTED, diameter, y, heights[4]);
}

/* ---- Agenda page: next phone alarm, then the coming calendar events. ---- */

static void render_agenda(const projector_snapshot_t *s, const projector_page_t *page, const struct tm *local,
                          bool valid_time, int diameter)
{
    int level = projector_size_level(size_level(diameter), page->size);
    int wake_mode = page->show[AGENDA_WAKE];
    int max_events = PROJECTOR_MAX_EVENTS - page->show[AGENDA_EVENTS]; /* EVENTS_3 to EVENTS_OFF. */
    bool event_time = page->show[AGENDA_EVENT_TIME] == SHOW_ON;
    const lv_font_t *text_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;

    /* An unusable alarm sensor must not read as "no alarm set". */
    bool wake_known = s->ha_fresh && s->ha.wake_known;
    bool wake = wake_known && s->ha.wake_date[0];
    char day[12] = "";
    if (wake) day_name(s->ha.wake_date, local, valid_time, day, sizeof(day));
    row_item_t wake_row[2] = {
        {agenda_ui.wake_icon, wake ? BED_ICON_ALARM : BED_ICON_ALARM_OFF, icon_fonts,
         ARRAY_COUNT(icon_fonts), level + 1, wake ? COLOR_WARM : wake_known ? COLOR_MUTED : COLOR_DIM, 0},
        wake ? (row_item_t){agenda_ui.wake_time, s->ha.wake_time, bold_fonts, ARRAY_COUNT(bold_fonts),
                            level, COLOR_TEXT, 6}
             : (row_item_t){agenda_ui.wake_time, wake_known ? "Nessuna sveglia" : "Sveglia ?", text_fonts,
                            ARRAY_COUNT(text_fonts), level >= 2 ? 2 : 1, wake_known ? COLOR_MUTED : COLOR_DIM, 5},
    };

    char lines[PROJECTOR_MAX_EVENTS][96];
    uint32_t colors[PROJECTOR_MAX_EVENTS];
    int count = 0;
    if (!max_events) {
        /* No events on this page. */
    } else if (!s->ha_fresh || !s->ha.events_known) {
        snprintf(lines[0], sizeof(lines[0]), "Eventi ?");
        colors[count++] = COLOR_DIM;
    } else if (!s->ha.event_count) {
        snprintf(lines[0], sizeof(lines[0]), "Nessun evento");
        colors[count++] = COLOR_MUTED;
    } else {
        for (; count < s->ha.event_count && count < max_events; ++count) {
            char when[20];
            event_when(&s->ha.events[count], local, valid_time, when, sizeof(when));
            if (event_time)
                snprintf(lines[count], sizeof(lines[count]), "#%s %s# %s", hex(COLOR_MUTED), when,
                         s->ha.events[count].title);
            else snprintf(lines[count], sizeof(lines[count]), "%s", s->ha.events[count].title);
            colors[count] = COLOR_TEXT;
        }
    }

    const lv_font_t *small = &bed_font_text_10;
    /* The alarm, its day under it, then a rule over the events when both are shown. */
    bool wake_shown = wake_mode != AGENDA_WAKE_OFF, day_shown = wake && wake_mode == AGENDA_WAKE_TIME_DAY;
    bool divider = wake_shown && count;
    int heights[3 + PROJECTOR_MAX_EVENTS] = {wake_shown ? row_height(wake_row, 2) : 0,
                                             day_shown ? ink_height(small) : 0, divider ? 1 : 0};
    for (int i = 0; i < count; ++i) heights[3 + i] = ink_height(text_font);
    int gap = level >= 2 ? 5 : 7;
    int y = stack_top(diameter, heights, ARRAY_COUNT(heights), gap);
    show(agenda_ui.wake_icon, false);
    show(agenda_ui.wake_time, false);
    if (wake_shown) {
        layout_row(wake_row, 2, diameter, y, heights[0]);
        y += heights[0] + gap;
    }
    show(agenda_ui.wake_day, day_shown);
    if (day_shown) {
        int width = text_width(day, small);
        set_label(agenda_ui.wake_day, day, small, COLOR_MUTED);
        place_on_row(agenda_ui.wake_day, small, diameter / 2 - width / 2, width + 1, y, heights[1]);
        y += heights[1] + gap;
    }
    show(agenda_ui.divider, divider);
    if (divider) {
        int rule = circle_half_width(diameter, y, 1);
        set_area(agenda_ui.divider, diameter / 2 - rule / 2, y, rule, 1);
        y += 1 + gap;
    }
    for (int i = 0; i < PROJECTOR_MAX_EVENTS; ++i) {
        show(agenda_ui.events[i], i < count);
        if (i >= count) continue;
        place_marquee(agenda_ui.events[i], lines[i], text_font, colors[i], diameter, y, heights[3 + i]);
        y += heights[3 + i] + gap;
    }
}

/* ---- Thermostat page: a dial of ticks around the set and current temperature. ---- */

#define DIAL_TICKS 41
#define DIAL_START 2.35619449f /* 135 degrees: lower left, clockwise through the top. */
#define DIAL_SWEEP 4.71238898f /* 270 degrees, leaving a gap at the bottom. */
#define DIAL_SAMPLES 3         /* Per axis: 9 samples per pixel smooth the ticks. */
#define COLOR_HEAT 0xff8a3d
#define COLOR_DIAL 0x4a5466

/* Everything the dial drawing depends on: redrawn only when it changes. */
typedef struct {
    int diameter;
    int lit;          /* Ticks 0..lit-1 are in the accent colour; 0 for none. */
    int target;       /* Tick of the set temperature, or -1. */
    int current;      /* Tick of the room temperature, or -1. */
    uint32_t accent;
    bool active;      /* Heating or cooling right now: full accent, otherwise dimmed. */
    uint32_t tint;    /* The palette the dial was painted with. */
    uint8_t palette;
} dial_t;

static uint32_t scale_color(uint32_t color, float factor)
{
    uint32_t r = (uint32_t)(((color >> 16) & 0xff) * factor), g = (uint32_t)(((color >> 8) & 0xff) * factor);
    uint32_t b = (uint32_t)((color & 0xff) * factor);
    return r << 16 | g << 8 | b;
}

/* Colour of one sample at (x, y) from the centre, or 0 (the black background). */
static uint32_t dial_sample(const dial_t *d, float x, float y, float outer, float tick_length)
{
    float radius = sqrtf(x * x + y * y);
    if (radius > outer || radius < outer - tick_length - 4) return 0;
    float angle = atan2f(y, x) - DIAL_START;
    while (angle < 0) angle += 6.28318531f;
    float step = DIAL_SWEEP / (DIAL_TICKS - 1);
    int tick = (int)(angle / step + 0.5f);
    if (tick >= DIAL_TICKS) return 0;
    /* The set temperature is a long accent tick, the room temperature a white one. */
    bool target = tick == d->target, current = !target && tick == d->current;
    float half_width = target ? 1.6f : current ? 1.3f : 0.9f;
    float inner = outer - tick_length - (target ? 4 : current ? 2 : 0);
    if (radius < inner || fabsf(sinf(angle - tick * step)) * radius > half_width) return 0;
    if (target) return d->active ? d->accent : scale_color(d->accent, 0.8f);
    if (current) return COLOR_TEXT;
    if (tick >= d->lit) return COLOR_DIAL;
    /* The lit ticks brighten towards the set temperature. */
    float factor = 0.45f + 0.55f * (tick + 1) / (d->lit > 0 ? d->lit : 1);
    return scale_color(d->accent, d->active ? factor : factor * 0.6f);
}

static void draw_dial(const dial_t *d)
{
    static dial_t drawn;
    if (canvas_owner == climate_ui.dial && !memcmp(&drawn, d, sizeof(drawn))) return;
    canvas_owner = climate_ui.dial;
    drawn = *d;
    int size = d->diameter;
    lv_canvas_set_buffer(climate_ui.dial, lv_draw_buf_align(canvas_buffer, LV_COLOR_FORMAT_RGB565),
                         size, size, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(climate_ui.dial, lv_color_black(), LV_OPA_COVER);
    float outer = size / 2.0f - 2, tick_length = size / 18.0f, centre = size / 2.0f;
    float reach = outer - tick_length - 5;
    for (int py = 0; py < size; ++py)
        for (int px = 0; px < size; ++px) {
            float x = px + 0.5f - centre, y = py + 0.5f - centre;
            float distance = sqrtf(x * x + y * y);
            if (distance > outer + 1 || distance < reach) continue;
            uint32_t r = 0, g = 0, b = 0;
            for (int sy = 0; sy < DIAL_SAMPLES; ++sy)
                for (int sx = 0; sx < DIAL_SAMPLES; ++sx) {
                    uint32_t color = dial_sample(d, px + (sx + 0.5f) / DIAL_SAMPLES - centre,
                                                 py + (sy + 0.5f) / DIAL_SAMPLES - centre, outer, tick_length);
                    r += color >> 16;
                    g += (color >> 8) & 0xff;
                    b += color & 0xff;
                }
            const int samples = DIAL_SAMPLES * DIAL_SAMPLES;
            if (r || g || b)
                lv_canvas_set_px(climate_ui.dial, px, py,
                                 lv_color_hex(paint((r / samples) << 16 | (g / samples) << 8 | b / samples)),
                                 LV_OPA_COVER);
        }
}

typedef struct {
    const char *icon, *label;
    uint32_t color;
    bool active;
} climate_status_t;

/* What the thermostat is doing: HA's hvac_action when reported, otherwise its mode. */
static climate_status_t climate_status(const projector_thermostat_t *t, uint32_t accent)
{
    const char *source = t->cooling ? BED_ICON_AIR_CONDITIONER : BED_ICON_RADIATOR;
    if (!strcmp(t->mode, "off")) return (climate_status_t){BED_ICON_POWER, "Spento", COLOR_MUTED, false};
    static const struct {
        const char *action, *icon, *label;
        uint32_t color;
        bool active;
    } actions[] = {
        {"heating", BED_ICON_FIRE, "Riscalda", COLOR_HEAT, true},
        {"preheating", BED_ICON_FIRE, "Preriscalda", COLOR_HEAT, true},
        {"cooling", BED_ICON_SNOWFLAKE, "Raffresca", COLOR_COOL, true},
        {"defrosting", BED_ICON_SNOWFLAKE, "Sbrinamento", COLOR_COOL, true},
        {"drying", BED_ICON_WATER_PERCENT, "Deumidifica", COLOR_COOL, true},
        {"fan", BED_ICON_AIR_CONDITIONER, "Ventilazione", COLOR_COOL, true},
    };
    for (int i = 0; i < ARRAY_COUNT(actions); ++i)
        if (!strcmp(t->action, actions[i].action))
            return (climate_status_t){actions[i].icon, actions[i].label, actions[i].color, actions[i].active};
    if (t->action[0]) return (climate_status_t){source, "In pausa", COLOR_MUTED, false}; /* idle, off. */
    static const struct {
        const char *mode, *label;
    } modes[] = {
        {"heat", "Riscaldamento"}, {"cool", "Raffrescamento"}, {"heat_cool", "Automatico"},
        {"auto", "Automatico"}, {"dry", "Deumidifica"}, {"fan_only", "Ventilazione"},
    };
    for (int i = 0; i < ARRAY_COUNT(modes); ++i)
        if (!strcmp(t->mode, modes[i].mode)) return (climate_status_t){source, modes[i].label, accent, false};
    return (climate_status_t){source, "?", COLOR_DIM, false};
}

static void render_climate(const projector_snapshot_t *s, const projector_page_t *page, int diameter)
{
    int level = projector_size_level(size_level(diameter), page->size);
    bool dial_shown = page->show[CLIMATE_DIAL] == SHOW_ON;
    const projector_thermostat_t *t = &s->ha.thermostat;
    bool known = s->ha_fresh && t->known;
    bool off = known && !strcmp(t->mode, "off");
    uint32_t accent = known && t->cooling ? COLOR_COOL : COLOR_HEAT;
    climate_status_t status = known ? climate_status(t, accent) :
        (climate_status_t){BED_ICON_HELP_CIRCLE_OUTLINE, "Clima ?", COLOR_DIM, false};
    bool target = known && !off && t->target != PROJECTOR_UNKNOWN_TENTHS;
    bool current = known && t->current != PROJECTOR_UNKNOWN_TENTHS;

    dial_t dial;
    memset(&dial, 0, sizeof(dial)); /* Compared with memcmp: padding must be zero too. */
    dial.diameter = diameter;
    dial.target = dial.current = -1;
    dial.accent = accent;
    dial.active = status.active;
    dial.palette = theme.palette;
    dial.tint = theme.tint;
    if (target) dial.lit = dial.target = projector_dial_tick(t->target, t->min, t->max, DIAL_TICKS);
    if (current) dial.current = projector_dial_tick(t->current, t->min, t->max, DIAL_TICKS);
    show(climate_ui.dial, dial_shown);
    if (dial_shown) {
        draw_dial(&dial);
        set_area(climate_ui.dial, 0, 0, diameter, diameter);
    }

    /* Inside the ring: status, the set temperature (the room's when off), the room temperature
     * or, when off, which appliance this is. Without the ring the text takes the whole circle. */
    int inset = dial_shown ? diameter / 18 + 6 : 4;
    int inner = diameter - 2 * inset;
    set_area(climate_ui.inner, inset, inset, inner, inner);
    const lv_font_t *text_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;
    const lv_font_t *status_icon = level >= 2 ? &bed_icons_12 : &bed_icons_14;
    char big[16] = "--\xC2\xB0", details[48] = "", value[8];
    uint32_t big_color = COLOR_DIM;
    if (target || (off && current)) {
        projector_format_tenths(target ? t->target : t->current, big, sizeof(big) - 2);
        strcat(big, "\xC2\xB0");
        big_color = target ? COLOR_TEXT : COLOR_MUTED;
    }
    if (off) snprintf(details, sizeof(details), "%s", t->cooling ? "Condizionatore" : "Caloriferi");
    else if (current && page->show[CLIMATE_CURRENT] == SHOW_ON) {
        projector_format_tenths(t->current, value, sizeof(value));
        snprintf(details, sizeof(details), "#%s %s# %s\xC2\xB0", hex(COLOR_MUTED),
                 BED_ICON_THERMOMETER, value);
    }
    char label[48];
    if (known && !off && t->preset[0] && page->show[CLIMATE_PRESET] == SHOW_ON)
        snprintf(label, sizeof(label), "%s \xC2\xB7 %s", status.label, t->preset);
    else snprintf(label, sizeof(label), "%s", status.label);
    row_item_t target_row[1] = {{climate_ui.target, big, bold_fonts, ARRAY_COUNT(bold_fonts), level,
                                 big_color, 0}};
    int row = ink_height(status_icon) > ink_height(text_font) ? ink_height(status_icon) : ink_height(text_font);
    /* The room temperature keeps a font with inline icons for its thermometer. */
    const lv_font_t *details_font = &bed_font_text_12;
    int status_mode = page->show[CLIMATE_STATUS];
    int heights[3] = {status_mode != LABELED_OFF ? row : 0, row_height(target_row, 1),
                      details[0] ? ink_height(details_font) : 0};
    int gap = level >= 2 ? 4 : 5;
    /* The page dots sit in the gap of the ring, below this inner circle. */
    int reserve = bottom_reserve;
    bottom_reserve = 0;
    int y = stack_top(inner, heights, 3, gap);
    bottom_reserve = reserve;
    /* Small circles drop the icon rather than scroll a short status. */
    if (status_mode == LABELED_ICON_LABEL &&
        text_width(status.icon, status_icon) + 4 + text_width(label, &bed_font_text_10) + 2 >
        circle_half_width(inner, y, row) * 2) status_mode = LABELED_LABEL;
    /* Alone, the label takes the status colour. */
    uint32_t label_color = status_mode == LABELED_LABEL ? status.color : known ? COLOR_TEXT : COLOR_DIM;
    layout_labeled(status_mode, climate_ui.status_icon, status.icon, status.color, status_icon,
                   climate_ui.status, label, label_color, text_font, inner, y, row);
    if (heights[0]) y += heights[0] + gap;
    layout_row(target_row, 1, inner, y, heights[1]);
    y += heights[1] + gap;
    show(climate_ui.current, details[0]);
    if (details[0])
        place_marquee(climate_ui.current, details, details_font, off ? COLOR_MUTED : COLOR_TEXT, inner, y,
                      heights[2]);

    /* Humidity in the gap at the bottom of the ring, above the page dots. */
    bool humidity = known && !off && t->humidity >= 0 && page->show[CLIMATE_HUMIDITY] == SHOW_ON;
    show(climate_ui.humidity, humidity);
    if (humidity) {
        const lv_font_t *small = &bed_font_text_12; /* The smallest font with inline icons. */
        char text[32];
        snprintf(text, sizeof(text), "#%s %s#%d%%", hex(COLOR_RAIN), BED_ICON_WATER_PERCENT,
                 t->humidity);
        int width = text_width(text, small), height = ink_height(small);
        int bottom = (bottom_reserve ? diameter - bottom_reserve : diameter - inset / 2) - 1;
        set_label(climate_ui.humidity, text, small, COLOR_MUTED);
        place_on_row(climate_ui.humidity, small, diameter / 2 - width / 2, width + 1, bottom - height, height);
    }
}

/* ---- Message from HA: icon above up to two lines of text. ---- */

/* The longest start of text ending before a space that fits width; false if none. */
static bool split_line(const char *text, const lv_font_t *font, int width, char *first, size_t size,
                       const char **rest)
{
    bool found = false;
    for (const char *space = strchr(text, ' '); space; space = strchr(space + 1, ' ')) {
        size_t length = (size_t)(space - text);
        if (!length || length >= size) continue;
        char candidate[64];
        memcpy(candidate, text, length);
        candidate[length] = 0;
        if (text_width(candidate, font) + 2 > width) break;
        strcpy(first, candidate);
        *rest = space + 1;
        found = true;
    }
    return found;
}

static void render_message(const projector_message_t *message, int diameter)
{
    int level = size_level(diameter);
    const lv_font_t *icon_font = level >= 2 ? &bed_icons_22 : &bed_icons_30;
    const lv_font_t *font = level >= 2 ? &bed_font_text_12 : &bed_font_text_14;
    /* The level sets the icon colour and, without a chosen icon, the icon itself. */
    static const struct {
        const char *icon;
        uint32_t color;
    } levels[MESSAGE_LEVEL_COUNT] = {
        [MESSAGE_INFO] = {BED_ICON_BELL, COLOR_LIGHT},
        [MESSAGE_WARNING] = {BED_ICON_ALERT, COLOR_OPEN},
        [MESSAGE_ALERT] = {BED_ICON_ALERT, COLOR_ALERT},
    };
    const char *icon = levels[message->level].icon;
    for (int i = 0; i < ARRAY_COUNT(message_icons); ++i)
        if (!strcmp(message->icon, message_icons[i].name)) icon = message_icons[i].icon;

    /* Two lines when the text is wider than the circle and has a space to break
     * at; the second line scrolls if it is still too long. */
    int gap = level >= 2 ? 6 : 8;
    int heights[3] = {ink_height(icon_font), ink_height(font), ink_height(font)};
    int y = stack_top(diameter, heights, 3, gap);
    int first_y = y + heights[0] + gap;
    char first[64];
    const char *rest = NULL;
    int available = circle_half_width(diameter, first_y, heights[1]) * 2;
    bool split = text_width(message->text, font) + 2 > available &&
        split_line(message->text, font, available, first, sizeof(first), &rest) && *rest;
    if (!split) {
        heights[2] = 0;
        y = stack_top(diameter, heights, 3, gap);
    }
    int icon_width = text_width(icon, icon_font);
    set_label(message_ui.icon, icon, icon_font, levels[message->level].color);
    place_on_row(message_ui.icon, icon_font, diameter / 2 - icon_width / 2, icon_width, y, heights[0]);
    y += heights[0] + gap;
    /* An alert is red throughout, so it reads as one even at a glance. */
    uint32_t color = message->level == MESSAGE_ALERT ? COLOR_ALERT_TEXT : COLOR_TEXT;
    place_marquee(message_ui.lines[0], split ? first : message->text, font, color, diameter, y, heights[1]);
    y += heights[1] + gap;
    show(message_ui.lines[1], split);
    if (split) place_marquee(message_ui.lines[1], rest, font, color, diameter, y, heights[2]);
}

/* ---- Good morning screen: greeting, time, today's forecast and first event, from the alarm on. ---- */

static void render_wake(const projector_snapshot_t *s, const struct tm *local, bool valid_time, int diameter)
{
    int level = size_level(diameter);
    const char *greeting = valid_time && local->tm_hour >= 18 ? "Buonasera" : "Buongiorno";
    row_item_t title_row[1] = {{wake_ui.title, greeting, text_fonts, ARRAY_COUNT(text_fonts), level >= 2 ? 2 : 1,
                                COLOR_WARM, 0}};
    char time_text[8] = "--:--";
    if (valid_time) snprintf(time_text, sizeof(time_text), "%02d:%02d", local->tm_hour, local->tm_min);
    row_item_t time_row[1] = {{wake_ui.time, time_text, clock_fonts, ARRAY_COUNT(clock_fonts), level + 2,
                               COLOR_TEXT, 0}};

    /* Today's forecast as on the clock page; unknown shows as a grey "?" rather than a guess. */
    const projector_day_t *day = forecast_day(s, local, valid_time, 0);
    weather_look_t look;
    bool weather = day && weather_look(day->condition, false, &look);
    char high[12], low[12];
    degrees(high, sizeof(high), weather && day->high_known, weather ? day->high : 0);
    degrees(low, sizeof(low), weather && day->low_known, weather ? day->low : 0);
    row_item_t weather_row[3] = {
        {wake_ui.weather_icon, weather ? look.icon : BED_ICON_HELP_CIRCLE_OUTLINE, icon_fonts,
         ARRAY_COUNT(icon_fonts), level + 2, weather ? look.color : COLOR_DIM, 0},
        {wake_ui.high, high, bold_fonts, ARRAY_COUNT(bold_fonts), level + 2, weather ? COLOR_WARM : COLOR_DIM, 5},
        {wake_ui.low, low, bold_fonts, ARRAY_COUNT(bold_fonts), level + 2, weather ? COLOR_COOL : COLOR_DIM, 6},
    };

    /* The first of today's events: HA sends them in order, from now to the end of tomorrow. */
    char event[64];
    uint32_t event_color = COLOR_TEXT;
    char today[11] = "";
    if (valid_time) projector_date_after(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday, 0, today);
    const projector_event_t *first = NULL;
    for (int i = 0; s->ha_fresh && s->ha.events_known && !first && i < s->ha.event_count; ++i)
        if (!strcmp(s->ha.events[i].date, today)) first = &s->ha.events[i];
    if (!s->ha_fresh || !s->ha.events_known) {
        snprintf(event, sizeof(event), "Eventi ?");
        event_color = COLOR_DIM;
    } else if (first) {
        snprintf(event, sizeof(event), "%s%s%s", first->time, first->time[0] ? " " : "", first->title);
    } else {
        snprintf(event, sizeof(event), "Nessun evento");
        event_color = COLOR_MUTED;
    }
    const lv_font_t *event_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;

    int heights[4] = {row_height(title_row, 1), row_height(time_row, 1), row_height(weather_row, 3),
                      ink_height(event_font)};
    int gap = level >= 2 ? 4 : 6;
    int y = stack_top(diameter, heights, 4, gap);
    layout_row(title_row, 1, diameter, y, heights[0]);
    y += heights[0] + gap;
    layout_row(time_row, 1, diameter, y, heights[1]);
    y += heights[1] + gap;
    layout_row(weather_row, 3, diameter, y, heights[2]);
    y += heights[2] + gap;
    place_marquee(wake_ui.event, event, event_font, event_color, diameter, y, heights[3]);
}

/* ---- Page indicator: one square per page at the bottom of the circle. ---- */

static int dots_top(int diameter)
{
    return diameter - DOT_SIZE - (diameter >= 104 ? 7 : 5);
}

static void render_dots(const projector_snapshot_t *s, bool visible, int diameter)
{
    int count = s->page_count;
    int left = diameter / 2 - (count * DOT_SIZE + (count - 1) * DOT_SPACING) / 2;
    for (int i = 0; i < PROJECTOR_MAX_PAGES; ++i) {
        show(page_dots[i], visible && i < count);
        if (!visible || i >= count) continue;
        set_background(page_dots[i], i == s->page_index ? COLOR_TEXT : COLOR_LINE);
        set_area(page_dots[i], left + i * (DOT_SIZE + DOT_SPACING), dots_top(diameter), DOT_SIZE, DOT_SIZE);
    }
}

/* ---- Generic page of widget lines, also used for the access point screen. ---- */

/* A list line: label + value is the full line, value alone the short one; icon may be NULL. */
typedef struct {
    char label[24];
    char value[64];
    const char *icon;
    uint32_t icon_color;
} widget_text_t;

static void text_widget(const projector_widget_t *widget, const projector_snapshot_t *s, const struct tm *local,
                        bool valid_time, widget_text_t *out)
{
    memset(out, 0, sizeof(*out));
    char *label = out->label, *value = out->value;
    size_t label_size = sizeof(out->label), size = sizeof(out->value);
    switch (widget->type) {
    case WIDGET_OPENINGS: {
        bool known = s->ha_fresh && s->ha.openings_known;
        snprintf(label, label_size, "Porte/finestre: ");
        if (known) snprintf(value, size, "%u", s->ha.openings);
        else snprintf(value, size, "?");
        out->icon = known && !s->ha.openings ? BED_ICON_DOOR_CLOSED : BED_ICON_DOOR_OPEN;
        out->icon_color = !known ? COLOR_DIM : s->ha.openings ? COLOR_OPEN : COLOR_OK;
        break;
    }
    case WIDGET_ALARM: {
        const alarm_mode_t *alarm = alarm_mode(s);
        snprintf(label, label_size, "Allarme: ");
        snprintf(value, size, "%s", alarm->label);
        out->icon = alarm->icon;
        out->icon_color = alarm->color;
        break;
    }
    case WIDGET_WEATHER: {
        weather_look_t look;
        if (weather_look(s->ha.weather, false, &look)) {
            snprintf(value, size, "%s %s", look.label, s->ha.temperature);
            out->icon = look.icon;
            out->icon_color = look.color;
        } else {
            snprintf(value, size, "Meteo ?");
            out->icon = BED_ICON_HELP_CIRCLE_OUTLINE;
            out->icon_color = COLOR_DIM;
        }
        break;
    }
    case WIDGET_DATE:
        if (valid_time) snprintf(value, size, "%s %d %s", weekdays[local->tm_wday], local->tm_mday,
                                 months[local->tm_mon]);
        else snprintf(value, size, "In attesa dell'ora");
        out->icon = BED_ICON_CALENDAR;
        out->icon_color = COLOR_MUTED;
        break;
    case WIDGET_TEXT:
        snprintf(value, size, "%s", widget->text);
        break;
    case WIDGET_INDOOR:
        snprintf(label, label_size, "Camera ");
        snprintf(value, size, "%s", s->ha_fresh && s->ha.indoor[0] ? s->ha.indoor : "?");
        out->icon = BED_ICON_BED;
        out->icon_color = 0xc8a8ff;
        break;
    case WIDGET_OUTDOOR:
        snprintf(label, label_size, "Esterno ");
        snprintf(value, size, "%s", s->ha_fresh && s->ha.outdoor[0] ? s->ha.outdoor : "?");
        out->icon = BED_ICON_PINE_TREE;
        out->icon_color = COLOR_OK;
        break;
    case WIDGET_LIGHTS: {
        bool known = s->ha_fresh && s->ha.lights_known;
        snprintf(label, label_size, "Luci accese: ");
        if (known) snprintf(value, size, "%u", s->ha.lights);
        else snprintf(value, size, "?");
        out->icon = known && s->ha.lights ? BED_ICON_LIGHTBULB_ON : BED_ICON_LIGHTBULB_OUTLINE;
        out->icon_color = !known ? COLOR_DIM : s->ha.lights ? COLOR_LIGHT : COLOR_MUTED;
        break;
    }
    case WIDGET_SUN: {
        const projector_day_t *today = forecast_day(s, local, valid_time, 0);
        out->icon = BED_ICON_SUNRISE;
        out->icon_color = COLOR_SUNRISE;
        if (today && today->sunrise[0] && today->sunset[0]) {
            snprintf(label, label_size, "Alba ");
            snprintf(value, size, "%s \xC2\xB7 Tramonto %s", today->sunrise, today->sunset);
        } else {
            snprintf(label, label_size, "Alba e tramonto ");
            snprintf(value, size, "?");
        }
        break;
    }
    case WIDGET_WAKE: {
        char day[12];
        bool known = s->ha_fresh && s->ha.wake_known;
        out->icon = known && s->ha.wake_date[0] ? BED_ICON_ALARM : BED_ICON_ALARM_OFF;
        out->icon_color = !known ? COLOR_DIM : s->ha.wake_date[0] ? COLOR_WARM : COLOR_MUTED;
        if (!known) {
            snprintf(label, label_size, "Sveglia ");
            snprintf(value, size, "?");
        } else if (!s->ha.wake_date[0]) snprintf(value, size, "Nessuna sveglia");
        else {
            day_name(s->ha.wake_date, local, valid_time, day, sizeof(day));
            /* Lower-case "oggi"/"domani" read better mid-sentence. */
            if (!strcmp(day, "Oggi") || !strcmp(day, "Domani")) day[0] += 'a' - 'A';
            snprintf(label, label_size, "Sveglia ");
            snprintf(value, size, "%s %s", day, s->ha.wake_time);
        }
        break;
    }
    case WIDGET_EVENT: {
        char when[20];
        out->icon = BED_ICON_CALENDAR;
        out->icon_color = COLOR_MUTED;
        if (!s->ha_fresh || !s->ha.events_known) snprintf(value, size, "Eventi ?");
        else if (!s->ha.event_count) snprintf(value, size, "Nessun evento");
        else {
            event_when(&s->ha.events[0], local, valid_time, when, sizeof(when));
            snprintf(value, size, "%s %s", when, s->ha.events[0].title);
        }
        break;
    }
    case WIDGET_THERMOSTAT: {
        const projector_thermostat_t *t = &s->ha.thermostat;
        char target[8], current[8];
        bool known = s->ha_fresh && t->known;
        snprintf(label, label_size, "Termostato ");
        out->icon = known && t->cooling ? BED_ICON_AIR_CONDITIONER : BED_ICON_RADIATOR;
        out->icon_color = !known ? COLOR_DIM : t->cooling ? COLOR_COOL : COLOR_HEAT;
        if (!known) snprintf(value, size, "?");
        else if (!strcmp(t->mode, "off")) snprintf(value, size, "spento");
        else {
            if (t->target != PROJECTOR_UNKNOWN_TENTHS) projector_format_tenths(t->target, target, sizeof(target));
            else snprintf(target, sizeof(target), "?");
            if (t->current != PROJECTOR_UNKNOWN_TENTHS) projector_format_tenths(t->current, current, sizeof(current));
            else snprintf(current, sizeof(current), "?");
            snprintf(value, size, "%s\xC2\xB0, camera %s\xC2\xB0", target, current);
        }
        break;
    }
    case WIDGET_MOON:
        if (valid_time) snprintf(value, size, "%s", moon_phases[projector_moon_phase(local->tm_year + 1900,
                                 local->tm_mon + 1, local->tm_mday)]);
        else snprintf(value, size, "Luna ?");
        out->icon = BED_ICON_WEATHER_NIGHT;
        out->icon_color = 0x9aa8ff;
        break;
    case WIDGET_RAIN: {
        const projector_day_t *today = forecast_day(s, local, valid_time, 0);
        snprintf(label, label_size, "Pioggia ");
        if (today && today->precipitation >= 0) snprintf(value, size, "%d%%", today->precipitation);
        else snprintf(value, size, "?");
        out->icon = BED_ICON_UMBRELLA;
        out->icon_color = today && today->precipitation >= 0 ? COLOR_RAIN : COLOR_DIM;
        break;
    }
    case WIDGET_HUMIDITY: {
        bool known = s->ha_fresh && s->ha.humidity >= 0;
        snprintf(label, label_size, "Umidit\xC3\xA0 ");
        if (known) snprintf(value, size, "%d%%", s->ha.humidity);
        else snprintf(value, size, "?");
        out->icon = BED_ICON_WATER_PERCENT;
        out->icon_color = known ? COLOR_RAIN : COLOR_DIM;
        break;
    }
    case WIDGET_WIND: {
        bool known = s->ha_fresh && s->ha.wind[0];
        snprintf(label, label_size, "Vento ");
        snprintf(value, size, "%s", known ? s->ha.wind : "?");
        out->icon = BED_ICON_WEATHER_WINDY;
        out->icon_color = known ? COLOR_WIND : COLOR_DIM;
        break;
    }
    case WIDGET_COUNTDOWN: {
        int year, month, day;
        const char *name = "";
        bool parsed = projector_parse_countdown(widget->text, &year, &month, &day, &name);
        if (name[0]) snprintf(label, label_size, "%.20s: ", name);
        out->icon = BED_ICON_CALENDAR;
        out->icon_color = COLOR_WARM;
        if (!parsed || !valid_time) {
            snprintf(value, size, "?");
            break;
        }
        long days = projector_days_between(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday,
                                           year, month, day);
        if (days == 0) snprintf(value, size, "oggi");
        else if (days == 1) snprintf(value, size, "domani");
        else if (days > 1) snprintf(value, size, "%ld giorni", days);
        else if (days == -1) snprintf(value, size, "ieri");
        else snprintf(value, size, "%ld giorni fa", -days);
        break;
    }
    default: { /* WIDGET_ENTITY1-4 */
        int index = widget->type - WIDGET_ENTITY1;
        snprintf(value, size, "%s", s->ha_fresh && s->ha.extras[index][0] ? s->ha.extras[index] : "Dato HA ?");
        break;
    }
    }
}

/* A header (NULL for none) over up to four lines, each with an optional icon before it. */
static void render_lines(const char *header, const char *const *texts, const char *const *icons,
                         const uint32_t *icon_colors, int count, int level, int diameter)
{
    const lv_font_t *font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;
    const lv_font_t *icon_font = level >= 2 ? &bed_icons_12 : &bed_icons_14;
    int heights[1 + PROJECTOR_MAX_WIDGETS] = {header ? ink_height(&bed_font_text_10) : 0};
    for (int i = 0; i < count; ++i)
        heights[i + 1] = icons && icons[i] && ink_height(icon_font) > ink_height(font) ?
            ink_height(icon_font) : ink_height(font);
    /* Four lines need tighter gaps to stay inside the circle. */
    int gap = count > 3 ? (diameter >= 104 ? 7 : 5) : diameter >= 104 ? 12 : 9;
    int y = stack_top(diameter, heights, 1 + PROJECTOR_MAX_WIDGETS, gap);
    show(list_ui.header, header);
    if (header) {
        int width = text_width(header, &bed_font_text_10);
        set_label(list_ui.header, header, &bed_font_text_10, COLOR_MUTED);
        place_on_row(list_ui.header, &bed_font_text_10, diameter / 2 - width / 2, width, y, heights[0]);
        y += heights[0] + gap;
    }
    for (int i = 0; i < PROJECTOR_MAX_WIDGETS; ++i) {
        bool icon = i < count && icons && icons[i];
        show(list_ui.icons[i], icon);
        show(list_ui.lines[i], i < count);
        if (i >= count) continue;
        if (icon)
            layout_icon_text(list_ui.icons[i], icons[i], icon_colors[i], icon_font, list_ui.lines[i], texts[i],
                             COLOR_TEXT, font, diameter, y, heights[i + 1]);
        else place_marquee(list_ui.lines[i], texts[i], font, COLOR_TEXT, diameter, y, heights[i + 1]);
        y += heights[i + 1] + gap;
    }
}

static void render_list(const projector_snapshot_t *s, const projector_page_t *page, const struct tm *local,
                        bool valid_time, int diameter)
{
    static widget_text_t widgets[PROJECTOR_MAX_WIDGETS];
    char values[PROJECTOR_MAX_WIDGETS][96];
    const char *texts[PROJECTOR_MAX_WIDGETS], *icons[PROJECTOR_MAX_WIDGETS];
    uint32_t colors[PROJECTOR_MAX_WIDGETS];
    for (int i = 0; i < page->widget_count; ++i) {
        const projector_widget_t *widget = &page->widgets[i];
        text_widget(widget, s, local, valid_time, &widgets[i]);
        bool with_label = widget->style == STYLE_LABEL_VALUE;
        /* Label and value fit: 23 + 63 bytes. */
        snprintf(values[i], sizeof(values[i]), "%.23s%.63s", with_label ? widgets[i].label : "",
                 widgets[i].value);
        texts[i] = values[i];
        icons[i] = widget->style == STYLE_ICON_VALUE ? widgets[i].icon : NULL;
        colors[i] = widgets[i].icon_color;
    }
    char header[17];
    snprintf(header, sizeof(header), "%s", page->name);
    for (char *c = header; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 'a' - 'A';
    render_lines(page->show[LIST_HEADER] == SHOW_ON ? header : NULL, texts, icons, colors, page->widget_count,
                 projector_size_level(size_level(diameter), page->size), diameter);
}

/* ---- Watch face: hands or the digital time, with up to four complications. ---- */

#define FACE_SAMPLES 3 /* Per axis, as on the thermostat dial. */
#define COLOR_MINUTE_TICK 0x5a6472

/* A complication as the face shows it: a short value, with an icon unless the style is
 * value only; long values are cut with an ellipsis rather than scrolled. */
typedef struct {
    bool used;
    const char *icon;
    uint32_t icon_color, color;
    char value[48];
} complication_t;

/* Everything the analog face drawing depends on: redrawn only when it changes. */
typedef struct {
    int diameter, level;
    bool hands;       /* Analog; the digital face draws only its marks here. */
    int minutes;      /* Minutes since midnight, or -1 before the clock is set. */
    uint8_t marks;    /* MARKS_* */
    bool dots;        /* Leave room for the page dots at six o'clock. */
    uint8_t palette;
    uint32_t tint;
    complication_t slots[SLOT_COUNT];
} face_t;

/* Cut text with an ellipsis, on a character boundary, until it fits width. */
static void fit_text(char *text, size_t size, const lv_font_t *font, int width)
{
    if (text_width(text, font) <= width) return;
    size_t length = strlen(text);
    while (length > 0) {
        do --length; while (length > 0 && ((unsigned char)text[length] & 0xc0) == 0x80);
        while (length > 0 && text[length - 1] == ' ') --length;
        if (length + 4 > size) continue;
        memcpy(text + length, "\xE2\x80\xA6", 4); /* "…" and the terminator. */
        if (text_width(text, font) <= width) return;
    }
    snprintf(text, size, "\xE2\x80\xA6");
}

/* The short form of a widget for a face slot: the figure that matters, not the sentence. */
static void face_complication(const projector_widget_t *widget, const projector_snapshot_t *s,
                              const struct tm *local, bool valid_time, complication_t *out)
{
    widget_text_t text;
    text_widget(widget, s, local, valid_time, &text);
    *out = (complication_t){.used = true, .icon_color = text.icon_color, .color = COLOR_TEXT};
    out->icon = widget->style == STYLE_VALUE ? NULL : text.icon;
    snprintf(out->value, sizeof(out->value), "%.47s", text.value);
    if (widget->style == STYLE_LABEL_VALUE && !text.icon)
        snprintf(out->value, sizeof(out->value), "%.23s%.24s", text.label, text.value);
    switch (widget->type) {
    case WIDGET_WEATHER:
        if (s->ha_fresh && s->ha.temperature[0])
            snprintf(out->value, sizeof(out->value), "%s", s->ha.temperature);
        break;
    case WIDGET_DATE:
        if (valid_time)
            snprintf(out->value, sizeof(out->value), "%s %d", weekdays[local->tm_wday], local->tm_mday);
        break;
    case WIDGET_WAKE:
        if (s->ha_fresh && s->ha.wake_known && s->ha.wake_date[0])
            snprintf(out->value, sizeof(out->value), "%s", s->ha.wake_time);
        else if (s->ha_fresh && s->ha.wake_known) snprintf(out->value, sizeof(out->value), "--:--");
        break;
    case WIDGET_SUN: {
        /* The next of today's sunrise and sunset, then tomorrow's sunrise. */
        char now[6] = "";
        if (valid_time) snprintf(now, sizeof(now), "%02d:%02d", local->tm_hour, local->tm_min);
        const projector_day_t *today = forecast_day(s, local, valid_time, 0);
        const projector_day_t *tomorrow = forecast_day(s, local, valid_time, 1);
        const char *time = NULL;
        if (today && today->sunrise[0] && strcmp(now, today->sunrise) < 0) time = today->sunrise;
        else if (today && today->sunset[0] && strcmp(now, today->sunset) < 0) {
            time = today->sunset;
            out->icon = widget->style == STYLE_VALUE ? NULL : BED_ICON_SUNSET;
            out->icon_color = COLOR_SUNSET;
        } else if (tomorrow && tomorrow->sunrise[0]) time = tomorrow->sunrise;
        snprintf(out->value, sizeof(out->value), "%s", time ? time : "?");
        break;
    }
    case WIDGET_THERMOSTAT: {
        const projector_thermostat_t *t = &s->ha.thermostat;
        if (s->ha_fresh && t->known && strcmp(t->mode, "off") && t->target != PROJECTOR_UNKNOWN_TENTHS) {
            projector_format_tenths(t->target, out->value, sizeof(out->value) - 2);
            strcat(out->value, "\xC2\xB0");
        }
        break;
    }
    case WIDGET_OPENINGS:
    case WIDGET_LIGHTS:
        if (widget->style != STYLE_VALUE) out->color = text.icon_color;
        break;
    }
    if (!strcmp(out->value, "?")) out->color = COLOR_DIM;
}

/* Colour of one sample of the marks around the rim at (x, y) from the centre, or 0. */
static uint32_t face_mark(const face_t *f, float x, float y, float outer, float length)
{
    float radius = sqrtf(x * x + y * y);
    if (radius > outer || radius < outer - length) return 0;
    /* Clockwise from twelve o'clock, in minutes. */
    float angle = atan2f(x, -y);
    if (angle < 0) angle += 6.28318531f;
    int minute = (int)(angle / 6.28318531f * 60 + 0.5f) % 60;
    float offset = fabsf(sinf(angle - minute * 6.28318531f / 60)) * radius;
    bool hour = minute % 5 == 0, quarter = minute % 15 == 0;
    if (f->dots && minute >= 28 && minute <= 32) return 0;
    if (f->marks == MARKS_NUMBERS && quarter && !f->slots[minute / 15].used) return 0;
    if (hour) {
        float width = quarter ? 1.3f : 0.9f;
        return offset <= width ? (quarter ? COLOR_TEXT : COLOR_MUTED) : 0;
    }
    return offset <= 0.5f && radius >= outer - length * 0.45f ? COLOR_MINUTE_TICK : 0;
}

/* Coverage, 0-1, of a capsule from the centre back by tail and out to (dx, dy), half_width wide. */
static float hand_cover(float x, float y, float dx, float dy, float tail, float half_width)
{
    float length = sqrtf(dx * dx + dy * dy);
    float ux = dx / length, uy = dy / length;
    /* Most pixels are far from the hand: one test at the centre skips the samples. */
    float along = x * ux + y * uy;
    along = along < -tail ? -tail : along > length ? length : along;
    float cx = x - along * ux, cy = y - along * uy;
    if (cx * cx + cy * cy > (half_width + 1) * (half_width + 1)) return 0;
    int inside = 0;
    for (int sy = 0; sy < FACE_SAMPLES; ++sy)
        for (int sx = 0; sx < FACE_SAMPLES; ++sx) {
            float px = x + (sx + 0.5f) / FACE_SAMPLES - 0.5f, py = y + (sy + 0.5f) / FACE_SAMPLES - 0.5f;
            float along = px * ux + py * uy;
            if (along < -tail) along = -tail;
            if (along > length) along = length;
            float ex = px - along * ux, ey = py - along * uy;
            if (ex * ex + ey * ey <= half_width * half_width) ++inside;
        }
    return (float)inside / (FACE_SAMPLES * FACE_SAMPLES);
}

static uint32_t mix(uint32_t from, uint32_t to, float amount)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        float a = (from >> shift) & 0xff, b = (to >> shift) & 0xff;
        out |= (uint32_t)(a + (b - a) * amount + 0.5f) << shift;
    }
    return out;
}

/* Draw text centred on (cx, row y..y+height) in the canvas layer. text must live until the
 * layer is finished. */
static void face_text(lv_layer_t *layer, const char *text, const lv_font_t *font, uint32_t color, int cx,
                      int y, int height)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = text;
    dsc.font = font;
    dsc.color = lv_color_hex(paint(color));
    dsc.align = LV_TEXT_ALIGN_CENTER;
    int width = text_width(text, font) + 2;
    ink_t ink = ink_of(font);
    int top = y + height / 2 - (ink.top + ink.bottom) / 2;
    lv_area_t area = {cx - width / 2, top, cx - width / 2 + width - 1, top + lv_font_get_line_height(font) - 1};
    lv_draw_label(layer, &dsc, &area);
}

static void draw_face(const face_t *f)
{
    static face_t drawn;
    if (canvas_owner == face_ui.dial && !memcmp(&drawn, f, sizeof(drawn))) return;
    canvas_owner = face_ui.dial;
    drawn = *f;
    int size = f->diameter;
    float centre = size / 2.0f, outer = size / 2.0f - 2, mark_length = size / 16.0f;
    lv_canvas_set_buffer(face_ui.dial, lv_draw_buf_align(canvas_buffer, LV_COLOR_FORMAT_RGB565),
                         size, size, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(face_ui.dial, lv_color_black(), LV_OPA_COVER);

    /* Marks around the rim. */
    if (f->marks != MARKS_OFF)
        for (int py = 0; py < size; ++py)
            for (int px = 0; px < size; ++px) {
                float x = px + 0.5f - centre, y = py + 0.5f - centre;
                float distance = sqrtf(x * x + y * y);
                if (distance > outer + 1 || distance < outer - mark_length - 1) continue;
                uint32_t r = 0, g = 0, b = 0;
                for (int sy = 0; sy < FACE_SAMPLES; ++sy)
                    for (int sx = 0; sx < FACE_SAMPLES; ++sx) {
                        uint32_t color = face_mark(f, px + (sx + 0.5f) / FACE_SAMPLES - centre,
                                                   py + (sy + 0.5f) / FACE_SAMPLES - centre, outer, mark_length);
                        r += color >> 16;
                        g += (color >> 8) & 0xff;
                        b += color & 0xff;
                    }
                const int samples = FACE_SAMPLES * FACE_SAMPLES;
                if (r || g || b)
                    lv_canvas_set_px(face_ui.dial, px, py,
                                     lv_color_hex(paint((r / samples) << 16 | (g / samples) << 8 | b / samples)),
                                     LV_OPA_COVER);
            }

    /* Numbers and complications, under the hands. */
    lv_layer_t layer;
    lv_canvas_init_layer(face_ui.dial, &layer);
    const lv_font_t *number_font = bold_fonts[f->level >= 2 ? 3 : 2];
    if (f->marks == MARKS_NUMBERS) {
        /* In slot order; a complication takes the place of the number beside it. */
        static const char *const numbers[SLOT_COUNT] = {[SLOT_TOP] = "12", [SLOT_RIGHT] = "3",
                                                        [SLOT_BOTTOM] = "6", [SLOT_LEFT] = "9"};
        float reach = outer - ink_height(number_font) / 2.0f - 2;
        for (int i = 0; i < SLOT_COUNT; ++i) {
            if ((i == SLOT_BOTTOM && f->dots) || f->slots[i].used) continue;
            int dx = i == 1 ? 1 : i == 3 ? -1 : 0, dy = i == 0 ? -1 : i == 2 ? 1 : 0;
            int height = ink_height(number_font);
            int extra = dx ? text_width(numbers[i], number_font) / 2 - height / 2 : 0;
            face_text(&layer, numbers[i], number_font, COLOR_TEXT, (int)(centre + dx * (reach - extra)),
                      (int)(centre + dy * reach) - height / 2, height);
        }
    }
    const lv_font_t *icon_font = f->level >= 2 ? &bed_icons_12 : &bed_icons_14;
    const lv_font_t *value_font = f->level >= 2 ? &bed_font_text_10 : &bed_font_text_12;
    int icon_height = ink_height(icon_font), value_height = ink_height(value_font), gap = 2;
    float reach = size * (f->marks == MARKS_NUMBERS ? 0.23f : 0.25f);
    for (int slot = 0; slot < SLOT_COUNT; ++slot) {
        const complication_t *c = &f->slots[slot];
        if (!c->used) continue;
        int cx = (int)(centre + (slot == SLOT_RIGHT ? reach : slot == SLOT_LEFT ? -reach : 0));
        int cy = (int)(centre + (slot == SLOT_BOTTOM ? reach : slot == SLOT_TOP ? -reach : 0));
        int height = (c->icon ? icon_height + gap : 0) + value_height;
        int y = cy - height / 2;
        if (c->icon) {
            face_text(&layer, c->icon, icon_font, c->icon_color, cx, y, icon_height);
            y += icon_height + gap;
        }
        face_text(&layer, c->value, value_font, c->color, cx, y, value_height);
    }
    if (f->hands && f->minutes < 0)
        face_text(&layer, "--:--", bold_fonts[2], COLOR_DIM, (int)centre,
                  (int)centre - ink_height(bold_fonts[2]) / 2, ink_height(bold_fonts[2]));
    lv_canvas_finish_layer(face_ui.dial, &layer);
    if (!f->hands || f->minutes < 0) return;

    /* Hands over everything, with a black rim that keeps them apart from the text below. */
    float hour_x, hour_y, minute_x, minute_y;
    projector_hand_end(f->minutes % 720, 720, size * 0.27f, &hour_x, &hour_y);
    projector_hand_end(f->minutes % 60, 60, size * 0.40f, &minute_x, &minute_y);
    float tail = size * 0.06f, hour_width = size / 56.0f + 0.4f, minute_width = size / 80.0f + 0.4f;
    int reach_px = (int)(size * 0.40f) + 3;
    for (int py = (int)centre - reach_px; py <= (int)centre + reach_px; ++py)
        for (int px = (int)centre - reach_px; px <= (int)centre + reach_px; ++px) {
            if (px < 0 || py < 0 || px >= size || py >= size) continue;
            float x = px + 0.5f - centre, y = py + 0.5f - centre;
            float rim = hand_cover(x, y, hour_x, hour_y, tail, hour_width + 1);
            float minute_rim = hand_cover(x, y, minute_x, minute_y, tail, minute_width + 1);
            if (minute_rim > rim) rim = minute_rim;
            if (rim <= 0) continue;
            lv_color32_t old = lv_canvas_get_px(face_ui.dial, px, py);
            uint32_t color = mix((uint32_t)old.red << 16 | (uint32_t)old.green << 8 | old.blue, 0, rim);
            color = mix(color, paint(COLOR_TEXT), hand_cover(x, y, hour_x, hour_y, tail, hour_width));
            color = mix(color, paint(COLOR_TEXT), hand_cover(x, y, minute_x, minute_y, tail, minute_width));
            float d = sqrtf(x * x + y * y), cap = hour_width + 1.2f;
            if (d <= cap + 0.5f) color = mix(color, paint(COLOR_WARM), d <= cap - 0.5f ? 1 : cap + 0.5f - d);
            if (d <= 1.0f) color = 0;
            lv_canvas_set_px(face_ui.dial, px, py, lv_color_hex(color), LV_OPA_COVER);
        }
}

/* Digital: a row per complication above and below the time, left and right side by side below it. */
static void render_face_digital(complication_t *slots, const char *time, int level, int diameter, int inset)
{
    int inner = diameter - 2 * inset;
    set_area(face_ui.inner, inset, inset, inner, inner);
    /* Rows of complications: the top one, left and right together, the bottom one. */
    row_item_t rows[3][4];
    int counts[3] = {0}, heights[4];
    static const int row_of[SLOT_COUNT] = {[SLOT_TOP] = 0, [SLOT_RIGHT] = 1, [SLOT_BOTTOM] = 2, [SLOT_LEFT] = 1};
    static const int order[SLOT_COUNT] = {SLOT_TOP, SLOT_LEFT, SLOT_RIGHT, SLOT_BOTTOM};
    for (int i = 0; i < SLOT_COUNT; ++i) {
        int slot = order[i], row = row_of[slot];
        const complication_t *c = &slots[slot];
        show(face_ui.icons[slot], false);
        show(face_ui.values[slot], false);
        if (!c->used) continue;
        int gap = counts[row] ? 10 : 0;
        if (c->icon) {
            rows[row][counts[row]++] = (row_item_t){face_ui.icons[slot], c->icon, icon_fonts,
                ARRAY_COUNT(icon_fonts), level >= 2 ? 4 : 3, c->icon_color, gap};
            gap = 3;
        }
        rows[row][counts[row]++] = (row_item_t){face_ui.values[slot], c->value, text_fonts,
            ARRAY_COUNT(text_fonts), level >= 2 ? 2 : 1, c->color, gap};
    }
    /* With complications above and below, a smaller time keeps their rows away from the narrow rim. */
    int crowded = (counts[0] > 0) + (counts[1] > 0) + (counts[2] > 0) >= 2;
    row_item_t time_row[1] = {{face_ui.time, time, clock_fonts, ARRAY_COUNT(clock_fonts), level + 1 + crowded,
                               COLOR_TEXT, 0}};
    heights[0] = counts[0] ? row_height(rows[0], counts[0]) : 0;
    heights[1] = row_height(time_row, 1);
    heights[2] = counts[1] ? row_height(rows[1], counts[1]) : 0;
    heights[3] = counts[2] ? row_height(rows[2], counts[2]) : 0;
    int gap = crowded ? (level >= 2 ? 3 : 4) : level >= 2 ? 5 : 7;
    int reserve = bottom_reserve;
    bottom_reserve = reserve > inset ? reserve - inset : 0;
    int y = stack_top(inner, heights, 4, gap);
    bottom_reserve = reserve;
    row_item_t *const all[4] = {rows[0], time_row, rows[1], rows[2]};
    int all_counts[4] = {counts[0], 1, counts[1], counts[2]};
    for (int i = 0; i < 4; ++i) {
        if (!heights[i]) continue;
        /* Widths at the smallest fonts layout_row may step down to. */
        int available = circle_half_width(inner, y, heights[i]) * 2, icons = 0, values = 0, used = 0;
        for (int j = 0; i != 1 && j < all_counts[i]; ++j) {
            const row_item_t *item = &all[i][j];
            int width = text_width(item->text, item_font(item, ARRAY_COUNT(icon_fonts)));
            used += item->gap + width;
            if (item->fonts == icon_fonts) icons += width + item->gap;
            else ++values;
        }
        /* Two complications side by side drop their icons before their values are cut. */
        if (values > 1 && icons && used > available) {
            int kept = 0;
            for (int j = 0; j < all_counts[i]; ++j)
                if (all[i][j].fonts == text_fonts) {
                    all[i][kept] = all[i][j];
                    all[i][kept].gap = kept ? 10 : 0;
                    ++kept;
                }
            all_counts[i] = kept;
        }
        /* Values share what the circle leaves of the row: only what still does not fit is cut. */
        for (int j = 0; i != 1 && j < all_counts[i]; ++j) {
            const row_item_t *item = &all[i][j];
            available -= item->gap;
            if (item->fonts == icon_fonts)
                available -= text_width(item->text, item_font(item, ARRAY_COUNT(icon_fonts)));
        }
        /* A value may use what the others leave: each of them takes at most an even share. */
        int share = values ? available / values : 0;
        for (int j = 0; i != 1 && values && j < all_counts[i]; ++j) {
            const row_item_t *item = &all[i][j];
            if (item->fonts != text_fonts) continue;
            int limit = available;
            for (int k = 0; k < all_counts[i]; ++k) {
                const row_item_t *other = &all[i][k];
                if (k == j || other->fonts != text_fonts) continue;
                int width = text_width(other->text, item_font(other, ARRAY_COUNT(text_fonts)));
                limit -= width < share ? width : share;
            }
            for (int slot = 0; slot < SLOT_COUNT; ++slot)
                if (item->text == slots[slot].value)
                    fit_text(slots[slot].value, sizeof(slots[slot].value),
                             item_font(item, ARRAY_COUNT(text_fonts)), limit);
        }
        layout_row(all[i], all_counts[i], inner, y, heights[i]);
        y += heights[i] + gap;
    }
}

static void render_face(const projector_snapshot_t *s, const projector_page_t *page, const struct tm *local,
                        bool valid_time, bool dots, int diameter)
{
    int level = projector_size_level(size_level(diameter), page->size);
    static face_t face;
    memset(&face, 0, sizeof(face)); /* Compared with memcmp: padding must be zero too. */
    face.diameter = diameter;
    face.level = level;
    face.hands = page->show[FACE_DIAL] == DIAL_ANALOG;
    face.minutes = valid_time ? local->tm_hour * 60 + local->tm_min : -1;
    face.marks = page->show[FACE_MARKS];
    face.dots = dots;
    face.palette = theme.palette;
    face.tint = theme.tint;
    for (int i = 0; i < page->widget_count; ++i)
        face_complication(&page->widgets[i], s, local, valid_time, &face.slots[page->widgets[i].slot]);
    bool analog = face.hands;
    /* Analog complications stay between the centre and the marks. */
    const lv_font_t *value_font = level >= 2 ? &bed_font_text_10 : &bed_font_text_12;
    for (int slot = 0; analog && slot < SLOT_COUNT; ++slot) {
        complication_t *c = &face.slots[slot];
        bool vertical = slot == SLOT_TOP || slot == SLOT_BOTTOM;
        if (c->used) fit_text(c->value, sizeof(c->value), value_font, diameter * (vertical ? 3 : 2) / 5);
    }
    show(face_ui.inner, !analog);
    show(face_ui.dial, analog || face.marks != MARKS_OFF);
    if (analog) {
        draw_face(&face);
        set_area(face_ui.dial, 0, 0, diameter, diameter);
        return;
    }
    /* Digital: the canvas holds only the marks; numbers would crowd the rows, so they show as ticks. */
    static face_t marks;
    memset(&marks, 0, sizeof(marks));
    marks.diameter = diameter;
    marks.level = level;
    marks.minutes = -1;
    marks.dots = dots;
    marks.palette = theme.palette;
    marks.tint = theme.tint;
    marks.marks = face.marks == MARKS_OFF ? MARKS_OFF : MARKS_TICKS;
    if (marks.marks != MARKS_OFF) {
        draw_face(&marks);
        set_area(face_ui.dial, 0, 0, diameter, diameter);
    }
    char time[8] = "--:--";
    if (valid_time) snprintf(time, sizeof(time), "%02d:%02d", local->tm_hour, local->tm_min);
    render_face_digital(face.slots, time, level, diameter, marks.marks != MARKS_OFF ? diameter / 16 + 3 : 2);
}

/* ---- Calibration overlay ---- */

/* 2-pixel ring touching the edge of a diameter x diameter box, using doubled
 * coordinates so the centre falls between pixels for even diameters. */
static void draw_calibration_ring(int diameter)
{
    if (canvas_owner == calibration_ring && diameter == ring_diameter) return;
    canvas_owner = calibration_ring;
    ring_diameter = diameter;
    lv_canvas_set_buffer(calibration_ring, lv_draw_buf_align(canvas_buffer, LV_COLOR_FORMAT_RGB565),
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
        &clock_ui.alarm_icon, &clock_ui.wake_icon, &clock_ui.wake_text};
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

    agenda_ui.page = new_page();
    agenda_ui.wake_icon = new_label(agenda_ui.page, LV_LABEL_LONG_MODE_CLIP);
    agenda_ui.wake_time = new_label(agenda_ui.page, LV_LABEL_LONG_MODE_CLIP);
    agenda_ui.wake_day = new_label(agenda_ui.page, LV_LABEL_LONG_MODE_CLIP);
    agenda_ui.divider = new_line(agenda_ui.page);
    for (int i = 0; i < PROJECTOR_MAX_EVENTS; ++i)
        agenda_ui.events[i] = new_label(agenda_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    climate_ui.page = new_page();
    /* Created first so the labels draw over it. */
    climate_ui.dial = lv_canvas_create(climate_ui.page);
    climate_ui.inner = lv_obj_create(climate_ui.page);
    lv_obj_remove_style_all(climate_ui.inner);
    lv_obj_remove_flag(climate_ui.inner, LV_OBJ_FLAG_SCROLLABLE);
    climate_ui.status_icon = new_label(climate_ui.inner, LV_LABEL_LONG_MODE_CLIP);
    climate_ui.target = new_label(climate_ui.inner, LV_LABEL_LONG_MODE_CLIP);
    climate_ui.status = new_label(climate_ui.inner, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    climate_ui.current = new_label(climate_ui.inner, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    climate_ui.humidity = new_label(climate_ui.page, LV_LABEL_LONG_MODE_CLIP);

    list_ui.page = new_page();
    list_ui.header = new_label(list_ui.page, LV_LABEL_LONG_MODE_CLIP);
    for (int i = 0; i < PROJECTOR_MAX_WIDGETS; ++i) {
        list_ui.icons[i] = new_label(list_ui.page, LV_LABEL_LONG_MODE_CLIP);
        list_ui.lines[i] = new_label(list_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    }

    face_ui.page = new_page();
    /* Created first so the digital labels draw over its marks. */
    face_ui.dial = lv_canvas_create(face_ui.page);
    face_ui.inner = lv_obj_create(face_ui.page);
    lv_obj_remove_style_all(face_ui.inner);
    lv_obj_remove_flag(face_ui.inner, LV_OBJ_FLAG_SCROLLABLE);
    face_ui.time = new_label(face_ui.inner, LV_LABEL_LONG_MODE_CLIP);
    for (int i = 0; i < SLOT_COUNT; ++i) {
        face_ui.icons[i] = new_label(face_ui.inner, LV_LABEL_LONG_MODE_CLIP);
        face_ui.values[i] = new_label(face_ui.inner, LV_LABEL_LONG_MODE_CLIP);
    }

    wake_ui.page = new_page();
    lv_obj_t **wake_labels[] = {&wake_ui.title, &wake_ui.time, &wake_ui.weather_icon, &wake_ui.high, &wake_ui.low};
    for (int i = 0; i < ARRAY_COUNT(wake_labels); ++i)
        *wake_labels[i] = new_label(wake_ui.page, LV_LABEL_LONG_MODE_CLIP);
    wake_ui.event = new_label(wake_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    message_ui.page = new_page();
    message_ui.icon = new_label(message_ui.page, LV_LABEL_LONG_MODE_CLIP);
    for (int i = 0; i < 2; ++i)
        message_ui.lines[i] = new_label(message_ui.page, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    for (int i = 0; i < PROJECTOR_MAX_PAGES; ++i) {
        page_dots[i] = new_bar(projection);
        show(page_dots[i], false);
    }

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
    theme = s->theme;
    for (int i = 0; i < hairline_count; ++i) set_background(hairlines[i], COLOR_LINE);
    const projector_page_t *page = &s->pages[s->page_index];
    lv_obj_t *active = list_ui.page;
    /* From the alarm on, the good morning screen covers the pages and all but alert messages. */
    bool alert = s->message.active && s->message.level == MESSAGE_ALERT;
    bool wake_screen = s->wake_phase == WAKE_RINGING && s->wake.screen && !alert;
    bool dots = s->theme.dots && !ap_ssid && !s->message.active && !wake_screen && s->page_index > 0 &&
        s->page_count > 1;
    bottom_reserve = dots ? diameter - dots_top(diameter) + 3 : 0;
    if (ap_ssid) {
        char password_a[9], password_b[9];
        snprintf(password_a, sizeof(password_a), "%.8s", ap_password);
        snprintf(password_b, sizeof(password_b), "%s", strlen(ap_password) > 8 ? ap_password + 8 : "");
        const char *lines[3] = {ap_ssid, password_a, password_b};
        render_lines("WI-FI AP", lines, NULL, NULL, 3, size_level(diameter), diameter);
    } else if (wake_screen) {
        active = wake_ui.page;
        render_wake(s, &local, valid_time, diameter);
    } else if (s->message.active) {
        active = message_ui.page;
        render_message(&s->message, diameter);
    } else if (page->layout == LAYOUT_CLOCK) {
        active = clock_ui.page;
        render_clock(s, page, &local, valid_time, diameter);
    } else if (page->layout == LAYOUT_HOME) {
        active = home_ui.page;
        render_home(s, page, diameter);
    } else if (page->layout == LAYOUT_WEATHER) {
        active = weather_ui.page;
        render_weather(s, page, &local, valid_time, diameter);
    } else if (page->layout == LAYOUT_AGENDA) {
        active = agenda_ui.page;
        render_agenda(s, page, &local, valid_time, diameter);
    } else if (page->layout == LAYOUT_CLIMATE) {
        active = climate_ui.page;
        render_climate(s, page, diameter);
    } else if (page->layout == LAYOUT_FACE) {
        active = face_ui.page;
        render_face(s, page, &local, valid_time, dots, diameter);
    } else render_list(s, page, &local, valid_time, diameter);
    lv_obj_t *const pages[] = {clock_ui.page, home_ui.page, weather_ui.page, agenda_ui.page, climate_ui.page,
        list_ui.page, face_ui.page, wake_ui.page, message_ui.page};
    for (int i = 0; i < ARRAY_COUNT(pages); ++i) {
        set_area(pages[i], 0, 0, diameter, diameter);
        show(pages[i], pages[i] == active);
    }
    render_dots(s, dots, diameter);
}

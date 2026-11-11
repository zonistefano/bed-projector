#include "projector_logic.h"
#include <stdio.h>
#include <string.h>

bool projector_data_fresh(int64_t last_us, int64_t now_us, int64_t ttl_us)
{
    return last_us > 0 && now_us >= last_us && now_us - last_us <= ttl_us;
}

/* Whether duration_us has passed since start_us; an unset start never expires. */
bool projector_expired(int64_t start_us, int64_t now_us, int64_t duration_us)
{
    return start_us > 0 && now_us >= start_us && now_us - start_us >= duration_us;
}

/* A timeout of zero keeps the selected page. */
bool projector_page_timed_out(unsigned page_index, int64_t selected_us,
                              int64_t now_us, int64_t timeout_us)
{
    return page_index != 0 && timeout_us > 0 && projector_expired(selected_us, now_us, timeout_us);
}

unsigned projector_move_page(unsigned index, unsigned count, bool forward)
{
    if (!count) return 0;
    if (forward) return (index + 1) % count;
    return (index + count - 1) % count;
}

bool projector_calibration_valid(int center_x, int center_y, int diameter)
{
    if (diameter < 80 || diameter > 128 || (diameter & 1)) return false;
    int radius = diameter / 2;
    return center_x >= radius && center_x <= 128 - radius &&
           center_y >= radius && center_y <= 128 - radius;
}

/* Orientation maps act on 128x128 pixel coordinates. Panel maps follow the
 * esp_lcd/MADCTL order used by esp_lvgl_port: swap axes first, then mirror. */
static void panel_map(unsigned panel, int *x, int *y)
{
    if (panel & 4) { int t = *x; *x = *y; *y = t; }
    if (panel & 2) *x = 127 - *x;
    if (panel & 1) *y = 127 - *y;
}

/* Where content drawn at (x, y) appears in the base orientation:
 * a horizontal mirror followed by quarter turns. */
static void content_map(unsigned rotation, bool mirror, int *x, int *y)
{
    if (mirror) *x = 127 - *x;
    for (unsigned i = 0; i < rotation % 4; ++i) { int t = *x; *x = 127 - *y; *y = t; }
}

static void content_unmap(unsigned rotation, bool mirror, int *x, int *y)
{
    for (unsigned i = 0; i < rotation % 4; ++i) { int t = *y; *y = 127 - *x; *x = t; }
    if (mirror) *x = 127 - *x;
}

void projector_orientation_panel(unsigned rotation, bool mirror,
                                 bool *swap_xy, bool *mirror_x, bool *mirror_y)
{
    /* esp_lvgl_port sets swap_xy, mirror_x for mirror(true, true) at 90°. */
    static const unsigned base = 4 | 2;
    static const int probes[3][2] = {{0, 0}, {1, 0}, {0, 1}};
    for (unsigned panel = 0; panel < 8; ++panel) {
        bool match = true;
        for (int i = 0; i < 3 && match; ++i) {
            int ax = probes[i][0], ay = probes[i][1], bx = ax, by = ay;
            panel_map(panel, &ax, &ay);
            content_map(rotation, mirror, &bx, &by);
            panel_map(base, &bx, &by);
            match = ax == bx && ay == by;
        }
        if (match) {
            *swap_xy = panel & 4;
            *mirror_x = panel & 2;
            *mirror_y = panel & 1;
            return;
        }
    }
}

void projector_orientation_center(unsigned rotation, bool mirror, int center_x, int center_y,
                                  int diameter, int *x, int *y)
{
    int radius = diameter / 2;
    int x1 = center_x - radius, y1 = center_y - radius;
    int x2 = center_x + radius - 1, y2 = center_y + radius - 1;
    content_unmap(rotation, mirror, &x1, &y1);
    content_unmap(rotation, mirror, &x2, &y2);
    *x = (x1 < x2 ? x1 : x2) + radius;
    *y = (y1 < y2 ? y1 : y2) + radius;
}

static int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[month - 1];
}

/* YYYY-MM-DD of the local date days_after days later, as HA sends forecast dates. */
void projector_date_after(int year, int month, int day, int days_after, char out[11])
{
    for (int i = 0; i < days_after; ++i)
        if (++day > days_in_month(year, month)) {
            day = 1;
            if (++month > 12) { month = 1; ++year; }
        }
    snprintf(out, 11, "%04u-%02u-%02u", (unsigned)year % 10000u, (unsigned)month % 100u,
             (unsigned)day % 100u);
}

/* Days from the local date to an HA YYYY-MM-DD date in the coming week, or -1. */
int projector_relative_day(int year, int month, int day, const char *date)
{
    char candidate[11];
    for (int i = 0; i < 7; ++i) {
        projector_date_after(year, month, day, i, candidate);
        if (!strcmp(candidate, date)) return i;
    }
    return -1;
}

/* From the hour from (21:00 by default) the forecast that matters is the next day's;
 * PROJECTOR_TOMORROW_NEVER keeps today's. */
bool projector_shows_tomorrow(int hour, int from)
{
    return from < PROJECTOR_TOMORROW_NEVER && hour >= from;
}

/* Whether an alarm wake_day days from today at wake_hour rings in the coming morning:
 * this morning until noon, tomorrow's from noon on. */
bool projector_wake_next_morning(int hour, int wake_day, int wake_hour)
{
    return wake_day == (hour < 12 ? 0 : 1) && wake_hour >= 0 && wake_hour < 12;
}

/* Time left until an alarm wake_day days from now, as "7h 05m" or "45m". Counted on the
 * wall clock, so it is an hour off across a daylight saving change. */
void projector_format_countdown(int hour, int minute, int wake_day, int wake_hour, int wake_minute,
                                char *out, size_t size)
{
    int left = wake_day * 1440 + wake_hour * 60 + wake_minute - (hour * 60 + minute);
    if (left < 0) left = 0;
    if (left >= 60) snprintf(out, size, "%dh %02dm", left / 60, left % 60);
    else snprintf(out, size, "%dm", left);
}

/* Days since 1970-01-01 of a proleptic Gregorian date. */
static long days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    long era = (year >= 0 ? year : year - 399) / 400;
    long year_of_era = year - era * 400;
    long day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    long day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

/* Moon phase at noon of a date, 0-7: new, waxing crescent, first quarter, waxing
 * gibbous, full, waning gibbous, last quarter, waning crescent. Counted from the
 * new moon of 2000-01-06 18:14 UTC; the mean synodic month keeps it within a day. */
int projector_moon_phase(int year, int month, int day)
{
    const double synodic = 29.530588853, reference = 10962 + 18.23 / 24;
    double age = days_from_civil(year, month, day) + 0.5 - reference;
    age -= synodic * (long)(age / synodic);
    if (age < 0) age += synodic;
    return (int)(age / synodic * 8 + 0.5) % 8;
}

bool projector_page_timeout_valid(int seconds)
{
    return seconds == 0 || (seconds >= 5 && seconds <= 3600);
}

/* Delay before Wi-Fi reconnection attempt number attempt (from 0): 1 s doubling
 * up to 30 s, so a missing router does not keep the radio busy and the
 * recovery access point stays on its channel. */
uint32_t projector_backoff_ms(unsigned attempt)
{
    uint32_t delay = 1000u << (attempt < 5 ? attempt : 5);
    return delay < 30000u ? delay : 30000u;
}

/* Tick of a thermostat dial with ticks marks spread evenly from min to max: the nearest one,
 * clamped to the ends so values off the scale still point at its edge. */
int projector_dial_tick(int value, int min, int max, int ticks)
{
    if (ticks < 2 || max <= min || value <= min) return 0;
    if (value >= max) return ticks - 1;
    long scaled = (long)(value - min) * (ticks - 1);
    return (int)((scaled * 2 + (max - min)) / (2L * (max - min)));
}

/* Tenths of a degree as "21.5", or "22" when whole: the thermostat's large numbers. */
void projector_format_tenths(int tenths, char *out, size_t size)
{
    int magnitude = tenths < 0 ? -tenths : tenths;
    const char *sign = tenths < 0 ? "-" : "";
    if (magnitude % 10) snprintf(out, size, "%s%d.%d", sign, magnitude / 10, magnitude % 10);
    else snprintf(out, size, "%s%d", sign, magnitude / 10);
}

/* ---- Page schema ---- */

#define COUNT(items) ((uint8_t)(sizeof(items) / sizeof((items)[0])))

static const char *const on_off[] = {[SHOW_ON] = "on", [SHOW_OFF] = "off"};
static const char *const status_modes[] = {[STATUS_COUNT] = "count", [STATUS_ICON] = "icon",
                                           [STATUS_OFF] = "off"};
static const char *const labeled_modes[] = {[LABELED_ICON_LABEL] = "icon_label", [LABELED_LABEL] = "label",
                                            [LABELED_ICON] = "icon", [LABELED_OFF] = "off"};
static const char *const names_modes[] = {[NAMES_ALL] = "names", [NAMES_COUNT] = "count", [NAMES_OFF] = "off"};

static const projector_element_t clock_elements[] = {
    [CLOCK_FORECAST] = {"forecast", (const char *const[]){[FORECAST_ICON_TEMPS] = "icon_temps",
        [FORECAST_TEMPS] = "temps", [FORECAST_ICON] = "icon", [FORECAST_OFF] = "off"}, 4},
    [CLOCK_OPENINGS] = {"openings", status_modes, COUNT(status_modes)},
    [CLOCK_LIGHTS] = {"lights", status_modes, COUNT(status_modes)},
    [CLOCK_ALARM] = {"alarm", (const char *const[]){[SHOW_ON] = "icon", [SHOW_OFF] = "off"}, 2},
    [CLOCK_WAKE] = {"wake", (const char *const[]){[WAKE_ICON] = "icon", [WAKE_TIME] = "time",
        [WAKE_COUNTDOWN] = "countdown", [WAKE_OFF] = "off"}, 4},
    [CLOCK_DATE] = {"date", (const char *const[]){[DATE_FULL] = "full", [DATE_SHORT] = "short",
        [DATE_OFF] = "off"}, 3},
    [CLOCK_SUN] = {"sun", on_off, COUNT(on_off)},
};

static const projector_element_t home_elements[] = {
    [HOME_ALARM] = {"alarm", labeled_modes, COUNT(labeled_modes)},
    [HOME_TEMPERATURES] = {"temperatures", (const char *const[]){[TEMPS_ICONS] = "icons",
        [TEMPS_VALUES] = "values", [TEMPS_OFF] = "off"}, 3},
    [HOME_OPENINGS] = {"openings", names_modes, COUNT(names_modes)},
    [HOME_LIGHTS] = {"lights", names_modes, COUNT(names_modes)},
};

static const projector_element_t weather_elements[] = {
    [WEATHER_NOW] = {"now", (const char *const[]){[NOW_ICON_TEMP] = "icon_temp", [NOW_TEMP] = "temp",
        [NOW_ICON] = "icon"}, 3},
    [WEATHER_DETAILS] = {"details", (const char *const[]){[DETAILS_FULL] = "full",
        [DETAILS_CONDITION] = "condition", [DETAILS_OFF] = "off"}, 3},
    [WEATHER_PERIODS] = {"periods", on_off, COUNT(on_off)},
    [WEATHER_RAIN] = {"rain", on_off, COUNT(on_off)},
    [WEATHER_SUMMARY] = {"summary", on_off, COUNT(on_off)},
};

static const projector_element_t agenda_elements[] = {
    [AGENDA_WAKE] = {"wake", (const char *const[]){[AGENDA_WAKE_TIME_DAY] = "time_day",
        [AGENDA_WAKE_TIME] = "time", [AGENDA_WAKE_OFF] = "off"}, 3},
    [AGENDA_EVENTS] = {"events", (const char *const[]){[EVENTS_3] = "3", [EVENTS_2] = "2", [EVENTS_1] = "1",
        [EVENTS_OFF] = "off"}, 4},
    [AGENDA_EVENT_TIME] = {"event_time", on_off, COUNT(on_off)},
};

static const projector_element_t climate_elements[] = {
    [CLIMATE_DIAL] = {"dial", on_off, COUNT(on_off)},
    [CLIMATE_STATUS] = {"status", labeled_modes, COUNT(labeled_modes)},
    [CLIMATE_PRESET] = {"preset", on_off, COUNT(on_off)},
    [CLIMATE_CURRENT] = {"current", on_off, COUNT(on_off)},
    [CLIMATE_HUMIDITY] = {"humidity", on_off, COUNT(on_off)},
};

static const projector_element_t list_elements[] = {
    [LIST_HEADER] = {"header", on_off, COUNT(on_off)},
};

const projector_layout_t projector_layouts[LAYOUT_COUNT] = {
    [LAYOUT_CLOCK] = {"clock", clock_elements, COUNT(clock_elements)},
    [LAYOUT_HOME] = {"home", home_elements, COUNT(home_elements)},
    [LAYOUT_WEATHER] = {"weather", weather_elements, COUNT(weather_elements)},
    [LAYOUT_AGENDA] = {"agenda", agenda_elements, COUNT(agenda_elements)},
    [LAYOUT_CLIMATE] = {"climate", climate_elements, COUNT(climate_elements)},
    [LAYOUT_LIST] = {"list", list_elements, COUNT(list_elements)},
};

const char *const projector_widgets[WIDGET_COUNT] = {
    [WIDGET_OPENINGS] = "openings", [WIDGET_ALARM] = "alarm", [WIDGET_DATE] = "date",
    [WIDGET_WEATHER] = "weather", [WIDGET_ENTITY1] = "entity1", [WIDGET_ENTITY2] = "entity2",
    [WIDGET_ENTITY3] = "entity3", [WIDGET_ENTITY4] = "entity4", [WIDGET_TEXT] = "text",
    [WIDGET_INDOOR] = "indoor", [WIDGET_OUTDOOR] = "outdoor", [WIDGET_LIGHTS] = "lights",
    [WIDGET_SUN] = "sun", [WIDGET_WAKE] = "wake", [WIDGET_EVENT] = "event", [WIDGET_MOON] = "moon",
    [WIDGET_THERMOSTAT] = "thermostat",
};
const char *const projector_styles[STYLE_COUNT] = {
    [STYLE_LABEL_VALUE] = "label_value", [STYLE_VALUE] = "value", [STYLE_ICON_VALUE] = "icon_value",
};
const char *const projector_sizes[SIZE_COUNT] = {
    [SIZE_NORMAL] = "normal", [SIZE_SMALL] = "small", [SIZE_LARGE] = "large",
};
const char *const projector_palettes[PALETTE_COUNT] = {
    [PALETTE_COLOR] = "color", [PALETTE_MONO] = "mono", [PALETTE_RED] = "red",
};

/* Index of name in names, or -1. */
int projector_name_index(const char *name, const char *const *names, int count)
{
    for (int i = 0; name && i < count; ++i)
        if (!strcmp(name, names[i])) return i;
    return -1;
}

int projector_layout_index(const char *name)
{
    for (int i = 0; name && i < LAYOUT_COUNT; ++i)
        if (!strcmp(name, projector_layouts[i].name)) return i;
    return -1;
}

int projector_element_index(int layout, const char *name)
{
    if (layout < 0 || layout >= LAYOUT_COUNT) return -1;
    const projector_layout_t *l = &projector_layouts[layout];
    for (int i = 0; name && i < l->element_count; ++i)
        if (!strcmp(name, l->elements[i].name)) return i;
    return -1;
}

int projector_mode_index(int layout, int element, const char *name)
{
    if (layout < 0 || layout >= LAYOUT_COUNT || element < 0 ||
        element >= projector_layouts[layout].element_count) return -1;
    const projector_element_t *e = &projector_layouts[layout].elements[element];
    return projector_name_index(name, e->modes, e->mode_count);
}

/* Font level 0 (largest) to 3 (smallest) for a circle's level and a page text size. */
int projector_size_level(int level, int size)
{
    level += size == SIZE_SMALL ? 1 : size == SIZE_LARGE ? -1 : 0;
    return level < 0 ? 0 : level > 3 ? 3 : level;
}

/* A colour as the palette shows it: unchanged, or its luminance in one hue (tint for mono,
 * deep red for the night palette, which spares dark-adapted eyes). Black stays black. */
uint32_t projector_tint(uint32_t color, int palette, uint32_t tint)
{
    if (palette != PALETTE_MONO && palette != PALETTE_RED) return color;
    if (palette == PALETTE_RED) tint = 0xff2000;
    uint32_t luminance = (((color >> 16) & 0xff) * 77 + ((color >> 8) & 0xff) * 150 + (color & 0xff) * 29) >> 8;
    uint32_t r = ((tint >> 16) & 0xff) * luminance / 255, g = ((tint >> 8) & 0xff) * luminance / 255;
    uint32_t b = (tint & 0xff) * luminance / 255;
    return r << 16 | g << 8 | b;
}

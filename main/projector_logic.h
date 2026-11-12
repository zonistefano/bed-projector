#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How long a pulsing LED (urgent message, alarm) pulses before it stays steady. */
#define PROJECTOR_PULSE_MS 60000
/* projector_seconds_to_wake() when there is no alarm it can place. */
#define PROJECTOR_NO_WAKE INT32_MIN

bool projector_data_fresh(int64_t last_us, int64_t now_us, int64_t ttl_us);
bool projector_expired(int64_t start_us, int64_t now_us, int64_t duration_us);
bool projector_page_timed_out(unsigned page_index, int64_t selected_us,
                              int64_t now_us, int64_t timeout_us);
unsigned projector_move_page(unsigned index, unsigned count, bool forward);
bool projector_calibration_valid(int center_x, int center_y, int diameter);
void projector_orientation_panel(unsigned rotation, bool mirror,
                                 bool *swap_xy, bool *mirror_x, bool *mirror_y);
void projector_orientation_center(unsigned rotation, bool mirror, int center_x, int center_y,
                                  int diameter, int *x, int *y);
void projector_date_after(int year, int month, int day, int days_after, char out[11]);
int projector_relative_day(int year, int month, int day, const char *date);
bool projector_shows_tomorrow(int hour, int from);
bool projector_wake_next_morning(int hour, int wake_day, int wake_hour);
void projector_format_countdown(int hour, int minute, int wake_day, int wake_hour, int wake_minute,
                                char *out, size_t size);
int projector_moon_phase(int year, int month, int day);
bool projector_page_timeout_valid(int seconds);
bool projector_carousel_valid(int seconds);
uint32_t projector_backoff_ms(unsigned attempt);
int projector_dial_tick(int value, int min, int max, int ticks);
void projector_format_tenths(int tenths, char *out, size_t size);
void projector_hand_end(int position, int turn, float length, float *dx, float *dy);
uint8_t projector_pulse_level(uint8_t level, int64_t elapsed_ms);
int32_t projector_seconds_to_wake(int year, int month, int day, int hour, int minute, int second,
                                  const char *wake_date, const char *wake_time);
uint8_t projector_sunrise_level(int64_t left_ms, int64_t ramp_ms, uint8_t brightness);
bool projector_parse_countdown(const char *text, int *year, int *month, int *day, const char **name);
long projector_days_between(int year, int month, int day, int to_year, int to_month, int to_day);

/* ---- Page schema: layouts, their configurable elements and display modes. Mode 0 of every
 * element is its default, so a zeroed page shows everything as designed. ---- */

#define PROJECTOR_MAX_ELEMENTS 7
#define PROJECTOR_TOMORROW_DEFAULT 21 /* Hour from which clock and weather show tomorrow. */
#define PROJECTOR_TOMORROW_NEVER 24

enum { LAYOUT_CLOCK, LAYOUT_HOME, LAYOUT_WEATHER, LAYOUT_AGENDA, LAYOUT_CLIMATE, LAYOUT_LIST, LAYOUT_FACE,
       LAYOUT_COUNT };

/* Elements of each layout, indexes into projector_page_t.show. */
enum { CLOCK_FORECAST, CLOCK_OPENINGS, CLOCK_LIGHTS, CLOCK_ALARM, CLOCK_WAKE, CLOCK_DATE, CLOCK_SUN };
enum { HOME_ALARM, HOME_TEMPERATURES, HOME_OPENINGS, HOME_LIGHTS };
enum { WEATHER_NOW, WEATHER_DETAILS, WEATHER_PERIODS, WEATHER_RAIN, WEATHER_SUMMARY };
enum { AGENDA_WAKE, AGENDA_EVENTS, AGENDA_EVENT_TIME };
enum { CLIMATE_DIAL, CLIMATE_STATUS, CLIMATE_PRESET, CLIMATE_CURRENT, CLIMATE_HUMIDITY };
enum { LIST_HEADER };
enum { FACE_DIAL, FACE_MARKS };

/* Modes, in the order of each element's mode list. */
enum { SHOW_ON, SHOW_OFF };
enum { FORECAST_ICON_TEMPS, FORECAST_TEMPS, FORECAST_ICON, FORECAST_OFF };
enum { STATUS_COUNT, STATUS_ICON, STATUS_OFF };
enum { WAKE_ICON, WAKE_TIME, WAKE_COUNTDOWN, WAKE_OFF };
enum { DATE_FULL, DATE_SHORT, DATE_OFF };
enum { LABELED_ICON_LABEL, LABELED_LABEL, LABELED_ICON, LABELED_OFF };
enum { TEMPS_ICONS, TEMPS_VALUES, TEMPS_OFF };
enum { NAMES_ALL, NAMES_COUNT, NAMES_OFF };
enum { NOW_ICON_TEMP, NOW_TEMP, NOW_ICON };
enum { DETAILS_FULL, DETAILS_CONDITION, DETAILS_OFF };
enum { AGENDA_WAKE_TIME_DAY, AGENDA_WAKE_TIME, AGENDA_WAKE_OFF };
enum { EVENTS_3, EVENTS_2, EVENTS_1, EVENTS_OFF };
enum { DIAL_ANALOG, DIAL_DIGITAL };
enum { MARKS_TICKS, MARKS_NUMBERS, MARKS_OFF };

enum { WIDGET_OPENINGS, WIDGET_ALARM, WIDGET_DATE, WIDGET_WEATHER, WIDGET_ENTITY1, WIDGET_ENTITY2,
       WIDGET_ENTITY3, WIDGET_ENTITY4, WIDGET_TEXT, WIDGET_INDOOR, WIDGET_OUTDOOR, WIDGET_LIGHTS,
       WIDGET_SUN, WIDGET_WAKE, WIDGET_EVENT, WIDGET_MOON, WIDGET_THERMOSTAT, WIDGET_RAIN, WIDGET_HUMIDITY,
       WIDGET_WIND, WIDGET_COUNTDOWN, WIDGET_COUNT };
enum { STYLE_LABEL_VALUE, STYLE_VALUE, STYLE_ICON_VALUE, STYLE_COUNT };
/* Where a complication sits on a watch face page. */
enum { SLOT_TOP, SLOT_RIGHT, SLOT_BOTTOM, SLOT_LEFT, SLOT_COUNT };
enum { SIZE_NORMAL, SIZE_SMALL, SIZE_LARGE, SIZE_COUNT };
enum { PALETTE_COLOR, PALETTE_MONO, PALETTE_RED, PALETTE_COUNT };
enum { MESSAGE_INFO, MESSAGE_WARNING, MESSAGE_ALERT, MESSAGE_LEVEL_COUNT };

typedef struct {
    const char *name;
    const char *const *modes;
    uint8_t mode_count;
} projector_element_t;

typedef struct {
    const char *name;
    const projector_element_t *elements;
    uint8_t element_count;
} projector_layout_t;

extern const projector_layout_t projector_layouts[LAYOUT_COUNT];
extern const char *const projector_widgets[WIDGET_COUNT];
extern const char *const projector_styles[STYLE_COUNT];
extern const char *const projector_slots[SLOT_COUNT];
extern const char *const projector_sizes[SIZE_COUNT];
extern const char *const projector_palettes[PALETTE_COUNT];
extern const char *const projector_message_levels[MESSAGE_LEVEL_COUNT];

int projector_name_index(const char *name, const char *const *names, int count);
int projector_layout_index(const char *name);
int projector_element_index(int layout, const char *name);
int projector_mode_index(int layout, int element, const char *name);
int projector_size_level(int level, int size);
uint32_t projector_tint(uint32_t color, int palette, uint32_t tint);

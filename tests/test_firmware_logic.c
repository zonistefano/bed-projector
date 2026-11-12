#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "projector_logic.h"

static void physical(bool swap, bool mirror_x, bool mirror_y, int *x, int *y)
{
    if (swap) { int t = *x; *x = *y; *y = t; }
    if (mirror_x) *x = 127 - *x;
    if (mirror_y) *y = 127 - *y;
}

/* Every orientation must keep the calibrated circle on the same panel pixels. */
static void assert_circle_fixed(unsigned rotation, bool mirror)
{
    bool swap, mirror_x, mirror_y;
    int x, y;
    projector_orientation_panel(rotation, mirror, &swap, &mirror_x, &mirror_y);
    projector_orientation_center(rotation, mirror, 53, 75, 106, &x, &y);
    int corners[2][2] = {{x - 53, y - 53}, {x + 52, y + 52}};
    int base[2][2] = {{0, 22}, {105, 127}};
    for (int i = 0; i < 2; ++i) {
        physical(swap, mirror_x, mirror_y, &corners[i][0], &corners[i][1]);
        physical(true, true, false, &base[i][0], &base[i][1]);
    }
    int min_x = corners[0][0] < corners[1][0] ? corners[0][0] : corners[1][0];
    int min_y = corners[0][1] < corners[1][1] ? corners[0][1] : corners[1][1];
    int base_x = base[0][0] < base[1][0] ? base[0][0] : base[1][0];
    int base_y = base[0][1] < base[1][1] ? base[0][1] : base[1][1];
    assert(min_x == base_x && min_y == base_y);
}

int main(void)
{
    assert(projector_move_page(0, 3, true) == 1);
    assert(projector_move_page(2, 3, true) == 0);
    assert(projector_move_page(0, 3, false) == 2);
    assert(projector_move_page(0, 0, false) == 0);
    assert(!projector_page_timed_out(0, 100, 30100000, 30000000));
    assert(!projector_page_timed_out(1, 100, 30000099, 30000000));
    assert(projector_page_timed_out(1, 100, 30000100, 30000000));
    assert(!projector_page_timed_out(1, 100, 999000000000, 0));
    assert(!projector_expired(0, 999000000, 1));
    assert(!projector_expired(100, 30000099, 30000000));
    assert(projector_expired(100, 30000100, 30000000));
    assert(!projector_expired(100, 50, 1));
    assert(projector_relative_day(2026, 12, 31, "2026-12-31") == 0);
    assert(projector_relative_day(2026, 12, 31, "2027-01-01") == 1);
    assert(projector_relative_day(2026, 12, 31, "2027-01-06") == 6);
    assert(projector_relative_day(2026, 12, 31, "2027-01-07") == -1);
    assert(projector_relative_day(2026, 12, 31, "2026-12-30") == -1);
    assert(projector_relative_day(2026, 12, 31, "") == -1);
    /* April 2024: new moon 8th, first quarter 15th, full 23rd, last quarter 1 May. */
    assert(projector_moon_phase(2024, 4, 8) == 0);
    assert(projector_moon_phase(2024, 4, 15) == 2);
    assert(projector_moon_phase(2024, 4, 23) == 4);
    assert(projector_moon_phase(2024, 5, 1) == 6);
    assert(projector_moon_phase(2000, 1, 6) == 0);
    assert(projector_moon_phase(1999, 12, 22) == 4);
    assert(projector_moon_phase(2026, 11, 24) == 4);
    assert(projector_page_timeout_valid(0) && projector_page_timeout_valid(5) &&
           projector_page_timeout_valid(3600));
    assert(!projector_page_timeout_valid(4) && !projector_page_timeout_valid(3601) &&
           !projector_page_timeout_valid(-1));
    assert(projector_data_fresh(100, 120000100, 120000000));
    assert(!projector_data_fresh(100, 120000101, 120000000));
    assert(!projector_data_fresh(0, 1, 120000000));
    assert(projector_calibration_valid(64, 64, 128));
    assert(projector_calibration_valid(53, 75, 106));
    assert(!projector_calibration_valid(52, 75, 106));
    assert(!projector_calibration_valid(64, 64, 79));
    assert(!projector_calibration_valid(64, 64, 127));
    bool swap, mirror_x, mirror_y;
    projector_orientation_panel(0, false, &swap, &mirror_x, &mirror_y);
    assert(swap && mirror_x && !mirror_y);
    projector_orientation_panel(2, false, &swap, &mirror_x, &mirror_y);
    assert(swap && !mirror_x && mirror_y);
    projector_orientation_panel(0, true, &swap, &mirror_x, &mirror_y);
    assert(swap && mirror_x && mirror_y);
    projector_orientation_panel(1, false, &swap, &mirror_x, &mirror_y);
    assert(!swap);
    int x, y;
    projector_orientation_center(0, false, 53, 75, 106, &x, &y);
    assert(x == 53 && y == 75);
    projector_orientation_center(2, false, 53, 75, 106, &x, &y);
    assert(x == 75 && y == 53);
    projector_orientation_center(1, false, 53, 75, 106, &x, &y);
    assert(x == 75 && y == 75);
    projector_orientation_center(3, false, 53, 75, 106, &x, &y);
    assert(x == 53 && y == 53);
    projector_orientation_center(0, true, 53, 75, 106, &x, &y);
    assert(x == 75 && y == 75);
    char date[11];
    projector_date_after(2026, 10, 10, 1, date);
    assert(!strcmp(date, "2026-10-11"));
    projector_date_after(2026, 12, 31, 1, date);
    assert(!strcmp(date, "2027-01-01"));
    projector_date_after(2028, 2, 28, 1, date);
    assert(!strcmp(date, "2028-02-29"));
    projector_date_after(2026, 2, 28, 1, date);
    assert(!strcmp(date, "2026-03-01"));
    assert(!projector_shows_tomorrow(20, PROJECTOR_TOMORROW_DEFAULT));
    assert(projector_shows_tomorrow(21, PROJECTOR_TOMORROW_DEFAULT));
    assert(projector_shows_tomorrow(18, 18) && !projector_shows_tomorrow(17, 18));
    assert(projector_shows_tomorrow(0, 0) && !projector_shows_tomorrow(23, PROJECTOR_TOMORROW_NEVER));
    assert(projector_wake_next_morning(22, 1, 6) && projector_wake_next_morning(12, 1, 11));
    assert(projector_wake_next_morning(2, 0, 7) && projector_wake_next_morning(11, 0, 11));
    assert(!projector_wake_next_morning(22, 0, 6) && !projector_wake_next_morning(2, 1, 6));
    assert(!projector_wake_next_morning(22, 1, 12) && !projector_wake_next_morning(22, 2, 6));
    assert(!projector_wake_next_morning(22, -1, 6));
    char countdown[12];
    projector_format_countdown(22, 50, 1, 6, 45, countdown, sizeof(countdown));
    assert(!strcmp(countdown, "7h 55m"));
    projector_format_countdown(6, 0, 0, 6, 45, countdown, sizeof(countdown));
    assert(!strcmp(countdown, "45m"));
    projector_format_countdown(5, 45, 0, 6, 45, countdown, sizeof(countdown));
    assert(!strcmp(countdown, "1h 00m"));
    projector_format_countdown(6, 45, 0, 6, 45, countdown, sizeof(countdown));
    assert(!strcmp(countdown, "0m"));
    assert(projector_backoff_ms(0) == 1000 && projector_backoff_ms(1) == 2000);
    assert(projector_backoff_ms(4) == 16000 && projector_backoff_ms(5) == 30000);
    assert(projector_backoff_ms(1000) == 30000);
    assert(projector_dial_tick(100, 100, 300, 41) == 0 && projector_dial_tick(300, 100, 300, 41) == 40);
    assert(projector_dial_tick(200, 100, 300, 41) == 20 && projector_dial_tick(203, 100, 300, 41) == 21);
    assert(projector_dial_tick(202, 100, 300, 41) == 20);
    assert(projector_dial_tick(-50, 100, 300, 41) == 0 && projector_dial_tick(999, 100, 300, 41) == 40);
    assert(projector_dial_tick(200, 300, 100, 41) == 0 && projector_dial_tick(200, 100, 300, 1) == 0);
    char tenths[8];
    projector_format_tenths(215, tenths, sizeof(tenths));
    assert(!strcmp(tenths, "21.5"));
    projector_format_tenths(220, tenths, sizeof(tenths));
    assert(!strcmp(tenths, "22"));
    projector_format_tenths(-5, tenths, sizeof(tenths));
    assert(!strcmp(tenths, "-0.5"));
    projector_format_tenths(-120, tenths, sizeof(tenths));
    assert(!strcmp(tenths, "-12"));
    /* Page schema: names round-trip, default modes are index 0, unknown names are rejected. */
    assert(projector_layout_index("clock") == LAYOUT_CLOCK && projector_layout_index("list") == LAYOUT_LIST);
    assert(projector_layout_index("bogus") == -1 && projector_layout_index(NULL) == -1);
    for (int layout = 0; layout < LAYOUT_COUNT; ++layout) {
        const projector_layout_t *l = &projector_layouts[layout];
        assert(l->element_count >= 1 && l->element_count <= PROJECTOR_MAX_ELEMENTS);
        for (int element = 0; element < l->element_count; ++element) {
            assert(projector_element_index(layout, l->elements[element].name) == element);
            assert(l->elements[element].mode_count >= 2);
            for (int mode = 0; mode < l->elements[element].mode_count; ++mode)
                assert(projector_mode_index(layout, element, l->elements[element].modes[mode]) == mode);
        }
    }
    assert(projector_element_index(LAYOUT_CLOCK, "wake") == CLOCK_WAKE);
    assert(projector_mode_index(LAYOUT_CLOCK, CLOCK_WAKE, "countdown") == WAKE_COUNTDOWN);
    assert(projector_mode_index(LAYOUT_CLOCK, CLOCK_WAKE, "names") == -1);
    assert(projector_mode_index(LAYOUT_HOME, HOME_OPENINGS, "count") == NAMES_COUNT);
    assert(projector_mode_index(LAYOUT_AGENDA, AGENDA_EVENTS, "1") == EVENTS_1);
    assert(projector_mode_index(LAYOUT_CLIMATE, CLIMATE_STATUS, "icon") == LABELED_ICON);
    assert(projector_element_index(LAYOUT_HOME, "wake") == -1 && projector_element_index(LAYOUT_COUNT, "wake") == -1);
    assert(projector_name_index("thermostat", projector_widgets, WIDGET_COUNT) == WIDGET_THERMOSTAT);
    assert(projector_name_index("icon_value", projector_styles, STYLE_COUNT) == STYLE_ICON_VALUE);
    assert(projector_name_index("huge", projector_sizes, SIZE_COUNT) == -1);
    assert(projector_size_level(0, SIZE_LARGE) == 0 && projector_size_level(1, SIZE_LARGE) == 0);
    assert(projector_size_level(3, SIZE_SMALL) == 3 && projector_size_level(1, SIZE_SMALL) == 2);
    assert(projector_size_level(2, SIZE_NORMAL) == 2);
    /* Palettes: colour unchanged, black stays black, white takes the tint. */
    assert(projector_tint(0x123456, PALETTE_COLOR, 0xffffff) == 0x123456);
    assert(projector_tint(0x000000, PALETTE_MONO, 0xffb36b) == 0);
    assert(projector_tint(0xffffff, PALETTE_MONO, 0xffb36b) == 0xffb36b);
    assert(projector_tint(0xffffff, PALETTE_RED, 0x00ff00) == 0xff2000);
    uint32_t dim = projector_tint(0x6a7482, PALETTE_RED, 0);
    assert((dim & 0xff) == 0 && ((dim >> 8) & 0xff) < 0x10 && (dim >> 16) > 0x40 && (dim >> 16) < 0x80);
    assert(projector_carousel_valid(0) && projector_carousel_valid(5) && projector_carousel_valid(600));
    assert(!projector_carousel_valid(4) && !projector_carousel_valid(601) && !projector_carousel_valid(-1));
    /* Watch hands: up at 0, right at a quarter, down at half a turn; whole turns wrap. */
    float dx, dy;
#define NEAR(a, b) ((a) - (b) < 0.01f && (b) - (a) < 0.01f)
    projector_hand_end(0, 60, 40, &dx, &dy);
    assert(NEAR(dx, 0) && NEAR(dy, -40));
    projector_hand_end(15, 60, 40, &dx, &dy);
    assert(NEAR(dx, 40) && NEAR(dy, 0));
    projector_hand_end(30, 60, 40, &dx, &dy);
    assert(NEAR(dx, 0) && NEAR(dy, 40));
    projector_hand_end(6 * 60, 720, 20, &dx, &dy);
    assert(NEAR(dx, 0) && NEAR(dy, 20));
    projector_hand_end(9 * 60 + 720, 720, 20, &dx, &dy);
    assert(NEAR(dx, -20) && NEAR(dy, 0));
    projector_hand_end(5, 60, 10, &dx, &dy); /* 30°: x = 5, y = -8.66 */
    assert(NEAR(dx, 5) && NEAR(dy, -8.660254f));
    projector_hand_end(50, 60, 10, &dx, &dy); /* 300° */
    assert(NEAR(dx, -8.660254f) && NEAR(dy, -5));
    projector_hand_end(2 * 60 + 11, 720, 10, &dx, &dy); /* 02:11 stays on the 02:00 step. */
    assert(NEAR(dx, 8.660254f) && NEAR(dy, -5));
#undef NEAR
    assert(projector_layout_index("face") == LAYOUT_FACE);
    assert(projector_mode_index(LAYOUT_FACE, FACE_DIAL, "digital") == DIAL_DIGITAL);
    assert(projector_name_index("left", projector_slots, SLOT_COUNT) == SLOT_LEFT);
    /* Pulsing LED: full, dim, full again, steady after PROJECTOR_PULSE_MS. */
    assert(projector_pulse_level(50, 0) == 50 && projector_pulse_level(50, 499) == 50);
    assert(projector_pulse_level(50, 500) == 10 && projector_pulse_level(50, 1000) == 50);
    assert(projector_pulse_level(3, 500) == 1);
    assert(projector_pulse_level(50, PROJECTOR_PULSE_MS + 500) == 50);
    assert(projector_pulse_level(50, -500) == 50);
    assert(projector_name_index("alert", projector_message_levels, MESSAGE_LEVEL_COUNT) == MESSAGE_ALERT);
    /* Alarm: seconds left from HA's local date and time, across midnight and the year's end. */
    assert(projector_seconds_to_wake(2026, 11, 12, 6, 30, 0, "2026-11-12", "06:45") == 900);
    assert(projector_seconds_to_wake(2026, 11, 12, 6, 45, 30, "2026-11-12", "06:45") == -30);
    assert(projector_seconds_to_wake(2026, 12, 31, 23, 50, 0, "2027-01-01", "00:10") == 1200);
    assert(projector_seconds_to_wake(2026, 11, 12, 22, 0, 0, "2026-11-13", "06:00") == 8 * 3600);
    assert(projector_seconds_to_wake(2026, 11, 12, 6, 0, 0, "2026-11-20", "06:00") == PROJECTOR_NO_WAKE);
    assert(projector_seconds_to_wake(2026, 11, 12, 6, 0, 0, "2026-11-12", "6:00") == PROJECTOR_NO_WAKE);
    assert(projector_seconds_to_wake(2026, 11, 12, 6, 0, 0, "2026-11-12", "24:00") == PROJECTOR_NO_WAKE);
    assert(projector_seconds_to_wake(2026, 11, 12, 6, 0, 0, "", "06:00") == PROJECTOR_NO_WAKE);
    /* Sunrise: off before the ramp, 1% at its start, a quarter at half, full at the alarm. */
    assert(projector_sunrise_level(600000, 600000, 80) == 0);
    assert(projector_sunrise_level(599000, 600000, 80) == 1);
    assert(projector_sunrise_level(300000, 600000, 80) == 20);
    assert(projector_sunrise_level(1, 600000, 80) == 79);
    assert(projector_sunrise_level(0, 600000, 80) == 80 && projector_sunrise_level(-5000, 600000, 80) == 80);
    assert(projector_sunrise_level(1000, 0, 40) == 40);
    /* Countdown widget text. */
    int year, month, day;
    const char *name;
    assert(projector_parse_countdown("2026-12-24 Vigilia", &year, &month, &day, &name));
    assert(year == 2026 && month == 12 && day == 24 && !strcmp(name, "Vigilia"));
    assert(projector_parse_countdown("2028-02-29", &year, &month, &day, &name) && !strcmp(name, ""));
    assert(!projector_parse_countdown("2027-02-29 Mai", &year, &month, &day, &name));
    assert(!projector_parse_countdown("2026-13-01", &year, &month, &day, &name));
    assert(!projector_parse_countdown("2026-1-01 X", &year, &month, &day, &name));
    assert(!projector_parse_countdown("2026-12-24X", &year, &month, &day, &name));
    assert(!projector_parse_countdown("Natale", &year, &month, &day, &name));
    assert(!projector_parse_countdown(NULL, &year, &month, &day, &name));
    assert(projector_days_between(2026, 12, 31, 2027, 1, 1) == 1);
    assert(projector_days_between(2026, 11, 11, 2026, 12, 24) == 43);
    assert(projector_days_between(2028, 3, 1, 2028, 2, 28) == -2);
    assert(projector_days_between(2026, 5, 5, 2026, 5, 5) == 0);
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        assert_circle_fixed(rotation, false);
        assert_circle_fixed(rotation, true);
    }
    return 0;
}

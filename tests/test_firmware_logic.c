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
    assert(!projector_shows_tomorrow(20) && projector_shows_tomorrow(21));
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
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        assert_circle_fixed(rotation, false);
        assert_circle_fixed(rotation, true);
    }
    return 0;
}

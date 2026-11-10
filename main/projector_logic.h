#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
bool projector_shows_tomorrow(int hour);
bool projector_wake_next_morning(int hour, int wake_day, int wake_hour);
void projector_format_countdown(int hour, int minute, int wake_day, int wake_hour, int wake_minute,
                                char *out, size_t size);
int projector_moon_phase(int year, int month, int day);
bool projector_page_timeout_valid(int seconds);
uint32_t projector_backoff_ms(unsigned attempt);
int projector_dial_tick(int value, int min, int max, int ticks);
void projector_format_tenths(int tenths, char *out, size_t size);

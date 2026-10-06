#pragma once

#include <stdbool.h>
#include <stdint.h>

bool projector_data_fresh(int64_t last_us, int64_t now_us, int64_t ttl_us);
bool projector_page_timed_out(unsigned page_index, int64_t selected_us,
                              int64_t now_us, int64_t timeout_us);
unsigned projector_move_page(unsigned index, unsigned count, bool forward);
bool projector_calibration_valid(int center_x, int center_y, int diameter);
void projector_orientation_panel(unsigned rotation, bool mirror,
                                 bool *swap_xy, bool *mirror_x, bool *mirror_y);
void projector_orientation_center(unsigned rotation, bool mirror, int center_x, int center_y,
                                  int diameter, int *x, int *y);

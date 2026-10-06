#include "projector_logic.h"

bool projector_data_fresh(int64_t last_us, int64_t now_us, int64_t ttl_us)
{
    return last_us > 0 && now_us >= last_us && now_us - last_us <= ttl_us;
}

bool projector_page_timed_out(unsigned page_index, int64_t selected_us,
                              int64_t now_us, int64_t timeout_us)
{
    return page_index != 0 && selected_us > 0 && now_us >= selected_us &&
           now_us - selected_us >= timeout_us;
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

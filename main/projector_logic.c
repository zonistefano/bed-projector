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

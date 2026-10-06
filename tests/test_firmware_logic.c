#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
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
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        assert_circle_fixed(rotation, false);
        assert_circle_fixed(rotation, true);
    }
    return 0;
}

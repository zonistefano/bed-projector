#include <assert.h>
#include <stdint.h>
#include "projector_logic.h"

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
    return 0;
}

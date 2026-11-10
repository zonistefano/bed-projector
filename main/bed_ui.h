#pragma once

#include <time.h>
#include "lvgl.h"
#include "projector.h"

/* Pure LVGL page rendering, shared by the firmware and the host preview. */
void bed_ui_create(lv_obj_t *screen);
/* ap_ssid is NULL unless the Wi-Fi configuration access point is running. */
void bed_ui_render(const projector_snapshot_t *s, time_t now, const char *ap_ssid,
                   const char *ap_password);

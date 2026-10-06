#pragma once

#include "esp_err.h"

esp_err_t bed_display_init(void);
void bed_display_start(void);
void bed_display_provisioning(const char *ssid, const char *password);
void bed_display_provisioning_done(void);

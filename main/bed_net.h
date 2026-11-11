#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t bed_net_start(void);
/* True while the recovery access point is up. */
bool bed_net_is_provisioning(void);
/* SSID and password of the recovery access point; false when it is not up. */
bool bed_net_ap_credentials(const char **ssid, const char **password);
/* Stable identifier (Wi-Fi MAC as 12 lowercase hex digits), set by bed_net_start(). */
const char *bed_net_device_id(void);
/* Publishes or withdraws the Home Assistant discovery service after projector_discovery() changes. */
void bed_net_update_discovery(void);

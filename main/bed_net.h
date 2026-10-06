#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t bed_net_start(void);
bool bed_net_is_provisioning(void);

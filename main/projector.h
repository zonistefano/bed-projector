#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#define PROJECTOR_MAX_PAGES 5
#define PROJECTOR_MAX_WIDGETS 3
#define PROJECTOR_EXTRA_VALUES 4

typedef struct {
    char id[17];
    char name[17];
    char layout[9];
    char widgets[PROJECTOR_MAX_WIDGETS][17];
    char text[33];
} projector_page_t;

typedef struct {
    uint8_t center_x;
    uint8_t center_y;
    uint8_t diameter;
    bool active;
} projector_calibration_t;

typedef struct {
    bool power;
    uint8_t brightness;
    uint8_t page_count;
    uint8_t page_index;
    projector_page_t pages[PROJECTOR_MAX_PAGES];
    bool ha_fresh;
    bool openings_known;
    uint8_t openings;
    char alarm[24];
    char weather[32];
    char temperature[16];
    char extras[PROJECTOR_EXTRA_VALUES][32];
    bool wifi_connected;
    projector_calibration_t calibration;
} projector_snapshot_t;

esp_err_t projector_init(void);
void projector_snapshot(projector_snapshot_t *out);
void projector_tick(void);
esp_err_t projector_set_pages(const cJSON *root, char *error, size_t error_size);
cJSON *projector_pages_json(void);
esp_err_t projector_set_command(const cJSON *root, char *error, size_t error_size);
esp_err_t projector_set_ha_state(const cJSON *root, char *error, size_t error_size);
const char *projector_token(void);
void projector_set_wifi_connected(bool connected);
void projector_read_network(char *ssid, size_t ssid_size, char *password, size_t password_size);
esp_err_t projector_write_network(const char *ssid, const char *password, const char *timezone);
void projector_timezone(char *out, size_t size);
esp_err_t projector_set_calibration(const cJSON *root, char *error, size_t error_size);

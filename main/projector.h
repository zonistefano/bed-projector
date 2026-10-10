#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#define PROJECTOR_MAX_PAGES 5
#define PROJECTOR_MAX_WIDGETS 3
#define PROJECTOR_EXTRA_VALUES 4
#define PROJECTOR_MAX_NAMES 8
#define PROJECTOR_FORECAST_DAYS 2
#define PROJECTOR_UNKNOWN_PERCENT -1
/* Seconds on a secondary page before the clock returns; 0 keeps the page.
 * projector_page_timeout_valid() holds the accepted range. */
#define PROJECTOR_PAGE_TIMEOUT_DEFAULT 30

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
    uint8_t rotation; /* Quarter turns, 0-3. */
    bool mirror;
    bool active;
} projector_calibration_t;

/* Part of a forecast day (daytime or evening); an empty condition means unknown. */
typedef struct {
    char condition[16];
    bool temperature_known;
    int16_t temperature;
    int8_t precipitation; /* Percent or PROJECTOR_UNKNOWN_PERCENT. */
} projector_period_t;

/* One forecast day as sent by HA; date is HA's local YYYY-MM-DD, empty if unused. */
typedef struct {
    char date[11];
    char condition[16];
    bool high_known;
    bool low_known;
    int16_t high;
    int16_t low;
    int8_t precipitation;
    char sunrise[6]; /* Local HH:MM, empty if unknown. */
    char sunset[6];
    char summary[64];
    projector_period_t day;
    projector_period_t evening;
} projector_day_t;

typedef struct {
    bool power;
    uint8_t brightness;
    uint8_t page_count;
    uint8_t page_index;
    uint16_t page_timeout; /* Seconds, 0 = never return to the clock. */
    projector_page_t pages[PROJECTOR_MAX_PAGES];
    bool ha_fresh;
    bool openings_known;
    uint8_t openings;
    char alarm[24];
    char weather[32];
    char temperature[16];
    char condition[16];
    bool temp_high_known;
    bool temp_low_known;
    int16_t temp_high;
    int16_t temp_low;
    char extras[PROJECTOR_EXTRA_VALUES][32];
    bool lights_known;
    uint8_t lights;
    uint8_t opening_name_count;
    uint8_t light_name_count;
    char opening_names[PROJECTOR_MAX_NAMES][32];
    char light_names[PROJECTOR_MAX_NAMES][32];
    char indoor[12];  /* Preformatted, e.g. "21.5°"; empty if unknown. */
    char outdoor[12];
    bool temp_now_known;
    int16_t temp_now;
    char feels[12];
    char wind[16];
    int8_t humidity;
    projector_day_t days[PROJECTOR_FORECAST_DAYS];
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

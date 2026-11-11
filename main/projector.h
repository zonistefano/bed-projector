#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"
#include "projector_logic.h"

#define PROJECTOR_MAX_PAGES 5
#define PROJECTOR_MAX_WIDGETS 4
#define PROJECTOR_EXTRA_VALUES 4
#define PROJECTOR_MAX_NAMES 8
#define PROJECTOR_FORECAST_DAYS 2
#define PROJECTOR_MAX_EVENTS 3
#define PROJECTOR_UNKNOWN_PERCENT -1
/* Thermostat temperatures are tenths of a degree in this range; outside it means unknown. */
#define PROJECTOR_TENTHS_MIN -500
#define PROJECTOR_TENTHS_MAX 1000
#define PROJECTOR_UNKNOWN_TENTHS INT16_MIN
/* Seconds on a secondary page before the clock returns; 0 keeps the page.
 * projector_page_timeout_valid() holds the accepted range. */
#define PROJECTOR_PAGE_TIMEOUT_DEFAULT 30

/* One line of a list page. */
typedef struct {
    uint8_t type;  /* WIDGET_* */
    uint8_t style; /* STYLE_* */
    char text[33]; /* Free text of a WIDGET_TEXT line. */
} projector_widget_t;

/* A page and how it shows its elements; see the page schema in projector_logic.h. */
typedef struct {
    char id[17];
    char name[17];
    uint8_t layout;        /* LAYOUT_* */
    uint8_t size;          /* SIZE_* */
    uint8_t tomorrow_from; /* Clock and weather: hour from which tomorrow's forecast shows. */
    uint8_t show[PROJECTOR_MAX_ELEMENTS]; /* Mode of each element of the layout. */
    uint8_t widget_count;
    projector_widget_t widgets[PROJECTOR_MAX_WIDGETS];
} projector_page_t;

/* Look shared by every page. */
typedef struct {
    uint8_t palette; /* PALETTE_* */
    uint32_t tint;   /* 0xrrggbb, used by PALETTE_MONO. */
    bool dots;       /* Page indicator on secondary pages. */
} projector_theme_t;

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

/* A calendar event from now to the end of tomorrow; an empty time means all day. */
typedef struct {
    char date[11]; /* HA local YYYY-MM-DD. */
    char time[6];
    char title[32];
} projector_event_t;

/* The bedroom thermostat chosen by HA: radiators in winter, air conditioner in summer. */
typedef struct {
    bool known;         /* False: not configured, unavailable or stale. */
    bool cooling;       /* The air conditioner rather than the radiators. */
    char mode[10];      /* HA hvac_mode: off, heat, cool, heat_cool, auto, dry, fan_only. */
    char action[12];    /* HA hvac_action, empty if the entity does not report it. */
    int16_t current;    /* Tenths of a degree or PROJECTOR_UNKNOWN_TENTHS. */
    int16_t target;
    int16_t min;        /* Dial scale in tenths, min < max. */
    int16_t max;
    int8_t humidity;    /* Percent or PROJECTOR_UNKNOWN_PERCENT. */
    char preset[16];    /* HA preset_mode as sent, empty if none. */
} projector_thermostat_t;

/* Everything Home Assistant sends. Unknown values: see projector_snapshot(). */
typedef struct {
    bool openings_known;
    uint8_t openings;
    bool lights_known;
    uint8_t lights;
    char alarm[24];       /* alarm_control_panel state or "unknown". */
    char weather[32];     /* Current HA condition, e.g. "partlycloudy"; empty if unknown. */
    char temperature[16]; /* Current temperature with unit, e.g. "19°C". */
    char extras[PROJECTOR_EXTRA_VALUES][32];
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
    int8_t humidity;  /* Percent or PROJECTOR_UNKNOWN_PERCENT. */
    projector_day_t days[PROJECTOR_FORECAST_DAYS];
    bool wake_known;   /* The next-alarm sensor is configured and available. */
    char wake_date[11]; /* Next phone alarm, HA local date and HH:MM; empty if none. */
    char wake_time[6];
    bool events_known;
    uint8_t event_count;
    projector_event_t events[PROJECTOR_MAX_EVENTS];
    projector_thermostat_t thermostat;
} projector_ha_t;

/* A temporary message sent by HA, shown over every page until it expires. */
typedef struct {
    bool active;
    char text[64];
    char icon[16]; /* A name from the message icon list, or empty. */
} projector_message_t;

typedef struct {
    bool power;
    uint8_t brightness;
    uint8_t page_count;
    uint8_t page_index;
    uint16_t page_timeout; /* Seconds, 0 = never return to the clock. */
    projector_page_t pages[PROJECTOR_MAX_PAGES];
    projector_theme_t theme;
    bool preview;          /* Pages and theme are an unsaved trial from the web UI. */
    bool ha_fresh;         /* False after HA_TTL without updates: ha is then all unknown. */
    projector_ha_t ha;
    bool wifi_connected;
    projector_calibration_t calibration;
    projector_message_t message;
} projector_snapshot_t;

esp_err_t projector_init(void);
void projector_snapshot(projector_snapshot_t *out);
void projector_tick(void);
esp_err_t projector_set_pages(const cJSON *root, char *error, size_t error_size);
esp_err_t projector_preview_pages(const cJSON *root, char *error, size_t error_size);
cJSON *projector_pages_json(void);
esp_err_t projector_set_command(const cJSON *root, char *error, size_t error_size);
esp_err_t projector_set_ha_state(const cJSON *root, char *error, size_t error_size);
esp_err_t projector_set_message(const cJSON *root, char *error, size_t error_size);
const char *projector_token(void);
void projector_set_wifi_connected(bool connected);
void projector_read_network(char *ssid, size_t ssid_size, char *password, size_t password_size);
esp_err_t projector_write_network(const char *ssid, const char *password, const char *timezone);
void projector_timezone(char *out, size_t size);
/* Whether Home Assistant may discover the projector via mDNS; saved in NVS. */
bool projector_discovery(void);
esp_err_t projector_set_config(const cJSON *root, char *error, size_t error_size);
esp_err_t projector_set_calibration(const cJSON *root, char *error, size_t error_size);

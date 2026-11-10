#include "projector.h"
#include "projector_logic.h"
#include "bed_build_config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* NVS namespace holding every saved setting: Wi-Fi, token, pages, calibration. */
#define NS "bed_projector"
#define HA_TTL_US (120LL * 1000000LL)
#define MESSAGE_DURATION_DEFAULT 30 /* Seconds. */
#define DEFAULT_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#define ARRAY_COUNT(items) (sizeof(items) / sizeof((items)[0]))

static const char *const layouts[] = {"clock", "home", "weather", "agenda", "climate", "list"};
static const char *const widgets[] = {
    "openings", "alarm", "date", "weather", "entity1", "entity2", "entity3", "entity4", "text",
    "indoor", "outdoor", "lights", "sun", "wake", "event", "moon", "thermostat"
};
/* Names HA may pass as a message icon; bed_ui.c maps each to its glyph. */
static const char *const message_icons[] = {
    "bell", "information", "alert", "washing-machine", "package-variant", "door-open",
    "lightbulb-on", "phone", "alarm", "calendar"
};
/* Home Assistant's HVACMode and HVACAction values. */
static const char *const hvac_modes[] = {"off", "heat", "cool", "heat_cool", "auto", "dry", "fan_only"};
static const char *const hvac_actions[] = {
    "off", "heating", "cooling", "drying", "idle", "fan", "preheating", "defrosting"
};
/* What the clock page shows of the coming morning's wake alarm; stored as the index. */
static const char *const wake_displays[] = {"none", "icon", "time", "countdown"};
static const char *const alarm_states[] = {
    "disarmed", "armed_home", "armed_away", "armed_night", "armed_vacation",
    "armed_custom_bypass", "arming", "pending", "triggered", "unknown"
};

static SemaphoreHandle_t lock;
static projector_snapshot_t state;
static int64_t ha_received_us;
static int64_t page_selected_us;
static int64_t message_shown_us;
static int64_t message_duration_us;
static char api_token[33];
static char tz_value[128] = DEFAULT_TIMEZONE;
static projector_calibration_t saved_calibration;

/* ---- Validation helpers ---- */

static bool in_list(const char *value, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(value, list[i])) return true;
    return false;
}

static bool valid_display_text(const char *value, size_t max)
{
    if (!value || strlen(value) > max) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}

static bool valid_name(const char *value, size_t max, bool identifier)
{
    if (!value || !*value || !valid_display_text(value, max)) return false;
    for (const unsigned char *p = (const unsigned char *)value; identifier && *p; ++p)
        if (!(isalnum(*p) || *p == '_')) return false;
    return true;
}

static bool is_integer(const cJSON *item, int min, int max)
{
    return cJSON_IsNumber(item) && item->valuedouble == item->valueint &&
           item->valueint >= min && item->valueint <= max;
}

/* ---- Persistence ---- */

typedef esp_err_t (*nvs_writer_t)(nvs_handle_t handle, const void *context);

/* Open the namespace, run write and commit; any failure leaves NVS unchanged. */
static esp_err_t nvs_save(nvs_writer_t write, const void *context)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = write(handle, context);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t write_presentation(nvs_handle_t handle, const void *context)
{
    const projector_snapshot_t *s = context;
    esp_err_t err = nvs_set_u8(handle, "proj_on", s->power ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(handle, "proj_bri", s->brightness);
    if (err == ESP_OK) err = nvs_set_u16(handle, "proj_return", s->page_timeout);
    for (uint8_t i = 0; err == ESP_OK && i < ARRAY_COUNT(wake_displays); ++i)
        if (!strcmp(s->wake_display, wake_displays[i])) err = nvs_set_u8(handle, "proj_wake", i);
    return err;
}

static esp_err_t write_pages(nvs_handle_t handle, const void *context)
{
    return nvs_set_str(handle, "proj_pages", context);
}

static esp_err_t write_calibration(nvs_handle_t handle, const void *context)
{
    const projector_calibration_t *c = context;
    uint8_t geometry[3] = {c->center_x, c->center_y, c->diameter};
    esp_err_t err = nvs_set_blob(handle, "proj_circle", geometry, sizeof(geometry));
    if (err == ESP_OK) err = nvs_set_u8(handle, "proj_orient", c->rotation | (c->mirror ? 4 : 0));
    return err;
}

typedef struct {
    const char *ssid, *password, *timezone;
} network_t;

static esp_err_t write_network(nvs_handle_t handle, const void *context)
{
    const network_t *n = context;
    esp_err_t err = nvs_set_str(handle, "wifi_ssid", n->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "wifi_pass", n->password);
    if (err == ESP_OK && n->timezone) err = nvs_set_str(handle, "timezone", n->timezone);
    return err;
}

/* ---- Defaults ---- */

/* HA data as shown when nothing fresh has arrived. */
static void ha_unknown(projector_ha_t *ha)
{
    memset(ha, 0, sizeof(*ha));
    strcpy(ha->alarm, "unknown");
    ha->humidity = PROJECTOR_UNKNOWN_PERCENT;
    ha->thermostat.current = ha->thermostat.target = PROJECTOR_UNKNOWN_TENTHS;
    ha->thermostat.humidity = PROJECTOR_UNKNOWN_PERCENT;
}

static void defaults(void)
{
    memset(&state, 0, sizeof(state));
    state.power = true;
    state.brightness = 30;
    state.page_timeout = PROJECTOR_PAGE_TIMEOUT_DEFAULT;
    strcpy(state.wake_display, "icon");
    state.calibration = (projector_calibration_t){.center_x=64, .center_y=64, .diameter=112};
    state.page_count = 3;
    /* clock, home and weather are fixed designs; only the list layout shows widgets. */
    state.pages[0] = (projector_page_t){.id="clock", .name="Ora", .layout="clock"};
    state.pages[1] = (projector_page_t){.id="home", .name="Casa", .layout="home"};
    state.pages[2] = (projector_page_t){.id="weather", .name="Meteo", .layout="weather"};
    ha_unknown(&state.ha);
}

/* ---- Pages ---- */

/* Caller owns the state mutex. Return NULL on any allocation failure. */
static cJSON *pages_json_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *pages = cJSON_AddArrayToObject(root, "pages");
    if (!pages) goto no_memory;
    for (unsigned i = 0; i < state.page_count; ++i) {
        const projector_page_t *page = &state.pages[i];
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(pages, item)) {
            cJSON_Delete(item);
            goto no_memory;
        }
        cJSON *list = cJSON_AddArrayToObject(item, "widgets");
        if (!cJSON_AddStringToObject(item, "id", page->id) ||
            !cJSON_AddStringToObject(item, "name", page->name) ||
            !cJSON_AddStringToObject(item, "layout", page->layout) ||
            !cJSON_AddStringToObject(item, "text", page->text) || !list) goto no_memory;
        for (unsigned j = 0; j < PROJECTOR_MAX_WIDGETS; ++j) {
            if (!page->widgets[j][0]) continue;
            cJSON *widget = cJSON_CreateString(page->widgets[j]);
            if (!widget || !cJSON_AddItemToArray(list, widget)) {
                cJSON_Delete(widget);
                goto no_memory;
            }
        }
    }
    return root;
no_memory:
    cJSON_Delete(root);
    return NULL;
}

static esp_err_t save_pages_locked(void)
{
    cJSON *root = pages_json_locked();
    char *json = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_save(write_pages, json);
    cJSON_free(json);
    return err;
}

static bool parse_page(const cJSON *item, int index, const projector_page_t *previous,
                       projector_page_t *out)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
    const cJSON *layout = cJSON_GetObjectItemCaseSensitive(item, "layout");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(item, "widgets");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
    if (!cJSON_IsString(id) || !valid_name(id->valuestring, sizeof(out->id) - 1, true) ||
        !cJSON_IsString(name) || !valid_name(name->valuestring, sizeof(out->name) - 1, false) ||
        !cJSON_IsString(layout) || !in_list(layout->valuestring, layouts, ARRAY_COUNT(layouts)) ||
        !cJSON_IsArray(list) || cJSON_GetArraySize(list) > PROJECTOR_MAX_WIDGETS ||
        (text && (!cJSON_IsString(text) ||
                  !valid_display_text(text->valuestring, sizeof(out->text) - 1)))) return false;
    /* The clock is always the first page and only the first. */
    bool clock = !strcmp(id->valuestring, "clock") || !strcmp(layout->valuestring, "clock");
    bool first_is_clock = !strcmp(id->valuestring, "clock") && !strcmp(layout->valuestring, "clock");
    if (index == 0 ? !first_is_clock : clock) return false;
    for (int i = 0; i < index; ++i)
        if (!strcmp(previous[i].id, id->valuestring)) return false;
    int slot = 0;
    const cJSON *widget;
    cJSON_ArrayForEach(widget, list) {
        if (!cJSON_IsString(widget) || !in_list(widget->valuestring, widgets, ARRAY_COUNT(widgets)))
            return false;
        strcpy(out->widgets[slot++], widget->valuestring);
    }
    strcpy(out->id, id->valuestring);
    strcpy(out->name, name->valuestring);
    strcpy(out->layout, layout->valuestring);
    if (text) strcpy(out->text, text->valuestring);
    return true;
}

static esp_err_t parse_pages(const cJSON *root, projector_page_t *parsed, uint8_t *count,
                             char *error, size_t error_size)
{
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    int length = cJSON_IsArray(pages) ? cJSON_GetArraySize(pages) : 0;
    memset(parsed, 0, sizeof(projector_page_t) * PROJECTOR_MAX_PAGES);
    bool valid = length >= 1 && length <= PROJECTOR_MAX_PAGES;
    for (int i = 0; valid && i < length; ++i)
        valid = parse_page(cJSON_GetArrayItem(pages, i), i, parsed, &parsed[i]);
    if (!valid) {
        snprintf(error, error_size, "Invalid pages: 1-%d pages with unique ids and 1-16 character "
                 "names, clock first, up to %d known widgets", PROJECTOR_MAX_PAGES,
                 PROJECTOR_MAX_WIDGETS);
        return ESP_ERR_INVALID_ARG;
    }
    *count = (uint8_t)length;
    return ESP_OK;
}

static void load_pages(nvs_handle_t handle)
{
    size_t size = 0;
    if (nvs_get_str(handle, "proj_pages", NULL, &size) != ESP_OK || size > 2048) return;
    char *json = malloc(size);
    if (json && nvs_get_str(handle, "proj_pages", json, &size) == ESP_OK) {
        cJSON *root = cJSON_Parse(json);
        projector_page_t parsed[PROJECTOR_MAX_PAGES];
        uint8_t count;
        char error[160];
        if (root && parse_pages(root, parsed, &count, error, sizeof(error)) == ESP_OK) {
            memcpy(state.pages, parsed, sizeof(parsed));
            state.page_count = count;
        }
        cJSON_Delete(root);
    }
    free(json);
}

/* ---- Startup ---- */

/* The .env Wi-Fi is written once per change, so a network set later from the
 * web UI survives reboots until the .env itself changes. */
static esp_err_t apply_build_wifi(nvs_handle_t handle)
{
    if (!BED_BUILD_WIFI_SSID[0]) return ESP_OK;
    char saved[17] = {0};
    size_t size = sizeof(saved);
    if (nvs_get_str(handle, "proj_wifi_src", saved, &size) == ESP_OK &&
        !strcmp(saved, BED_BUILD_WIFI_FINGERPRINT)) return ESP_OK;
    network_t network = {BED_BUILD_WIFI_SSID, BED_BUILD_WIFI_PASSWORD, NULL};
    esp_err_t err = write_network(handle, &network);
    return err == ESP_OK ? nvs_set_str(handle, "proj_wifi_src", BED_BUILD_WIFI_FINGERPRINT) : err;
}

/* The .env token wins; without one a saved or new random token is used. */
static esp_err_t load_token(nvs_handle_t handle)
{
    size_t size = sizeof(api_token);
    bool saved = nvs_get_str(handle, "proj_token", api_token, &size) == ESP_OK &&
        strlen(api_token) == 32 && strspn(api_token, "0123456789abcdef") == 32;
    if (BED_BUILD_API_TOKEN[0]) {
        if (saved && !strcmp(api_token, BED_BUILD_API_TOKEN)) return ESP_OK;
        strcpy(api_token, BED_BUILD_API_TOKEN);
    } else if (!saved) {
        static const char hex[] = "0123456789abcdef";
        uint8_t random_bytes[16];
        esp_fill_random(random_bytes, sizeof(random_bytes));
        for (unsigned i = 0; i < sizeof(random_bytes); ++i) {
            api_token[i * 2] = hex[random_bytes[i] >> 4];
            api_token[i * 2 + 1] = hex[random_bytes[i] & 15];
        }
        api_token[32] = 0;
    } else return ESP_OK;
    return nvs_set_str(handle, "proj_token", api_token);
}

static void load_presentation(nvs_handle_t handle)
{
    uint8_t value;
    uint16_t timeout;
    if (nvs_get_u8(handle, "proj_bri", &value) == ESP_OK && value >= 1 && value <= 100)
        state.brightness = value;
    if (nvs_get_u8(handle, "proj_on", &value) == ESP_OK) state.power = value != 0;
    if (nvs_get_u16(handle, "proj_return", &timeout) == ESP_OK && projector_page_timeout_valid(timeout))
        state.page_timeout = timeout;
    if (nvs_get_u8(handle, "proj_wake", &value) == ESP_OK && value < ARRAY_COUNT(wake_displays))
        strcpy(state.wake_display, wake_displays[value]);
    uint8_t geometry[3];
    size_t size = sizeof(geometry);
    if (nvs_get_blob(handle, "proj_circle", geometry, &size) == ESP_OK && size == sizeof(geometry) &&
        projector_calibration_valid(geometry[0], geometry[1], geometry[2])) {
        state.calibration.center_x = geometry[0];
        state.calibration.center_y = geometry[1];
        state.calibration.diameter = geometry[2];
    }
    if (nvs_get_u8(handle, "proj_orient", &value) == ESP_OK && value < 8) {
        state.calibration.rotation = value & 3;
        state.calibration.mirror = value & 4;
    }
    saved_calibration = state.calibration;
    size = sizeof(tz_value);
    if (nvs_get_str(handle, "timezone", tz_value, &size) != ESP_OK || !tz_value[0])
        strcpy(tz_value, DEFAULT_TIMEZONE);
}

esp_err_t projector_init(void)
{
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;
    defaults();
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    load_presentation(handle);
    load_pages(handle);
    err = apply_build_wifi(handle);
    if (err == ESP_OK) err = load_token(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/* ---- State access ---- */

void projector_snapshot(projector_snapshot_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = state;
    out->ha_fresh = projector_data_fresh(ha_received_us, esp_timer_get_time(), HA_TTL_US);
    if (!out->ha_fresh) ha_unknown(&out->ha);
    xSemaphoreGive(lock);
}

void projector_tick(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (projector_page_timed_out(state.page_index, page_selected_us,
                                 esp_timer_get_time(), state.page_timeout * 1000000LL))
        state.page_index = 0;
    if (state.message.active &&
        projector_expired(message_shown_us, esp_timer_get_time(), message_duration_us))
        memset(&state.message, 0, sizeof(state.message));
    xSemaphoreGive(lock);
}

esp_err_t projector_set_pages(const cJSON *root, char *error, size_t error_size)
{
    projector_page_t parsed[PROJECTOR_MAX_PAGES];
    uint8_t count;
    esp_err_t err = parse_pages(root, parsed, &count, error, error_size);
    if (err != ESP_OK) return err;
    xSemaphoreTake(lock, portMAX_DELAY);
    projector_snapshot_t previous = state;
    int64_t previous_selected_us = page_selected_us;
    /* Stay on the shown page when it still exists. */
    char current_id[sizeof(state.pages[0].id)];
    strcpy(current_id, state.pages[state.page_index].id);
    memcpy(state.pages, parsed, sizeof(parsed));
    state.page_count = count;
    state.page_index = 0;
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(current_id, state.pages[i].id)) state.page_index = i;
    page_selected_us = esp_timer_get_time();
    err = save_pages_locked();
    if (err != ESP_OK) {
        memcpy(state.pages, previous.pages, sizeof(state.pages));
        state.page_count = previous.page_count;
        state.page_index = previous.page_index;
        page_selected_us = previous_selected_us;
        snprintf(error, error_size, "Saving pages failed");
    }
    xSemaphoreGive(lock);
    return err;
}

cJSON *projector_pages_json(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON *root = pages_json_locked();
    xSemaphoreGive(lock);
    return root;
}

esp_err_t projector_set_command(const cJSON *root, char *error, size_t error_size)
{
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");
    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    const cJSON *page = cJSON_GetObjectItemCaseSensitive(root, "page");
    const cJSON *move = cJSON_GetObjectItemCaseSensitive(root, "move");
    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(root, "page_timeout");
    const cJSON *wake = cJSON_GetObjectItemCaseSensitive(root, "wake_display");
    bool valid_move = !move || (cJSON_IsString(move) &&
        (!strcmp(move->valuestring, "next") || !strcmp(move->valuestring, "previous")));
    if ((power && !cJSON_IsBool(power)) || (brightness && !is_integer(brightness, 1, 100)) ||
        (page && !cJSON_IsString(page)) || !valid_move ||
        (timeout && (!is_integer(timeout, 0, 65535) || !projector_page_timeout_valid(timeout->valueint))) ||
        (wake && (!cJSON_IsString(wake) ||
                  !in_list(wake->valuestring, wake_displays, ARRAY_COUNT(wake_displays)))) ||
        (page && move) || (!power && !brightness && !page && !move && !timeout && !wake)) {
        snprintf(error, error_size, "Invalid command");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    int next_page = state.page_index;
    if (page) {
        next_page = -1;
        for (unsigned i = 0; i < state.page_count; ++i)
            if (!strcmp(page->valuestring, state.pages[i].id)) next_page = i;
    } else if (move) {
        next_page = projector_move_page(state.page_index, state.page_count,
                                        !strcmp(move->valuestring, "next"));
    }
    projector_snapshot_t next = state;
    if (power) next.power = cJSON_IsTrue(power);
    if (brightness) next.brightness = brightness->valueint;
    if (timeout) next.page_timeout = timeout->valueint;
    if (wake) strcpy(next.wake_display, wake->valuestring);
    esp_err_t err = ESP_OK;
    if (next_page < 0) {
        err = ESP_ERR_INVALID_ARG;
        snprintf(error, error_size, "Unknown page");
    } else if (next.power != state.power || next.brightness != state.brightness ||
               next.page_timeout != state.page_timeout ||
               strcmp(next.wake_display, state.wake_display)) {
        err = nvs_save(write_presentation, &next);
        if (err != ESP_OK) snprintf(error, error_size, "NVS write failed");
    }
    if (err == ESP_OK) {
        state.power = next.power;
        state.brightness = next.brightness;
        state.page_timeout = next.page_timeout;
        strcpy(state.wake_display, next.wake_display);
        state.page_index = next_page;
        if (page || move) page_selected_us = esp_timer_get_time();
    }
    xSemaphoreGive(lock);
    return err;
}

/* ---- Home Assistant state ---- */

/* Optional whole-degree temperature: absent or null means unknown. */
static bool parse_degrees(const cJSON *item, bool *known, int16_t *value)
{
    *known = false;
    if (!item || cJSON_IsNull(item)) return true;
    if (!is_integer(item, -99, 199)) return false;
    *known = true;
    *value = item->valueint;
    return true;
}

/* Optional display string: absent or null is empty, otherwise it must fit out. */
static bool parse_text(const cJSON *item, char *out, size_t size)
{
    out[0] = 0;
    if (!item || cJSON_IsNull(item)) return true;
    if (!cJSON_IsString(item) || !valid_display_text(item->valuestring, size - 1)) return false;
    strcpy(out, item->valuestring);
    return true;
}

/* Required display string. */
static bool parse_required_text(const cJSON *item, char *out, size_t size)
{
    return cJSON_IsString(item) && parse_text(item, out, size);
}

/* Optional 0-100 percentage: absent or null means unknown. */
static bool parse_percent(const cJSON *item, int8_t *out)
{
    *out = PROJECTOR_UNKNOWN_PERCENT;
    if (!item || cJSON_IsNull(item)) return true;
    if (!is_integer(item, 0, 100)) return false;
    *out = item->valueint;
    return true;
}

/* A count of open doors or lights on, present only when known. */
static bool parse_count(const cJSON *known, const cJSON *count, bool *out_known, uint8_t *out)
{
    if (!cJSON_IsBool(known)) return false;
    *out_known = cJSON_IsTrue(known);
    if (!*out_known) return true;
    if (!is_integer(count, 0, 99)) return false;
    *out = count->valueint;
    return true;
}

/* Optional array of non-empty names. */
static bool parse_names(const cJSON *item, char names[][32], uint8_t *count)
{
    *count = 0;
    if (!item) return true;
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) > PROJECTOR_MAX_NAMES) return false;
    const cJSON *name;
    cJSON_ArrayForEach(name, item) {
        if (!parse_required_text(name, names[*count], 32) || !names[*count][0]) return false;
        ++*count;
    }
    return true;
}

static bool parse_period(const cJSON *item, projector_period_t *out)
{
    memset(out, 0, sizeof(*out));
    out->precipitation = PROJECTOR_UNKNOWN_PERCENT;
    if (!item || cJSON_IsNull(item)) return true;
#define FIELD(name) cJSON_GetObjectItemCaseSensitive(item, name)
    return cJSON_IsObject(item) &&
        parse_text(FIELD("condition"), out->condition, sizeof(out->condition)) &&
        parse_degrees(FIELD("temperature"), &out->temperature_known, &out->temperature) &&
        parse_percent(FIELD("precipitation"), &out->precipitation);
#undef FIELD
}

static bool parse_day(const cJSON *item, projector_day_t *out)
{
    memset(out, 0, sizeof(*out));
    out->precipitation = PROJECTOR_UNKNOWN_PERCENT;
    out->day.precipitation = out->evening.precipitation = PROJECTOR_UNKNOWN_PERCENT;
    if (!item) return true;
    if (!cJSON_IsObject(item)) return false;
#define FIELD(name) cJSON_GetObjectItemCaseSensitive(item, name)
    return parse_text(FIELD("date"), out->date, sizeof(out->date)) &&
        parse_text(FIELD("condition"), out->condition, sizeof(out->condition)) &&
        parse_degrees(FIELD("high"), &out->high_known, &out->high) &&
        parse_degrees(FIELD("low"), &out->low_known, &out->low) &&
        parse_percent(FIELD("precipitation"), &out->precipitation) &&
        parse_text(FIELD("sunrise"), out->sunrise, sizeof(out->sunrise)) &&
        parse_text(FIELD("sunset"), out->sunset, sizeof(out->sunset)) &&
        parse_text(FIELD("summary"), out->summary, sizeof(out->summary)) &&
        parse_period(FIELD("day"), &out->day) &&
        parse_period(FIELD("evening"), &out->evening);
#undef FIELD
}

/* Optional array of calendar events, each with a date and a title. */
static bool parse_events(const cJSON *item, projector_event_t *events, uint8_t *count)
{
    *count = 0;
    if (!item) return true;
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) > PROJECTOR_MAX_EVENTS) return false;
    const cJSON *event;
    cJSON_ArrayForEach(event, item) {
        projector_event_t *out = &events[*count];
#define FIELD(name) cJSON_GetObjectItemCaseSensitive(event, name)
        if (!cJSON_IsObject(event) || !parse_required_text(FIELD("date"), out->date, sizeof(out->date)) ||
            !parse_text(FIELD("time"), out->time, sizeof(out->time)) ||
            !parse_required_text(FIELD("title"), out->title, sizeof(out->title)) ||
            !out->date[0] || !out->title[0]) return false;
#undef FIELD
        ++*count;
    }
    return true;
}

/* The next phone alarm: null means none, otherwise both date and time. */
static bool parse_wake(const cJSON *item, projector_ha_t *out)
{
    if (!item || cJSON_IsNull(item)) return true;
    return cJSON_IsObject(item) &&
        parse_required_text(cJSON_GetObjectItemCaseSensitive(item, "date"), out->wake_date,
                            sizeof(out->wake_date)) &&
        parse_required_text(cJSON_GetObjectItemCaseSensitive(item, "time"), out->wake_time,
                            sizeof(out->wake_time)) &&
        out->wake_date[0] && out->wake_time[0];
}

/* Optional temperature in tenths: absent or null means unknown. */
static bool parse_tenths(const cJSON *item, int16_t *out)
{
    *out = PROJECTOR_UNKNOWN_TENTHS;
    if (!item || cJSON_IsNull(item)) return true;
    if (!is_integer(item, PROJECTOR_TENTHS_MIN, PROJECTOR_TENTHS_MAX)) return false;
    *out = item->valueint;
    return true;
}

/* The thermostat: null means unknown, otherwise a source, a known mode and a dial scale. */
static bool parse_thermostat(const cJSON *item, projector_thermostat_t *out)
{
    if (!item || cJSON_IsNull(item)) return true;
    if (!cJSON_IsObject(item)) return false;
#define FIELD(name) cJSON_GetObjectItemCaseSensitive(item, name)
    const cJSON *source = FIELD("source"), *mode = FIELD("mode"), *action = FIELD("action");
    const cJSON *min = FIELD("min"), *max = FIELD("max");
    if (!cJSON_IsString(source) ||
        (strcmp(source->valuestring, "heat") && strcmp(source->valuestring, "cool")) ||
        !cJSON_IsString(mode) || !in_list(mode->valuestring, hvac_modes, ARRAY_COUNT(hvac_modes)) ||
        (action && !cJSON_IsNull(action) && (!cJSON_IsString(action) ||
            !in_list(action->valuestring, hvac_actions, ARRAY_COUNT(hvac_actions)))) ||
        !is_integer(min, PROJECTOR_TENTHS_MIN, PROJECTOR_TENTHS_MAX) ||
        !is_integer(max, PROJECTOR_TENTHS_MIN, PROJECTOR_TENTHS_MAX) || min->valueint >= max->valueint ||
        !parse_tenths(FIELD("current"), &out->current) || !parse_tenths(FIELD("target"), &out->target) ||
        !parse_percent(FIELD("humidity"), &out->humidity) ||
        !parse_text(FIELD("preset"), out->preset, sizeof(out->preset))) return false;
#undef FIELD
    out->known = true;
    out->cooling = !strcmp(source->valuestring, "cool");
    strcpy(out->mode, mode->valuestring);
    if (cJSON_IsString(action)) strcpy(out->action, action->valuestring);
    out->min = min->valueint;
    out->max = max->valueint;
    return true;
}

static bool parse_ha(const cJSON *root, projector_ha_t *out)
{
    ha_unknown(out);
#define FIELD(name) cJSON_GetObjectItemCaseSensitive(root, name)
    const cJSON *alarm = FIELD("alarm"), *extras = FIELD("extras"), *days = FIELD("days");
    if (!cJSON_IsString(alarm) || !in_list(alarm->valuestring, alarm_states, ARRAY_COUNT(alarm_states)) ||
        !cJSON_IsArray(extras) || cJSON_GetArraySize(extras) > PROJECTOR_EXTRA_VALUES ||
        (days && (!cJSON_IsArray(days) || cJSON_GetArraySize(days) > PROJECTOR_FORECAST_DAYS)))
        return false;
    strcpy(out->alarm, alarm->valuestring);
    out->wake_known = cJSON_IsTrue(FIELD("wake_known"));
    out->events_known = cJSON_IsTrue(FIELD("events_known"));
    int index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, extras)
        if (!parse_required_text(item, out->extras[index++], sizeof(out->extras[0]))) return false;
    for (index = 0; index < PROJECTOR_FORECAST_DAYS; ++index)
        if (!parse_day(days ? cJSON_GetArrayItem(days, index) : NULL, &out->days[index])) return false;
    return parse_count(FIELD("openings_known"), FIELD("openings"), &out->openings_known, &out->openings) &&
        parse_count(FIELD("lights_known"), FIELD("lights"), &out->lights_known, &out->lights) &&
        parse_required_text(FIELD("weather"), out->weather, sizeof(out->weather)) &&
        parse_required_text(FIELD("temperature"), out->temperature, sizeof(out->temperature)) &&
        parse_names(FIELD("opening_names"), out->opening_names, &out->opening_name_count) &&
        parse_names(FIELD("light_names"), out->light_names, &out->light_name_count) &&
        parse_text(FIELD("indoor"), out->indoor, sizeof(out->indoor)) &&
        parse_text(FIELD("outdoor"), out->outdoor, sizeof(out->outdoor)) &&
        parse_text(FIELD("feels"), out->feels, sizeof(out->feels)) &&
        parse_text(FIELD("wind"), out->wind, sizeof(out->wind)) &&
        parse_degrees(FIELD("temp_now"), &out->temp_now_known, &out->temp_now) &&
        parse_percent(FIELD("humidity"), &out->humidity) &&
        cJSON_IsBool(FIELD("wake_known")) && parse_wake(FIELD("wake"), out) &&
        cJSON_IsBool(FIELD("events_known")) &&
        parse_events(FIELD("events"), out->events, &out->event_count) &&
        parse_thermostat(FIELD("thermostat"), &out->thermostat);
#undef FIELD
}

esp_err_t projector_set_ha_state(const cJSON *root, char *error, size_t error_size)
{
    /* About 1.5 KB: static keeps it off the HTTP server task stack; requests are serialized. */
    static projector_ha_t parsed;
    if (!parse_ha(root, &parsed)) {
        snprintf(error, error_size, "Invalid HA state");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    state.ha = parsed;
    ha_received_us = esp_timer_get_time();
    xSemaphoreGive(lock);
    return ESP_OK;
}

/* ---- Messages ---- */

esp_err_t projector_set_message(const cJSON *root, char *error, size_t error_size)
{
    const cJSON *clear = cJSON_GetObjectItemCaseSensitive(root, "clear");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *icon = cJSON_GetObjectItemCaseSensitive(root, "icon");
    const cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
    projector_message_t message = {0};
    bool clearing = cJSON_IsTrue(clear) && !text && !icon && !duration;
    bool showing = !clear && parse_required_text(text, message.text, sizeof(message.text)) &&
        message.text[0] && (!icon || (cJSON_IsString(icon) &&
            in_list(icon->valuestring, message_icons, ARRAY_COUNT(message_icons)))) &&
        (!duration || is_integer(duration, 5, 3600));
    if (!clearing && !showing) {
        snprintf(error, error_size, "Invalid message: text of 1-63 bytes, known icon, duration 5-3600 s");
        return ESP_ERR_INVALID_ARG;
    }
    message.active = showing;
    if (showing && icon) strcpy(message.icon, icon->valuestring);
    xSemaphoreTake(lock, portMAX_DELAY);
    state.message = message;
    message_shown_us = esp_timer_get_time();
    message_duration_us = (duration ? duration->valueint : MESSAGE_DURATION_DEFAULT) * 1000000LL;
    xSemaphoreGive(lock);
    return ESP_OK;
}

/* ---- Network and identity ---- */

const char *projector_token(void) { return api_token; }

void projector_set_wifi_connected(bool connected)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    state.wifi_connected = connected;
    xSemaphoreGive(lock);
}

void projector_read_network(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    ssid[0] = password[0] = 0;
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return;
    if (nvs_get_str(handle, "wifi_ssid", ssid, &ssid_size) != ESP_OK) ssid[0] = 0;
    if (nvs_get_str(handle, "wifi_pass", password, &password_size) != ESP_OK) password[0] = 0;
    nvs_close(handle);
}

esp_err_t projector_write_network(const char *ssid, const char *password, const char *timezone)
{
    if (!valid_name(ssid, 32, false) || !password || strlen(password) > 63 ||
        (password[0] && strlen(password) < 8) || !valid_name(timezone, sizeof(tz_value) - 1, false))
        return ESP_ERR_INVALID_ARG;
    network_t network = {ssid, password, timezone};
    esp_err_t err = nvs_save(write_network, &network);
    if (err == ESP_OK) {
        xSemaphoreTake(lock, portMAX_DELAY);
        strcpy(tz_value, timezone);
        xSemaphoreGive(lock);
    }
    return err;
}

void projector_timezone(char *out, size_t size)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(out, size, "%s", tz_value);
    xSemaphoreGive(lock);
}

/* ---- Calibration ---- */

esp_err_t projector_set_calibration(const cJSON *root, char *error, size_t error_size)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(root, "center_x");
    const cJSON *y = cJSON_GetObjectItemCaseSensitive(root, "center_y");
    const cJSON *diameter = cJSON_GetObjectItemCaseSensitive(root, "diameter");
    const cJSON *rotation = cJSON_GetObjectItemCaseSensitive(root, "rotation");
    const cJSON *mirror = cJSON_GetObjectItemCaseSensitive(root, "mirror");
    bool rotation_valid = !rotation || (is_integer(rotation, 0, 270) && rotation->valueint % 90 == 0);
    bool adjust = !mode && is_integer(x, 0, 128) && is_integer(y, 0, 128) &&
        is_integer(diameter, 0, 128) &&
        projector_calibration_valid(x->valueint, y->valueint, diameter->valueint) &&
        rotation_valid && (!mirror || cJSON_IsBool(mirror));
    static const char *const actions[] = {"start", "save", "cancel"};
    bool action = cJSON_IsString(mode) && !x && !y && !diameter && !rotation && !mirror &&
        in_list(mode->valuestring, actions, ARRAY_COUNT(actions));
    if (!adjust && !action) {
        snprintf(error, error_size, "Invalid circle geometry, orientation or action");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    projector_calibration_t *c = &state.calibration;
    esp_err_t err = ESP_OK;
    if (action && !strcmp(mode->valuestring, "start")) {
        *c = saved_calibration;
        c->active = true;
    } else if (action && !strcmp(mode->valuestring, "cancel")) {
        *c = saved_calibration;
    } else if (!c->active) {
        err = ESP_ERR_INVALID_STATE;
        snprintf(error, error_size, "Start calibration first");
    } else if (adjust) {
        c->center_x = x->valueint;
        c->center_y = y->valueint;
        c->diameter = diameter->valueint;
        if (rotation) c->rotation = rotation->valueint / 90;
        if (mirror) c->mirror = cJSON_IsTrue(mirror);
    } else { /* save */
        err = nvs_save(write_calibration, c);
        if (err == ESP_OK) {
            c->active = false;
            saved_calibration = *c;
        } else snprintf(error, error_size, "NVS write failed");
    }
    xSemaphoreGive(lock);
    return err;
}

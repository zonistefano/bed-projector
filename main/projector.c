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

#define NS "frixos"
#define HA_TTL_US (120LL * 1000000LL)
#define PAGE_TIMEOUT_US (30LL * 1000000LL)

static SemaphoreHandle_t lock;
static projector_snapshot_t state;
static int64_t ha_received_us;
static int64_t page_selected_us;
static char api_token[33];
static char tz_value[128] = "CET-1CEST,M3.5.0,M10.5.0/3";
static projector_calibration_t saved_calibration;

static bool valid_display_text(const char *value, size_t max)
{
    if (!value || strlen(value) > max) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}

static bool valid_name(const char *value, size_t max, bool identifier)
{
    if (!value || !*value || strlen(value) > max) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p < 32 || *p == 127) return false;
        if (identifier && !(isalnum(*p) || *p == '_')) return false;
    }
    return true;
}

static bool valid_widget(const char *widget)
{
    static const char *const options[] = {
        "openings", "alarm", "date", "weather", "entity1", "entity2",
        "entity3", "entity4", "text"
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i)
        if (strcmp(widget, options[i]) == 0) return true;
    return false;
}

static void defaults(void)
{
    memset(&state, 0, sizeof(state));
    state.power = true;
    state.brightness = 30;
    state.calibration = (projector_calibration_t){.center_x=64, .center_y=64, .diameter=112};
    state.page_count = 3;
    state.pages[0] = (projector_page_t){.id="clock", .name="Ora", .layout="clock",
        .widgets={"openings", "alarm", "date"}};
    state.pages[1] = (projector_page_t){.id="home", .name="Casa", .layout="home",
        .widgets={"openings", "alarm", "entity1"}};
    state.pages[2] = (projector_page_t){.id="weather", .name="Meteo", .layout="weather",
        .widgets={"weather", "date", "entity2"}};
    strcpy(state.alarm, "unknown");
}

static esp_err_t save_presentation(bool power, uint8_t brightness)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "proj_on", power ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(handle, "proj_bri", brightness);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/* Caller owns the state mutex. Return NULL on any allocation failure. */
static cJSON *pages_json_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *pages = cJSON_AddArrayToObject(root, "pages");
    if (!pages) goto no_memory;
    for (unsigned i = 0; i < state.page_count; ++i) {
        const projector_page_t *page = &state.pages[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) goto no_memory;
        if (!cJSON_AddItemToArray(pages, item)) {
            cJSON_Delete(item);
            goto no_memory;
        }
        if (!cJSON_AddStringToObject(item, "id", page->id) ||
            !cJSON_AddStringToObject(item, "name", page->name) ||
            !cJSON_AddStringToObject(item, "layout", page->layout) ||
            !cJSON_AddStringToObject(item, "text", page->text)) goto no_memory;
        cJSON *widgets = cJSON_AddArrayToObject(item, "widgets");
        if (!widgets) goto no_memory;
        for (unsigned j = 0; j < PROJECTOR_MAX_WIDGETS; ++j)
            if (page->widgets[j][0]) {
                cJSON *widget = cJSON_CreateString(page->widgets[j]);
                if (!widget) goto no_memory;
                if (!cJSON_AddItemToArray(widgets, widget)) {
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
    if (!root) return ESP_ERR_NO_MEM;
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "proj_pages", json);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    cJSON_free(json);
    return err;
}

static esp_err_t parse_pages(const cJSON *root, projector_page_t *parsed, uint8_t *count,
                              char *error, size_t error_size)
{
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    int length = cJSON_IsArray(pages) ? cJSON_GetArraySize(pages) : 0;
    if (length < 1 || length > PROJECTOR_MAX_PAGES) goto invalid;
    memset(parsed, 0, sizeof(projector_page_t) * PROJECTOR_MAX_PAGES);
    for (int i = 0; i < length; ++i) {
        const cJSON *item = cJSON_GetArrayItem(pages, i);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *layout = cJSON_GetObjectItemCaseSensitive(item, "layout");
        const cJSON *widgets = cJSON_GetObjectItemCaseSensitive(item, "widgets");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
        if (!cJSON_IsString(id) || !valid_name(id->valuestring, 16, true) ||
            !cJSON_IsString(name) || !valid_name(name->valuestring, 16, false) ||
            !cJSON_IsString(layout) || !cJSON_IsArray(widgets)) goto invalid;
        if (strcmp(layout->valuestring, "clock") && strcmp(layout->valuestring, "home") &&
            strcmp(layout->valuestring, "weather")) goto invalid;
        if ((i == 0 && (strcmp(id->valuestring, "clock") || strcmp(layout->valuestring, "clock"))) ||
            (i != 0 && (!strcmp(id->valuestring, "clock") || !strcmp(layout->valuestring, "clock")))) goto invalid;
        for (int previous = 0; previous < i; ++previous)
            if (!strcmp(parsed[previous].id, id->valuestring)) goto invalid;
        int widget_count = cJSON_GetArraySize(widgets);
        if (widget_count > PROJECTOR_MAX_WIDGETS) goto invalid;
        if (i == 0 && (widget_count < 2 ||
            !cJSON_IsString(cJSON_GetArrayItem(widgets, 0)) ||
            !cJSON_IsString(cJSON_GetArrayItem(widgets, 1)) ||
            strcmp(cJSON_GetArrayItem(widgets, 0)->valuestring, "openings") ||
            strcmp(cJSON_GetArrayItem(widgets, 1)->valuestring, "alarm"))) goto invalid;
        for (int j = 0; j < widget_count; ++j) {
            const cJSON *widget = cJSON_GetArrayItem(widgets, j);
            if (!cJSON_IsString(widget) || !valid_widget(widget->valuestring)) goto invalid;
            strcpy(parsed[i].widgets[j], widget->valuestring);
        }
        if (text && (!cJSON_IsString(text) || !valid_display_text(text->valuestring, 32))) goto invalid;
        strcpy(parsed[i].id, id->valuestring);
        strcpy(parsed[i].name, name->valuestring);
        strcpy(parsed[i].layout, layout->valuestring);
        if (text) strcpy(parsed[i].text, text->valuestring);
    }
    *count = (uint8_t)length;
    return ESP_OK;
invalid:
    snprintf(error, error_size, "Invalid pages: 1-5 unique pages, clock first, up to 3 valid widgets");
    return ESP_ERR_INVALID_ARG;
}

esp_err_t projector_init(void)
{
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;
    defaults();
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (BED_BUILD_WIFI_SSID[0]) {
        char saved_fingerprint[17] = {0};
        size_t fingerprint_size = sizeof(saved_fingerprint);
        if (nvs_get_str(handle, "proj_wifi_src", saved_fingerprint, &fingerprint_size) != ESP_OK ||
            strcmp(saved_fingerprint, BED_BUILD_WIFI_FINGERPRINT) != 0) {
            err = nvs_set_str(handle, "wifi_ssid", BED_BUILD_WIFI_SSID);
            if (err == ESP_OK) err = nvs_set_str(handle, "wifi_pass", BED_BUILD_WIFI_PASSWORD);
            if (err == ESP_OK) err = nvs_set_str(handle, "proj_wifi_src", BED_BUILD_WIFI_FINGERPRINT);
            if (err != ESP_OK) goto finish;
        }
    }
    uint8_t value;
    if (nvs_get_u8(handle, "proj_bri", &value) == ESP_OK && value >= 1 && value <= 100)
        state.brightness = value;
    else {
        uint8_t legacy[2];
        size_t size = sizeof(legacy);
        if (nvs_get_blob(handle, "brightness", legacy, &size) == ESP_OK &&
            size == sizeof(legacy) && legacy[1] >= 1 && legacy[1] <= 100)
            state.brightness = legacy[1];
        err = nvs_set_u8(handle, "proj_bri", state.brightness);
        if (err != ESP_OK) goto finish;
    }
    if (nvs_get_u8(handle, "proj_on", &value) == ESP_OK) state.power = value != 0;
    else {
        err = nvs_set_u8(handle, "proj_on", 1);
        if (err != ESP_OK) goto finish;
    }
    uint8_t geometry[3];
    size_t geometry_size = sizeof(geometry);
    if (nvs_get_blob(handle, "proj_circle", geometry, &geometry_size) == ESP_OK &&
        geometry_size == sizeof(geometry) &&
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
    size_t size = sizeof(tz_value);
    if (nvs_get_str(handle, "timezone", tz_value, &size) != ESP_OK || !tz_value[0])
        strcpy(tz_value, "CET-1CEST,M3.5.0,M10.5.0/3");
    size = 0;
    if (nvs_get_str(handle, "proj_pages", NULL, &size) == ESP_OK && size <= 2048) {
        char *json = malloc(size);
        if (json && nvs_get_str(handle, "proj_pages", json, &size) == ESP_OK) {
            cJSON *root = cJSON_Parse(json);
            projector_page_t parsed[PROJECTOR_MAX_PAGES];
            uint8_t count;
            char error[96];
            if (root && parse_pages(root, parsed, &count, error, sizeof(error)) == ESP_OK) {
                memcpy(state.pages, parsed, sizeof(parsed));
                state.page_count = count;
            }
            cJSON_Delete(root);
        }
        free(json);
    }
    size = sizeof(api_token);
    bool saved_token_valid = nvs_get_str(handle, "proj_token", api_token, &size) == ESP_OK &&
        strlen(api_token) == 32 && strspn(api_token, "0123456789abcdef") == 32;
    if (BED_BUILD_API_TOKEN[0]) {
        if (!saved_token_valid || strcmp(api_token, BED_BUILD_API_TOKEN) != 0) {
            strcpy(api_token, BED_BUILD_API_TOKEN);
            err = nvs_set_str(handle, "proj_token", api_token);
            if (err != ESP_OK) goto finish;
        }
    } else if (!saved_token_valid) {
        static const char hex[] = "0123456789abcdef";
        uint8_t random_bytes[16];
        esp_fill_random(random_bytes, sizeof(random_bytes));
        for (unsigned i = 0; i < sizeof(random_bytes); ++i) {
            api_token[i * 2] = hex[random_bytes[i] >> 4];
            api_token[i * 2 + 1] = hex[random_bytes[i] & 15];
        }
        api_token[32] = 0;
        err = nvs_set_str(handle, "proj_token", api_token);
        if (err != ESP_OK) goto finish;
    }
    err = nvs_commit(handle);
finish:
    nvs_close(handle);
    return err;
}

void projector_snapshot(projector_snapshot_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(out, &state, sizeof(*out));
    out->ha_fresh = projector_data_fresh(ha_received_us, esp_timer_get_time(), HA_TTL_US);
    if (!out->ha_fresh) {
        out->openings_known = false;
        strcpy(out->alarm, "unknown");
        out->weather[0] = 0;
        out->temperature[0] = 0;
        out->condition[0] = 0;
        out->temp_high_known = out->temp_low_known = false;
        for (unsigned i = 0; i < PROJECTOR_EXTRA_VALUES; ++i) out->extras[i][0] = 0;
    }
    xSemaphoreGive(lock);
}

void projector_tick(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (projector_page_timed_out(state.page_index, page_selected_us,
                                 esp_timer_get_time(), PAGE_TIMEOUT_US))
        state.page_index = 0;
    xSemaphoreGive(lock);
}

esp_err_t projector_set_pages(const cJSON *root, char *error, size_t error_size)
{
    projector_page_t parsed[PROJECTOR_MAX_PAGES];
    uint8_t count;
    esp_err_t err = parse_pages(root, parsed, &count, error, error_size);
    if (err != ESP_OK) return err;
    xSemaphoreTake(lock, portMAX_DELAY);
    projector_page_t previous_pages[PROJECTOR_MAX_PAGES];
    memcpy(previous_pages, state.pages, sizeof(previous_pages));
    uint8_t previous_count = state.page_count;
    uint8_t previous_index = state.page_index;
    int64_t previous_selected_us = page_selected_us;
    char current_id[17];
    strcpy(current_id, state.pages[state.page_index].id);
    memcpy(state.pages, parsed, sizeof(parsed));
    state.page_count = count;
    state.page_index = 0;
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(current_id, state.pages[i].id)) state.page_index = i;
    page_selected_us = esp_timer_get_time();
    err = save_pages_locked();
    if (err != ESP_OK) {
        memcpy(state.pages, previous_pages, sizeof(previous_pages));
        state.page_count = previous_count;
        state.page_index = previous_index;
        page_selected_us = previous_selected_us;
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
    bool invalid_move = move && (!cJSON_IsString(move) ||
        (strcmp(move->valuestring, "next") && strcmp(move->valuestring, "previous")));
    if ((power && !cJSON_IsBool(power)) ||
        (brightness && (!cJSON_IsNumber(brightness) || brightness->valuedouble != brightness->valueint ||
                        brightness->valueint < 1 || brightness->valueint > 100)) ||
        (page && !cJSON_IsString(page)) ||
        invalid_move ||
        (page && move) || (!power && !brightness && !page && !move)) {
        snprintf(error, error_size, "Invalid command");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    int next_page = state.page_index;
    if (page) {
        next_page = -1;
        for (unsigned i = 0; i < state.page_count; ++i)
            if (!strcmp(page->valuestring, state.pages[i].id)) next_page = i;
        if (next_page < 0) {
            xSemaphoreGive(lock);
            snprintf(error, error_size, "Unknown page");
            return ESP_ERR_INVALID_ARG;
        }
    } else if (move) {
        next_page = projector_move_page(state.page_index, state.page_count,
                                        !strcmp(move->valuestring, "next"));
    }
    bool new_power = power ? cJSON_IsTrue(power) : state.power;
    uint8_t new_brightness = brightness ? brightness->valueint : state.brightness;
    esp_err_t err = ESP_OK;
    if (new_power != state.power || new_brightness != state.brightness)
        err = save_presentation(new_power, new_brightness);
    if (err != ESP_OK) {
        xSemaphoreGive(lock);
        snprintf(error, error_size, "NVS write failed");
        return err;
    }
    state.power = new_power;
    state.brightness = new_brightness;
    state.page_index = next_page;
    if (page || move) page_selected_us = esp_timer_get_time();
    xSemaphoreGive(lock);
    return err;
}

/* Optional whole-degree forecast temperature: absent or null means unknown. */
static bool parse_degrees(const cJSON *item, bool *known, int16_t *value)
{
    *known = false;
    if (!item || cJSON_IsNull(item)) return true;
    if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
        item->valueint < -99 || item->valueint > 199) return false;
    *known = true;
    *value = item->valueint;
    return true;
}

esp_err_t projector_set_ha_state(const cJSON *root, char *error, size_t error_size)
{
    const cJSON *openings = cJSON_GetObjectItemCaseSensitive(root, "openings");
    const cJSON *openings_known = cJSON_GetObjectItemCaseSensitive(root, "openings_known");
    const cJSON *alarm = cJSON_GetObjectItemCaseSensitive(root, "alarm");
    const cJSON *weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    const cJSON *extras = cJSON_GetObjectItemCaseSensitive(root, "extras");
    const cJSON *condition = cJSON_GetObjectItemCaseSensitive(root, "condition");
    bool high_known, low_known;
    int16_t high = 0, low = 0;
    if (!parse_degrees(cJSON_GetObjectItemCaseSensitive(root, "temp_high"), &high_known, &high) ||
        !parse_degrees(cJSON_GetObjectItemCaseSensitive(root, "temp_low"), &low_known, &low) ||
        (condition && (!cJSON_IsString(condition) || strlen(condition->valuestring) > 15))) goto invalid;
    if (!cJSON_IsBool(openings_known) || !cJSON_IsNumber(openings) || openings->valuedouble != openings->valueint ||
        openings->valueint < 0 || openings->valueint > 99 || !cJSON_IsString(alarm) ||
        strlen(alarm->valuestring) > 23 || !cJSON_IsString(weather) ||
        !valid_display_text(weather->valuestring, 31) || !cJSON_IsString(temperature) ||
        !valid_display_text(temperature->valuestring, 15) || !cJSON_IsArray(extras) ||
        cJSON_GetArraySize(extras) > PROJECTOR_EXTRA_VALUES) goto invalid;
    static const char *const alarms[] = {"disarmed", "armed_home", "armed_away", "armed_night",
        "armed_vacation", "armed_custom_bypass", "arming", "pending", "triggered", "unknown"};
    bool valid_alarm = false;
    for (unsigned i = 0; i < sizeof(alarms)/sizeof(alarms[0]); ++i)
        if (!strcmp(alarm->valuestring, alarms[i])) valid_alarm = true;
    if (!valid_alarm) goto invalid;
    char parsed_extras[PROJECTOR_EXTRA_VALUES][32] = {{0}};
    for (int i = 0; i < cJSON_GetArraySize(extras); ++i) {
        const cJSON *item = cJSON_GetArrayItem(extras, i);
        if (!cJSON_IsString(item) || !valid_display_text(item->valuestring, 31)) goto invalid;
        strcpy(parsed_extras[i], item->valuestring);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    state.openings = openings->valueint;
    state.openings_known = cJSON_IsTrue(openings_known);
    strcpy(state.alarm, alarm->valuestring);
    strcpy(state.weather, weather->valuestring);
    strcpy(state.temperature, temperature->valuestring);
    strcpy(state.condition, condition ? condition->valuestring : "");
    state.temp_high_known = high_known;
    state.temp_high = high;
    state.temp_low_known = low_known;
    state.temp_low = low;
    memcpy(state.extras, parsed_extras, sizeof(parsed_extras));
    ha_received_us = esp_timer_get_time();
    xSemaphoreGive(lock);
    return ESP_OK;
invalid:
    snprintf(error, error_size, "Invalid HA state");
    return ESP_ERR_INVALID_ARG;
}

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
    nvs_get_str(handle, "wifi_ssid", ssid, &ssid_size);
    nvs_get_str(handle, "wifi_pass", password, &password_size);
    nvs_close(handle);
}

esp_err_t projector_write_network(const char *ssid, const char *password, const char *timezone)
{
    if (!ssid || !*ssid || strlen(ssid) > 32 || !password || strlen(password) > 63 ||
        !timezone || !*timezone || strlen(timezone) > 127) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "wifi_ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "wifi_pass", password);
    if (err == ESP_OK) err = nvs_set_str(handle, "timezone", timezone);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
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

esp_err_t projector_set_calibration(const cJSON *root, char *error, size_t error_size)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(root, "center_x");
    const cJSON *y = cJSON_GetObjectItemCaseSensitive(root, "center_y");
    const cJSON *diameter = cJSON_GetObjectItemCaseSensitive(root, "diameter");
    const cJSON *rotation = cJSON_GetObjectItemCaseSensitive(root, "rotation");
    const cJSON *mirror = cJSON_GetObjectItemCaseSensitive(root, "mirror");
    bool rotation_valid = !rotation || (cJSON_IsNumber(rotation) &&
        (rotation->valuedouble == 0 || rotation->valuedouble == 90 ||
         rotation->valuedouble == 180 || rotation->valuedouble == 270));
    bool adjust = !mode && x && y && diameter &&
        cJSON_IsNumber(x) && cJSON_IsNumber(y) && cJSON_IsNumber(diameter) &&
        x->valuedouble == x->valueint && y->valuedouble == y->valueint &&
        diameter->valuedouble == diameter->valueint &&
        projector_calibration_valid(x->valueint, y->valueint, diameter->valueint) &&
        rotation_valid && (!mirror || cJSON_IsBool(mirror));
    bool action = cJSON_IsString(mode) && !x && !y && !diameter && !rotation && !mirror &&
        (!strcmp(mode->valuestring, "start") || !strcmp(mode->valuestring, "save") ||
         !strcmp(mode->valuestring, "cancel"));
    if (!adjust && !action) {
        snprintf(error, error_size, "Invalid circle geometry, orientation or action");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (adjust && state.calibration.active) {
        state.calibration.center_x = x->valueint;
        state.calibration.center_y = y->valueint;
        state.calibration.diameter = diameter->valueint;
        if (rotation) state.calibration.rotation = rotation->valueint / 90;
        if (mirror) state.calibration.mirror = cJSON_IsTrue(mirror);
    } else if (action && !strcmp(mode->valuestring, "start")) {
        state.calibration = saved_calibration;
        state.calibration.active = true;
    } else if (action && !strcmp(mode->valuestring, "cancel")) {
        state.calibration = saved_calibration;
    } else if (action && !strcmp(mode->valuestring, "save") && state.calibration.active) {
        uint8_t geometry[3] = {state.calibration.center_x, state.calibration.center_y,
                               state.calibration.diameter};
        uint8_t orientation = state.calibration.rotation | (state.calibration.mirror ? 4 : 0);
        nvs_handle_t handle;
        err = nvs_open(NS, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, "proj_circle", geometry, sizeof(geometry));
            if (err == ESP_OK) err = nvs_set_u8(handle, "proj_orient", orientation);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        if (err == ESP_OK) {
            state.calibration.active = false;
            saved_calibration = state.calibration;
        }
    } else err = ESP_ERR_INVALID_STATE;
    xSemaphoreGive(lock);
    if (err != ESP_OK) snprintf(error, error_size, err == ESP_ERR_INVALID_STATE ?
                                "Start calibration first" : "NVS write failed");
    return err;
}

#include "projector.h"
#include "projector_logic.h"
#include "bed_build_config.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* NVS namespace holding every saved setting: Wi-Fi, token, pages, calibration. */
#define NS "bed_projector"
#define HA_TTL_US (120LL * 1000000LL)
#define MESSAGE_DURATION_DEFAULT 30 /* Seconds. */
#define PREVIEW_US (120LL * 1000000LL) /* A forgotten trial from the web UI ends by itself. */
#define PAGES_JSON_MAX 4000           /* nvs_set_str limit, terminator included. */
#define DEFAULT_TINT 0xffb36b
#define DEFAULT_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
/* Clocks before 2020 have not been set by NTP yet. */
#define VALID_TIME 1577836800
#define WAKE_TEST_MS 60000LL /* "Prova sveglia": a minute of sunrise, then the alarm. */
#define ARRAY_COUNT(items) (sizeof(items) / sizeof((items)[0]))

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
static const char *const alarm_states[] = {
    "disarmed", "armed_home", "armed_away", "armed_night", "armed_vacation",
    "armed_custom_bypass", "arming", "pending", "triggered", "unknown"
};

static SemaphoreHandle_t lock;
static projector_snapshot_t state;
static int64_t ha_received_us;
static int64_t page_selected_us;
static int64_t message_duration_us;
static int64_t preview_started_us;
static char api_token[33];
static char tz_value[128] = DEFAULT_TIMEZONE;
static projector_calibration_t saved_calibration;
static bool discovery = true; /* Advertise the _bed-projector service for Home Assistant. */
/* "YYYY-MM-DD HH:MM" of the last alarm followed, so it is not followed again while HA still sends it. */
static char wake_followed[17];

/* Pages and theme as one document: what /api/pages reads and writes. */
typedef struct {
    projector_page_t pages[PROJECTOR_MAX_PAGES];
    uint8_t count;
    projector_theme_t theme;
} look_t;

/* The look stored in NVS; state differs from it only during a trial. */
static look_t saved_look;

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

/* Optional display string: absent or null is empty, otherwise it must fit out. */
static bool parse_text(const cJSON *item, char *out, size_t size)
{
    out[0] = 0;
    if (!item || cJSON_IsNull(item)) return true;
    if (!cJSON_IsString(item) || !valid_display_text(item->valuestring, size - 1)) return false;
    strcpy(out, item->valuestring);
    return true;
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
    if (err == ESP_OK) err = nvs_set_u16(handle, "proj_carousel", s->carousel);
    return err;
}

static esp_err_t write_pages(nvs_handle_t handle, const void *context)
{
    return nvs_set_str(handle, "proj_pages", context);
}

static esp_err_t write_wake(nvs_handle_t handle, const void *context)
{
    const projector_wake_t *w = context;
    esp_err_t err = nvs_set_u8(handle, "proj_sunrise", w->sunrise);
    if (err == ESP_OK) err = nvs_set_u8(handle, "proj_wakescr", w->screen);
    if (err == ESP_OK) err = nvs_set_u8(handle, "proj_wakeblink", w->blink ? 1 : 0);
    return err;
}

static esp_err_t write_calibration(nvs_handle_t handle, const void *context)
{
    const projector_calibration_t *c = context;
    uint8_t geometry[3] = {c->center_x, c->center_y, c->diameter};
    esp_err_t err = nvs_set_blob(handle, "proj_circle", geometry, sizeof(geometry));
    if (err == ESP_OK) err = nvs_set_u8(handle, "proj_orient", c->rotation | (c->mirror ? 4 : 0));
    return err;
}

static esp_err_t write_discovery(nvs_handle_t handle, const void *context)
{
    return nvs_set_u8(handle, "proj_discover", *(const bool *)context ? 1 : 0);
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

static projector_page_t fixed_page(const char *id, const char *name, int layout)
{
    projector_page_t page = {.layout = layout, .tomorrow_from = PROJECTOR_TOMORROW_DEFAULT};
    strcpy(page.id, id);
    strcpy(page.name, name);
    return page;
}

static void defaults(void)
{
    memset(&state, 0, sizeof(state));
    state.power = true;
    state.brightness = 30;
    state.page_timeout = PROJECTOR_PAGE_TIMEOUT_DEFAULT;
    state.calibration = (projector_calibration_t){.center_x=64, .center_y=64, .diameter=112};
    memset(&saved_look, 0, sizeof(saved_look));
    saved_look.count = 3;
    saved_look.pages[0] = fixed_page("clock", "Ora", LAYOUT_CLOCK);
    saved_look.pages[1] = fixed_page("home", "Casa", LAYOUT_HOME);
    saved_look.pages[2] = fixed_page("weather", "Meteo", LAYOUT_WEATHER);
    saved_look.theme = (projector_theme_t){.palette = PALETTE_COLOR, .tint = DEFAULT_TINT, .dots = true};
    memcpy(state.pages, saved_look.pages, sizeof(state.pages));
    state.page_count = saved_look.count;
    state.theme = saved_look.theme;
    ha_unknown(&state.ha);
}

/* ---- Pages and theme ---- */

static bool add_string(cJSON *object, const char *key, const char *value)
{
    return cJSON_AddStringToObject(object, key, value) != NULL;
}

static bool page_json(const projector_page_t *page, cJSON *item)
{
    const projector_layout_t *layout = &projector_layouts[page->layout];
    if (!add_string(item, "id", page->id) || !add_string(item, "name", page->name) ||
        !add_string(item, "layout", layout->name) || !add_string(item, "size", projector_sizes[page->size]))
        return false;
    if ((page->layout == LAYOUT_CLOCK || page->layout == LAYOUT_WEATHER) &&
        !cJSON_AddNumberToObject(item, "tomorrow_from", page->tomorrow_from)) return false;
    cJSON *show = cJSON_AddObjectToObject(item, "show");
    if (!show) return false;
    for (int i = 0; i < layout->element_count; ++i)
        if (!add_string(show, layout->elements[i].name, layout->elements[i].modes[page->show[i]])) return false;
    if (page->layout != LAYOUT_LIST && page->layout != LAYOUT_FACE) return true;
    cJSON *list = cJSON_AddArrayToObject(item, "widgets");
    if (!list) return false;
    for (int i = 0; i < page->widget_count; ++i) {
        const projector_widget_t *widget = &page->widgets[i];
        cJSON *entry = cJSON_CreateObject();
        if (!entry || !cJSON_AddItemToArray(list, entry)) {
            cJSON_Delete(entry);
            return false;
        }
        if (!add_string(entry, "widget", projector_widgets[widget->type]) ||
            !add_string(entry, "style", projector_styles[widget->style]) ||
            ((widget->type == WIDGET_TEXT || widget->type == WIDGET_COUNTDOWN) &&
             !add_string(entry, "text", widget->text)) ||
            (page->layout == LAYOUT_FACE && !add_string(entry, "slot", projector_slots[widget->slot])))
            return false;
    }
    return true;
}

/* NULL on any allocation failure. */
static cJSON *look_json(const look_t *look)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *theme = cJSON_AddObjectToObject(root, "theme");
    cJSON *pages = cJSON_AddArrayToObject(root, "pages");
    char tint[8];
    snprintf(tint, sizeof(tint), "#%06" PRIx32, look->theme.tint & 0xffffff);
    if (!theme || !pages || !add_string(theme, "palette", projector_palettes[look->theme.palette]) ||
        !add_string(theme, "tint", tint) || !cJSON_AddBoolToObject(theme, "dots", look->theme.dots))
        goto no_memory;
    for (unsigned i = 0; i < look->count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(pages, item)) {
            cJSON_Delete(item);
            goto no_memory;
        }
        if (!page_json(&look->pages[i], item)) goto no_memory;
    }
    return root;
no_memory:
    cJSON_Delete(root);
    return NULL;
}

/* Optional enumerated string: absent keeps *out. */
static bool parse_choice(const cJSON *item, const char *const *names, int count, uint8_t *out)
{
    if (!item) return true;
    int index = cJSON_IsString(item) ? projector_name_index(item->valuestring, names, count) : -1;
    if (index < 0) return false;
    *out = index;
    return true;
}

/* Element modes by name; elements left out keep their default. */
static bool parse_show(const cJSON *item, projector_page_t *out)
{
    if (!item) return true;
    if (!cJSON_IsObject(item)) return false;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item) {
        int element = projector_element_index(out->layout, entry->string);
        int mode = cJSON_IsString(entry) ? projector_mode_index(out->layout, element, entry->valuestring) : -1;
        if (mode < 0) return false;
        out->show[element] = mode;
    }
    return true;
}

/* List lines {"widget", "style", "text"} or watch face complications, which also need a
 * "slot" of their own; other pages have none. */
static bool parse_widgets(const cJSON *item, projector_page_t *out)
{
    if (!item) return true;
    bool face = out->layout == LAYOUT_FACE;
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) > PROJECTOR_MAX_WIDGETS ||
        (out->layout != LAYOUT_LIST && !face && cJSON_GetArraySize(item))) return false;
    unsigned used_slots = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item) {
        projector_widget_t *widget = &out->widgets[out->widget_count++];
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(entry, "widget");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(entry, "text");
        const cJSON *slot = cJSON_GetObjectItemCaseSensitive(entry, "slot");
        if (!cJSON_IsObject(entry) || !type || face != (slot != NULL) ||
            !parse_choice(type, projector_widgets, WIDGET_COUNT, &widget->type) ||
            !parse_choice(cJSON_GetObjectItemCaseSensitive(entry, "style"), projector_styles, STYLE_COUNT,
                          &widget->style) ||
            !parse_choice(slot, projector_slots, SLOT_COUNT, &widget->slot) ||
            (used_slots & (1u << widget->slot)) ||
            !parse_text(text, widget->text, sizeof(widget->text))) return false;
        if (face) used_slots |= 1u << widget->slot;
        int year, month, day;
        const char *name;
        if (widget->type == WIDGET_COUNTDOWN &&
            !projector_parse_countdown(widget->text, &year, &month, &day, &name)) return false;
        if (widget->type != WIDGET_TEXT && widget->type != WIDGET_COUNTDOWN) widget->text[0] = 0;
    }
    return true;
}

static bool parse_page(const cJSON *item, int index, const projector_page_t *previous,
                       projector_page_t *out)
{
#define FIELD(name) cJSON_GetObjectItemCaseSensitive(item, name)
    const cJSON *id = FIELD("id"), *name = FIELD("name"), *layout = FIELD("layout");
    const cJSON *tomorrow = FIELD("tomorrow_from");
    memset(out, 0, sizeof(*out));
    out->tomorrow_from = PROJECTOR_TOMORROW_DEFAULT;
    int layout_index = cJSON_IsString(layout) ? projector_layout_index(layout->valuestring) : -1;
    if (!cJSON_IsObject(item) ||
        !cJSON_IsString(id) || !valid_name(id->valuestring, sizeof(out->id) - 1, true) ||
        !cJSON_IsString(name) || !valid_name(name->valuestring, sizeof(out->name) - 1, false) ||
        layout_index < 0 || (tomorrow && !is_integer(tomorrow, 0, PROJECTOR_TOMORROW_NEVER))) return false;
    out->layout = layout_index;
    if (tomorrow) out->tomorrow_from = tomorrow->valueint;
    if (!parse_choice(FIELD("size"), projector_sizes, SIZE_COUNT, &out->size) ||
        !parse_show(FIELD("show"), out) || !parse_widgets(FIELD("widgets"), out)) return false;
#undef FIELD
    /* The first page, and only the first, is the clock: the classic one or a watch face. */
    bool clock_id = !strcmp(id->valuestring, "clock");
    bool first_is_clock = clock_id && (layout_index == LAYOUT_CLOCK || layout_index == LAYOUT_FACE);
    if (index == 0 ? !first_is_clock : clock_id || layout_index == LAYOUT_CLOCK) return false;
    for (int i = 0; i < index; ++i)
        if (!strcmp(previous[i].id, id->valuestring)) return false;
    strcpy(out->id, id->valuestring);
    strcpy(out->name, name->valuestring);
    return true;
}

/* Optional theme: absent fields keep the defaults. */
static bool parse_theme(const cJSON *item, projector_theme_t *out)
{
    *out = (projector_theme_t){.palette = PALETTE_COLOR, .tint = DEFAULT_TINT, .dots = true};
    if (!item) return true;
    const cJSON *tint = cJSON_GetObjectItemCaseSensitive(item, "tint");
    const cJSON *dots = cJSON_GetObjectItemCaseSensitive(item, "dots");
    if (!cJSON_IsObject(item) ||
        !parse_choice(cJSON_GetObjectItemCaseSensitive(item, "palette"), projector_palettes, PALETTE_COUNT,
                      &out->palette) ||
        (dots && !cJSON_IsBool(dots))) return false;
    if (dots) out->dots = cJSON_IsTrue(dots);
    if (!tint) return true;
    const char *hex = cJSON_IsString(tint) ? tint->valuestring : "";
    if (strlen(hex) != 7 || hex[0] != '#' || strspn(hex + 1, "0123456789abcdefABCDEF") != 6) return false;
    out->tint = (uint32_t)strtoul(hex + 1, NULL, 16);
    return true;
}

static esp_err_t parse_look(const cJSON *root, look_t *out, char *error, size_t error_size)
{
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    int length = cJSON_IsArray(pages) ? cJSON_GetArraySize(pages) : 0;
    memset(out, 0, sizeof(*out));
    bool valid = length >= 1 && length <= PROJECTOR_MAX_PAGES &&
        parse_theme(cJSON_GetObjectItemCaseSensitive(root, "theme"), &out->theme);
    for (int i = 0; valid && i < length; ++i)
        valid = parse_page(cJSON_GetArrayItem(pages, i), i, out->pages, &out->pages[i]);
    if (!valid) {
        snprintf(error, error_size, "Invalid pages: 1-%d pages with unique ids and 1-16 character names, "
                 "clock first (clock or face layout), known layouts, elements and modes, up to %d widgets "
                 "on list and face pages, one per slot on face pages, "
                 "countdown text \"YYYY-MM-DD name\", "
                 "valid theme", PROJECTOR_MAX_PAGES, PROJECTOR_MAX_WIDGETS);
        return ESP_ERR_INVALID_ARG;
    }
    out->count = (uint8_t)length;
    return ESP_OK;
}

static esp_err_t save_look(const look_t *look, char *error, size_t error_size)
{
    cJSON *root = look_json(look);
    char *json = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    esp_err_t err = !json ? ESP_ERR_NO_MEM : strlen(json) + 1 > PAGES_JSON_MAX ? ESP_ERR_INVALID_SIZE :
        nvs_save(write_pages, json);
    cJSON_free(json);
    if (err != ESP_OK) snprintf(error, error_size, err == ESP_ERR_INVALID_SIZE ? "Pages too large to save" :
                                "Saving pages failed");
    return err;
}

/* Show look; stay on the current page when it still exists, unless show_id names another.
 * Caller owns the state mutex. */
static void apply_look_locked(const look_t *look, const char *show_id)
{
    char current_id[sizeof(state.pages[0].id)];
    strcpy(current_id, show_id ? show_id : state.pages[state.page_index].id);
    memcpy(state.pages, look->pages, sizeof(state.pages));
    state.page_count = look->count;
    state.theme = look->theme;
    unsigned previous_index = state.page_index;
    state.page_index = 0;
    for (unsigned i = 0; i < look->count; ++i)
        if (!strcmp(current_id, state.pages[i].id)) state.page_index = i;
    if (show_id || state.page_index != previous_index) page_selected_us = esp_timer_get_time();
}

static void load_pages(nvs_handle_t handle)
{
    size_t size = 0;
    if (nvs_get_str(handle, "proj_pages", NULL, &size) != ESP_OK || size > PAGES_JSON_MAX) return;
    char *json = malloc(size);
    if (json && nvs_get_str(handle, "proj_pages", json, &size) == ESP_OK) {
        cJSON *root = cJSON_Parse(json);
        static look_t parsed;
        char error[200];
        if (root && parse_look(root, &parsed, error, sizeof(error)) == ESP_OK) {
            saved_look = parsed;
            apply_look_locked(&saved_look, NULL);
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
    if (nvs_get_u16(handle, "proj_carousel", &timeout) == ESP_OK && projector_carousel_valid(timeout))
        state.carousel = timeout;
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
    if (nvs_get_u8(handle, "proj_discover", &value) == ESP_OK) discovery = value != 0;
    if (nvs_get_u8(handle, "proj_sunrise", &value) == ESP_OK && value <= 60) state.wake.sunrise = value;
    if (nvs_get_u8(handle, "proj_wakescr", &value) == ESP_OK && value <= 30) state.wake.screen = value;
    if (nvs_get_u8(handle, "proj_wakeblink", &value) == ESP_OK) state.wake.blink = value != 0;
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
    page_selected_us = esp_timer_get_time(); /* The rotation counts from boot. */
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

/* How long the alarm lasts from its time: the good morning screen or the minute of pulsing,
 * whichever is longer. */
static int64_t ringing_us(const projector_wake_t *w)
{
    int64_t screen = w->screen * 60000000LL, pulse = w->blink ? PROJECTOR_PULSE_MS * 1000LL : 0;
    return screen > pulse ? screen : pulse;
}

/* Follow the next alarm HA sends: catch it once the sunrise should start, then carry on by the
 * esp_timer clock, so HA moving on to the next alarm or going stale does not cut it short.
 * Caller owns the state mutex. */
static void follow_wake_locked(int64_t now)
{
    const projector_wake_t *w = &state.wake;
    if (state.wake_phase == WAKE_IDLE && (w->sunrise || w->screen || w->blink) && state.ha.wake_known &&
        state.ha.wake_date[0] && projector_data_fresh(ha_received_us, now, HA_TTL_US)) {
        time_t clock = time(NULL);
        struct tm local;
        localtime_r(&clock, &local);
        char key[sizeof(wake_followed)];
        snprintf(key, sizeof(key), "%.10s %.5s", state.ha.wake_date, state.ha.wake_time);
        int32_t left = clock > VALID_TIME ?
            projector_seconds_to_wake(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                                      local.tm_min, local.tm_sec, state.ha.wake_date, state.ha.wake_time) :
            PROJECTOR_NO_WAKE;
        /* Within the sunrise, or in the alarm's own minute. */
        if (left != PROJECTOR_NO_WAKE && left <= w->sunrise * 60 && left > -60 && strcmp(key, wake_followed)) {
            strcpy(wake_followed, key);
            state.wake_at_us = now + left * 1000000LL;
            state.wake_ramp_ms = w->sunrise * 60000LL;
            state.wake_phase = WAKE_SUNRISE;
        }
    }
    if (state.wake_phase == WAKE_SUNRISE && now >= state.wake_at_us) {
        state.wake_phase = WAKE_RINGING;
        state.power = true;
    }
    if (state.wake_phase == WAKE_RINGING && now >= state.wake_at_us + ringing_us(w))
        state.wake_phase = WAKE_IDLE;
}

void projector_tick(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    follow_wake_locked(now);
    /* A trial keeps its page until it ends; the rotation also waits while something covers the pages. */
    if (state.carousel) {
        bool covered = state.message.active || state.calibration.active ||
            (state.wake_phase == WAKE_RINGING && state.wake.screen);
        if (!state.preview && !covered &&
            projector_expired(page_selected_us, now, state.carousel * 1000000LL)) {
            state.page_index = projector_move_page(state.page_index, state.page_count, true);
            page_selected_us = now;
        }
    } else if (!state.preview && projector_page_timed_out(state.page_index, page_selected_us, now,
                                                          state.page_timeout * 1000000LL))
        state.page_index = 0;
    if (state.preview && projector_expired(preview_started_us, now, PREVIEW_US)) {
        state.preview = false;
        apply_look_locked(&saved_look, NULL);
    }
    /* A duration of zero keeps the message until it is cleared. */
    if (state.message.active && message_duration_us &&
        projector_expired(state.message.shown_us, now, message_duration_us))
        memset(&state.message, 0, sizeof(state.message));
    xSemaphoreGive(lock);
}

esp_err_t projector_set_pages(const cJSON *root, char *error, size_t error_size)
{
    /* Static: about 1 KB, and HTTP requests are serialized. */
    static look_t parsed;
    esp_err_t err = parse_look(root, &parsed, error, error_size);
    if (err != ESP_OK) return err;
    xSemaphoreTake(lock, portMAX_DELAY);
    err = save_look(&parsed, error, error_size);
    if (err == ESP_OK) {
        saved_look = parsed;
        state.preview = false;
        apply_look_locked(&saved_look, NULL);
    }
    xSemaphoreGive(lock);
    return err;
}

/* Show a draft without saving it: {"cancel": true} or the /api/pages document with an optional
 * "page" id to show. It ends on save, on cancel or after PREVIEW_US. */
esp_err_t projector_preview_pages(const cJSON *root, char *error, size_t error_size)
{
    static look_t parsed;
    const cJSON *cancel = cJSON_GetObjectItemCaseSensitive(root, "cancel");
    const cJSON *page = cJSON_GetObjectItemCaseSensitive(root, "page");
    if (cancel) {
        if (!cJSON_IsTrue(cancel) || cJSON_GetArraySize(root) != 1) {
            snprintf(error, error_size, "Invalid preview: cancel alone or a pages document");
            return ESP_ERR_INVALID_ARG;
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        if (state.preview) {
            state.preview = false;
            apply_look_locked(&saved_look, NULL);
        }
        xSemaphoreGive(lock);
        return ESP_OK;
    }
    esp_err_t err = parse_look(root, &parsed, error, error_size);
    if (err != ESP_OK) return err;
    int shown = -1;
    for (unsigned i = 0; page && cJSON_IsString(page) && i < parsed.count; ++i)
        if (!strcmp(page->valuestring, parsed.pages[i].id)) shown = i;
    if (page && shown < 0) {
        snprintf(error, error_size, "Unknown page");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    apply_look_locked(&parsed, page ? page->valuestring : NULL);
    state.preview = true;
    preview_started_us = esp_timer_get_time();
    xSemaphoreGive(lock);
    return ESP_OK;
}

cJSON *projector_pages_json(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON *root = look_json(&saved_look);
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
    const cJSON *carousel = cJSON_GetObjectItemCaseSensitive(root, "carousel");
    bool valid_move = !move || (cJSON_IsString(move) &&
        (!strcmp(move->valuestring, "next") || !strcmp(move->valuestring, "previous")));
    if ((power && !cJSON_IsBool(power)) || (brightness && !is_integer(brightness, 1, 100)) ||
        (page && !cJSON_IsString(page)) || !valid_move ||
        (timeout && (!is_integer(timeout, 0, 65535) || !projector_page_timeout_valid(timeout->valueint))) ||
        (carousel && (!is_integer(carousel, 0, 65535) || !projector_carousel_valid(carousel->valueint))) ||
        (page && move) || (!power && !brightness && !page && !move && !timeout && !carousel)) {
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
    /* Static: the snapshot is too large for the HTTP task stack; requests are serialized. */
    static projector_snapshot_t next;
    next = state;
    if (power) next.power = cJSON_IsTrue(power);
    if (brightness) next.brightness = brightness->valueint;
    if (timeout) next.page_timeout = timeout->valueint;
    if (carousel) next.carousel = carousel->valueint;
    esp_err_t err = ESP_OK;
    if (next_page < 0) {
        err = ESP_ERR_INVALID_ARG;
        snprintf(error, error_size, "Unknown page");
    } else if (next.power != state.power || next.brightness != state.brightness ||
               next.page_timeout != state.page_timeout || next.carousel != state.carousel) {
        err = nvs_save(write_presentation, &next);
        if (err != ESP_OK) snprintf(error, error_size, "NVS write failed");
    }
    if (err == ESP_OK) {
        /* Turning the light off also stops a sunrise or an alarm under way. */
        if (power && !next.power) state.wake_phase = WAKE_IDLE;
        state.power = next.power;
        state.brightness = next.brightness;
        state.page_timeout = next.page_timeout;
        state.page_index = next_page;
        /* A new rotation period counts from now, not from the last page change. */
        if (page || move || (carousel && next.carousel != state.carousel))
            page_selected_us = esp_timer_get_time();
        state.carousel = next.carousel;
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
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(root, "level");
    const cJSON *blink = cJSON_GetObjectItemCaseSensitive(root, "blink");
    projector_message_t message = {0};
    bool clearing = cJSON_IsTrue(clear) && cJSON_GetArraySize(root) == 1;
    bool showing = !clear && parse_required_text(text, message.text, sizeof(message.text)) &&
        message.text[0] && (!icon || (cJSON_IsString(icon) &&
            in_list(icon->valuestring, message_icons, ARRAY_COUNT(message_icons)))) &&
        (!duration ||
         (is_integer(duration, 0, 3600) && (duration->valueint == 0 || duration->valueint >= 5))) &&
        parse_choice(level, projector_message_levels, MESSAGE_LEVEL_COUNT, &message.level) &&
        (!blink || cJSON_IsBool(blink));
    if (!clearing && !showing) {
        snprintf(error, error_size, "Invalid message: text of 1-63 bytes, known icon and level, "
                 "duration 0 (until cleared) or 5-3600 s, boolean blink");
        return ESP_ERR_INVALID_ARG;
    }
    message.active = showing;
    message.blink = showing && cJSON_IsTrue(blink);
    if (showing && icon) strcpy(message.icon, icon->valuestring);
    if (showing) message.shown_us = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    state.message = message;
    message_duration_us = (duration ? duration->valueint : MESSAGE_DURATION_DEFAULT) * 1000000LL;
    xSemaphoreGive(lock);
    return ESP_OK;
}

/* ---- Alarm ---- */

const char *const projector_wake_phases[WAKE_PHASE_COUNT] = {
    [WAKE_IDLE] = "idle", [WAKE_SUNRISE] = "sunrise", [WAKE_RINGING] = "ringing",
};

/* {"sunrise", "screen", "blink"} in any combination saves them; {"dismiss": true} stops the
 * alarm under way and {"test": true} plays one now, each alone. */
esp_err_t projector_set_wake(const cJSON *root, char *error, size_t error_size)
{
    const cJSON *sunrise = cJSON_GetObjectItemCaseSensitive(root, "sunrise");
    const cJSON *screen = cJSON_GetObjectItemCaseSensitive(root, "screen");
    const cJSON *blink = cJSON_GetObjectItemCaseSensitive(root, "blink");
    const cJSON *dismiss = cJSON_GetObjectItemCaseSensitive(root, "dismiss");
    const cJSON *test = cJSON_GetObjectItemCaseSensitive(root, "test");
    int fields = cJSON_GetArraySize(root);
    bool action = fields == 1 && (cJSON_IsTrue(dismiss) || cJSON_IsTrue(test));
    bool settings = !dismiss && !test && fields == (sunrise != NULL) + (screen != NULL) + (blink != NULL) &&
        fields > 0 && (!sunrise || is_integer(sunrise, 0, 60)) && (!screen || is_integer(screen, 0, 30)) &&
        (!blink || cJSON_IsBool(blink));
    if (!action && !settings) {
        snprintf(error, error_size, "Invalid alarm: sunrise 0-60, screen 0-30 minutes, boolean blink; "
                 "or dismiss or test alone");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (cJSON_IsTrue(dismiss)) state.wake_phase = WAKE_IDLE;
    else if (cJSON_IsTrue(test)) {
        state.wake_at_us = esp_timer_get_time() + (state.wake.sunrise ? WAKE_TEST_MS * 1000 : 0);
        state.wake_ramp_ms = WAKE_TEST_MS;
        state.wake_phase = WAKE_SUNRISE;
    } else {
        projector_wake_t next = state.wake;
        if (sunrise) next.sunrise = sunrise->valueint;
        if (screen) next.screen = screen->valueint;
        if (blink) next.blink = cJSON_IsTrue(blink);
        err = nvs_save(write_wake, &next);
        if (err == ESP_OK) state.wake = next;
        else snprintf(error, error_size, "NVS write failed");
    }
    xSemaphoreGive(lock);
    return err;
}

cJSON *projector_wake_json(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    projector_wake_t w = state.wake;
    uint8_t phase = state.wake_phase;
    xSemaphoreGive(lock);
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "sunrise", w.sunrise) ||
        !cJSON_AddNumberToObject(root, "screen", w.screen) || !cJSON_AddBoolToObject(root, "blink", w.blink) ||
        !add_string(root, "phase", projector_wake_phases[phase])) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
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

bool projector_discovery(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool enabled = discovery;
    xSemaphoreGive(lock);
    return enabled;
}

esp_err_t projector_set_config(const cJSON *root, char *error, size_t error_size)
{
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "discovery");
    if (!cJSON_IsBool(enabled)) {
        snprintf(error, error_size, "discovery must be a boolean");
        return ESP_ERR_INVALID_ARG;
    }
    bool value = cJSON_IsTrue(enabled);
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = nvs_save(write_discovery, &value);
    if (err == ESP_OK) discovery = value;
    else snprintf(error, error_size, "NVS write failed");
    xSemaphoreGive(lock);
    return err;
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

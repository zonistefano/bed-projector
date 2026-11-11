#include "bed_api.h"
#include "bed_net.h"
#include "projector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ARRAY_COUNT(items) (sizeof(items) / sizeof((items)[0]))
/* The access point's own address, where pairing is allowed. */
#define AP_ADDRESS 0xc0a80401U /* 192.168.4.1 */
/* Where esp_app_desc_t sits in a firmware image: after the image and first segment headers. */
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

static const char *TAG = "bed-api";
static httpd_handle_t server;

/* ---- Requests and responses ---- */

static bool ap_client(httpd_req_t *req)
{
    struct sockaddr_storage local;
    socklen_t length = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &length) != 0) return false;
    uint32_t address;
    if (local.ss_family == AF_INET) {
        address = ntohl(((struct sockaddr_in *)&local)->sin_addr.s_addr);
    } else if (local.ss_family == AF_INET6) {
        /* With LWIP_IPV6 the server socket is dual-stack: IPv4 clients appear as ::ffff:a.b.c.d. */
        static const uint8_t v4_mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        const uint8_t *bytes = ((struct sockaddr_in6 *)&local)->sin6_addr.s6_addr;
        if (memcmp(bytes, v4_mapped, sizeof(v4_mapped)) != 0) return false;
        address = (uint32_t)bytes[12] << 24 | (uint32_t)bytes[13] << 16 | (uint32_t)bytes[14] << 8 | bytes[15];
    } else {
        return false;
    }
    return bed_net_is_provisioning() && address == AP_ADDRESS;
}

static esp_err_t json_response(httpd_req_t *req, cJSON *root)
{
    char *body = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static esp_err_t error_response(httpd_req_t *req, const char *status, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", error);
    httpd_resp_set_status(req, status);
    return json_response(req, root);
}

static const char *http_status(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_STATE: return "400 Bad Request";
    case ESP_ERR_NO_MEM: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

/* Constant-time check of "Authorization: Bearer <token>"; answers 401 itself. */
static bool authorized(httpd_req_t *req)
{
    char header[64] = {0};
    const char *token = projector_token();
    size_t token_length = strlen(token);
    size_t length = httpd_req_get_hdr_value_len(req, "Authorization");
    if (length == 7 + token_length && length < sizeof(header) &&
        httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        !memcmp(header, "Bearer ", 7)) {
        unsigned difference = 0;
        for (size_t i = 0; i < token_length; ++i) difference |= (unsigned)(header[7 + i] ^ token[i]);
        if (!difference) return true;
    }
    error_response(req, "401 Unauthorized", "unauthorized");
    return false;
}

/* Receive exactly length bytes of the body, tolerating a few socket timeouts. */
static bool receive(httpd_req_t *req, char *buffer, size_t length)
{
    size_t received = 0;
    for (int timeouts = 0; received < length;) {
        int n = httpd_req_recv(req, buffer + received, length - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

/* Parse a JSON object body; on failure the error response is already sent. */
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len < 2 || req->content_len > 4096) {
        error_response(req, "413 Payload Too Large", "JSON body must be 2-4096 bytes");
        return NULL;
    }
    char *body = malloc(req->content_len);
    if (!body) {
        error_response(req, "503 Service Unavailable", "No memory");
        return NULL;
    }
    cJSON *root = receive(req, body, req->content_len) ?
        cJSON_ParseWithLength(body, req->content_len) : NULL;
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        error_response(req, "400 Bad Request", "Invalid JSON object");
        return NULL;
    }
    return root;
}

static void restart_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(1000)); /* Let the response reach the client. */
    esp_restart();
}

static esp_err_t respond_and_restart(httpd_req_t *req)
{
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "restarting", true);
    esp_err_t result = json_response(req, response);
    if (xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL) != pdPASS) esp_restart();
    return result;
}

/* ---- JSON bodies ---- */

static cJSON *status_json(void)
{
    /* Static: about 3 KB, and the server handles one request at a time. */
    static projector_snapshot_t snapshot;
    projector_snapshot(&snapshot);
    const projector_ha_t *ha = &snapshot.ha;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", bed_net_device_id());
    cJSON_AddBoolToObject(root, "power", snapshot.power);
    cJSON_AddNumberToObject(root, "brightness", snapshot.brightness);
    cJSON_AddStringToObject(root, "page", snapshot.pages[snapshot.page_index].id);
    cJSON_AddNumberToObject(root, "page_index", snapshot.page_index);
    cJSON_AddNumberToObject(root, "page_timeout", snapshot.page_timeout);
    cJSON_AddBoolToObject(root, "preview", snapshot.preview);
    cJSON_AddBoolToObject(root, "ha_fresh", snapshot.ha_fresh);
    if (ha->openings_known) cJSON_AddNumberToObject(root, "openings", ha->openings);
    else cJSON_AddNullToObject(root, "openings");
    if (ha->lights_known) cJSON_AddNumberToObject(root, "lights", ha->lights);
    else cJSON_AddNullToObject(root, "lights");
    cJSON_AddStringToObject(root, "alarm", ha->alarm);
    cJSON_AddStringToObject(root, "weather", ha->weather);
    cJSON_AddStringToObject(root, "temperature", ha->temperature);
    cJSON_AddBoolToObject(root, "wifi_connected", snapshot.wifi_connected);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    return root;
}

static cJSON *calibration_json(void)
{
    static projector_snapshot_t snapshot; /* Static like in status_json(). */
    projector_snapshot(&snapshot);
    const projector_calibration_t *c = &snapshot.calibration;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "center_x", c->center_x);
    cJSON_AddNumberToObject(root, "center_y", c->center_y);
    cJSON_AddNumberToObject(root, "diameter", c->diameter);
    cJSON_AddNumberToObject(root, "rotation", c->rotation * 90);
    cJSON_AddBoolToObject(root, "mirror", c->mirror);
    cJSON_AddBoolToObject(root, "active", c->active);
    return root;
}

static cJSON *config_json(void)
{
    char timezone[128];
    projector_timezone(timezone, sizeof(timezone));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "timezone", timezone);
    cJSON_AddBoolToObject(root, "discovery", projector_discovery());
    return root;
}

static esp_err_t set_config(const cJSON *root, char *error, size_t error_size)
{
    esp_err_t err = projector_set_config(root, error, error_size);
    if (err == ESP_OK) bed_net_update_discovery();
    return err;
}

static esp_err_t set_network(const cJSON *root, char *error, size_t error_size)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *timezone = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    esp_err_t err = cJSON_IsString(ssid) && cJSON_IsString(password) && cJSON_IsString(timezone) ?
        projector_write_network(ssid->valuestring, password->valuestring, timezone->valuestring) :
        ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) snprintf(error, error_size, err == ESP_ERR_INVALID_ARG ?
        "Invalid network settings: SSID 1-32 bytes, password empty or 8-63, POSIX timezone" :
        "NVS write failed");
    return err;
}

/* ---- Handlers ---- */

typedef struct {
    /* Applies a JSON body; NULL for read-only routes. */
    esp_err_t (*set)(const cJSON *root, char *error, size_t error_size);
    /* Builds the response; NULL answers {"restarting":true} and reboots. */
    cJSON *(*respond)(void);
} json_route_t;

static esp_err_t json_handler(httpd_req_t *req)
{
    const json_route_t *route = req->user_ctx;
    if (!authorized(req)) return ESP_OK;
    if (route->set) {
        cJSON *root = read_json(req);
        if (!root) return ESP_OK;
        char error[160] = "Request failed";
        esp_err_t err = route->set(root, error, sizeof(error));
        cJSON_Delete(root);
        if (err != ESP_OK) return error_response(req, http_status(err), error);
    }
    return route->respond ? json_response(req, route->respond()) : respond_and_restart(req);
}

static esp_err_t pair_handler(httpd_req_t *req)
{
    if (!ap_client(req)) return error_response(req, "403 Forbidden", "Pairing requires provisioning AP");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "token", projector_token());
    return json_response(req, root);
}

/* The first block must carry this project's app description: a wrong file is
 * refused before anything is written. */
static bool own_firmware(const char *image)
{
    const esp_app_desc_t *description = (const esp_app_desc_t *)(image + APP_DESC_OFFSET);
    return description->magic_word == ESP_APP_DESC_MAGIC_WORD &&
           !strncmp(description->project_name, esp_app_get_description()->project_name,
                    sizeof(description->project_name));
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    size_t total = req->content_len;
    if (!partition || total < APP_DESC_OFFSET + sizeof(esp_app_desc_t) || total > partition->size)
        return error_response(req, "413 Payload Too Large", "Invalid firmware size");
    esp_ota_handle_t handle;
    /* Passing the size erases only the sectors the image needs. */
    esp_err_t err = esp_ota_begin(partition, total, &handle);
    if (err != ESP_OK) return error_response(req, "500 Internal Server Error", "OTA begin failed");
    static char buffer[4096]; /* Static: the HTTP server handles one request at a time. */
    for (size_t received = 0; err == ESP_OK && received < total;) {
        size_t length = total - received < sizeof(buffer) ? total - received : sizeof(buffer);
        if (!receive(req, buffer, length)) err = ESP_FAIL;
        else if (received == 0 && !own_firmware(buffer)) err = ESP_ERR_INVALID_ARG;
        else err = esp_ota_write(handle, buffer, length);
        received += length;
    }
    if (err == ESP_OK) err = esp_ota_end(handle);
    else esp_ota_abort(handle);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return error_response(req, "400 Bad Request", err == ESP_ERR_INVALID_ARG ?
                              "Not a Bed Projector firmware" : "Firmware rejected");
    }
    return respond_and_restart(req);
}

/* The web UI is linked into the app image, so an OTA updates it together with the firmware. */
#define EMBEDDED(name) extern const char name##_start[] asm("_binary_" #name "_start"); \
                       extern const char name##_end[] asm("_binary_" #name "_end");
EMBEDDED(index_html)
EMBEDDED(index_js)
EMBEDDED(index_css)

typedef struct {
    const char *start, *end, *type;
} static_file_t;

static esp_err_t file_handler(httpd_req_t *req)
{
    const static_file_t *file = req->user_ctx;
    httpd_resp_set_type(req, file->type);
    /* Revalidate every time: an OTA can replace the web UI. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, file->start, file->end - file->start);
}

esp_err_t bed_api_start(void)
{
    static const json_route_t status = {NULL, status_json};
    static const json_route_t pages_get = {NULL, projector_pages_json};
    static const json_route_t pages_put = {projector_set_pages, projector_pages_json};
    static const json_route_t pages_preview = {projector_preview_pages, status_json};
    static const json_route_t display = {projector_set_command, status_json};
    static const json_route_t calibration_get = {NULL, calibration_json};
    static const json_route_t calibration_post = {projector_set_calibration, calibration_json};
    static const json_route_t ha_state = {projector_set_ha_state, cJSON_CreateObject};
    static const json_route_t message = {projector_set_message, cJSON_CreateObject};
    static const json_route_t config_get = {NULL, config_json};
    static const json_route_t config_post = {set_config, config_json};
    static const json_route_t network = {set_network, NULL};
    static const static_file_t html = {index_html_start, index_html_end, "text/html; charset=utf-8"};
    static const static_file_t js = {index_js_start, index_js_end, "text/javascript; charset=utf-8"};
    static const static_file_t css = {index_css_start, index_css_end, "text/css; charset=utf-8"};
#define JSON(uri_, method_, route) {.uri=uri_, .method=method_, .handler=json_handler, \
                                    .user_ctx=(void *)&route}
#define FILE_ROUTE(uri_, file) {.uri=uri_, .method=HTTP_GET, .handler=file_handler, \
                                .user_ctx=(void *)&file}
    const httpd_uri_t routes[] = {
        {.uri="/api/pair", .method=HTTP_GET, .handler=pair_handler},
        JSON("/api/status", HTTP_GET, status),
        JSON("/api/pages", HTTP_GET, pages_get),
        JSON("/api/pages", HTTP_PUT, pages_put),
        JSON("/api/pages/preview", HTTP_POST, pages_preview),
        JSON("/api/display", HTTP_POST, display),
        JSON("/api/calibration", HTTP_GET, calibration_get),
        JSON("/api/calibration", HTTP_POST, calibration_post),
        JSON("/api/ha/state", HTTP_POST, ha_state),
        JSON("/api/message", HTTP_POST, message),
        JSON("/api/config", HTTP_GET, config_get),
        JSON("/api/config", HTTP_POST, config_post),
        JSON("/api/network", HTTP_POST, network),
        {.uri="/api/ota", .method=HTTP_POST, .handler=ota_handler},
        FILE_ROUTE("/", html),
        FILE_ROUTE("/index.html", html),
        FILE_ROUTE("/index.js", js),
        FILE_ROUTE("/index.css", css),
    };
#undef JSON
#undef FILE_ROUTE
    httpd_config_t httpd = HTTPD_DEFAULT_CONFIG();
    httpd.max_uri_handlers = ARRAY_COUNT(routes);
    httpd.stack_size = 8192;
    httpd.recv_wait_timeout = 10;
    httpd.send_wait_timeout = 10;
    /* Home Assistant, a browser and keep-alive connections can fill every
     * socket: close the least recently used one instead of refusing new clients. */
    httpd.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server, &httpd);
    for (size_t i = 0; err == ESP_OK && i < ARRAY_COUNT(routes); ++i)
        err = httpd_register_uri_handler(server, &routes[i]);
    return err;
}

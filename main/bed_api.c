#include "bed_api.h"
#include "bed_net.h"
#include "projector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "cJSON.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bed-api";
static httpd_handle_t server;

static bool ap_client(httpd_req_t *req)
{
    struct sockaddr_storage local;
    socklen_t length = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &length) != 0 ||
        local.ss_family != AF_INET) return false;
    uint32_t address = ntohl(((struct sockaddr_in *)&local)->sin_addr.s_addr);
    return bed_net_is_provisioning() && address == 0xc0a80401U;
}

static bool authorized(httpd_req_t *req)
{
    char header[64] = {0};
    size_t length = httpd_req_get_hdr_value_len(req, "Authorization");
    const char *token = projector_token();
    if (length != 7 + strlen(token) || length >= sizeof(header) ||
        httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK)
        goto denied;
    if (memcmp(header, "Bearer ", 7) != 0) goto denied;
    unsigned difference = 0;
    for (size_t i = 0; i < strlen(token); ++i) difference |= (unsigned)(header[7 + i] ^ token[i]);
    if (difference == 0) return true;
denied:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return false;
}

static esp_err_t json_response(httpd_req_t *req, cJSON *root)
{
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    char *body = cJSON_PrintUnformatted(root);
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

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len < 2 || req->content_len > 4096) {
        error_response(req, "413 Payload Too Large", "JSON body must be 2-4096 bytes");
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        error_response(req, "503 Service Unavailable", "No memory");
        return NULL;
    }
    size_t received = 0;
    int timeouts = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) {
            free(body);
            error_response(req, "400 Bad Request", "Incomplete body");
            return NULL;
        }
        received += n;
    }
    body[received] = 0;
    cJSON *root = cJSON_ParseWithLength(body, received);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        error_response(req, "400 Bad Request", "Invalid JSON object");
        return NULL;
    }
    return root;
}

static esp_err_t pair_handler(httpd_req_t *req)
{
    if (!ap_client(req)) return error_response(req, "403 Forbidden", "Pairing requires provisioning AP");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "token", projector_token());
    return json_response(req, root);
}

static esp_err_t status_response(httpd_req_t *req)
{
    projector_snapshot_t snapshot;
    projector_snapshot(&snapshot);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 1);
    cJSON_AddBoolToObject(root, "power", snapshot.power);
    cJSON_AddNumberToObject(root, "brightness", snapshot.brightness);
    cJSON_AddStringToObject(root, "page", snapshot.pages[snapshot.page_index].id);
    cJSON_AddNumberToObject(root, "page_index", snapshot.page_index);
    cJSON_AddNumberToObject(root, "page_timeout", snapshot.page_timeout);
    cJSON_AddBoolToObject(root, "ha_fresh", snapshot.ha_fresh);
    if (snapshot.ha_fresh && snapshot.openings_known) cJSON_AddNumberToObject(root, "openings", snapshot.openings);
    else cJSON_AddNullToObject(root, "openings");
    if (snapshot.ha_fresh && snapshot.lights_known) cJSON_AddNumberToObject(root, "lights", snapshot.lights);
    else cJSON_AddNullToObject(root, "lights");
    cJSON_AddStringToObject(root, "alarm", snapshot.alarm);
    cJSON_AddStringToObject(root, "weather", snapshot.weather);
    cJSON_AddStringToObject(root, "temperature", snapshot.temperature);
    cJSON_AddBoolToObject(root, "wifi_connected", snapshot.wifi_connected);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    return json_response(req, root);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    return status_response(req);
}

static esp_err_t pages_get_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    return json_response(req, projector_pages_json());
}

static esp_err_t pages_put_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *root = read_json(req);
    if (!root) return ESP_OK;
    char error[128];
    esp_err_t err = projector_set_pages(root, error, sizeof(error));
    cJSON_Delete(root);
    if (err != ESP_OK) return error_response(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" :
                                                "500 Internal Server Error", error);
    return json_response(req, projector_pages_json());
}

static esp_err_t command_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *root = read_json(req);
    if (!root) return ESP_OK;
    char error[128];
    esp_err_t err = projector_set_command(root, error, sizeof(error));
    cJSON_Delete(root);
    if (err != ESP_OK) return error_response(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" :
                                                "500 Internal Server Error", error);
    return status_response(req);
}

static cJSON *calibration_json(void)
{
    projector_snapshot_t snapshot;
    projector_snapshot(&snapshot);
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "center_x", snapshot.calibration.center_x);
    cJSON_AddNumberToObject(root, "center_y", snapshot.calibration.center_y);
    cJSON_AddNumberToObject(root, "diameter", snapshot.calibration.diameter);
    cJSON_AddNumberToObject(root, "rotation", snapshot.calibration.rotation * 90);
    cJSON_AddBoolToObject(root, "mirror", snapshot.calibration.mirror);
    cJSON_AddBoolToObject(root, "active", snapshot.calibration.active);
    return root;
}

static esp_err_t calibration_get_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    return json_response(req, calibration_json());
}

static esp_err_t calibration_post_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *root = read_json(req);
    if (!root) return ESP_OK;
    char error[128];
    esp_err_t err = projector_set_calibration(root, error, sizeof(error));
    cJSON_Delete(root);
    if (err != ESP_OK) return error_response(req, err == ESP_ERR_INVALID_ARG ||
        err == ESP_ERR_INVALID_STATE ? "400 Bad Request" : "500 Internal Server Error", error);
    return json_response(req, calibration_json());
}

static esp_err_t ha_state_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *root = read_json(req);
    if (!root) return ESP_OK;
    char error[128];
    esp_err_t err = projector_set_ha_state(root, error, sizeof(error));
    cJSON_Delete(root);
    if (err != ESP_OK) return error_response(req, "400 Bad Request", error);
    return json_response(req, cJSON_CreateObject());
}

static void restart_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t network_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *root = read_json(req);
    if (!root) return ESP_OK;
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *timezone = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && cJSON_IsString(password) && cJSON_IsString(timezone) &&
        (password->valuestring[0] == 0 || strlen(password->valuestring) >= 8))
        err = projector_write_network(ssid->valuestring, password->valuestring, timezone->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) return error_response(req, "400 Bad Request", "Invalid network settings");
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "restarting", true);
    esp_err_t result = json_response(req, response);
    xTaskCreate(restart_task, "network-restart", 2048, NULL, 5, NULL);
    return result;
}

static esp_err_t config_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    char timezone[128];
    projector_timezone(timezone, sizeof(timezone));
    cJSON_AddStringToObject(root, "timezone", timezone);
    return json_response(req, root);
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || req->content_len < sizeof(esp_image_header_t) ||
        req->content_len > partition->size)
        return error_response(req, "413 Payload Too Large", "Invalid firmware size");
    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) return error_response(req, "500 Internal Server Error", "OTA begin failed");
    char buffer[1024];
    size_t received = 0;
    int timeouts = 0;
    while (received < req->content_len) {
        size_t expected = req->content_len - received;
        if (expected > sizeof(buffer)) expected = sizeof(buffer);
        int n = httpd_req_recv(req, buffer, expected);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) { err = ESP_FAIL; break; }
        err = esp_ota_write(handle, buffer, n);
        if (err != ESP_OK) break;
        received += n;
    }
    if (err == ESP_OK && received == req->content_len) err = esp_ota_end(handle);
    else esp_ota_abort(handle);
    if (err == ESP_OK) {
        esp_app_desc_t description;
        err = esp_ota_get_partition_description(partition, &description);
        if (err == ESP_OK && strcmp(description.project_name, "bed-projector") != 0)
            err = ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) err = esp_ota_set_boot_partition(partition);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return error_response(req, "400 Bad Request", "Firmware rejected");
    }
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "restarting", true);
    esp_err_t result = json_response(req, response);
    xTaskCreate(restart_task, "ota-restart", 2048, NULL, 5, NULL);
    return result;
}

static esp_err_t file_handler(httpd_req_t *req)
{
    const char *path = !strcmp(req->uri, "/") ? "/index.html" : req->uri;
    const char *filename;
    const char *type;
    if (!strcmp(path, "/index.html")) { type = "text/html; charset=utf-8"; filename = "/spiffs/index.html"; }
    else if (!strcmp(path, "/index.js")) { type = "text/javascript; charset=utf-8"; filename = "/spiffs/index.js"; }
    else if (!strcmp(path, "/index.css")) { type = "text/css; charset=utf-8"; filename = "/spiffs/index.css"; }
    else return error_response(req, "404 Not Found", "Not found");
    FILE *file = fopen(filename, "rb");
    if (!file) return error_response(req, "404 Not Found", "Not found");
    httpd_resp_set_type(req, type);
    char buffer[1024];
    size_t length;
    esp_err_t err = ESP_OK;
    while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0)
        if (httpd_resp_send_chunk(req, buffer, length) != ESP_OK) { err = ESP_FAIL; break; }
    fclose(file);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

esp_err_t bed_api_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        {.uri="/api/v1/pair", .method=HTTP_GET, .handler=pair_handler},
        {.uri="/api/v1/status", .method=HTTP_GET, .handler=status_handler},
        {.uri="/api/v1/pages", .method=HTTP_GET, .handler=pages_get_handler},
        {.uri="/api/v1/pages", .method=HTTP_PUT, .handler=pages_put_handler},
        {.uri="/api/v1/display", .method=HTTP_POST, .handler=command_handler},
        {.uri="/api/v1/calibration", .method=HTTP_GET, .handler=calibration_get_handler},
        {.uri="/api/v1/calibration", .method=HTTP_POST, .handler=calibration_post_handler},
        {.uri="/api/v1/ha/state", .method=HTTP_POST, .handler=ha_state_handler},
        {.uri="/api/v1/network", .method=HTTP_POST, .handler=network_handler},
        {.uri="/api/v1/config", .method=HTTP_GET, .handler=config_handler},
        {.uri="/api/v1/ota", .method=HTTP_POST, .handler=ota_handler},
        {.uri="/", .method=HTTP_GET, .handler=file_handler},
        {.uri="/index.html", .method=HTTP_GET, .handler=file_handler},
        {.uri="/index.js", .method=HTTP_GET, .handler=file_handler},
        {.uri="/index.css", .method=HTTP_GET, .handler=file_handler},
    };
    for (unsigned i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

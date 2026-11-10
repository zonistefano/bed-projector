#include "bed_net.h"
#include "projector_logic.h"
#include "projector.h"
#include "bootloader_random.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"

#define AP_SSID "bed-projector"
/* DHCP and mDNS name: the router lists it and browsers open bed-projector.local. */
#define HOSTNAME "bed-projector"
#define CONNECTED_BIT BIT0

static const char *TAG = "bed-net";
static EventGroupHandle_t wifi_events;
static esp_timer_handle_t reconnect_timer;
static unsigned reconnect_attempt;
/* Read by the display and HTTP tasks, written by the Wi-Fi event task. */
static atomic_bool provisioning;
static bool has_credentials;
static bool sntp_started;
static bool mdns_started;
static bool wifi_started;
static bool ap_ready;
static char ap_password[17];
static char device_id[13];

bool bed_net_is_provisioning(void) { return atomic_load(&provisioning); }

const char *bed_net_device_id(void) { return device_id; }

bool bed_net_ap_credentials(const char **ssid, const char **password)
{
    if (!bed_net_is_provisioning()) return false;
    *ssid = AP_SSID;
    *password = ap_password; /* Written once before the access point starts. */
    return true;
}

static void reconnect(void *argument)
{
    (void)argument;
    esp_wifi_connect();
}

static void stop_recovery_ap(void)
{
    if (!atomic_load(&provisioning) || !ap_ready) return;
    atomic_store(&provisioning, false);
    ap_ready = false;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) ESP_LOGW(TAG, "Recovery AP stop failed: %s", esp_err_to_name(err));
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && has_credentials) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        projector_set_wifi_connected(false);
        /* Retry with a growing pause instead of immediately: back-to-back scans
         * would also move the recovery access point off its channel. */
        if (has_credentials && !esp_timer_is_active(reconnect_timer))
            esp_timer_start_once(reconnect_timer, projector_backoff_ms(reconnect_attempt++) * 1000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        reconnect_attempt = 0;
        projector_set_wifi_connected(true);
        xEventGroupSetBits(wifi_events, CONNECTED_BIT);
        stop_recovery_ap();
        if (!sntp_started) {
            char timezone[128];
            projector_timezone(timezone, sizeof(timezone));
            setenv("TZ", timezone, 1);
            tzset();
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            sntp_started = true;
        }
        if (!mdns_started && mdns_init() == ESP_OK) {
            mdns_hostname_set(HOSTNAME);
            mdns_instance_name_set("Bed Projector");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            /* Home Assistant discovers this service; the id keeps the device recognisable
             * when its address changes. */
            mdns_txt_item_t txt[] = {{"id", device_id}};
            mdns_service_add(NULL, "_bed-projector", "_tcp", 80, txt, 1);
            mdns_started = true;
        }
    }
}

static esp_err_t start_access_point(void)
{
    wifi_config_t config = {0};
    strcpy((char *)config.ap.ssid, AP_SSID);
    strcpy((char *)config.ap.password, ap_password);
    config.ap.ssid_len = strlen((char *)config.ap.ssid);
    config.ap.max_connection = 2;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    atomic_store(&provisioning, true);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(has_credentials ? WIFI_MODE_APSTA : WIFI_MODE_AP), TAG, "AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "AP config");
    if (!wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP start");
        wifi_started = true;
    }
    ap_ready = true;
    if (xEventGroupGetBits(wifi_events) & CONNECTED_BIT) stop_recovery_ap();
    ESP_LOGI(TAG, "Configuration Wi-Fi AP SSID: " AP_SSID);
    ESP_LOGI(TAG, "Configuration Wi-Fi AP password: %s", ap_password);
    return ESP_OK;
}

esp_err_t bed_net_start(void)
{
    static const char hex[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    uint8_t random[16];
    bootloader_random_enable();
    esp_fill_random(random, sizeof(random));
    bootloader_random_disable();
    for (unsigned i = 0; i < 16; ++i) ap_password[i] = hex[random[i] % (sizeof(hex) - 1)];
    ap_password[16] = 0;
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "MAC");
    snprintf(device_id, sizeof(device_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    wifi_events = xEventGroupCreate();
    if (!wifi_events) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t timer = {.callback=reconnect, .name="wifi-reconnect"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &reconnect_timer), TAG, "reconnect timer");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (!sta) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(sta, HOSTNAME), TAG, "hostname");
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "WiFi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   event_handler, NULL), TAG, "WiFi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   event_handler, NULL), TAG, "IP events");
    char ssid[33], password[64];
    projector_read_network(ssid, sizeof(ssid), password, sizeof(password));
    has_credentials = ssid[0] != 0;
    if (!has_credentials) return start_access_point();
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", password);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "STA start");
    wifi_started = true;
    if (!(xEventGroupWaitBits(wifi_events, CONNECTED_BIT, pdFALSE, pdFALSE,
                              pdMS_TO_TICKS(20000)) & CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi unavailable, starting recovery AP");
        return start_access_point();
    }
    return ESP_OK;
}

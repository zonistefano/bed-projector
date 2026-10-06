#include "bed_api.h"
#include "bed_display.h"
#include "bed_net.h"
#include "projector.h"
#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

static const char *TAG = "bed-projector";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    bootloader_random_enable();
    err = projector_init();
    bootloader_random_disable();
    ESP_ERROR_CHECK(err);
    esp_vfs_spiffs_conf_t filesystem = {.base_path="/spiffs", .partition_label="spiffs",
        .max_files=4, .format_if_mount_failed=false};
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&filesystem));
    ESP_ERROR_CHECK(bed_display_init());
    bed_display_start();
    ESP_ERROR_CHECK(bed_net_start());
    ESP_ERROR_CHECK(bed_api_start());
    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    }
    ESP_LOGI(TAG, "Projector ready");
}

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(BED_ENV_FILE "${PROJECT_DIR}/.env")
set(BED_CONFIG_GENERATOR "${PROJECT_DIR}/tools/generate_build_config.py")
set(BED_CONFIG_DIR "${BUILD_DIR}/esp-idf/main")
set(BED_CONFIG_HEADER "${BED_CONFIG_DIR}/bed_build_config.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BED_CONFIG_GENERATOR}")
if(EXISTS "${BED_ENV_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BED_ENV_FILE}")
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${BED_CONFIG_GENERATOR}" "${BED_ENV_FILE}" "${BED_CONFIG_HEADER}"
    RESULT_VARIABLE BED_CONFIG_RESULT
    ERROR_VARIABLE BED_CONFIG_ERROR
)
if(NOT BED_CONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "${BED_CONFIG_ERROR}")
endif()

idf_component_register(
    SRCS "main.c" "projector.c" "projector_logic.c" "bed_display.c" "bed_icons.c" "bed_net.c" "bed_api.c"
    INCLUDE_DIRS "." "${BED_CONFIG_DIR}"
    REQUIRES app_update bootloader_support driver esp_event esp_http_server esp_lcd esp_netif
             esp_timer esp_wifi json lvgl esp_lvgl_port mdns nvs_flash spiffs
             teriyakigod__esp_lcd_st7735
)

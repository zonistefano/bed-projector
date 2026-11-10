idf_component_register(SRCS "esp_lcd_st7735.c"
                    INCLUDE_DIRS "include"
                    REQUIRES "esp_driver_gpio" "esp_driver_spi" "esp_lcd")

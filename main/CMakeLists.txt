idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        rootCA.pem
        device_cert.pem.crt
        device_key.pem.key
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        mqtt
        json
        esp_https_ota
        app_update
        esp_http_server
        mdns
        efuse
        spi_flash
        esp_hw_support
)

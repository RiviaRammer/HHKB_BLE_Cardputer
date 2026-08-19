idf_component_register(
    # usb_host_hid is resolved through idf_component.yml.
    SRCS "main.c" "web_debug.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hid usb_host_hid usb bt nvs_flash esp_wifi esp_event
             esp_netif esp_http_server esp_timer
    EMBED_TXTFILES "debug_page.html"
)

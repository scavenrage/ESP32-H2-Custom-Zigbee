idf_component_register(
    SRCS
        "main.c"
        "sw_config.c"
        "relay.c"
        "shutter.c"
        "dimmer.c"
        "ota.c"
        "led.c"
        "diag.c"
        "ble_update.c"
        "maint.c"
        "ble_log.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        bt
        driver
        esp_timer
        nvs_flash
        app_update
        espressif__esp-zigbee-lib
)

# ZB2: gli header di compatibilità 1.x di esp-zigbee-lib 2.x (include/compat/*.h) emettono
# '#warning "This file is deprecated..."' a ogni inclusione. -Wno-cpp nasconde solo i #warning
# e solo per i file di questo componente; gli altri warning (con -Werror) restano attivi.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-cpp)

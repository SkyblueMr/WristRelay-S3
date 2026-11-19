set(wake_requires)
if(WR_WAKE_WAKENET)
    list(APPEND wake_requires wake_profile espressif__esp-sr)
endif()

idf_component_register(
    SRCS
        "wristrelay_main.c"
        "audio/wr_audio.c"
        "audio/wr_opus.c"
        "display/wr_display.c"
        "power/power.c"
        "voice/wr_voice.c"
        "wake/wake.c"
    INCLUDE_DIRS "." "audio" "display" "power" "voice" "wake"
    PRIV_REQUIRES
        esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_event esp_lcd esp_netif
        esp_partition esp_psram esp_timer esp_wifi firmware_core mbedtls nvs_flash tcp_transport
        wristrelay_audio ${wake_requires}
)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_transport_connect")

if(WR_WAKE_WAKENET)
    if(NOT CONFIG_SR_WN_WN9_HIESP OR NOT CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions-wakenet.csv")
        message(FATAL_ERROR "WakeNet requires the sdkconfig.wakenet defaults and a separate SDKCONFIG")
    endif()
    target_sources(${COMPONENT_LIB} PRIVATE "wake/wakenet.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_WR_WAKE_WAKENET=1)
endif()

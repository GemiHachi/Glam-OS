idf_component_register(
    SRCS
        "os_main.c"
        "boot_anim.c"
        "os_spk_installer.c"
        "wifi_portal.c"
        "hub_player.c"
        "hub_protocol.c"
        "cover_fetcher.c"
        "netease_api.c"
        "lrc_parser.c"
        "ui/os_ui.c"
        "ui/os_i18n.c"
        "ui/os_kbd24.c"
        "ui/os_xz_view.c"
        "ui/ui_manager.c"
        "ui/ui_font_cjk_14.c"
        "xz/xz_service.c"
        "xz/xz_mcp.c"
    INCLUDE_DIRS
        "."
        "ui"
        "xz"
    REQUIRES
        bsp
        swift_abi
        esp_opus
        esp_websocket_client
        esp-sr
        lvgl
        esp_lvgl_port
        json
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        lwip
        driver
        vfs
        fatfs
        esp_partition
        esp_timer
        esp_http_client
        esp_http_server
        esp_app_format
        mbedtls
        esp_rom
)

get_filename_component(BOOT_ANIM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../assets/boot_anim.gmv" ABSOLUTE)
if(EXISTS "${BOOT_ANIM_SRC}")
    set(BOOT_ANIM_BIN "${CMAKE_BINARY_DIR}/boot_anim.gmv")
    add_custom_command(
        OUTPUT "${BOOT_ANIM_BIN}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BOOT_ANIM_SRC}" "${BOOT_ANIM_BIN}"
        DEPENDS "${BOOT_ANIM_SRC}"
        COMMENT "Staging boot animation image (${BOOT_ANIM_SRC}) for storage partition"
    )
    add_custom_target(boot_anim_bin ALL DEPENDS "${BOOT_ANIM_BIN}")
    esptool_py_flash_to_partition(flash "storage" "${BOOT_ANIM_BIN}")
    add_dependencies(flash boot_anim_bin)
else()
    message(WARNING "Boot animation asset not found at ${BOOT_ANIM_SRC}; storage partition will not be flashed.")
endif()

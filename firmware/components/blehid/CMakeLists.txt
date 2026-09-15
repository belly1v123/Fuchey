# Fuchey — blehid IDF component (Bluedroid HIDD, port of badge-hackgdl-2025 ble_hid)
idf_build_get_property(idf_ver IDF_VERSION_MAJOR)
if(idf_ver GREATER_EQUAL 6)
    set(ble_hid_driver_deps esp_driver_gpio)
else()
    set(ble_hid_driver_deps driver)
endif()

idf_component_register(SRCS "blehid_remote.c"
                            "esp_hidd_prf_api.c"
                            "hid_dev.c"
                            "hid_device_le_prf.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES bt nvs_flash ${ble_hid_driver_deps})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)

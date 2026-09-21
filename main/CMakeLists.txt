# ============================================================================
# main component - Driver Drowsiness Detection
# ============================================================================
idf_component_register(
    SRCS
        "app_main.cpp"
        "camera_utils.cpp"
        "face_pipeline.cpp"
        "landmark_pfld.cpp"
        "drowsiness_detector.cpp"
        "alarm_control.cpp"
        "benchmark.cpp"
        "console_cmds.cpp"
        "web_server.cpp"
        "config_store.cpp"
        "report_client.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        console
        driver
        esp_wifi
        esp_netif
        esp_event
        esp_psram
        nvs_flash
        mdns
        esp_http_server
        esp_http_client
        app_update
)

# Embed HTML pages for web monitor
target_add_binary_data(${COMPONENT_LIB} "www/index.html" TEXT)
target_add_binary_data(${COMPONENT_LIB} "www/admin.html" TEXT)

# Embed .espdl models into RODATA
set(MODELS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../model)

foreach(model_file espdet_pico_224_224_face.espdl ${CONFIG_DROWSY_PFLD_MODEL_FILE})
    if(NOT EXISTS "${MODELS_DIR}/${model_file}")
        message(FATAL_ERROR "Model file '${model_file}' not found in ${MODELS_DIR}.")
    endif()
endforeach()

idf_build_get_property(component_targets __COMPONENT_TARGETS)
if("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
endif()

set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)

target_add_aligned_binary_data(${COMPONENT_LIB} "${MODELS_DIR}/espdet_pico_224_224_face.espdl" BINARY)
target_add_aligned_binary_data(${COMPONENT_LIB} "${MODELS_DIR}/${CONFIG_DROWSY_PFLD_MODEL_FILE}" BINARY)

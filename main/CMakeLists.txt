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
        nvs_flash
        mdns
        esp_http_server
        esp_http_client
        app_update
)

# Nhúng trang HTML cho web monitor (dấu chấm -> gạch dưới trong tên symbol)
target_add_binary_data(${COMPONENT_LIB} "www/index.html" TEXT)
target_add_binary_data(${COMPONENT_LIB} "www/admin.html" TEXT)

# ----------------------------------------------------------------------------
# NHÚNG model .espdl vào firmware (RODATA)
#  - espdet_pico_224_224_face.espdl : face detection PicoDet 1 stage (480KB)
#  - <CONFIG_DROWSY_PFLD_MODEL_FILE> : PFLD landmark (68 hoặc 98 điểm)
#     pfld68_pyfeat.espdl (68, mặc định) hoặc pfld_landmarks_98.espdl (98)
# Tên symbol truy cập: _binary_<tên_file_đổi_dấu_chấm_thành_gạch_dưới>_start
# ----------------------------------------------------------------------------
set(MODELS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../model)

# Kiểm tra model tồn tại trước khi build - báo lỗi rõ ràng nếu thiếu
foreach(model_file espdet_pico_224_224_face.espdl ${CONFIG_DROWSY_PFLD_MODEL_FILE})
    if(NOT EXISTS "${MODELS_DIR}/${model_file}")
        message(FATAL_ERROR
            "Model file '${model_file}' not found in ${MODELS_DIR}.\n"
            "  -> Face detect: ./model/download_models.sh\n"
            "  -> Quantize PFLD: python tools/quantize_pfld.py -i model/pfld68_pyfeat.onnx -o model/pfld68_pyfeat.espdl\n"
            "  Xem chi tiết: model/README.md và tools/quantize_pfld_espdl.md")
    endif()
endforeach()

# Tìm esp-dl (component từ registry - tên được prefix espressif__)
idf_build_get_property(component_targets __COMPONENT_TARGETS)
if("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
endif()
# BẮT BUỘC: phải set cmake_dir (dùng trong utilities.cmake khi nhúng model)
set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)

# Nhúng 2 file model - tên symbol theo tên file
target_add_aligned_binary_data(${COMPONENT_LIB} "${MODELS_DIR}/espdet_pico_224_224_face.espdl" BINARY)
target_add_aligned_binary_data(${COMPONENT_LIB} "${MODELS_DIR}/${CONFIG_DROWSY_PFLD_MODEL_FILE}" BINARY)

# ============================================================================
# Driver Drowsiness Detection - ESP32-S3-N16R8 (ESP-WHO + ESP-DL v3 + PFLD)
# ============================================================================
# Project hoàn toàn độc lập: dùng esp-dl v3.x + esp32-camera từ IDF Component
# Registry (xem main/idf_component.yml). KHÔNG dùng submodule esp-dl v2.0 của
# esp-who nên các example khác không bị ảnh hưởng.
#
# Yêu cầu: ESP-IDF >= 5.3  (esp-dl v3.3 yêu cầu >= 5.3)
# ============================================================================
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(driver_drowsiness_detection)

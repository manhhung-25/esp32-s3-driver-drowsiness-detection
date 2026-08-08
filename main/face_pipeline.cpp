// ============================================================================
// face_pipeline.cpp - Face detection bằng PicoDet 1 stage (ESP-DL v3)
// ----------------------------------------------------------------------------
// Replicate cách esp-dl v3 nạp model espdet_pico (từ human_face_detect.cpp):
//   - ImagePreprocessor mean={0,0,0} std={255,255,255} + letterbox(114,114,114)
//   - ESPDetPostprocessor stages {{8,8,4,4},{16,16,8,8},{32,32,16,16}}
// ============================================================================

#include "face_pipeline.hpp"

#include "esp_log.h"

static const char *TAG = "face_pipeline";

// Symbol model nhúng: model/espdet_pico_224_224_face.espdl
extern const uint8_t pico_model_data[] asm("_binary_espdet_pico_224_224_face_espdl_start");

// Ngưỡng phát hiện (PicoDet - default của esp-dl model zoo: 0.5; hạ 0.3 cho nhạy)
#define DET_SCORE_THR 0.3f
#define DET_NMS_THR   0.7f
#define DET_TOP_K     10

FacePipeline::~FacePipeline()
{
    delete m_post;
    delete m_pre;
    delete m_model;
}

bool FacePipeline::init()
{
    m_model = new dl::Model((const char *)pico_model_data, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    m_model->minimize();

    // PicoDet: chuẩn hoá /255 + letterbox nền xám (114,114,114) - giống bản gốc
    m_pre = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});
    m_pre->enable_letterbox({114, 114, 114});

    m_post = new dl::detect::ESPDetPostProcessor(
        m_model, m_pre, DET_SCORE_THR, DET_NMS_THR, DET_TOP_K,
        {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});

    ESP_LOGI(TAG, "PicoDet face model loaded (espdet_pico_224_224_face).");
    return true;
}

bool FacePipeline::detect(const dl::image::img_t &img,
                          std::vector<int> &face_box,
                          std::vector<int> &keypoints,
                          int *msr_count,
                          int *mnp_count)
{
    // ---- 1. Quét toàn ảnh (1 stage) ----
    m_pre->preprocess(img);
    m_model->run();
    m_post->clear_result();
    m_post->postprocess();
    std::list<dl::detect::result_t> &results = m_post->get_result(img.width, img.height);
    if (msr_count) *msr_count = (int)results.size();
    if (mnp_count) *mnp_count = (int)results.size();

    if (results.empty()) {
        return false;
    }

    // ---- 2. Chọn khuôn mặt GẦN NHẤT (bbox lớn nhất = tài xế) ----
    const dl::detect::result_t *best = nullptr;
    int best_area = 0;
    for (auto &r : results) {
        int area = r.box_area();
        if (area > best_area) {
            best_area = area;
            best = &r;
        }
    }
    if (best == nullptr) {
        return false;
    }

    face_box = best->box;
    keypoints.clear(); // PicoDet không có keypoint - EAR dùng landmark PFLD riêng
    return true;
}

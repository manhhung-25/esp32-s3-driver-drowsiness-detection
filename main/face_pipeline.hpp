#pragma once

// ============================================================================
// face_pipeline.hpp - Phát hiện khuôn mặt (PicoDet 1 stage)
// ----------------------------------------------------------------------------
// Dùng model pre-quantized .espdl của ESP-DL v3 (nhúng RODATA):
//   - espdet_pico_224_224_face.espdl (PicoDet - phát hiện mặt nhỏ/xa tốt,
//     nhanh hơn MSR+MNP hai tầng, input 224x224 có letterbox)
// Lý do đổi từ MSR+MNP (MTMN): với MTMN `msr=0` (không tìm thấy mặt nào dù
// ảnh có mặt) - PicoDet mạnh hơn với mặt nhỏ + nhạy hơn.
// Không cần vendor component models/human_face_detect - dùng trực tiếp
// postprocessor có sẵn trong lõi esp-dl (dl::detect::ESPDetPostprocessor).
// ============================================================================

#include <list>
#include <vector>

#include "dl_detect_espdet_postprocessor.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"

class FacePipeline {
public:
    FacePipeline() = default;
    ~FacePipeline();

    /**
     * @brief Nạp model face detect PicoDet từ RODATA (nhúng trong firmware).
     * @return true nếu thành công.
     */
    bool init();

    /**
     * @brief Phát hiện khuôn mặt gần nhất (bbox lớn nhất) trong frame.
     *
     * @param img       Ảnh RGB565 (wrap từ camera_fb_t).
     * @param face_box  [x0, y0, x1, y1] của khuôn mặt (tọa độ frame) nếu tìm thấy.
     * @param keypoints (không dùng với PicoDet - để trống).
     * @param msr_count debug: tổng detection trước khi chọn (đồng bộ log cũ).
     * @param mnp_count debug: số khuôn mặt sau postprocess/NMS.
     * @return true nếu phát hiện được khuôn mặt.
     */
    bool detect(const dl::image::img_t &img,
                std::vector<int> &face_box,
                std::vector<int> &keypoints,
                int *msr_count = NULL,
                int *mnp_count = NULL);

private:
    dl::Model *m_model = nullptr;
    dl::image::ImagePreprocessor *m_pre = nullptr;
    dl::detect::ESPDetPostProcessor *m_post = nullptr; // lưu ý viết hoa P
};

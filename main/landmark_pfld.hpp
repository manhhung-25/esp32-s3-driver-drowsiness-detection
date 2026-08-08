#pragma once

// ============================================================================
// landmark_pfld.hpp - PFLD landmark (68 hoặc 98 điểm) + solvePnP head pose 3D
// ----------------------------------------------------------------------------
// Model mặc định: py-feat/pfld 68 điểm (iBUG/300-W mapping) - output [1,136].
//   Input 112x112 RGB (normalize /255), output = N*2 điểm (x,y).
// Mapping theo CONFIG_DROWSY_PFLD_NUM_POINTS:
//   68 điểm (iBUG/300-W): mũi 27-35, MẮT 36-41 (T) / 42-47 (P), miệng 48-67
//   98 điểm (WFLW):        mũi 51-59, MẮT 60-67 (T) / 68-75 (P), miệng 76-97
// EAR: bbox h/w mắt - robust với thứ tự điểm bên trong từng mắt.
// Head pose 3D: solvePnP xấp xỉ từ landmark 2D + mô hình mặt 3D chuẩn.
// ============================================================================

#include <vector>

#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"

#define PFLD_INPUT_SIZE 112
#define PFLD_NUM_POINTS CONFIG_DROWSY_PFLD_NUM_POINTS
#define PFLD_OUTPUT_SIZE (PFLD_NUM_POINTS * 2)

class LandmarkPFLD {
public:
    LandmarkPFLD() = default;
    ~LandmarkPFLD();
    bool init();
    bool run(const dl::image::img_t &frame, const std::vector<int> &face_box, float *landmarks);

private:
    dl::Model *m_model = nullptr;
    dl::image::ImagePreprocessor *m_pre = nullptr;
};

// ----------------------------------------------------------------------------
// FaceMetrics - số liệu từ 98 landmark + pose 3D
// ----------------------------------------------------------------------------
struct FaceMetrics {
    float ear = 0.0f;      // Eye Aspect Ratio (0 = nhắm, ~0.3 = mở)
    float mar = 0.0f;      // Mouth Aspect Ratio
    float pitch = 0.35f;   // head pitch 3D (rad) từ solvePnP
    float yaw = 0.0f;      // head yaw 3D (rad)
    float roll = 0.0f;     // head roll 3D (rad)
    float roll_2d = 0.0f;  // roll ước lượng 2D (derotate)
};

bool compute_face_metrics(const float *landmarks, FaceMetrics *out);

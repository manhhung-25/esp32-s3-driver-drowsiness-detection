// ============================================================================
// landmark_pfld.cpp - PFLD landmark (68/98 điểm) + EAR/MAR + solvePnP pose 3D
// ============================================================================

#include "landmark_pfld.hpp"

#include <math.h>

#include "esp_log.h"
#include "dl_tensor_base.hpp"

static const char *TAG = "landmark_pfld";

// Symbol model nhúng: theo Kconfig CONFIG_DROWSY_PFLD_MODEL_FILE
//   "pfld68.espdl"               -> _binary_pfld68_espdl_start
//   "pfld_landmarks_98.espdl"    -> _binary_pfld_landmarks_98_espdl_start
#if CONFIG_DROWSY_PFLD_68PT
extern const uint8_t pfld_model_data[] asm("_binary_pfld68_espdl_start");
#else
extern const uint8_t pfld_model_data[] asm("_binary_pfld_landmarks_98_espdl_start");
#endif

LandmarkPFLD::~LandmarkPFLD()
{
    delete m_pre;
    delete m_model;
}

bool LandmarkPFLD::init()
{
    m_model = new dl::Model((const char *)pfld_model_data, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    m_model->minimize();

    const float mean = (float)CONFIG_DROWSY_PFLD_MEAN;
    const float std = (float)CONFIG_DROWSY_PFLD_STD;
#if CONFIG_DROWSY_PFLD_RGB_SWAP
    const bool rgb_swap = true;
#else
    const bool rgb_swap = false;
#endif
    m_pre = new dl::image::ImagePreprocessor(m_model, {mean, mean, mean}, {std, std, std}, rgb_swap);
    ESP_LOGI(TAG, "PFLD-%d model loaded (%d điểm)", PFLD_NUM_POINTS, PFLD_NUM_POINTS);
    return true;
}

bool LandmarkPFLD::run(const dl::image::img_t &frame, const std::vector<int> &face_box, float *landmarks)
{
    if (face_box.size() < 4 || landmarks == nullptr) return false;

    // ---- Chuyển face_box (chữ nhật PicoDet) thành SQUARE BOX (vuông + margin 1.15x) ----
    // PFLD model được train trên crop vuông -> nếu crop chữ nhật bị co giãn làm sai lệch landmark.
    float x1 = (float)face_box[0];
    float y1 = (float)face_box[1];
    float x2 = (float)face_box[2];
    float y2 = (float)face_box[3];

    float w = x2 - x1;
    float h = y2 - y1;
    if (w <= 0.0f || h <= 0.0f) return false;

    float cx = (x1 + x2) * 0.5f;
    float cy = (y1 + y2) * 0.5f;

    // Tỉ lệ mở rộng nhẹ (1.15x) để trùm hết cằm/trán/mắt
    float max_dim = (w > h ? w : h) * 1.15f;
    const float max_allowed = frame.width < frame.height ? (float)frame.width : (float)frame.height;
    if (max_dim > max_allowed) max_dim = max_allowed;

    float sq_x1 = cx - max_dim * 0.5f;
    float sq_y1 = cy - max_dim * 0.5f;
    float sq_x2 = cx + max_dim * 0.5f;
    float sq_y2 = cy + max_dim * 0.5f;

    // Clamp vào biên ảnh
    if (sq_x1 < 0.0f) { sq_x2 -= sq_x1; sq_x1 = 0.0f; }
    if (sq_y1 < 0.0f) { sq_y2 -= sq_y1; sq_y1 = 0.0f; }
    if (sq_x2 > (float)frame.width) { sq_x1 -= sq_x2 - (float)frame.width; sq_x2 = (float)frame.width; }
    if (sq_y2 > (float)frame.height) { sq_y1 -= sq_y2 - (float)frame.height; sq_y2 = (float)frame.height; }

    std::vector<int> sq_box = {(int)sq_x1, (int)sq_y1, (int)sq_x2, (int)sq_y2};

    m_pre->preprocess(frame, sq_box);
    m_model->run();

    dl::TensorBase *out = m_model->get_output();
    if (out == nullptr) { ESP_LOGE(TAG, "No output"); return false; }

    // Output [1,N*2] (N = 68 hoặc 98 điểm) - cần đúng PFLD_OUTPUT_SIZE phần tử
    std::vector<int> shape = out->get_shape();
    int total = 1;
    for (int d : shape) total *= d;
    if (total != PFLD_OUTPUT_SIZE) {
        ESP_LOGE(TAG, "PFLD-%d output size=%d (cần %d)", PFLD_NUM_POINTS, total, PFLD_OUTPUT_SIZE);
        return false;
    }

    const dl::dtype_t dtype = out->get_dtype();
    const float scale = dtype == dl::DATA_TYPE_FLOAT ? 1.0f : powf(2.0f, (float)out->get_exponent());
    const float box_w = (float)(sq_box[2] - sq_box[0]);
    const float box_h = (float)(sq_box[3] - sq_box[1]);

    for (int i = 0; i < PFLD_NUM_POINTS; ++i) {
        float nx = 0.0f;
        float ny = 0.0f;
        switch (dtype) {
        case dl::DATA_TYPE_INT8: {
            const int8_t *data = out->get_element_ptr<int8_t>();
            nx = (float)data[i * 2] * scale;
            ny = (float)data[i * 2 + 1] * scale;
            break;
        }
        case dl::DATA_TYPE_INT16: {
            const int16_t *data = out->get_element_ptr<int16_t>();
            nx = (float)data[i * 2] * scale;
            ny = (float)data[i * 2 + 1] * scale;
            break;
        }
        case dl::DATA_TYPE_FLOAT: {
            const float *data = out->get_element_ptr<float>();
            nx = data[i * 2];
            ny = data[i * 2 + 1];
            break;
        }
        default:
            ESP_LOGE(TAG, "Unsupported PFLD output dtype: %d", (int)dtype);
            return false;
        }

        if (!isfinite(nx) || !isfinite(ny) || nx < -0.15f || nx > 1.15f || ny < -0.15f || ny > 1.15f) {
            ESP_LOGW(TAG, "PFLD point %d outside normalized crop: %.3f, %.3f", i, nx, ny);
            return false;
        }
        landmarks[i * 2] = (float)sq_box[0] + nx * box_w;
        landmarks[i * 2 + 1] = (float)sq_box[1] + ny * box_h;
    }
    return true;
}

// ============================================================================
// EAR (công thức chuẩn Soukupová & Čech 2016) / MAR (bbox sau derotate) / pose
// ============================================================================

// ---- Khoảng cách Euclidean giữa 2 điểm landmark ----
static inline float point_dist(const float *lm, int a, int b)
{
    float dx = lm[a*2] - lm[b*2];
    float dy = lm[a*2+1] - lm[b*2+1];
    return sqrtf(dx*dx + dy*dy);
}

// ---- EAR 6 điểm chuẩn: EAR = (|p2-p6| + |p3-p5|) / (2*|p1-p4|) ----
// Bất biến với head roll vì chỉ dùng khoảng cách giữa các điểm đối xứng.
static inline float ear_6pt(const float *lm,
                            int p1, int p2, int p3, int p4, int p5, int p6)
{
    float w = point_dist(lm, p1, p4); // chiều rộng mắt (giữa 2 khoé)
    if (w < 1e-3f) return 0.0f;       // suy biến -> tránh chia 0
    return (point_dist(lm, p2, p6) + point_dist(lm, p3, p5)) / (2.0f * w);
}

// ---- EAR 8 điểm (WFLW): EAR = (|p2-p8| + |p3-p7| + |p4-p6|) / (3*|p1-p5|) ----
static inline float ear_8pt(const float *lm,
                            int p1, int p2, int p3, int p4,
                            int p5, int p6, int p7, int p8)
{
    float w = point_dist(lm, p1, p5); // chiều rộng mắt
    if (w < 1e-3f) return 0.0f;
    return (point_dist(lm, p2, p8) + point_dist(lm, p3, p7) +
            point_dist(lm, p4, p6)) / (3.0f * w);
}

// ---- Roll (góc nghiêng đầu) từ tâm 2 mắt - dùng cho derotate MAR ----
static inline float eye_roll(const float *lm,
                             int eyeL0, int eyeL1, int eyeR0, int eyeR1)
{
    float lx = 0, ly = 0, rx = 0, ry = 0;
    for (int i = eyeL0; i <= eyeL1; ++i) { lx += lm[i*2];   ly += lm[i*2+1]; }
    for (int i = eyeR0; i <= eyeR1; ++i) { rx += lm[i*2];   ry += lm[i*2+1]; }
    lx /= (eyeL1 - eyeL0 + 1); ly /= (eyeL1 - eyeL0 + 1);
    rx /= (eyeR1 - eyeR0 + 1); ry /= (eyeR1 - eyeR0 + 1);
    return atan2f(ry - ly, rx - lx);
}

// ---- bbox cao/rộng (h/w) của dải điểm SAU khi xoay ngược -roll quanh (cx,cy) ----
// Dùng cho MAR: miệng nghiêng theo đầu -> derotate về ngang rồi mới đo.
static inline float bbox_ratio_derotated(const float *lm, int start, int end,
                                         float roll, float cx, float cy)
{
    float c = cosf(-roll), s = sinf(-roll);
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (int i = start; i <= end; ++i) {
        float x = lm[i*2] - cx, y = lm[i*2+1] - cy;
        float rx = x*c - y*s, ry = x*s + y*c; // xoay quanh tâm mặt
        if (rx < min_x) min_x = rx;
        if (rx > max_x) max_x = rx;
        if (ry < min_y) min_y = ry;
        if (ry > max_y) max_y = ry;
    }
    float w = max_x - min_x, h = max_y - min_y;
    return (w > 1e-3f) ? h / w : 0.0f;
}

// ---- solvePnP xấp xỉ: 6 điểm ổn định (mắt T/P, mũi, miệng) -> yaw/pitch/roll ----
#define PNP_NPTS 6
static const float FACE3D[PNP_NPTS][3] = {
    {-0.5f,  0.0f, 0.0f},
    { 0.5f,  0.0f, 0.0f},
    { 0.0f, -0.2f, 0.0f},
    { 0.0f, -0.5f, 0.0f},
    {-0.3f, -0.6f, 0.0f},
    { 0.3f, -0.6f, 0.0f},
};

static bool estimate_head_pose(const float *lm, FaceMetrics *out)
{
    // Mapping theo model:
    //   68 điểm (iBUG): mắt 36-41/42-47, mũi 27-35, miệng 48-67
    //   98 điểm (WFLW): mắt 60-67/68-75, mũi 51-59, miệng 76-97
    // Dùng: mắt T, mắt P, chóp mũi, dưới mũi, khóe miệng T/P
#if PFLD_NUM_POINTS == 68
    const int eyeL0 = 36, eyeL1 = 41, eyeR0 = 42, eyeR1 = 47;
    const int nose_tip = 30, nose_bot = 33, mouthL = 48, mouthR = 54;
#else
    const int eyeL0 = 60, eyeL1 = 67, eyeR0 = 68, eyeR1 = 75;
    const int nose_tip = 58, nose_bot = 55, mouthL = 76, mouthR = 82;
#endif
    float pts2d[PNP_NPTS][2];
    float ex = 0, ey = 0;
    for (int i = eyeL0; i <= eyeL1; ++i) { ex += lm[i*2]; ey += lm[i*2+1]; }
    pts2d[0][0] = ex/(eyeL1-eyeL0+1); pts2d[0][1] = ey/(eyeL1-eyeL0+1);
    ex = 0; ey = 0;
    for (int i = eyeR0; i <= eyeR1; ++i) { ex += lm[i*2]; ey += lm[i*2+1]; }
    pts2d[1][0] = ex/(eyeR1-eyeR0+1); pts2d[1][1] = ey/(eyeR1-eyeR0+1);
    pts2d[2][0] = lm[nose_tip*2];   pts2d[2][1] = lm[nose_tip*2+1];
    pts2d[3][0] = lm[nose_bot*2];   pts2d[3][1] = lm[nose_bot*2+1];
    pts2d[4][0] = lm[mouthL*2];     pts2d[4][1] = lm[mouthL*2+1];
    pts2d[5][0] = lm[mouthR*2];     pts2d[5][1] = lm[mouthR*2+1];

    float iod = sqrtf(powf(pts2d[1][0]-pts2d[0][0],2) + powf(pts2d[1][1]-pts2d[0][1],2));
    if (iod < 1e-3f) return false;
    float mx = (pts2d[0][0]+pts2d[1][0])*0.5f, my = (pts2d[0][1]+pts2d[1][1])*0.5f;
    for (int i = 0; i < PNP_NPTS; ++i) {
        pts2d[i][0] = (pts2d[i][0]-mx)/iod;
        pts2d[i][1] = (pts2d[i][1]-my)/iod;
    }

    float roll = atan2f(pts2d[1][1]-pts2d[0][1], pts2d[1][0]-pts2d[0][0]);
    out->roll_2d = roll;

    float c = cosf(-roll), s = sinf(-roll);
    for (int i = 0; i < PNP_NPTS; ++i) {
        float x = pts2d[i][0], y = pts2d[i][1];
        pts2d[i][0] = x*c - y*s;
        pts2d[i][1] = x*s + y*c;
    }

    float nose_x = pts2d[2][0];
    float nose_y = pts2d[2][1];
    float mouth_y = pts2d[4][1];
    // fix M1: mouth_y -> 0 KHÔNG phải lỗi mà là lúc đầu GỤC mạnh (dấu hiệu
    // buồn ngủ rõ nhất) -> pitch phải đạt MAX chứ không được bỏ frame.
    float pitch;
    if (fabsf(mouth_y) > 1e-3f) {
        float pitch_ratio = nose_y / mouth_y;
        pitch = (pitch_ratio - 0.33f) * 1.5f;
    } else {
        pitch = (nose_y >= 0.0f) ? 1.0f : -1.0f; // gục/cúi mạnh -> max
    }
    if (pitch > 1.0f) pitch = 1.0f;
    if (pitch < -1.0f) pitch = -1.0f;
    out->pitch = pitch;
    out->yaw = nose_x * 1.2f;
    out->roll = roll;
    return true;
}

bool compute_face_metrics(const float *lm, FaceMetrics *out)
{
    if (lm == nullptr || out == nullptr) return false;

    // ---- Mapping landmark theo model ----
#if PFLD_NUM_POINTS == 68
    // iBUG/300-W: mắt 36-41 (T) / 42-47 (P), miệng ngoài 48-59
    const int eyeL0 = 36, eyeL1 = 41, eyeR0 = 42, eyeR1 = 47;
    const int mouth0 = 48, mouth1 = 59;
#else
    // WFLW: mắt 60-67 (T) / 68-75 (P), miệng 76-95
    const int eyeL0 = 60, eyeL1 = 67, eyeR0 = 68, eyeR1 = 75;
    const int mouth0 = 76, mouth1 = 95;
#endif

    // ---- Roll từ tâm 2 mắt (dùng cho MAR derotate) + tâm mặt ----
    float roll = eye_roll(lm, eyeL0, eyeL1, eyeR0, eyeR1);
    float lx = 0, ly = 0, rx = 0, ry = 0;
    for (int i = eyeL0; i <= eyeL1; ++i) { lx += lm[i*2];   ly += lm[i*2+1]; }
    for (int i = eyeR0; i <= eyeR1; ++i) { rx += lm[i*2];   ry += lm[i*2+1]; }
    lx /= (eyeL1 - eyeL0 + 1); ly /= (eyeL1 - eyeL0 + 1);
    rx /= (eyeR1 - eyeR0 + 1); ry /= (eyeR1 - eyeR0 + 1);
    const float cx = (lx + rx) * 0.5f, cy = (ly + ry) * 0.5f;

    // ---- EAR chuẩn (Soukupová & Čech 2016): bất biến với head roll ----
#if PFLD_NUM_POINTS == 68
    // Mắt trái 36-41: p1=36,p2=37,p3=38,p4=39,p5=40,p6=41
    // Mắt phải 42-47: p1=42,p2=43,p3=44,p4=45,p5=46,p6=47
    float ear_l = ear_6pt(lm, 36, 37, 38, 39, 40, 41);
    float ear_r = ear_6pt(lm, 42, 43, 44, 45, 46, 47);
#else
    // Mắt trái 60-67: p1=60..p8=67 ; Mắt phải 68-75: p1=68..p8=75
    float ear_l = ear_8pt(lm, 60, 61, 62, 63, 64, 65, 66, 67);
    float ear_r = ear_8pt(lm, 68, 69, 70, 71, 72, 73, 74, 75);
#endif
    out->ear = (ear_l + ear_r) * 0.5f;

    // ---- MAR: bbox cao/rộng miệng SAU khi derotate theo -roll ----
    out->mar = bbox_ratio_derotated(lm, mouth0, mouth1, roll, cx, cy);

    // ---- Pose 3D (solvePnP xấp xỉ) ----
    // fix M1: KHÔNG vứt bỏ EAR/MAR khi pose ước lượng fail! EAR/MAR đã tính
    // xong và hợp lệ ở trên. Pose fail (mắt trùng) chỉ làm pitch/yaw/roll
    // không tin cậy -> dùng default, frame vẫn phải qua state machine
    // (tránh đóng băng PERCLOS/state đúng lúc gục đầu).
    estimate_head_pose(lm, out);

    // ---- Sanity (EAR/MAR vẫn được kiểm tra độc lập) ----
    if (out->ear > 0.75f) return false;
    if (out->mar > 2.0f) return false;
    if (fabsf(out->roll_2d) > 0.9f) return false;
    return true;
}

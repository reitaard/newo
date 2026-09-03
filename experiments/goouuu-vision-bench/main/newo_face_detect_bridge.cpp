#include <algorithm>
#include <list>

#include "esp_log.h"
#include "human_face_detect.hpp"

namespace {
constexpr const char *TAG = "newo_landmarks";
constexpr float LANDMARK_SCORE_THRESHOLD = 0.30f;
constexpr float LANDMARK_NMS_THRESHOLD = 0.50f;

class EspDetMnpLandmarkPipeline : public dl::detect::Detect {
public:
    EspDetMnpLandmarkPipeline(const char *espdet_model,
                              float detector_score_thr,
                              float detector_nms_thr,
                              const char *mnp_model,
                              float landmark_score_thr,
                              float landmark_nms_thr) :
        m_detector(espdet_model, detector_score_thr, detector_nms_thr),
        m_landmarks(mnp_model, landmark_score_thr, landmark_nms_thr)
    {
        ESP_LOGI(TAG,
                 "ESPDet-224 + MNP landmark bridge ready (det=%.2f landmark=%.2f)",
                 static_cast<double>(detector_score_thr),
                 static_cast<double>(landmark_score_thr));
    }

    std::list<dl::detect::result_t> &run(const dl::image::img_t &img) override
    {
        auto &detected = m_detector.run(img);
        m_results = detected;
        if (m_results.empty()) return m_results;

        // Recognition only uses the largest face. Keep the physically validated
        // ESPDet box/score and ask MNP only for the 5 facial landmarks MFN needs.
        auto largest = std::max_element(m_results.begin(), m_results.end(), [](const auto &a, const auto &b) {
            return a.box_area() < b.box_area();
        });

        std::list<dl::detect::result_t> candidate;
        candidate.push_back(*largest);
        auto &refined = m_landmarks.run(img, candidate);
        if (!refined.empty()) {
            const auto best_refined = std::max_element(refined.begin(), refined.end(), [](const auto &a, const auto &b) {
                return a.score < b.score;
            });
            if (best_refined != refined.end() && best_refined->keypoint.size() == 10) {
                largest->keypoint = best_refined->keypoint;
            }
        }
        return m_results;
    }

    dl::detect::Detect &set_score_thr(float score_thr, int idx) override
    {
        if (idx == 0) m_detector.set_score_thr(score_thr);
        else m_landmarks.set_score_thr(score_thr);
        return *this;
    }

    dl::detect::Detect &set_nms_thr(float nms_thr, int idx) override
    {
        if (idx == 0) m_detector.set_nms_thr(nms_thr);
        else m_landmarks.set_nms_thr(nms_thr);
        return *this;
    }

    dl::Model *get_raw_model(int idx) override
    {
        return idx == 0 ? m_detector.get_raw_model() : m_landmarks.get_raw_model();
    }

private:
    human_face_detect::ESPDet m_detector;
    human_face_detect::MNP m_landmarks;
    std::list<dl::detect::result_t> m_results;
};
} // namespace

// The main recognition firmware was written against HumanFaceDetect. CMake
// renames that class to NewoFaceDetect only inside this component so we can
// preserve the existing benchmark source while adding the missing landmark
// stage. Upstream ESPDet intentionally returns boxes with an empty keypoint
// vector; MNP supplies the 10 x/y landmark integers required by MFN.
NewoFaceDetect::NewoFaceDetect(model_type_t model_type, bool lazy_load) : m_model_type(model_type)
{
    m_model = nullptr;
    m_score_thr[0] = human_face_detect::ESPDet::default_score_thr;
    m_nms_thr[0] = human_face_detect::ESPDet::default_nms_thr;
    m_score_thr[1] = LANDMARK_SCORE_THRESHOLD;
    m_nms_thr[1] = LANDMARK_NMS_THRESHOLD;
    if (!lazy_load) load_model();
}

void NewoFaceDetect::load_model()
{
    if (m_model) return;
    if (m_model_type != model_type_t::ESPDET_PICO_224_224_FACE) {
        ESP_LOGE(TAG, "recognition bridge only supports ESPDET_PICO_224_224_FACE");
        abort();
    }

    m_model = new EspDetMnpLandmarkPipeline("espdet_pico_224_224_face.espdl",
                                            m_score_thr[0],
                                            m_nms_thr[0],
                                            "human_face_detect_mnp_s8_v1.espdl",
                                            m_score_thr[1],
                                            m_nms_thr[1]);
    if (!m_model) abort();
}

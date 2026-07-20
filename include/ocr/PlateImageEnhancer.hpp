#pragma once

#include <string>

namespace ocr {

struct PlatePreprocessResult {
    std::string enhanced_path;
    bool candidate_detected{false};
};

// detect_candidate=true이면 IVA/차량 장면에서 번호판 후보를 먼저 찾고 원근 보정한다.
// false이면 입력 자체가 Plate BestShot이라고 보고 보수적인 OCR 전처리만 적용한다.
PlatePreprocessResult preprocessPlateImage(const std::string& original_path,
                                           bool detect_candidate);

// 기존 진단 도구 호환용. Plate BestShot 모드의 전처리본 경로만 반환한다.
std::string enhancePlateImage(const std::string& original_path);

// IVA ROI 전체에 후보 검출/OCR/이진화 없이 화질 개선만 적용한다.
// scene의 형제 디렉터리인 enhanced에 PNG로 저장한다.
std::string enhanceIvaSceneImage(const std::string& original_path);

}

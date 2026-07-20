#pragma once

#include <string>

namespace ocr {

struct OcrResult {
    bool success{false};
    bool readable{false};
    std::string plate_number;
    double confidence{0.0};
    std::string error;
};

// Plate BestShot JPEG를 Gemini generateContent REST API로 보내 번호판을 읽는다.
class GeminiOcrClient {
public:
    GeminiOcrClient(std::string api_key, std::string model,
                    long connect_timeout_sec, long request_timeout_sec,
                    std::string fallback_model = "");
    OcrResult recognizePlate(const std::string& image_path,
                             const std::string& enhanced_image_path = "") const;
    bool configured() const;

private:
    OcrResult recognizePlateWithModel(const std::string& model,
                             const std::string& image_path,
                             const std::string& enhanced_image_path) const;
    std::string api_key_;
    std::string model_;
    std::string fallback_model_;
    long connect_timeout_sec_;
    long request_timeout_sec_;
};

}

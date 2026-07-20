#include "ocr/GeminiOcrClient.hpp"
#include "ocr/PlateImageEnhancer.hpp"
#include "ocr/PlateNormalizer.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::string envOrDefault(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

}

int main(int argc, char** argv) {
    const bool scene_mode = argc == 3 && std::string(argv[1]) == "--scene";
    if ((!scene_mode && argc != 2) || (scene_mode && argc != 3)) {
        std::cerr << "usage: ./gemini-ocr-test [--scene] <image>\n";
        return 2;
    }
    const char* image_path = argv[scene_mode ? 2 : 1];

    const char* api_key = std::getenv("GEMINI_API_KEY");
    if (api_key == nullptr || *api_key == '\0') {
        std::cerr << "GEMINI_API_KEY is not configured\n";
        return 2;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::cerr << "curl initialization failed\n";
        return 1;
    }

    ocr::GeminiOcrClient client(
        api_key, envOrDefault("GEMINI_MODEL", "gemini-3-flash-preview"), 5, 30);
    const ocr::PlatePreprocessResult processed =
        ocr::preprocessPlateImage(image_path, scene_mode);
    if (scene_mode && !processed.candidate_detected) {
        std::cout << "candidate=<not-found>\n";
        curl_global_cleanup();
        return 3;
    }
    ocr::OcrResult result = client.recognizePlate(
        image_path, processed.enhanced_path);
    curl_global_cleanup();

    if (!result.success) {
        std::cerr << "OCR failed: " << result.error << '\n';
        return 1;
    }
    const std::string plate = ocr::normalizePlateNumber(result.plate_number);
    std::cout << "enhanced="
              << (processed.enhanced_path.empty() ? "<failed>" : processed.enhanced_path) << '\n'
              << "readable=" << (result.readable ? "true" : "false")
              << " plate=" << (plate.empty() ? "<empty>" : plate)
              << " confidence=" << result.confidence << '\n';
    return result.readable && ocr::isPlausibleKoreanPlate(plate) ? 0 : 3;
}

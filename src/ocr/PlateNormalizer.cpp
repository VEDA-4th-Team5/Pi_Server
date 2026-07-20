#include "ocr/PlateNormalizer.hpp"

#include <cctype>

namespace ocr {

std::string normalizePlateNumber(const std::string& value) {
    std::string output;
    for (unsigned char c : value) {
        if (c >= 0x80 || std::isalnum(c)) output.push_back(static_cast<char>(c));
    }
    return output;
}

bool isPlausibleKoreanPlate(const std::string& value) {
    int digits = 0;
    bool has_korean_bytes = false;
    for (unsigned char c : value) {
        if (std::isdigit(c)) ++digits;
        if (c >= 0x80) has_korean_bytes = true;
    }
    return digits >= 6 && has_korean_bytes && value.size() <= 20;
}

}

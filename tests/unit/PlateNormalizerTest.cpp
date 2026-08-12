#include "ocr/PlateNormalizer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testNormalizeStripsSeparators() {
    // "223 로 2825" 같은 OCR 원문에서 공백/하이픈만 제거하고 숫자·한글은
    // 그대로 남아야 한다. 한글 UTF-8 바이트(>=0x80)는 isalnum() 판정 밖이라
    // 별도로 통과시켜야 한다.
    require(ocr::normalizePlateNumber("223 로 2825") == "223로2825",
            "spaces between plate segments must be stripped");
    require(ocr::normalizePlateNumber("12가-3456") == "12가3456",
            "hyphen must be stripped while Korean characters survive");
    require(ocr::normalizePlateNumber("") == "",
            "empty input must normalize to empty output");
}

void testNormalizeDropsPunctuationOnly() {
    require(ocr::normalizePlateNumber("!!!---   ") == "",
            "pure punctuation/whitespace must normalize to empty string");
    require(ocr::normalizePlateNumber("223로2825!!") == "223로2825",
            "trailing punctuation must be dropped");
}

void testPlausibleKoreanPlateAcceptsRealShape() {
    require(ocr::isPlausibleKoreanPlate("223로2825"),
            "7-digit plate with Korean syllable must be plausible");
    require(ocr::isPlausibleKoreanPlate("12가3456"),
            "6-digit plate with Korean syllable must be plausible");
}

void testPlausibleKoreanPlateRejectsTooFewDigits() {
    require(!ocr::isPlausibleKoreanPlate("1가2"),
            "fewer than 6 digits must not be plausible");
    require(!ocr::isPlausibleKoreanPlate("가나다"),
            "text with zero digits must not be plausible");
}

void testPlausibleKoreanPlateRejectsNoKoreanBytes() {
    require(!ocr::isPlausibleKoreanPlate("1234567"),
            "pure ASCII digits without Korean bytes must not be plausible "
            "(guards against misreading a non-plate number string as one)");
}

void testPlausibleKoreanPlateRejectsOverlength() {
    // 6자리 이상 숫자 + 한글 조건은 만족하지만 21바이트를 넘는 경우.
    const std::string overlong = "123456789012345가나다라마";
    require(!ocr::isPlausibleKoreanPlate(overlong),
            "input longer than 20 bytes must be rejected even if otherwise "
            "shaped like a plate");
}

}  // namespace

int main() {
    try {
        testNormalizeStripsSeparators();
        testNormalizeDropsPunctuationOnly();
        testPlausibleKoreanPlateAcceptsRealShape();
        testPlausibleKoreanPlateRejectsTooFewDigits();
        testPlausibleKoreanPlateRejectsNoKoreanBytes();
        testPlausibleKoreanPlateRejectsOverlength();
    } catch (const std::exception& error) {
        std::cerr << "PlateNormalizerTest failed: " << error.what()
                  << std::endl;
        return 1;
    }

    std::cout << "PlateNormalizerTest passed" << std::endl;
    return 0;
}

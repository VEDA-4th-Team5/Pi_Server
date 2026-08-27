#pragma once

#include <cstddef>
#include <string_view>

namespace ocr {

struct PlateSimilarity {
    bool comparable{};
    bool exact{};
    std::size_t mismatches{};
    double score{};
};

/**
 * 정규화된 한국 번호판을 UTF-8 문자 단위로 비교한다. 근접 일치는 마지막
 * 네 자리 숫자가 같고 전체에서 한 글자만 다를 때만 허용한다.
 */
PlateSimilarity comparePlateNumbers(std::string_view observed,
                                    std::string_view candidate);

/**
 * 두 정상 형식 번호판에서 ASCII 숫자 전체가 같은지 확인한다. 한글 OCR
 * 오인식을 등록 번호판으로 보정할 때 사용하며 숫자 하나라도 다르면 false다.
 */
bool samePlateDigits(std::string_view observed,
                     std::string_view candidate);

}  // namespace ocr

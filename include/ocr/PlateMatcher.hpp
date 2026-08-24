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

}  // namespace ocr

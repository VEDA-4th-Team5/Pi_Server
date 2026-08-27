#include "ocr/PlateMatcher.hpp"

#include <cctype>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

std::optional<std::vector<std::uint32_t>> decodeUtf8(
    const std::string_view value) {
    std::vector<std::uint32_t> output;
    for (std::size_t index = 0; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::uint32_t codepoint{};
        std::size_t length{};
        if (lead < 0x80U) {
            codepoint = lead;
            length = 1;
        } else if ((lead & 0xE0U) == 0xC0U) {
            codepoint = lead & 0x1FU;
            length = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            codepoint = lead & 0x0FU;
            length = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            codepoint = lead & 0x07U;
            length = 4;
        } else {
            return std::nullopt;
        }
        if (index + length > value.size()) return std::nullopt;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xC0U) != 0x80U) return std::nullopt;
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        output.push_back(codepoint);
        index += length;
    }
    return output;
}

bool asciiDigit(const std::uint32_t value) {
    return value >= static_cast<std::uint32_t>('0') &&
           value <= static_cast<std::uint32_t>('9');
}

bool plausiblePlateShape(const std::vector<std::uint32_t>& value) {
    if (value.size() < 7 || value.size() > 10) return false;
    std::size_t digits{};
    bool hasNonAscii{};
    for (const auto character : value) {
        if (asciiDigit(character)) ++digits;
        if (character > 0x7FU) hasNonAscii = true;
    }
    return digits >= 6 && hasNonAscii;
}

}  // namespace

namespace ocr {

PlateSimilarity comparePlateNumbers(const std::string_view observed,
                                    const std::string_view candidate) {
    PlateSimilarity result;
    const auto left = decodeUtf8(observed);
    const auto right = decodeUtf8(candidate);
    if (!left.has_value() || !right.has_value() ||
        left->size() != right->size() || !plausiblePlateShape(*left) ||
        !plausiblePlateShape(*right)) {
        return result;
    }

    result.comparable = true;
    for (std::size_t index = 0; index < left->size(); ++index) {
        if ((*left)[index] != (*right)[index]) ++result.mismatches;
    }
    result.exact = result.mismatches == 0;
    result.score = 1.0 - static_cast<double>(result.mismatches) /
                             static_cast<double>(left->size());
    if (result.exact) return result;

    if (result.mismatches != 1 || left->size() < 4) {
        result.comparable = false;
        result.score = 0.0;
        return result;
    }
    const std::size_t suffixStart = left->size() - 4;
    for (std::size_t index = suffixStart; index < left->size(); ++index) {
        if (!asciiDigit((*left)[index]) || !asciiDigit((*right)[index]) ||
            (*left)[index] != (*right)[index]) {
            result.comparable = false;
            result.score = 0.0;
            return result;
        }
    }
    return result;
}

bool samePlateDigits(const std::string_view observed,
                     const std::string_view candidate) {
    const auto left = decodeUtf8(observed);
    const auto right = decodeUtf8(candidate);
    if (!left.has_value() || !right.has_value() ||
        !plausiblePlateShape(*left) || !plausiblePlateShape(*right)) {
        return false;
    }

    std::vector<std::uint32_t> leftDigits;
    std::vector<std::uint32_t> rightDigits;
    for (const auto value : *left) {
        if (asciiDigit(value)) leftDigits.push_back(value);
    }
    for (const auto value : *right) {
        if (asciiDigit(value)) rightDigits.push_back(value);
    }
    return leftDigits == rightDigits;
}

}  // namespace ocr

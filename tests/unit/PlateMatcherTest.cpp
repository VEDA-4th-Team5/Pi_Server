#include "ocr/PlateMatcher.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void exactAndSingleCharacterMatches() {
    const auto exact = ocr::comparePlateNumbers("294마3087", "294마3087");
    require(exact.comparable && exact.exact && exact.mismatches == 0 &&
                std::abs(exact.score - 1.0) < 0.000001,
            "exact Korean plate did not match");

    const auto hangul = ocr::comparePlateNumbers("294미3087", "294마3087");
    require(hangul.comparable && !hangul.exact && hangul.mismatches == 1 &&
                hangul.score > 0.87,
            "single Hangul OCR error was not accepted");
}

void unsafeMatchesAreRejected() {
    require(!ocr::comparePlateNumbers("294마3081", "294마3087").comparable,
            "last four digit mismatch was accepted");
    require(!ocr::comparePlateNumbers("295미3087", "294마3087").comparable,
            "two-character mismatch was accepted");
    require(!ocr::comparePlateNumbers("not-a-plate", "294마3087").comparable,
            "malformed plate was accepted");
}

void digitOnlyMatchingIsStrict() {
    require(ocr::samePlateDigits("294미3087", "294마3087"),
            "same digits with a Hangul OCR error did not match");
    require(ocr::samePlateDigits("315너8504", "315다8504"),
            "registered demo plate digits did not match");
    require(!ocr::samePlateDigits("294미3081", "294마3087"),
            "different suffix digits were accepted");
    require(!ocr::samePlateDigits("295미3087", "294마3087"),
            "different prefix digits were accepted");
    require(!ocr::samePlateDigits("not-a-plate", "294마3087"),
            "malformed plate passed digit matching");
}

}  // namespace

int main() {
    try {
        exactAndSingleCharacterMatches();
        unsafeMatchesAreRejected();
        digitOnlyMatchingIsStrict();
        std::cout << "PlateMatcherTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "PlateMatcherTest failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

#include "entrance/EntranceImageDeduplicator.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void writePlate(const std::filesystem::path& path, const std::string& text,
                const bool inverted = false) {
    cv::Mat image(140, 400, CV_8UC3,
                  inverted ? cv::Scalar(0, 0, 0)
                           : cv::Scalar(245, 245, 245));
    const cv::Scalar color = inverted ? cv::Scalar(255, 255, 255)
                                      : cv::Scalar(0, 0, 0);
    cv::rectangle(image, {8, 8, 384, 124}, color, 5);
    cv::putText(image, text, {28, 92}, cv::FONT_HERSHEY_SIMPLEX, 2.0,
                color, 5, cv::LINE_AA);
    require(cv::imwrite(path.string(), image), "test image write failed");
}

void differentObjectIdsShareOneFingerprint() {
    const auto root = std::filesystem::temp_directory_path() /
                      "entrance-image-dedup-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    const auto firstPath = root / "first.jpg";
    const auto samePath = root / "same.jpg";
    const auto otherPath = root / "other.jpg";
    writePlate(firstPath, "294M3087");
    writePlate(samePath, "294M3087");
    writePlate(otherPath, "999Z9999", true);

    entrance::EntranceImageDeduplicator deduplicator(20s, 10, 8);
    const entrance::ObjectKey first{"cam01", "ch02", "100"};
    const entrance::ObjectKey repeated{"cam01", "ch02", "101"};
    const entrance::ObjectKey other{"cam01", "ch02", "102"};
    const entrance::ObjectKey otherChannel{"cam01", "ch03", "103"};

    const auto accepted = deduplicator.observe(first, firstPath.string(), 1000);
    require(accepted.fingerprinted && !accepted.duplicate,
            "first image was not accepted");
    require(deduplicator.bindEventId(first, 77),
            "canonical event ID was not bound");

    const auto duplicate =
        deduplicator.observe(repeated, samePath.string(), 5000);
    require(duplicate.duplicate && duplicate.hammingDistance == 0 &&
                duplicate.canonicalEventId == 77,
            "same image with a new ObjectId was not suppressed");

    const auto different = deduplicator.observe(other, otherPath.string(), 6000);
    require(!different.duplicate,
            "visually different entrance image was suppressed");

    const auto isolated =
        deduplicator.observe(otherChannel, samePath.string(), 7000);
    require(!isolated.duplicate,
            "same image from a different channel was incorrectly suppressed");

    const auto expired =
        deduplicator.observe(repeated, samePath.string(), 26001);
    require(!expired.duplicate,
            "expired fingerprint prevented ObjectId reuse");
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    try {
        differentObjectIdsShareOneFingerprint();
        std::cout << "EntranceImageDeduplicatorTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "EntranceImageDeduplicatorTest failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}

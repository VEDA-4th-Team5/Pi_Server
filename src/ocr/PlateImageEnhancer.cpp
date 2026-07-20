#include "ocr/PlateImageEnhancer.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::array<cv::Point2f, 4> orderCorners(const cv::Point2f points[4]) {
    std::array<cv::Point2f, 4> ordered{};  // TL, TR, BR, BL
    float min_sum = std::numeric_limits<float>::max();
    float max_sum = std::numeric_limits<float>::lowest();
    float min_diff = std::numeric_limits<float>::max();
    float max_diff = std::numeric_limits<float>::lowest();
    for (int i = 0; i < 4; ++i) {
        const float sum = points[i].x + points[i].y;
        const float diff = points[i].y - points[i].x;
        if (sum < min_sum) { min_sum = sum; ordered[0] = points[i]; }
        if (sum > max_sum) { max_sum = sum; ordered[2] = points[i]; }
        if (diff < min_diff) { min_diff = diff; ordered[1] = points[i]; }
        if (diff > max_diff) { max_diff = diff; ordered[3] = points[i]; }
    }
    return ordered;
}

cv::Mat rectifyCandidate(const cv::Mat& image, const cv::RotatedRect& box) {
    cv::Point2f points[4];
    box.points(points);
    const auto source = orderCorners(points);
    const float top = cv::norm(source[1] - source[0]);
    const float bottom = cv::norm(source[2] - source[3]);
    const float left = cv::norm(source[3] - source[0]);
    const float right = cv::norm(source[2] - source[1]);
    int width = std::max(1, static_cast<int>(std::round(std::max(top, bottom))));
    int height = std::max(1, static_cast<int>(std::round(std::max(left, right))));
    if (height > width) std::swap(width, height);
    if (width < 60 || height < 18) return {};

    std::array<cv::Point2f, 4> destination{
        cv::Point2f(0, 0), cv::Point2f(width - 1.0F, 0),
        cv::Point2f(width - 1.0F, height - 1.0F),
        cv::Point2f(0, height - 1.0F)};
    cv::Mat transform = cv::getPerspectiveTransform(source.data(), destination.data());
    cv::Mat result;
    cv::warpPerspective(image, result, transform, cv::Size(width, height),
                        cv::INTER_CUBIC, cv::BORDER_REPLICATE);
    return result;
}

cv::Mat findPlateCandidate(const cv::Mat& image) {
    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0.0);

    // 밝은 번호판 안의 어두운 문자와 수평 배열을 강조한다.
    cv::Mat blackhat;
    cv::morphologyEx(blurred, blackhat, cv::MORPH_BLACKHAT,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(17, 5)));
    cv::Mat gradient;
    cv::Sobel(blackhat, gradient, CV_32F, 1, 0, 3);
    cv::convertScaleAbs(gradient, gradient);
    cv::normalize(gradient, gradient, 0, 255, cv::NORM_MINMAX);
    cv::morphologyEx(gradient, gradient, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(17, 3)));
    cv::Mat mask;
    cv::threshold(gradient, mask, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(9, 3)));

    // 밝은 번호판 몸체 후보도 별도로 만들고 문자 Edge 후보와 함께 평가한다.
    cv::Mat bright_mask;
    cv::threshold(blurred, bright_mask, 0, 255,
                  cv::THRESH_BINARY | cv::THRESH_OTSU);
    cv::morphologyEx(bright_mask, bright_mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(9, 3)));

    cv::Mat fixed_bright;
    cv::threshold(blurred, fixed_bright, 115, 255, cv::THRESH_BINARY);
    cv::morphologyEx(fixed_bright, fixed_bright, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(13, 5)));

    cv::Mat hsv;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
    cv::Mat neutral_bright;
    cv::inRange(hsv, cv::Scalar(0, 0, 105), cv::Scalar(180, 135, 255),
                neutral_bright);
    cv::morphologyEx(neutral_bright, neutral_bright, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(13, 5)));

    cv::Mat canny_mask;
    cv::Canny(blurred, canny_mask, 45, 135);
    cv::morphologyEx(canny_mask, canny_mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 3)));

    std::vector<cv::Mat> masks{mask, bright_mask, fixed_bright,
                               neutral_bright, canny_mask};
    const double image_area = static_cast<double>(image.cols) * image.rows;
    double best_score = -1.0;
    cv::RotatedRect best;
    for (const cv::Mat& candidate_mask : masks) {
        std::vector<std::vector<cv::Point>> contours;
        // 번호판이 밝은 차체/배경과 닿아도 내부 사각 윤곽을 잃지 않도록 LIST를 사용한다.
        cv::findContours(candidate_mask.clone(), contours, cv::RETR_LIST,
                         cv::CHAIN_APPROX_SIMPLE);
        for (const auto& contour : contours) {
            cv::RotatedRect box = cv::minAreaRect(contour);
            double width = box.size.width;
            double height = box.size.height;
            if (height > width) std::swap(width, height);
            if (height < 18.0 || width < 60.0) continue;
            const double ratio = width / height;
            const double area = width * height;
            const double area_fraction = area / image_area;
            if (ratio < 2.0 || ratio > 5.8 || area_fraction < 0.0007 ||
                area_fraction > 0.20)
                continue;
            const double fill = std::abs(cv::contourArea(contour)) / area;
            if (fill < 0.12) continue;

            cv::Mat crop = rectifyCandidate(image, box);
            if (crop.empty()) continue;
            cv::Mat crop_gray;
            cv::cvtColor(crop, crop_gray, cv::COLOR_BGR2GRAY);
            const double brightness = cv::mean(crop_gray)[0] / 255.0;
            cv::Mat crop_edges;
            cv::Canny(crop_gray, crop_edges, 60, 160);
            const double edge_density = static_cast<double>(cv::countNonZero(crop_edges)) /
                                        crop_edges.total();
            const double edge_score = 1.0 -
                std::min(1.0, std::abs(edge_density - 0.14) / 0.14);
            const double ratio_score = 1.0 -
                std::min(1.0, std::abs(ratio - 4.0) / 2.0);
            const double vertical = box.center.y / std::max(1, image.rows);
            const double score = ratio_score * 2.4 + brightness * 1.8 +
                                 edge_score * 1.2 + fill * 0.5 +
                                 std::sqrt(area_fraction) * 2.0 + vertical * 0.35;
            if (score > best_score) { best_score = score; best = box; }
        }
    }
    return best_score < 0.0 ? cv::Mat{} : rectifyCandidate(image, best);
}

fs::path outputRoot(const fs::path& input, bool detect_candidate) {
    return detect_candidate ? input.parent_path().parent_path() : input.parent_path();
}

bool savePng(const fs::path& path, const cv::Mat& image) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    return !error && cv::imwrite(path.string(), image,
                                  {cv::IMWRITE_PNG_COMPRESSION, 2});
}

}

namespace ocr {

PlatePreprocessResult preprocessPlateImage(const std::string& original_path,
                                           bool detect_candidate) {
    PlatePreprocessResult result;
    cv::Mat original = cv::imread(original_path, cv::IMREAD_COLOR);
    if (original.empty()) return result;

    cv::Mat candidate = detect_candidate ? findPlateCandidate(original) : original.clone();
    if (candidate.empty()) return result;
    result.candidate_detected = detect_candidate;

    const fs::path input(original_path);
    const fs::path root = outputRoot(input, detect_candidate);
    const std::string stem = input.stem().string();
    // OCR에 충분한 폭까지만 확대한다. 이미 큰 이미지는 확대하지 않는다.
    const double scale = candidate.cols < 600
        ? std::min(2.0, 600.0 / std::max(1, candidate.cols)) : 1.0;
    cv::Mat enlarged;
    cv::resize(candidate, enlarged, cv::Size(), scale, scale,
               scale > 1.0 ? cv::INTER_LANCZOS4 : cv::INTER_AREA);

    // 노이즈를 먼저 약하게 줄인 뒤 밝기 채널만 보수적으로 보정한다.
    cv::Mat denoised;
    cv::bilateralFilter(enlarged, denoised, 3, 18.0, 18.0);
    cv::Mat lab;
    cv::cvtColor(denoised, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels;
    cv::split(lab, channels);
    cv::createCLAHE(1.35, cv::Size(8, 8))->apply(channels[0], channels[0]);
    cv::merge(channels, lab);
    cv::Mat contrast;
    cv::cvtColor(lab, contrast, cv::COLOR_Lab2BGR);
    cv::Mat blurred;
    cv::GaussianBlur(contrast, blurred, cv::Size(0, 0), 0.8);
    cv::Mat enhanced;
    cv::addWeighted(contrast, 1.18, blurred, -0.18, 0.0, enhanced);

    fs::path enhanced_path = root / "enhanced" / (stem + "_enhanced.png");
    if (!savePng(enhanced_path, enhanced)) return {};
    result.enhanced_path = enhanced_path.string();

    return result;
}

std::string enhancePlateImage(const std::string& original_path) {
    return preprocessPlateImage(original_path, false).enhanced_path;
}

std::string enhanceIvaSceneImage(const std::string& original_path) {
    const fs::path input(original_path);
    cv::Mat original = cv::imread(original_path, cv::IMREAD_COLOR);
    if (original.empty() || input.parent_path().filename() != "scene") return "";

    // ROI 전체의 세부 형태를 보존하도록 약한 노이즈 제거와 국부 대비 보정만 한다.
    cv::Mat denoised;
    cv::bilateralFilter(original, denoised, 3, 18.0, 18.0);
    cv::Mat lab;
    cv::cvtColor(denoised, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels;
    cv::split(lab, channels);
    cv::createCLAHE(1.25, cv::Size(8, 8))->apply(channels[0], channels[0]);
    cv::merge(channels, lab);
    cv::Mat contrast;
    cv::cvtColor(lab, contrast, cv::COLOR_Lab2BGR);
    cv::Mat blurred;
    cv::GaussianBlur(contrast, blurred, cv::Size(0, 0), 0.7);
    cv::Mat enhanced;
    cv::addWeighted(contrast, 1.12, blurred, -0.12, 0.0, enhanced);

    fs::path output = input.parent_path().parent_path() / "enhanced" /
        (input.stem().string() + "_enhanced.png");
    return savePng(output, enhanced) ? output.string() : "";
}

}

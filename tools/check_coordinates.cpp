#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string imagePath;
    std::string rtspEnvironment;
    std::string slotId{"EV01"};
    std::string outputPath;
    std::optional<cv::Rect> rectangle;
    int displayMaxWidth{1280};
    int warmupFrames{5};
    bool showHelp{false};
};

void printUsage(std::ostream& output) {
    output
        << "Usage:\n"
        << "  check_coordinates --image <jpeg> --slot EV01\n"
        << "  check_coordinates --rtsp-env CAMERA_RTSP --slot EV01\n"
        << "  check_coordinates --image <jpeg> --slot EV01 "
           "--rect x,y,width,height\n\n"
        << "Options:\n"
        << "  --image <path>            Load an existing reference image.\n"
        << "  --rtsp-env <name>         Read the RTSP URL from an environment "
           "variable.\n"
        << "  --slot <id>               Slot used in generated IVA_* settings "
           "(default: EV01).\n"
        << "  --rect <x,y,w,h>          Headless pixel ROI. Without this option, "
           "select with a mouse.\n"
        << "  --output <path>           Preview image path (default: "
           "data/roi_checks/<slot>_roi_preview.jpg).\n"
        << "  --display-max-width <px>  Maximum interactive image width "
           "(default: 1280).\n"
        << "  --warmup-frames <count>   RTSP frames read before selection "
           "(default: 5).\n"
        << "  --help                     Show this help.\n\n"
        << "If no source is supplied, CAMERA_RTSP_CH1 and then CAMERA_RTSP are "
           "checked.\n";
}

bool parseInteger(const std::string_view text, int& value) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

std::optional<cv::Rect> parseRectangle(const std::string& value) {
    std::vector<int> fields;
    std::istringstream input(value);
    std::string token;
    while (std::getline(input, token, ',')) {
        int field{};
        if (!parseInteger(token, field)) return std::nullopt;
        fields.push_back(field);
    }
    if (fields.size() != 4) return std::nullopt;
    return cv::Rect(fields[0], fields[1], fields[2], fields[3]);
}

bool readValue(int& index, const int argc, char** argv,
               std::string& value, std::string& error) {
    if (index + 1 >= argc) {
        error = std::string(argv[index]) + " requires a value";
        return false;
    }
    value = argv[++index];
    return true;
}

std::optional<Options> parseOptions(const int argc, char** argv,
                                    std::string& error) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        std::string value;
        if (argument == "--help" || argument == "-h") {
            options.showHelp = true;
        } else if (argument == "--image") {
            if (!readValue(index, argc, argv, options.imagePath, error))
                return std::nullopt;
        } else if (argument == "--rtsp-env") {
            if (!readValue(index, argc, argv, options.rtspEnvironment,
                           error))
                return std::nullopt;
        } else if (argument == "--slot") {
            if (!readValue(index, argc, argv, options.slotId, error))
                return std::nullopt;
        } else if (argument == "--output") {
            if (!readValue(index, argc, argv, options.outputPath, error))
                return std::nullopt;
        } else if (argument == "--rect") {
            if (!readValue(index, argc, argv, value, error))
                return std::nullopt;
            options.rectangle = parseRectangle(value);
            if (!options.rectangle) {
                error = "--rect must be x,y,width,height integers";
                return std::nullopt;
            }
        } else if (argument == "--display-max-width") {
            if (!readValue(index, argc, argv, value, error) ||
                !parseInteger(value, options.displayMaxWidth) ||
                options.displayMaxWidth <= 0) {
                error = "--display-max-width must be a positive integer";
                return std::nullopt;
            }
        } else if (argument == "--warmup-frames") {
            if (!readValue(index, argc, argv, value, error) ||
                !parseInteger(value, options.warmupFrames) ||
                options.warmupFrames <= 0 || options.warmupFrames > 300) {
                error = "--warmup-frames must be between 1 and 300";
                return std::nullopt;
            }
        } else {
            error = "unknown option: " + argument;
            return std::nullopt;
        }
    }

    if (!options.imagePath.empty() && !options.rtspEnvironment.empty()) {
        error = "--image and --rtsp-env cannot be used together";
        return std::nullopt;
    }
    if (options.slotId.empty() ||
        !std::all_of(options.slotId.begin(), options.slotId.end(),
                     [](const unsigned char character) {
                         return std::isalnum(character) != 0 ||
                                character == '_' || character == '-';
                     })) {
        error = "--slot may contain only letters, digits, '_' and '-'";
        return std::nullopt;
    }
    std::transform(options.slotId.begin(), options.slotId.end(),
                   options.slotId.begin(), [](const unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    return options;
}

std::optional<std::string> resolveRtspUrl(const Options& options,
                                          std::string& sourceName) {
    std::vector<std::string> candidates;
    if (!options.rtspEnvironment.empty()) {
        candidates.push_back(options.rtspEnvironment);
    } else if (options.imagePath.empty()) {
        candidates = {"CAMERA_RTSP_CH1", "CAMERA_RTSP"};
    }
    for (const auto& candidate : candidates) {
        const char* value = std::getenv(candidate.c_str());
        if (value != nullptr && *value != '\0') {
            sourceName = candidate;
            return std::string(value);
        }
    }
    return std::nullopt;
}

cv::Mat loadRtspFrame(const std::string& url, const int warmupFrames) {
    if (std::getenv("OPENCV_FFMPEG_CAPTURE_OPTIONS") == nullptr) {
        setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS",
               "rtsp_transport;tcp|stimeout;5000000|max_delay;500000", 0);
    }
    cv::VideoCapture capture;
    if (!capture.open(url, cv::CAP_FFMPEG)) return {};
    capture.set(cv::CAP_PROP_BUFFERSIZE, 1);
    cv::Mat frame;
    for (int count = 0; count < warmupFrames; ++count) {
        cv::Mat next;
        if (!capture.read(next) || next.empty()) continue;
        frame = std::move(next);
    }
    capture.release();
    return frame;
}

bool rectangleIsValid(const cv::Rect& rectangle, const cv::Size& imageSize) {
    if (rectangle.x < 0 || rectangle.y < 0 || rectangle.width <= 0 ||
        rectangle.height <= 0) {
        return false;
    }
    return rectangle.x <= imageSize.width - rectangle.width &&
           rectangle.y <= imageSize.height - rectangle.height;
}

std::optional<cv::Rect> selectRectangle(const cv::Mat& image,
                                        const int displayMaxWidth) {
    const double scale = std::min(
        1.0, static_cast<double>(displayMaxWidth) / image.cols);
    cv::Mat display;
    if (scale < 1.0) {
        cv::resize(image, display, cv::Size(), scale, scale, cv::INTER_AREA);
    } else {
        display = image;
    }
    constexpr const char* windowName = "check_coordinates: drag ROI, ENTER";
    const cv::Rect selected = cv::selectROI(windowName, display, false, false);
    cv::destroyWindow(windowName);
    if (selected.width <= 0 || selected.height <= 0) return std::nullopt;
    cv::Rect original{
        static_cast<int>(std::lround(selected.x / scale)),
        static_cast<int>(std::lround(selected.y / scale)),
        static_cast<int>(std::lround(selected.width / scale)),
        static_cast<int>(std::lround(selected.height / scale))};
    original &= cv::Rect(0, 0, image.cols, image.rows);
    return original;
}

bool savePreview(const cv::Mat& image, const cv::Rect& rectangle,
                 const std::string& slotId, const std::string& outputPath) {
    const std::filesystem::path path(outputPath);
    if (path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            std::cerr << "cannot create preview directory: "
                      << error.message() << '\n';
            return false;
        }
    }
    cv::Mat preview = image.clone();
    const int thickness = std::max(2, image.cols / 640);
    cv::rectangle(preview, rectangle, cv::Scalar(0, 255, 0), thickness);
    const cv::Point labelPosition(rectangle.x, std::max(20, rectangle.y - 8));
    cv::putText(preview, slotId, labelPosition, cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar(0, 255, 0), thickness, cv::LINE_AA);
    return cv::imwrite(outputPath, preview);
}

void printCoordinates(const cv::Size& imageSize, const cv::Rect& rectangle,
                      const std::string& slotId,
                      const std::string& outputPath) {
    const double x = static_cast<double>(rectangle.x) / imageSize.width;
    const double y = static_cast<double>(rectangle.y) / imageSize.height;
    const double width = static_cast<double>(rectangle.width) / imageSize.width;
    const double height =
        static_cast<double>(rectangle.height) / imageSize.height;
    std::cout << "image_size=" << imageSize.width << 'x' << imageSize.height
              << '\n'
              << "slot_id=" << slotId << '\n'
              << "pixel_roi=" << rectangle.x << ',' << rectangle.y << ','
              << rectangle.width << ',' << rectangle.height << '\n'
              << std::fixed << std::setprecision(6)
              << "IVA_" << slotId << "_ROI_X=" << x << '\n'
              << "IVA_" << slotId << "_ROI_Y=" << y << '\n'
              << "IVA_" << slotId << "_ROI_WIDTH=" << width << '\n'
              << "IVA_" << slotId << "_ROI_HEIGHT=" << height << '\n'
              << "preview_path=" << outputPath << '\n';
}

}  // namespace

int main(const int argc, char** argv) {
    std::string error;
    const auto parsed = parseOptions(argc, argv, error);
    if (!parsed) {
        std::cerr << "error: " << error << "\n\n";
        printUsage(std::cerr);
        return EXIT_FAILURE;
    }
    Options options = *parsed;
    if (options.showHelp) {
        printUsage(std::cout);
        return EXIT_SUCCESS;
    }

    cv::Mat image;
    if (!options.imagePath.empty()) {
        image = cv::imread(options.imagePath, cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cerr << "error: cannot read image: " << options.imagePath
                      << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::string sourceName;
        const auto url = resolveRtspUrl(options, sourceName);
        if (!url) {
            std::cerr << "error: no image and no RTSP environment variable "
                         "was provided\n";
            return EXIT_FAILURE;
        }
        std::cout << "capturing RTSP reference frame from environment="
                  << sourceName << '\n';
        image = loadRtspFrame(*url, options.warmupFrames);
        if (image.empty()) {
            std::cerr << "error: RTSP frame capture failed\n";
            return EXIT_FAILURE;
        }
    }

    if (options.outputPath.empty()) {
        options.outputPath =
            "data/roi_checks/" + options.slotId + "_roi_preview.jpg";
    }

    std::optional<cv::Rect> rectangle = options.rectangle;
    if (!rectangle) {
        if (std::getenv("DISPLAY") == nullptr &&
            std::getenv("WAYLAND_DISPLAY") == nullptr) {
            std::cerr << "error: no graphical display is available; use "
                         "--rect x,y,width,height or run with X forwarding\n";
            return EXIT_FAILURE;
        }
        try {
            rectangle = selectRectangle(image, options.displayMaxWidth);
        } catch (const cv::Exception& exception) {
            std::cerr << "error: OpenCV ROI window failed: "
                      << exception.what() << '\n';
            return EXIT_FAILURE;
        }
        if (!rectangle) {
            std::cerr << "error: ROI selection was canceled\n";
            return EXIT_FAILURE;
        }
    }

    if (!rectangleIsValid(*rectangle, image.size())) {
        std::cerr << "error: ROI is outside image bounds " << image.cols
                  << 'x' << image.rows << '\n';
        return EXIT_FAILURE;
    }
    if (!savePreview(image, *rectangle, options.slotId, options.outputPath)) {
        std::cerr << "error: preview image could not be saved\n";
        return EXIT_FAILURE;
    }
    printCoordinates(image.size(), *rectangle, options.slotId,
                     options.outputPath);
    return EXIT_SUCCESS;
}

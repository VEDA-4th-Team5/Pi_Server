#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
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
    int webPort{8091};
    std::string bindAddress{"127.0.0.1"};
    std::string piApiBase{"http://127.0.0.1:8080"};
    bool webMode{false};
    bool showHelp{false};
};

void printUsage(std::ostream& output) {
    output
        << "Usage:\n"
        << "  check_coordinates --image <jpeg> --slot EV01\n"
        << "  check_coordinates --rtsp-env CAMERA_RTSP --slot EV01\n"
        << "  check_coordinates --image <jpeg> --slot EV01 "
           "--rect x,y,width,height\n\n"
        << "  check_coordinates --image <jpeg> --web --port 8091\n\n"
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
        << "  --web                      Start the browser ROI selector.\n"
        << "  --bind <address>           Web listen address (default: "
           "127.0.0.1).\n"
        << "  --port <number>            Web listen port (default: 8091).\n"
        << "  --pi-api-base <url>        Running Pi server API base (default: "
           "http://127.0.0.1:8080).\n"
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
        } else if (argument == "--web") {
            options.webMode = true;
        } else if (argument == "--bind") {
            if (!readValue(index, argc, argv, options.bindAddress, error))
                return std::nullopt;
        } else if (argument == "--port") {
            if (!readValue(index, argc, argv, value, error) ||
                !parseInteger(value, options.webPort) ||
                options.webPort < 1024 || options.webPort > 65535) {
                error = "--port must be between 1024 and 65535";
                return std::nullopt;
            }
        } else if (argument == "--pi-api-base") {
            if (!readValue(index, argc, argv, options.piApiBase, error))
                return std::nullopt;
        } else {
            error = "unknown option: " + argument;
            return std::nullopt;
        }
    }

    if (!options.imagePath.empty() && !options.rtspEnvironment.empty()) {
        error = "--image and --rtsp-env cannot be used together";
        return std::nullopt;
    }
    if (options.bindAddress.empty()) {
        error = "--bind must not be empty";
        return std::nullopt;
    }
    if (options.webMode && options.rectangle) {
        error = "--web and --rect cannot be used together";
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

std::string coordinateReport(const cv::Size& imageSize,
                             const cv::Rect& rectangle,
                             const std::string& slotId,
                             const std::string& outputPath) {
    const double x = static_cast<double>(rectangle.x) / imageSize.width;
    const double y = static_cast<double>(rectangle.y) / imageSize.height;
    const double width = static_cast<double>(rectangle.width) / imageSize.width;
    const double height =
        static_cast<double>(rectangle.height) / imageSize.height;
    std::ostringstream output;
    output << "image_size=" << imageSize.width << 'x' << imageSize.height
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
    return output.str();
}

bool validSlotId(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(),
                       [](const unsigned char character) {
                           return std::isalnum(character) != 0 ||
                                  character == '_' || character == '-';
                       });
}

std::string normalizedSlotId(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    return value;
}

std::string buildWebPage(const std::string& initialSlot) {
    std::ostringstream html;
    html << R"HTML(<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IVA ROI Coordinate Checker</title>
<style>
body{margin:0;background:#111827;color:#f3f4f6;font-family:system-ui,sans-serif}
main{max-width:1400px;margin:auto;padding:20px}h1{font-size:22px;margin:0 0 12px}
.toolbar{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:12px}
select,button{font:inherit;padding:8px 12px;border-radius:6px;border:1px solid #4b5563}
button{background:#2563eb;color:white;cursor:pointer}button:disabled{opacity:.45}
.canvas-wrap{background:#000;border:1px solid #374151;overflow:auto;text-align:center}
.stage{position:relative;display:inline-block;max-width:100%;line-height:0}
#frame{display:block;max-width:100%;height:auto}
#overlay{position:absolute;inset:0;width:100%;height:100%;cursor:crosshair;touch-action:none}
pre{white-space:pre-wrap;background:#030712;padding:14px;border-radius:6px;min-height:120px}
.hint{color:#9ca3af}.ok{color:#86efac}.error{color:#fca5a5}
</style>
</head>
<body><main>
<h1>IVA ROI Coordinate Checker</h1>
<div class="toolbar"><label>주차면 <select id="slot">)HTML";
    for (const std::string slot : {"EV01", "EV02", "EV03", "EV04"}) {
        html << "<option value=\"" << slot << "\""
             << (slot == initialSlot ? " selected" : "") << '>' << slot
             << "</option>";
    }
    html << R"HTML(</select></label>
<button id="save" disabled>좌표 저장 및 즉시 적용</button>
<button id="reset">다시 선택</button>
<button id="refresh">새 프레임 가져오기</button>
<span class="hint">주차면 왼쪽 위에서 오른쪽 아래로 드래그하세요.</span></div>
<div class="canvas-wrap"><div class="stage">
<img id="frame" src="/frame.jpg" alt="카메라 기준 프레임">
<canvas id="overlay"></canvas>
</div></div>
<pre id="result">프레임을 불러오는 중입니다...</pre>
</main>
<script>
const frame=document.getElementById('frame'),overlay=document.getElementById('overlay');
const ctx=overlay.getContext('2d'),result=document.getElementById('result');
const save=document.getElementById('save'),slot=document.getElementById('slot'),refresh=document.getElementById('refresh');let start=null,current=null,selection=null,refreshMessage='';
function point(event){const r=overlay.getBoundingClientRect();return{
x:Math.max(0,Math.min(overlay.width,Math.round((event.clientX-r.left)*overlay.width/r.width))),
y:Math.max(0,Math.min(overlay.height,Math.round((event.clientY-r.top)*overlay.height/r.height)))};}
function rectangle(a,b){return{x:Math.min(a.x,b.x),y:Math.min(a.y,b.y),width:Math.abs(b.x-a.x),height:Math.abs(b.y-a.y)};}
function draw(){ctx.clearRect(0,0,overlay.width,overlay.height);const area=start&&current?rectangle(start,current):selection;if(!area)return;
ctx.fillStyle='rgba(0,255,102,.14)';ctx.fillRect(area.x,area.y,area.width,area.height);ctx.strokeStyle='#00ff66';
ctx.lineWidth=Math.max(3,overlay.width/650);ctx.strokeRect(area.x,area.y,area.width,area.height);}
frame.onload=()=>{overlay.width=frame.naturalWidth;overlay.height=frame.naturalHeight;draw();result.className=refreshMessage?'ok':'';result.textContent=refreshMessage||`image_size=${overlay.width}x${overlay.height}\n1. 슬롯 선택 → 2. 주차 영역 드래그 → 3. 저장 버튼 클릭`;refreshMessage='';};
frame.onerror=()=>{result.className='error';result.textContent='카메라 프레임을 불러오지 못했습니다. /frame.jpg 연결을 확인하세요.'};
overlay.addEventListener('pointerdown',e=>{e.preventDefault();start=point(e);current=start;selection=null;save.disabled=true;overlay.setPointerCapture(e.pointerId);draw();});
overlay.addEventListener('pointermove',e=>{if(!start)return;current=point(e);draw();const area=rectangle(start,current);result.textContent=`선택 중: ${area.x},${area.y},${area.width},${area.height}`;});
overlay.addEventListener('pointerup',e=>{if(!start)return;current=point(e);const area=rectangle(start,current);start=null;current=null;
if(area.width<2||area.height<2){selection=null;draw();result.textContent='영역이 너무 작습니다. 다시 드래그하세요.';return;}
selection=area;draw();save.disabled=false;result.className='';result.textContent=`pixel_roi=${area.x},${area.y},${area.width},${area.height}\n저장 버튼을 누르면 정규화 좌표가 출력됩니다.`;});
overlay.addEventListener('pointercancel',()=>{start=null;current=null;draw();});
document.getElementById('reset').onclick=()=>{start=null;current=null;selection=null;save.disabled=true;draw();result.className='';result.textContent='영역을 다시 드래그하세요.'};
slot.onchange=()=>document.getElementById('reset').click();
refresh.onclick=async()=>{refresh.disabled=true;save.disabled=true;start=null;current=null;selection=null;draw();result.className='';result.textContent='카메라에서 새 프레임을 가져오는 중입니다...';
try{const response=await fetch('/refresh',{method:'POST'});const text=await response.text();if(!response.ok){result.className='error';result.textContent=text;return;}refreshMessage=text;frame.src=`/frame.jpg?v=${Date.now()}`;}catch(error){result.className='error';result.textContent=String(error);}finally{refresh.disabled=false;}};
save.onclick=async()=>{if(!selection)return;const body=new URLSearchParams({...selection,slot:slot.value});
try{const response=await fetch('/selection',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
const text=await response.text();result.className=response.ok?'ok':'error';result.textContent=text;}catch(error){result.className='error';result.textContent=String(error);}};
</script></body></html>)HTML";
    return html.str();
}

int runWebServer(cv::Mat image, const Options& options,
                 const std::function<cv::Mat()>& reloadFrame) {
    std::vector<unsigned char> jpeg;
    if (!cv::imencode(".jpg", image, jpeg,
                      {cv::IMWRITE_JPEG_QUALITY, 92})) {
        std::cerr << "error: web reference JPEG encoding failed\n";
        return EXIT_FAILURE;
    }
    std::mutex frameMutex;
    std::mutex refreshMutex;

    httplib::Server server;
    const std::string page = buildWebPage(options.slotId);
    server.Get("/", [&page](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(page, "text/html; charset=utf-8");
    });
    server.Get("/frame.jpg", [&jpeg, &frameMutex](const httplib::Request&,
                                                   httplib::Response& res) {
        std::lock_guard lock(frameMutex);
        res.set_header("Cache-Control", "no-store");
        res.set_content(reinterpret_cast<const char*>(jpeg.data()), jpeg.size(),
                        "image/jpeg");
    });
    server.Post("/refresh", [&image, &jpeg, &frameMutex, &refreshMutex,
                              &reloadFrame](const httplib::Request&,
                                            httplib::Response& res) {
        std::unique_lock refreshLock(refreshMutex, std::try_to_lock);
        if (!refreshLock.owns_lock()) {
            res.status = 409;
            res.set_content("another frame refresh is already running\n",
                            "text/plain; charset=utf-8");
            return;
        }
        cv::Mat refreshed = reloadFrame();
        if (refreshed.empty()) {
            res.status = 502;
            res.set_content("camera frame refresh failed; previous frame kept\n",
                            "text/plain; charset=utf-8");
            return;
        }
        std::vector<unsigned char> refreshedJpeg;
        if (!cv::imencode(".jpg", refreshed, refreshedJpeg,
                          {cv::IMWRITE_JPEG_QUALITY, 92})) {
            res.status = 500;
            res.set_content("refreshed frame JPEG encoding failed\n",
                            "text/plain; charset=utf-8");
            return;
        }
        {
            std::lock_guard frameLock(frameMutex);
            image = std::move(refreshed);
            jpeg = std::move(refreshedJpeg);
        }
        res.set_content("new frame loaded: " + std::to_string(image.cols) +
                            "x" + std::to_string(image.rows) +
                            "\nselect the parking ROI again\n",
                        "text/plain; charset=utf-8");
    });
    server.Get("/health", [](const httplib::Request&,
                              httplib::Response& res) {
        res.set_content("ok\n", "text/plain; charset=utf-8");
    });
    server.Post("/selection", [&image, &options, &frameMutex](
                                   const httplib::Request& req,
                                   httplib::Response& res) {
        for (const char* field : {"slot", "x", "y", "width", "height"}) {
            if (!req.has_param(field)) {
                res.status = 400;
                res.set_content(std::string("missing field: ") + field + '\n',
                                "text/plain; charset=utf-8");
                return;
            }
        }

        std::string slot = normalizedSlotId(req.get_param_value("slot"));
        int x{}, y{}, width{}, height{};
        if (!validSlotId(slot) ||
            !parseInteger(req.get_param_value("x"), x) ||
            !parseInteger(req.get_param_value("y"), y) ||
            !parseInteger(req.get_param_value("width"), width) ||
            !parseInteger(req.get_param_value("height"), height)) {
            res.status = 400;
            res.set_content("invalid slot or rectangle fields\n",
                            "text/plain; charset=utf-8");
            return;
        }

        cv::Mat selectionImage;
        {
            std::lock_guard lock(frameMutex);
            selectionImage = image.clone();
        }
        const cv::Rect rectangle(x, y, width, height);
        if (!rectangleIsValid(rectangle, selectionImage.size())) {
            res.status = 400;
            res.set_content("ROI is outside image bounds\n",
                            "text/plain; charset=utf-8");
            return;
        }
        const std::string outputPath = options.outputPath.empty()
            ? "data/roi_checks/" + slot + "_roi_preview.jpg"
            : options.outputPath;
        if (!savePreview(selectionImage, rectangle, slot, outputPath)) {
            res.status = 500;
            res.set_content("preview image could not be saved\n",
                            "text/plain; charset=utf-8");
            return;
        }
        const std::string report =
            coordinateReport(selectionImage.size(), rectangle, slot, outputPath);
        const double normalized_x =
            static_cast<double>(rectangle.x) / selectionImage.cols;
        const double normalized_y =
            static_cast<double>(rectangle.y) / selectionImage.rows;
        const double normalized_width =
            static_cast<double>(rectangle.width) / selectionImage.cols;
        const double normalized_height =
            static_cast<double>(rectangle.height) / selectionImage.rows;
        std::ostringstream json;
        json << std::setprecision(17)
             << "{\"x\":" << normalized_x
             << ",\"y\":" << normalized_y
             << ",\"width\":" << normalized_width
             << ",\"height\":" << normalized_height << '}';
        httplib::Client piClient(options.piApiBase);
        piClient.set_connection_timeout(2, 0);
        piClient.set_read_timeout(3, 0);
        const auto applied = piClient.Put(
            "/api/v1/settings/parking-slots/" + slot + "/roi",
            json.str(), "application/json");
        if (!applied) {
            res.status = 502;
            res.set_content(report +
                "apply_error=Pi server API connection failed\n",
                "text/plain; charset=utf-8");
            return;
        }
        if (applied->status != 200) {
            res.status = 502;
            res.set_content(report + "apply_error=Pi server rejected ROI " +
                std::to_string(applied->status) + " " + applied->body + "\n",
                "text/plain; charset=utf-8");
            return;
        }
        std::cout << report << std::flush;
        res.set_content(report + "applied_immediately=true\n",
                        "text/plain; charset=utf-8");
    });

    std::cout << "check_coordinates web mode started\n"
              << "listen=" << options.bindAddress << ':' << options.webPort
              << '\n';
    if (options.bindAddress == "127.0.0.1" ||
        options.bindAddress == "localhost") {
        std::cout << "open=http://127.0.0.1:" << options.webPort
                  << " (use VS Code port forwarding for remote SSH)\n";
    } else {
        std::cout << "open=http://<PI_IP>:" << options.webPort << '\n'
                  << "warning=the calibration page has no authentication\n";
    }
    return server.listen(options.bindAddress, options.webPort)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
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
    std::function<cv::Mat()> reloadFrame;
    if (!options.imagePath.empty()) {
        const std::string imagePath = options.imagePath;
        reloadFrame = [imagePath] {
            return cv::imread(imagePath, cv::IMREAD_COLOR);
        };
        image = reloadFrame();
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
        const std::string rtspUrl = *url;
        const int warmupFrames = options.warmupFrames;
        reloadFrame = [rtspUrl, warmupFrames] {
            return loadRtspFrame(rtspUrl, warmupFrames);
        };
        image = reloadFrame();
        if (image.empty()) {
            std::cerr << "error: RTSP frame capture failed\n";
            return EXIT_FAILURE;
        }
    }

    if (options.webMode) {
        return runWebServer(std::move(image), options, reloadFrame);
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
    std::cout << coordinateReport(image.size(), *rectangle, options.slotId,
                                  options.outputPath);
    return EXIT_SUCCESS;
}

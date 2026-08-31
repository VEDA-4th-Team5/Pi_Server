#include "ocr/PlateImageEnhancer.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/resource.h>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

static double cpu_ms() {
    rusage r{}; getrusage(RUSAGE_SELF, &r);
    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000.0 +
           (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0;
}
static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size()/2];
}

// 현재 SnapshotStorage.cpp 의 cropJpeg() 본문 그대로
static std::vector<unsigned char> cropJpeg(const std::vector<unsigned char>& jpeg,
                                           double rx, double ry, double rw, double rh) {
    const cv::Mat image = cv::imdecode(jpeg, cv::IMREAD_COLOR);
    const int left  = std::clamp((int)std::lround(rx * image.cols), 0, image.cols - 1);
    const int top   = std::clamp((int)std::lround(ry * image.rows), 0, image.rows - 1);
    const int right = std::clamp((int)std::lround((rx + rw) * image.cols), left + 1, image.cols);
    const int bottom= std::clamp((int)std::lround((ry + rh) * image.rows), top + 1, image.rows);
    const cv::Mat cropped = image(cv::Rect(left, top, right-left, bottom-top)).clone();
    std::vector<unsigned char> out;
    cv::imencode(".jpg", cropped, out, {cv::IMWRITE_JPEG_QUALITY, 95});
    return out;
}

int main(int argc, char** argv) {
    const int N = argc > 1 ? std::atoi(argv[1]) : 15;
    const int threads = argc > 2 ? std::atoi(argv[2]) : -1;
    if (threads > 0) cv::setNumThreads(threads);
    printf("# OpenCV %s  setNumThreads=%d  getNumThreads=%d  반복=%d\n",
           CV_VERSION, threads, cv::getNumThreads(), N);

    const std::string scene = "scene/frame.jpg";
    std::vector<double> w_old, c_old, w_new, c_new;
    std::string produced;

    // --- 옛 방식: d41e2f09 의 enhanceIvaSceneImage() 원본 코드 직접 호출 ---
    for (int i = 0; i < N; ++i) {
        fs::remove_all("enhanced");
        auto t0 = clk::now(); double u0 = cpu_ms();
        produced = ocr::enhanceIvaSceneImage(scene);
        double u1 = cpu_ms(); auto t1 = clk::now();
        if (produced.empty()) { printf("FAIL: enhanceIvaSceneImage returned empty\n"); return 1; }
        w_old.push_back(std::chrono::duration<double,std::milli>(t1-t0).count());
        c_old.push_back(u1-u0);
    }
    const auto png_bytes = fs::file_size(produced);

    // --- 현재 방식: cropJpeg() ---
    std::vector<unsigned char> raw;
    { FILE* f=fopen(scene.c_str(),"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
      raw.resize(n); if(fread(raw.data(),1,n,f)!=(size_t)n){} fclose(f); }
    std::vector<unsigned char> cropped;
    for (int i = 0; i < N; ++i) {
        auto t0 = clk::now(); double u0 = cpu_ms();
        cropped = cropJpeg(raw, 0.30, 0.25, 0.32, 0.70);
        double u1 = cpu_ms(); auto t1 = clk::now();
        w_new.push_back(std::chrono::duration<double,std::milli>(t1-t0).count());
        c_new.push_back(u1-u0);
    }

    printf("\n[옛 방식] ocr::enhanceIvaSceneImage()  — d41e2f09 원본 코드\n");
    printf("  wall 중앙값 %8.1f ms   CPU(user+sys) 중앙값 %8.1f ms\n", median(w_old), median(c_old));
    printf("  출력 PNG %.1f KB  (실파일 811.9 KB)\n", png_bytes/1024.0);
    printf("\n[현재 방식] cropJpeg()  — SnapshotStorage.cpp\n");
    printf("  wall 중앙값 %8.1f ms   CPU(user+sys) 중앙값 %8.1f ms\n", median(w_new), median(c_new));
    printf("  출력 JPEG %.1f KB\n", cropped.size()/1024.0);
    printf("\n  wall 비율 %.1fx   CPU 비율 %.1fx\n",
           median(w_old)/median(w_new), median(c_old)/median(c_new));
    fs::remove_all("enhanced");
    return 0;
}

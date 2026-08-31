#include "ocr/PlateImageEnhancer.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <algorithm>
namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;
static double med(std::vector<double> v){ std::sort(v.begin(),v.end()); return v[v.size()/2]; }
#define T(acc, code) { auto _a=clk::now(); code; acc.push_back(std::chrono::duration<double,std::milli>(clk::now()-_a).count()); }

int main(int argc,char**argv){
    const int N = argc>1?std::atoi(argv[1]):15;
    if(argc>2) cv::setNumThreads(std::atoi(argv[2]));
    printf("# OpenCV %s  getNumThreads=%d  반복=%d\n", CV_VERSION, cv::getNumThreads(), N);
    std::vector<double> t_read,t_bil,t_clahe,t_unsharp,t_png,t_total,t_real;

    for(int i=0;i<N;++i){
        cv::Mat original, denoised, lab, contrast, blurred, enhanced;
        std::vector<cv::Mat> ch;
        auto a=clk::now();
        T(t_read,  original = cv::imread("scene/frame.jpg", cv::IMREAD_COLOR));
        T(t_bil,   cv::bilateralFilter(original, denoised, 3, 18.0, 18.0));
        T(t_clahe, { cv::cvtColor(denoised, lab, cv::COLOR_BGR2Lab);
                     cv::split(lab, ch);
                     cv::createCLAHE(1.25, cv::Size(8,8))->apply(ch[0], ch[0]);
                     cv::merge(ch, lab);
                     cv::cvtColor(lab, contrast, cv::COLOR_Lab2BGR); });
        T(t_unsharp,{ cv::GaussianBlur(contrast, blurred, cv::Size(0,0), 0.7);
                      cv::addWeighted(contrast, 1.12, blurred, -0.12, 0.0, enhanced); });
        fs::create_directories("stg");
        T(t_png,   cv::imwrite("stg/o.png", enhanced, {cv::IMWRITE_PNG_COMPRESSION,2}));
        t_total.push_back(std::chrono::duration<double,std::milli>(clk::now()-a).count());
    }
    for(int i=0;i<N;++i){
        fs::remove_all("enhanced");
        auto a=clk::now(); ocr::enhanceIvaSceneImage("scene/frame.jpg");
        t_real.push_back(std::chrono::duration<double,std::milli>(clk::now()-a).count());
    }
    double s=med(t_read)+med(t_bil)+med(t_clahe)+med(t_unsharp)+med(t_png);
    printf("\n단계별 (중앙값 ms)                        비중\n");
    printf("  imread JPEG 1280x720        %7.1f   %4.1f%%\n", med(t_read),   med(t_read)/s*100);
    printf("  bilateralFilter(3,18,18)    %7.1f   %4.1f%%\n", med(t_bil),    med(t_bil)/s*100);
    printf("  Lab + CLAHE(1.25,8x8)       %7.1f   %4.1f%%\n", med(t_clahe),  med(t_clahe)/s*100);
    printf("  GaussianBlur + addWeighted  %7.1f   %4.1f%%\n", med(t_unsharp),med(t_unsharp)/s*100);
    printf("  imwrite PNG(COMPRESSION=2)  %7.1f   %4.1f%%\n", med(t_png),    med(t_png)/s*100);
    printf("  ── 단계 합                  %7.1f\n", s);
    printf("\n검증: 실제 enhanceIvaSceneImage() 총시간 %7.1f ms  (단계합 대비 오차 %+.1f%%)\n",
           med(t_real), (s-med(t_real))/med(t_real)*100);
    fs::remove_all("stg"); fs::remove_all("enhanced");
    return 0;
}

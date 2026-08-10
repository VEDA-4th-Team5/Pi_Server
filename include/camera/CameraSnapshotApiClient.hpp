#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace camera {

struct CameraSnapshotApiConfig {
    std::string openApiBase;
    std::string imageBase;
    std::string username;
    std::string password;
    int imageServerPort{8080};
    int connectTimeoutMs{3000};
    int requestTimeoutMs{30000};
    int jpegTimeoutMs{10000};
    int maxRetries{2};
    int retryDelayMs{250};
};

struct CameraGeneratedImages {
    std::string runId;
    int channel{-1};
    std::string detectedEnvironment;
    std::string autoFilter;
    std::vector<unsigned char> originalJpeg;
    std::vector<unsigned char> enhancedJpeg;
};

/**
 * @brief CV Snapshot CAP의 OpenAPI와 JPEG 서버를 호출하는 C++ 클라이언트다.
 *
 * 시작·채널 discovery와 이미지 생성은 OpenAPI base를 사용하고, 생성된 JPEG는
 * 응답의 image_path를 image base에 결합해 즉시 내려받는다. 최근 run 8개만
 * 유지되는 카메라 계약 때문에 generate() 안에서 두 파일 다운로드까지 끝낸다.
 */
class CameraSnapshotApiClient {
public:
    explicit CameraSnapshotApiClient(CameraSnapshotApiConfig config);

    [[nodiscard]] bool configured() const noexcept;
    /** @brief 이미지 서버 시작과 channels/filters discovery를 수행한다. */
    bool initialize();
    /** @brief 같은 카메라 프레임의 original/enhanced JPEG를 생성·다운로드한다. */
    bool generate(int channel, CameraGeneratedImages& images);

    [[nodiscard]] const std::string& lastError() const noexcept;
    [[nodiscard]] const std::vector<int>& channels() const noexcept;
    [[nodiscard]] const std::vector<std::string>& filters() const noexcept;

private:
    struct HttpResponse {
        long status{};
        std::string contentType;
        std::vector<unsigned char> body;
        std::string transportError;
    };

    bool initializeUnlocked();
    bool startImageServer();
    bool discoverChannels();
    bool discoverFilters();
    bool downloadJpeg(const std::string& path,
                      std::size_t expectedBytes,
                      std::vector<unsigned char>& output);
    HttpResponse request(const std::string& method,
                         const std::string& url,
                         const std::string& jsonBody,
                         int timeoutMs) const;
    void setError(std::string message);

    CameraSnapshotApiConfig config_;
    std::vector<int> channels_;
    std::vector<std::string> filters_;
    std::string lastError_;
    bool initialized_{};
    // 카메라 CAP는 한 번에 하나의 이미지 생성 요청만 처리하므로 증거와
    // 30/60초 촬영 요청을 직렬화한다.
    std::mutex requestMutex_;
};

}  // namespace camera

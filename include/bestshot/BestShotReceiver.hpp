#pragma once

#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "ocr/OcrWorker.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bestshot {

// Hanwha RTSP metadata track에서 차량/번호판 BestShot 참조를 수신하고,
// 실제 JPEG를 내려받아 주차 세션 및 이미지 로그와 연결한다.
class BestShotReceiver {
public:
    BestShotReceiver(std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
                     database::EventDatabase& database,
                     parking::ParkingTriggerCoordinator& trigger_coordinator,
                     ocr::OcrWorker& ocr_worker,
                     std::atomic<bool>& running,
                     std::string output_root = "data/bestshots");
    ~BestShotReceiver();

    void start();  // 설정된 RTSP 채널마다 metadata 수신 스레드를 시작한다.
    void stop();   // 실행 중인 수신 스레드가 모두 끝날 때까지 기다린다.

private:
    // 한 채널의 RTSP data stream을 읽어 완성된 MetadataStream XML을 추출한다.
    void receiveLoop(const std::shared_ptr<camera::CameraChannel>& channel);
    // XML 안의 차량/번호판 객체를 분류하고 이미지 및 DB 저장을 수행한다.
    void processMetadata(const std::string& channel_id, const std::string& xml,
                         const std::string& rtsp_url);
    // ImageRef를 카메라 HTTPS 주소로 변환해 Digest 인증으로 JPEG를 받는다.
    bool downloadImage(const std::string& rtsp_url, const std::string& image_ref,
                       const std::string& destination);
    static std::string slotIdForChannel(const std::string& channel_id);
    // 아래 세 객체는 main이 소유하며 BestShotReceiver의 수명 동안 유효해야 한다.
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels_;
    database::EventDatabase& database_;
    parking::ParkingTriggerCoordinator& trigger_coordinator_;
    ocr::OcrWorker& ocr_worker_;
    std::atomic<bool>& running_;

    std::string output_root_;
    std::vector<std::thread> workers_;

    // 여러 채널 worker가 중복 ImageRef와 활성 세션을 동시에 변경하지 못하게 보호한다.
    std::mutex state_mutex_;
    std::unordered_set<std::string> processed_refs_;
    struct ActiveSession {
        int session_id;
        std::string slot_id;
        std::chrono::steady_clock::time_point created_at;
    };
    // IVA와 연결된 차량 세션. 뒤따르는 번호판 이미지를 같은 주차면에 연결한다.
    std::unordered_map<std::string, ActiveSession> active_sessions_;
    struct PendingPlate {
        std::string image_path;
        std::string plate_text;
        std::chrono::steady_clock::time_point created_at;
    };
    // 일부 펌웨어/상황에서 Plate metadata가 Vehicle보다 먼저 도착하므로 잠시 보관한다.
    std::unordered_map<std::string, PendingPlate> pending_plates_;
};

}

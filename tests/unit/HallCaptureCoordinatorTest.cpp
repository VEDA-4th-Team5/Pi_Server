#include "parking/HallCaptureCoordinator.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

parking::ParkingTransitionResult transition(
    const parking::ParkingTransitionCode code, const std::int64_t sessionId,
    const std::string& slotId) {
    parking::ParkingTransitionResult result;
    result.code = code;
    result.sessionId = std::to_string(sessionId);
    result.slotId = slotId;
    return result;
}

struct Recorder {
    std::vector<std::int64_t> imageSessions;
    std::vector<std::int64_t> ocrSessions;
    std::vector<std::int64_t> timerSessions;
    std::vector<int> failureAttempts;

    parking::HallCapturePorts ports() {
        parking::HallCapturePorts ports;
        ports.writeImageLog = [this](const parking::CapturedImage& image) {
            imageSessions.push_back(image.sessionId);
            return parking::ImageStoreResult::Inserted;
        };
        ports.submitOcr = [this](const parking::CapturedImage& image) {
            ocrSessions.push_back(image.sessionId);
        };
        ports.writeOcrFailure =
            [this](std::int64_t, const std::string&, const int attempts) {
                failureAttempts.push_back(attempts);
            };
        ports.registerEvTimer =
            [this](const std::int64_t sessionId, const std::string&,
                   const std::string&) {
                timerSessions.push_back(sessionId);
            };
        return ports;
    }
};

parking::CapturedImage image(const std::int64_t sessionId,
                             const parking::CaptureStage stage,
                             const std::string& path) {
    return {sessionId, "EV01", stage, path, {}};
}

void sameSqliteIdFlowsThroughEverything() {
    Recorder recorder;
    parking::HallCaptureCoordinator coordinator(recorder.ports());
    coordinator.onTransition(transition(
        parking::ParkingTransitionCode::SessionStarted, 77, "EV01"));
    require(coordinator.onCaptureImage(image(
                77, parking::CaptureStage::First30s, "30.jpg")) ==
                parking::CaptureImageResult::Stored,
            "30s capture was not stored");
    coordinator.onOcrOutcome({77, parking::CaptureStage::First30s, true,
                              "52주3108", 0.95, "EV"});
    require(recorder.imageSessions == std::vector<std::int64_t>{77},
            "IMAGE_LOG did not reuse SQLite session_id");
    require(recorder.ocrSessions == std::vector<std::int64_t>{77},
            "OCR did not reuse SQLite session_id");
    require(recorder.timerSessions == std::vector<std::int64_t>{77},
            "timer did not reuse SQLite session_id");

    (void)coordinator.onCaptureImage(image(
        77, parking::CaptureStage::Second60s, "60.jpg"));
    require(recorder.ocrSessions.size() == 1,
            "60s OCR was not suppressed after 30s success");
    coordinator.onTransition(transition(
        parking::ParkingTransitionCode::SessionCompleted, 77, "EV01"));
    require(coordinator.trackedSessions() == 0,
            "completed session binding was retained");
}

void twoFailuresWriteUnknownOnce() {
    Recorder recorder;
    parking::HallCaptureCoordinator coordinator(recorder.ports());
    coordinator.onTransition(transition(
        parking::ParkingTransitionCode::SessionStarted, 88, "EV01"));
    (void)coordinator.onCaptureImage(image(
        88, parking::CaptureStage::First30s, "30.jpg"));
    coordinator.onOcrOutcome({88, parking::CaptureStage::First30s, false,
                              {}, 0.0, "OCR_FAILED"});
    (void)coordinator.onCaptureImage(image(
        88, parking::CaptureStage::Second60s, "60.jpg"));
    coordinator.onOcrOutcome({88, parking::CaptureStage::Second60s, false,
                              {}, 0.0, "OCR_FAILED"});
    require(recorder.failureAttempts == std::vector<int>{2},
            "two failures must record attempts=2 exactly once");
    require(recorder.timerSessions.empty(),
            "failed OCR must not start EV timer");
}

void inactiveAndDuplicateCapturesDoNotRunOcr() {
    Recorder recorder;
    auto ports = recorder.ports();
    ports.writeImageLog = [](const parking::CapturedImage&) {
        return parking::ImageStoreResult::Duplicate;
    };
    parking::HallCaptureCoordinator coordinator(std::move(ports));
    coordinator.onTransition(transition(
        parking::ParkingTransitionCode::SessionStarted, 99, "EV01"));
    require(coordinator.onCaptureImage(image(
                99, parking::CaptureStage::First30s, "duplicate.jpg")) ==
                parking::CaptureImageResult::Duplicate,
            "duplicate DB result was not propagated");
    require(recorder.ocrSessions.empty(), "duplicate image ran OCR");
    coordinator.onTransition(transition(
        parking::ParkingTransitionCode::SessionCompleted, 99, "EV01"));
    require(coordinator.onCaptureImage(image(
                99, parking::CaptureStage::Second60s, "late.jpg")) ==
                parking::CaptureImageResult::InactiveSession,
            "late capture was not rejected");
}

}  // namespace

int main() {
    try {
        sameSqliteIdFlowsThroughEverything();
        twoFailuresWriteUnknownOnce();
        inactiveAndDuplicateCapturesDoNotRunOcr();
        std::cout << "HallCaptureCoordinatorTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "HallCaptureCoordinatorTest failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}

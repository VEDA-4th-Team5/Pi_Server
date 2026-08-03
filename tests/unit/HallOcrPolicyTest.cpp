#include "parking/HallOcrPolicy.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr auto k30 = parking::CaptureStage::First30s;
constexpr auto k60 = parking::CaptureStage::Second60s;

void successSuppressesSecondOcr() {
    parking::HallOcrPolicy policy;
    policy.openSession("7");
    require(policy.onImageArrived("7", k30) == parking::ImageAction::RunOcr,
            "30s must run first OCR");
    const auto success = policy.onOcrResult("7", k30, true);
    require(success.resolved && !success.exhausted,
            "30s success must resolve");
    require(policy.onImageArrived("7", k60) ==
                parking::ImageAction::StoreEvidenceOnly,
            "60s must be evidence-only after success");
}

void twoFailuresBecomeUnknownCandidate() {
    parking::HallOcrPolicy policy;
    policy.openSession("8");
    (void)policy.onImageArrived("8", k30);
    auto fold = policy.onOcrResult("8", k30, false);
    require(fold.attempts == 1 && !fold.exhausted,
            "first failure must preserve retry");
    require(policy.onImageArrived("8", k60) == parking::ImageAction::RunOcr,
            "60s must run retry");
    fold = policy.onOcrResult("8", k60, false);
    require(fold.attempts == 2 && fold.exhausted,
            "two failed stages must exhaust budget");
    require(policy.state("8") == parking::OcrSessionState::Failed,
            "exhausted OCR must be Failed, never NON_EV");
}

void heldImageIsReleasedAfterFailure() {
    parking::HallOcrPolicy policy;
    policy.openSession("9");
    (void)policy.onImageArrived("9", k30);
    require(policy.onImageArrived("9", k60) ==
                parking::ImageAction::HoldForPendingOcr,
            "60s must wait for in-flight 30s OCR");
    const auto fold = policy.onOcrResult("9", k30, false);
    require(fold.releaseHeldImage && fold.attempts == 1,
            "failed 30s must release held 60s image");
}

void closeSettlesOnlyAnAttemptedSession() {
    parking::HallOcrPolicy attempted;
    attempted.openSession("10");
    (void)attempted.onImageArrived("10", k30);
    (void)attempted.onOcrResult("10", k30, false);
    const auto failed = attempted.closeSession("10");
    require(failed.settledAtClose && failed.attempts == 1,
            "unfinished attempted session must settle UNKNOWN");

    parking::HallOcrPolicy untouched;
    untouched.openSession("11");
    const auto pending = untouched.closeSession("11");
    require(!pending.settledAtClose && pending.attempts == 0,
            "capture failure without OCR must not claim an OCR attempt");
}

}  // namespace

int main() {
    try {
        successSuppressesSecondOcr();
        twoFailuresBecomeUnknownCandidate();
        heldImageIsReleasedAfterFailure();
        closeSettlesOnlyAnAttemptedSession();
        std::cout << "HallOcrPolicyTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "HallOcrPolicyTest failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

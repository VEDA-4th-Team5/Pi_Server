// Unit test for parking::HallOcrPolicy (EVDA-136).
//
// Drives the 30s-first / 60s-fallback OCR retry policy through its whole
// decision table. The policy is pure -- no clock, no I/O, no image paths -- so
// every case below is a plain sequence of calls with no sleeping and no fakes.
//
// The acceptance criteria under test:
//   * a successful 30s read suppresses the 60s OCR call entirely (quota),
//     while the 60s photo is still kept as evidence;
//   * a failed 30s read hands the second and last attempt to the 60s photo;
//   * two failures end as Failed, which callers persist as
//     ocr_status=FAILED / ev_status=UNKNOWN -- never NON_EV;
//   * a 60s photo that lands while the 30s OCR is still in flight is held,
//     then released or demoted depending on how the first attempt ends;
//   * duplicate, late and unknown-session deliveries never burn an attempt.

#include "parking/HallOcrPolicy.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

constexpr auto k30 = parking::CaptureStage::First30s;
constexpr auto k60 = parking::CaptureStage::Second60s;

// The happy path: the 30s photo reads, so the 60s photo must never cost a
// Gemini call. This is the quota guard in the acceptance criteria.
void thirtySecondSuccessSuppressesSixtySecondOcr() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV01-T0");

    require(policy.onImageArrived("EV01-T0", k30) == parking::ImageAction::RunOcr,
            "the 30s photo drives the first OCR");

    const auto fold = policy.onOcrResult("EV01-T0", k30, true);
    require(fold.tracked && fold.resolved, "a plausible plate resolves the session");
    require(!fold.exhausted, "a success never counts as an exhausted budget");
    require(policy.state("EV01-T0") == parking::OcrSessionState::Resolved,
            "session is Resolved after a successful read");

    require(policy.onImageArrived("EV01-T0", k60) ==
                parking::ImageAction::StoreEvidenceOnly,
            "the 60s photo is kept as evidence but never sent to Gemini");
}

// The 30s read fails, so the 60s photo gets the second attempt. Two failures
// end the session as Failed -- the caller writes UNKNOWN, not NON_EV.
void failedThirtyFallsBackToSixtyThenFails() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV02-T0");

    require(policy.onImageArrived("EV02-T0", k30) == parking::ImageAction::RunOcr,
            "first attempt uses the 30s photo");
    auto fold = policy.onOcrResult("EV02-T0", k30, false);
    require(fold.tracked && !fold.resolved, "an unreadable plate does not resolve");
    require(!fold.exhausted, "one failure does not spend the budget");
    require(fold.attempts == 1, "one attempt recorded");
    require(policy.state("EV02-T0") == parking::OcrSessionState::Pending,
            "still pending while a retry is possible");

    require(policy.onImageArrived("EV02-T0", k60) == parking::ImageAction::RunOcr,
            "the 60s photo takes the retry");
    fold = policy.onOcrResult("EV02-T0", k60, false);
    require(fold.exhausted, "the second failure spends the budget");
    require(fold.attempts == 2, "two attempts recorded");
    require(policy.state("EV02-T0") == parking::OcrSessionState::Failed,
            "session ends Failed -> ocr_status=FAILED / ev_status=UNKNOWN");
}

// The 60s read can still save a session whose 30s read failed.
void failedThirtyThenSixtySucceeds() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV03-T0");

    (void)policy.onImageArrived("EV03-T0", k30);
    policy.onOcrResult("EV03-T0", k30, false);
    require(policy.onImageArrived("EV03-T0", k60) == parking::ImageAction::RunOcr,
            "the 60s photo retries");

    const auto fold = policy.onOcrResult("EV03-T0", k60, true);
    require(fold.resolved && !fold.exhausted, "the retry resolves the session");
    require(policy.state("EV03-T0") == parking::OcrSessionState::Resolved,
            "a late success still resolves");
}

// A Gemini call easily outlives the 30s gap between captures, so the 60s photo
// routinely arrives mid-flight. It must wait rather than fire a second call.
void sixtyArrivingDuringThirtyOcrIsHeldThenDemoted() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV04-T0");

    (void)policy.onImageArrived("EV04-T0", k30);  // OCR now in flight
    require(policy.onImageArrived("EV04-T0", k60) ==
                parking::ImageAction::HoldForPendingOcr,
            "the 60s photo waits while the 30s read is unanswered");

    const auto fold = policy.onOcrResult("EV04-T0", k30, true);
    require(fold.resolved, "the 30s read still resolves");
    require(!fold.releaseHeldImage,
            "a resolved session must not spend quota on the held photo");
}

void sixtyArrivingDuringThirtyOcrIsReleasedOnFailure() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV05-T0");

    (void)policy.onImageArrived("EV05-T0", k30);
    require(policy.onImageArrived("EV05-T0", k60) ==
                parking::ImageAction::HoldForPendingOcr,
            "the 60s photo waits");

    auto fold = policy.onOcrResult("EV05-T0", k30, false);
    require(fold.releaseHeldImage, "a failed 30s read releases the held photo");
    require(!fold.exhausted, "releasing is not exhausting");

    fold = policy.onOcrResult("EV05-T0", k60, false);
    require(fold.exhausted, "the released photo was the last attempt");
    require(policy.state("EV05-T0") == parking::OcrSessionState::Failed,
            "session ends Failed");
}

// The 30s capture can fail outright (camera fault), so the 60s photo is the
// first image the session ever sees. It must take the first attempt.
void sixtyAloneTakesTheFirstAttempt() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV06-T0");

    require(policy.onImageArrived("EV06-T0", k60) == parking::ImageAction::RunOcr,
            "with no 30s photo the 60s photo reads first");
    const auto fold = policy.onOcrResult("EV06-T0", k60, false);
    require(fold.attempts == 1, "it counts as the first attempt");
    require(!fold.exhausted, "the budget is not spent by one failure");
}

// A redelivered or duplicated image must never consume an attempt, and a
// result for a stage we are not running must be ignored outright.
void duplicatesAndStaleResultsAreIgnored() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV07-T0");

    require(policy.onImageArrived("EV07-T0", k30) == parking::ImageAction::RunOcr,
            "first delivery reads");
    require(policy.onImageArrived("EV07-T0", k30) ==
                parking::ImageAction::StoreEvidenceOnly,
            "a duplicate 30s delivery is evidence only");

    require(!policy.onOcrResult("EV07-T0", k60, false).tracked,
            "a result for a stage that never ran is ignored");

    auto fold = policy.onOcrResult("EV07-T0", k30, false);
    require(fold.tracked && fold.attempts == 1, "the real result counts once");
    require(!policy.onOcrResult("EV07-T0", k30, false).tracked,
            "a duplicate result does not count twice");
}

void unknownSessionsAreDropped() {
    parking::HallOcrPolicy policy;

    require(policy.onImageArrived("nobody", k30) == parking::ImageAction::Drop,
            "an image for an unopened session is dropped");
    require(!policy.onOcrResult("nobody", k30, true).tracked,
            "a result for an unopened session is ignored");
    require(!policy.closeSession("nobody").tracked,
            "closing an unopened session reports nothing");
}

// The car can leave while a retry is still pending -- e.g. the 60s capture
// never came back. That session must not be left looking mid-read.
void closeSettlesAnUnfinishedRead() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV08-T0");
    (void)policy.onImageArrived("EV08-T0", k30);
    policy.onOcrResult("EV08-T0", k30, false);

    const auto report = policy.closeSession("EV08-T0");
    require(report.tracked, "the session was tracked");
    require(report.state == parking::OcrSessionState::Failed,
            "an attempted but unfinished read closes as Failed");
    require(report.attempts == 1, "attempts are reported for the event log");
    require(policy.trackedSessions() == 0, "closing frees the session");
}

// No capture ever arrived: that is a capture failure, not a failed read, so it
// must not be reported as a spent OCR budget.
void closeWithoutAnyAttemptStaysPending() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV09-T0");

    const auto report = policy.closeSession("EV09-T0");
    require(report.tracked, "the session was tracked");
    require(report.state == parking::OcrSessionState::Pending,
            "no attempt was ever made, so nothing failed");
    require(report.attempts == 0, "no attempts recorded");
}

void resolvedSessionClosesResolved() {
    parking::HallOcrPolicy policy;
    policy.openSession("EV10-T0");
    (void)policy.onImageArrived("EV10-T0", k30);
    policy.onOcrResult("EV10-T0", k30, true);

    require(policy.closeSession("EV10-T0").state ==
                parking::OcrSessionState::Resolved,
            "a resolved session is not downgraded at close");
}

}  // namespace

int main() {
    try {
        thirtySecondSuccessSuppressesSixtySecondOcr();
        failedThirtyFallsBackToSixtyThenFails();
        failedThirtyThenSixtySucceeds();
        sixtyArrivingDuringThirtyOcrIsHeldThenDemoted();
        sixtyArrivingDuringThirtyOcrIsReleasedOnFailure();
        sixtyAloneTakesTheFirstAttempt();
        duplicatesAndStaleResultsAreIgnored();
        unknownSessionsAreDropped();
        closeSettlesAnUnfinishedRead();
        closeWithoutAnyAttemptStaysPending();
        resolvedSessionClosesResolved();
    } catch (const std::exception& error) {
        std::cerr << "HallOcrPolicyTest failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "HallOcrPolicyTest passed\n";
    return EXIT_SUCCESS;
}

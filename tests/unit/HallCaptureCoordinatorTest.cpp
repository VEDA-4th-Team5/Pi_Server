// Unit test for parking::HallCaptureCoordinator (EVDA-136).
//
// This is the class that closes the gap the ticket is about: before it, the
// hall path tracked sessions in memory under a string id while the OCR and DB
// path keyed everything off PARKING_SESSION.session_id (an INTEGER), and
// nothing bound the two. The assertions below are all about that binding.
//
// Every effect is a std::function port, so the whole pipeline is exercised
// here with recording fakes -- no SQLite, no Gemini, no OpenCV, no threads.
//
// What is asserted:
//   * one occupancy produces exactly one session_id, and every capture, image
//     row, OCR submission and failure row carries that same id;
//   * a resolved EV/PHEV plate is registered on the overtime timer under the
//     existing session_id (registering must not mint a second session);
//   * NON_EV and UNKNOWN never reach the EV timer;
//   * evidence-only images still get an IMAGE_LOG row but no OCR call;
//   * a held 60s image is resubmitted with the right session_id on failure;
//   * two failures write the failure row exactly once -- never NON_EV;
//   * a session whose row could not be opened degrades quietly.

#include "parking/HallCaptureCoordinator.hpp"
#include "parking/ParkingOccupancySession.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

constexpr auto k30 = parking::CaptureStage::First30s;
constexpr auto k60 = parking::CaptureStage::Second60s;

parking::ParkingTransitionResult started(const std::string& sessionId,
                                         const std::string& slotId) {
    parking::ParkingTransitionResult result;
    result.code = parking::ParkingTransitionCode::SessionStarted;
    result.slotId = slotId;
    result.sessionId = sessionId;
    result.session.emplace(sessionId, slotId, "HALL01",
                           std::chrono::system_clock::now(),
                           std::chrono::steady_clock::now());
    return result;
}

parking::ParkingTransitionResult completed(const std::string& sessionId,
                                           const std::string& slotId) {
    parking::ParkingTransitionResult result;
    result.code = parking::ParkingTransitionCode::SessionCompleted;
    result.slotId = slotId;
    result.sessionId = sessionId;
    return result;
}

parking::CapturedImage capture(const std::string& sessionId,
                               const std::string& slotId,
                               parking::CaptureStage stage,
                               const std::string& path) {
    parking::CapturedImage image;
    image.sessionId = sessionId;
    image.slotId = slotId;
    image.stage = stage;
    image.originalPath = path;
    return image;
}

parking::HallOcrOutcome outcome(const std::string& sessionId,
                                parking::CaptureStage stage, bool recognized,
                                const std::string& plate,
                                const std::string& classification) {
    parking::HallOcrOutcome value;
    value.sessionId = sessionId;
    value.stage = stage;
    value.recognized = recognized;
    value.plateNumber = plate;
    value.classification = classification;
    return value;
}

// Recording fakes for every port, plus the failure switches the tests need.
struct Recorder {
    struct ImageRow {
        int dbSessionId;
        std::string path;
        std::string enhancementType;
    };
    struct OcrCall {
        int dbSessionId;
        std::string path;
        parking::CaptureStage stage;
    };
    struct FailureRow {
        int dbSessionId;
        std::string slotId;
        int attempts;
    };
    struct TimerCall {
        int dbSessionId;
        std::string slotId;
        std::string plate;
    };

    int nextSessionId{100};
    bool refuseToOpen{false};

    std::vector<int> opened;
    std::vector<int> closed;
    std::vector<ImageRow> imageRows;
    std::vector<OcrCall> ocrCalls;
    std::vector<FailureRow> failures;
    std::vector<TimerCall> timerCalls;

    parking::HallCapturePorts ports() {
        parking::HallCapturePorts p;
        p.openSessionRow =
            [this](const std::string&) -> std::optional<int> {
            if (refuseToOpen) return std::nullopt;
            const int id = nextSessionId++;
            opened.push_back(id);
            return id;
        };
        p.closeSessionRow = [this](int dbSessionId, const std::string&) {
            closed.push_back(dbSessionId);
        };
        p.writeImageLog = [this](int dbSessionId,
                                 const parking::CapturedImage& image) {
            imageRows.push_back({dbSessionId, image.originalPath,
                                 parking::toEnhancementType(image.stage)});
        };
        p.submitOcr = [this](int dbSessionId,
                             const parking::CapturedImage& image) {
            ocrCalls.push_back({dbSessionId, image.originalPath, image.stage});
        };
        p.writeOcrFailure = [this](int dbSessionId, const std::string& slotId,
                                   int attempts) {
            failures.push_back({dbSessionId, slotId, attempts});
        };
        p.registerEvTimer = [this](int dbSessionId, const std::string& slotId,
                                   const std::string& plate) {
            timerCalls.push_back({dbSessionId, slotId, plate});
        };
        return p;
    }
};

// The core of the ticket: capture, OCR, image row and timer all land on the
// one session_id that the occupancy opened.
void everythingHangsOffOneSessionId() {
    Recorder recorder;
    parking::HallCaptureCoordinator coordinator(recorder.ports());

    coordinator.onTransition(started("EV01-T0", "EV01"));
    require(recorder.opened.size() == 1, "one occupancy opens one session row");
    const int sessionId = recorder.opened.front();
    require(coordinator.dbSessionId("EV01-T0") == sessionId,
            "the DB session id is retrievable by the Core session id");

    coordinator.onCaptureImage(capture("EV01-T0", "EV01", k30, "scene/30.jpg"));
    require(recorder.imageRows.size() == 1, "the capture is recorded as evidence");
    require(recorder.imageRows.front().dbSessionId == sessionId,
            "IMAGE_LOG row carries the session id");
    require(recorder.imageRows.front().enhancementType == std::string("HALL_30S"),
            "the 30s capture is tagged HALL_30S");
    require(recorder.ocrCalls.size() == 1, "the 30s capture is sent to OCR");
    require(recorder.ocrCalls.front().dbSessionId == sessionId,
            "the OCR submission carries the session id");

    coordinator.onOcrOutcome(
        outcome("EV01-T0", k30, true, "12가3456", "EV"));
    require(recorder.timerCalls.size() == 1, "a confirmed EV goes on the timer");
    require(recorder.timerCalls.front().dbSessionId == sessionId,
            "the timer reuses the existing session id, it does not mint one");
    require(recorder.timerCalls.front().plate == "12가3456",
            "the timer gets the plate that was read");
    require(recorder.opened.size() == 1,
            "resolving a plate must not open a second session");

    coordinator.onTransition(completed("EV01-T0", "EV01"));
    require(recorder.closed.size() == 1 && recorder.closed.front() == sessionId,
            "the same session id is closed on VACANT");
    require(recorder.failures.empty(), "a resolved session writes no failure row");
    require(coordinator.trackedSessions() == 0, "the binding is released");
}

// PHEV is charging-bay eligible too, so it must reach the timer. NON_EV and
// UNKNOWN must not.
void onlyElectricVehiclesReachTheTimer() {
    for (const std::string& classification : {std::string("EV"),
                                              std::string("PHEV")}) {
        Recorder recorder;
        parking::HallCaptureCoordinator coordinator(recorder.ports());
        coordinator.onTransition(started("S", "EV01"));
        coordinator.onCaptureImage(capture("S", "EV01", k30, "a.jpg"));
        coordinator.onOcrOutcome(outcome("S", k30, true, "P", classification));
        require(recorder.timerCalls.size() == 1,
                classification + " is registered on the overtime timer");
    }

    for (const std::string& classification : {std::string("NON_EV"),
                                              std::string("UNKNOWN")}) {
        Recorder recorder;
        parking::HallCaptureCoordinator coordinator(recorder.ports());
        coordinator.onTransition(started("S", "EV01"));
        coordinator.onCaptureImage(capture("S", "EV01", k30, "a.jpg"));
        coordinator.onOcrOutcome(outcome("S", k30, true, "P", classification));
        require(recorder.timerCalls.empty(),
                classification + " is not registered on the overtime timer");
    }
}

// Quota guard: after a successful 30s read the 60s photo is still filed as
// evidence, but no second Gemini call is made.
void evidenceOnlyImageIsStoredButNotRead() {
    Recorder recorder;
    parking::HallCaptureCoordinator coordinator(recorder.ports());

    coordinator.onTransition(started("EV02-T0", "EV02"));
    coordinator.onCaptureImage(capture("EV02-T0", "EV02", k30, "30.jpg"));
    coordinator.onOcrOutcome(outcome("EV02-T0", k30, true, "P", "EV"));

    coordinator.onCaptureImage(capture("EV02-T0", "EV02", k60, "60.jpg"));
    require(recorder.imageRows.size() == 2,
            "the 60s photo is still filed as evidence");
    require(recorder.imageRows.back().enhancementType == std::string("HALL_60S"),
            "the 60s capture is tagged HALL_60S");
    require(recorder.ocrCalls.size() == 1,
            "the 60s photo costs no second OCR call");
}

// The 60s image lands while the 30s call is still in flight; the failure must
// release it for the retry, carrying the same session id.
void heldImageIsResubmittedWithTheSameSessionId() {
    Recorder recorder;
    parking::HallCaptureCoordinator coordinator(recorder.ports());

    coordinator.onTransition(started("EV03-T0", "EV03"));
    const int sessionId = recorder.opened.front();

    coordinator.onCaptureImage(capture("EV03-T0", "EV03", k30, "30.jpg"));
    coordinator.onCaptureImage(capture("EV03-T0", "EV03", k60, "60.jpg"));
    require(recorder.imageRows.size() == 2,
            "a held image is still filed as evidence right away");
    require(recorder.ocrCalls.size() == 1, "the held image is not read yet");

    coordinator.onOcrOutcome(outcome("EV03-T0", k30, false, "", "OCR_FAILED"));
    require(recorder.ocrCalls.size() == 2, "the held image is released for retry");
    require(recorder.ocrCalls.back().path == "60.jpg", "the held image is the 60s one");
    require(recorder.ocrCalls.back().dbSessionId == sessionId,
            "the retry carries the same session id");
    require(recorder.failures.empty(), "one failure is not a spent budget");
}

// Two failed reads settle as UNKNOWN. The point of the assertion is what is
// absent: no EV timer registration, and nothing that would read as NON_EV.
void twoFailuresWriteUnknownExactlyOnce() {
    Recorder recorder;
    parking::HallCaptureCoordinator coordinator(recorder.ports());

    coordinator.onTransition(started("EV04-T0", "EV04"));
    const int sessionId = recorder.opened.front();

    coordinator.onCaptureImage(capture("EV04-T0", "EV04", k30, "30.jpg"));
    coordinator.onOcrOutcome(outcome("EV04-T0", k30, false, "", "OCR_FAILED"));
    coordinator.onCaptureImage(capture("EV04-T0", "EV04", k60, "60.jpg"));
    coordinator.onOcrOutcome(outcome("EV04-T0", k60, false, "", "OCR_FAILED"));

    require(recorder.failures.size() == 1, "the failure row is written once");
    require(recorder.failures.front().dbSessionId == sessionId,
            "the failure row carries the session id");
    require(recorder.failures.front().attempts == 2, "both attempts are reported");
    require(recorder.timerCalls.empty(),
            "an unread plate never reaches the EV timer");

    coordinator.onTransition(completed("EV04-T0", "EV04"));
    require(recorder.failures.size() == 1,
            "closing an already-failed session does not write a second row");
}

// The car leaves with the 60s capture still outstanding: the session must not
// end looking like a read that is still in progress.
void closingAnUnfinishedReadSettlesIt() {
    Recorder recorder;
    parking::HallCaptureCoordinator coordinator(recorder.ports());

    coordinator.onTransition(started("EV05-T0", "EV05"));
    coordinator.onCaptureImage(capture("EV05-T0", "EV05", k30, "30.jpg"));
    coordinator.onOcrOutcome(outcome("EV05-T0", k30, false, "", "OCR_FAILED"));

    coordinator.onTransition(completed("EV05-T0", "EV05"));
    require(recorder.failures.size() == 1,
            "the unfinished read is settled as UNKNOWN at close");
    require(recorder.closed.size() == 1, "the session row is still closed");
}

// A DB fault must not take the hall state machine down with it.
void aSessionRowThatCannotOpenDegradesQuietly() {
    Recorder recorder;
    recorder.refuseToOpen = true;
    parking::HallCaptureCoordinator coordinator(recorder.ports());

    coordinator.onTransition(started("EV06-T0", "EV06"));
    require(coordinator.trackedSessions() == 0, "nothing is bound");
    require(!coordinator.dbSessionId("EV06-T0").has_value(),
            "there is no session id to hand out");

    coordinator.onCaptureImage(capture("EV06-T0", "EV06", k30, "30.jpg"));
    require(recorder.imageRows.empty() && recorder.ocrCalls.empty(),
            "captures for an unbound session are dropped, not misfiled");

    coordinator.onTransition(completed("EV06-T0", "EV06"));
    require(recorder.closed.empty(), "there is no row to close");
}

// Late deliveries after the car left must not resurrect a session.
void lateDeliveriesAfterCloseAreIgnored() {
    Recorder recorder;
    parking::HallCaptureCoordinator coordinator(recorder.ports());

    coordinator.onTransition(started("EV07-T0", "EV07"));
    coordinator.onTransition(completed("EV07-T0", "EV07"));

    coordinator.onCaptureImage(capture("EV07-T0", "EV07", k60, "60.jpg"));
    coordinator.onOcrOutcome(outcome("EV07-T0", k60, true, "P", "EV"));

    require(recorder.imageRows.empty(), "a late capture writes no row");
    require(recorder.ocrCalls.empty(), "a late capture triggers no OCR");
    require(recorder.timerCalls.empty(), "a late result registers no timer");
}

}  // namespace

int main() {
    try {
        everythingHangsOffOneSessionId();
        onlyElectricVehiclesReachTheTimer();
        evidenceOnlyImageIsStoredButNotRead();
        heldImageIsResubmittedWithTheSameSessionId();
        twoFailuresWriteUnknownExactlyOnce();
        closingAnUnfinishedReadSettlesIt();
        aSessionRowThatCannotOpenDegradesQuietly();
        lateDeliveriesAfterCloseAreIgnored();
    } catch (const std::exception& error) {
        std::cerr << "HallCaptureCoordinatorTest failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "HallCaptureCoordinatorTest passed\n";
    return EXIT_SUCCESS;
}

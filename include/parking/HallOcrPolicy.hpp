#pragma once

#include "parking/HallCaptureTypes.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace parking {

enum class ImageAction {
    RunOcr,
    StoreEvidenceOnly,
    HoldForPendingOcr,
    Drop
};

enum class OcrSessionState {
    Pending,
    Resolved,
    Failed
};

[[nodiscard]] const char* toString(ImageAction action) noexcept;
[[nodiscard]] const char* toString(OcrSessionState state) noexcept;

class HallOcrPolicy {
public:
    explicit HallOcrPolicy(int maxAttempts = 2);

    void openSession(const std::string& sessionId);

    struct OcrFold {
        bool tracked{false};
        bool resolved{false};
        bool exhausted{false};
        bool releaseHeldImage{false};
        int attempts{0};
    };

    struct CloseReport {
        bool tracked{false};
        OcrSessionState state{OcrSessionState::Pending};
        int attempts{0};
        bool settledAtClose{false};
    };

    CloseReport closeSession(const std::string& sessionId);
    [[nodiscard]] ImageAction onImageArrived(const std::string& sessionId,
                                             CaptureStage stage);
    OcrFold onOcrResult(const std::string& sessionId, CaptureStage stage,
                        bool recognized);
    [[nodiscard]] OcrSessionState state(const std::string& sessionId) const;
    [[nodiscard]] std::size_t trackedSessions() const;

private:
    enum class StageState {
        NotArrived,
        Held,
        Running,
        Done
    };

    struct SessionState {
        OcrSessionState result{OcrSessionState::Pending};
        StageState stage30{StageState::NotArrived};
        StageState stage60{StageState::NotArrived};
        int attempts{0};
    };

    [[nodiscard]] static StageState& stageRef(SessionState& session,
                                              CaptureStage stage) noexcept;

    mutable std::mutex mutex_;
    int maxAttempts_;
    std::unordered_map<std::string, SessionState> sessions_;
};

}  // namespace parking

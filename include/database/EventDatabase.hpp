#pragma once

#include "event/FirePersistence.hpp"
#include "parking/ParkingCorrelation.hpp"
#include "parking/SlotTransitionTypes.hpp"
#include "snapshot/NormalizedRoi.hpp"
#include "parking_timer/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct sqlite3;

namespace database {

/** @brief 카메라/MQTT 이벤트와 증거 경로를 DB에 전달하는 입력 레코드다. */
struct EventRecord {
    std::string camera_id;
    std::string channel_id;
    std::string slot_id;
    std::string source_type;
    std::string source_id;
    std::string event_type;
    std::string severity;
    double confidence;
    std::string snapshot_path;
    std::string clip_path;
    std::string raw_topic;
    std::string raw_payload;
    std::string payload_json;
    std::string created_at;
    snapshot::NormalizedRoi applied_roi{};
    std::uint64_t roi_revision{};
};

/** @brief HTTP API가 사용하는 주차면과 현재 활성 세션의 읽기 모델이다. */
struct ParkingSlotView {
    std::string slot_id;
    std::string slot_type;
    std::string parking_status;
    std::string sensor_type;
    std::string updated_at;
    int session_id{-1};
    std::string plate_number;
    std::string entry_time;
    int is_ev{-1};
};

/** @brief IMAGE_LOG 한 행을 파일 API가 사용하기 좋은 문자열로 표현한다. */
struct ImageView {
    int image_id{-1};
    int session_id{-1};
    std::string original_path;
    std::string enhanced_path;
    std::string enhancement_type;
    std::string evidence_reason;
    std::string ocr_result;
    std::string captured_at;
    std::optional<snapshot::NormalizedRoi> applied_roi;
    std::uint64_t roi_revision{};
};

/** @brief 앱 로그인 계정의 인증 및 관리용 DB 레코드다. */
struct AppUserRecord {
    std::int64_t user_id{-1};
    std::string account_id;
    std::string password_hash;
    std::string display_name;
    bool enabled{};
    std::int64_t created_at_utc{};
    std::int64_t updated_at_utc{};
};

/** @brief 유효한 Bearer 세션과 사용자를 조인한 인증 결과다. */
struct AppSessionPrincipal {
    std::int64_t user_id{-1};
    std::string account_id;
    std::string display_name;
    std::int64_t expires_at_utc{};
};

enum class EvidenceInsertResult {
    Inserted,
    Duplicate,
    InactiveSession
};

/**
 * @brief 카메라·OCR·HTTP·타이머가 공유하는 단일 SQLite 저장 서비스다.
 *
 * C db_manager와 타이머용 prepared statement가 같은 SQLite 연결을 사용한다.
 * 공개 메서드는 db_mutex_로 직렬화되며 객체 복사 대신 프로세스당 한 인스턴스를 사용한다.
 */
class EventDatabase {
public:
    EventDatabase();
    explicit EventDatabase(const std::filesystem::path& database_path);
    ~EventDatabase();

    /** @brief SQLite 파일을 열고 FK 검사를 활성화한다. */
    bool open(const std::string& db_path);
    /** @brief 열린 DB 연결을 닫는다. */
    void close();
    [[nodiscard]] bool runtimeSchemaReady() const noexcept;
    [[nodiscard]] bool occupancySchemaReady() const noexcept;
    /** @brief 정규화된 카메라 이벤트와 선택적 Snapshot을 IMAGE_LOG/EVENT_LOG에 기록한다. */
    bool insertEvent(const EventRecord& record);
    /** @brief 센서·통신 운영 이벤트를 기존 EVENT_LOG schema에 저장한다. */
    bool insertSystemEvent(const std::string& event_type,
                           const std::string& slot_id,
                           const std::string& message);
    /** @brief Vehicle BestShot을 근거로 OCCUPIED 슬롯과 ACTIVE 세션을 원자적으로 연결한다. */
    bool createEntryWithBestShot(const std::string& slot_id,
                                 const std::string& image_path,
                                 const std::string& object_id,
                                 int* session_id);
    /** @brief 홀센서 입차 Snapshot으로 ACTIVE 세션과 IMAGE/EVENT 로그를 만든다. */
    bool createEntryWithSnapshot(const std::string& slot_id,
                                 const std::string& image_path,
                                 const std::string& source_id,
                                 int* session_id);
    /** @brief 번호판 BestShot과 선택적 카메라 plate text를 기존 세션에 연결한다. */
    bool attachPlateBestShot(int session_id,
                             const std::string& image_path,
                             const std::string& plate_text);
    /** @brief 이미 활성 세션이 있는 슬롯에 차량 BestShot 이미지만 연결한다(세션/슬롯상태는 건드리지 않음). */
    bool attachVehicleBestShot(int session_id,
                               const std::string& image_path,
                               const std::string& object_id);
    /** @brief 원본 IMAGE_LOG 행에 OpenCV 전처리 파일 경로를 연결한다. */
    bool attachEnhancedPlateImage(const std::string& image_path,
                                  const std::string& enhanced_image_path);
    /** @brief OCR 결과를 저장하고 VEHICLE 조회 결과(EV/NON_EV/UNKNOWN)를 반환한다. */
    std::string applyPlateOcr(int session_id,
                              const std::string& slot_id,
                              const std::string& image_path,
                              const std::string& plate_number,
                              double confidence);
    /** @brief 전체 주차면과 활성 세션을 조회한다. */
    bool listParkingSlots(std::vector<ParkingSlotView>& rows);
    /** @brief slot_id 한 건의 상태를 조회한다. */
    bool getParkingSlot(const std::string& slot_id, ParkingSlotView& row);
    /** @brief 세션에 연결된 모든 이미지 메타데이터를 조회한다. */
    bool listSessionImages(int session_id, std::vector<ImageView>& rows);
    /** @brief image_id로 이미지 경로와 OCR 메타데이터를 조회한다. */
    bool getImage(int image_id, ImageView& row);
    /** @brief 파일 삭제가 끝난 조기 출차 세션의 IMAGE_LOG 행을 모두 제거한다. */
    bool deleteSessionImageRecords(int session_id);

    /** @brief 정규화된 account ID로 앱 사용자를 조회한다. */
    std::optional<AppUserRecord> findAppUser(
        const std::string& account_id) const;
    /** @brief Argon2id PHC 문자열을 가진 앱 사용자를 생성한다. */
    bool createAppUser(const std::string& account_id,
                       const std::string& password_hash,
                       const std::string& display_name,
                       std::int64_t now_utc,
                       std::int64_t* user_id);
    /** @brief 비밀번호 해시를 제외한 앱 사용자 목록을 반환한다. */
    std::vector<AppUserRecord> listAppUsers() const;
    /** @brief 계정 활성 상태를 변경하며 비활성화 시 세션을 모두 폐기한다. */
    bool setAppUserEnabled(const std::string& account_id,
                           bool enabled,
                           std::int64_t now_utc);
    /** @brief 비밀번호를 교체하고 해당 사용자의 세션을 모두 폐기한다. */
    bool resetAppUserPassword(const std::string& account_id,
                              const std::string& password_hash,
                              std::int64_t now_utc);
    /** @brief 원문 토큰이 아닌 SHA-256 digest로 로그인 세션을 만든다. */
    bool createAppSession(std::int64_t user_id,
                          const std::vector<unsigned char>& token_hash,
                          std::int64_t created_at_utc,
                          std::int64_t expires_at_utc);
    /** @brief digest와 현재 시각으로 활성 세션을 검증한다. */
    std::optional<AppSessionPrincipal> findActiveAppSession(
        const std::vector<unsigned char>& token_hash,
        std::int64_t now_utc) const;
    /** @brief 현재 토큰 하나만 폐기한다. */
    bool revokeAppSession(const std::vector<unsigned char>& token_hash,
                          std::int64_t now_utc);

    /** @brief 런타임 설정 문자열을 조회한다. 키가 없으면 nullopt를 반환한다. */
    std::optional<std::string> getSystemSetting(const std::string& key) const;
    /** @brief 런타임 설정을 원자적으로 추가하거나 갱신한다. */
    bool upsertSystemSetting(const std::string& key, const std::string& value);

    /** Atomically creates or validates the complete configured Fire topology. */
    event::FireStoreMutationResult initializeFireTopology(
        const std::vector<event::FireChannelBootstrap>& topology);
    /** Compare-and-swap state mutation plus zero or two Fire delivery intents. */
    event::FireStoreMutationResult applyFireStateMutation(
        const event::FireStateMutation& mutation);
    std::optional<event::FireAlarmStateRecord> getFireAlarmState(
        const std::string& channel_id) const;
    std::vector<event::FireAlarmStateRecord> listFireAlarmStates() const;
    std::optional<event::FireOutboxRecord> getFireDelivery(
        const std::string& delivery_key) const;
    std::vector<event::FireOutboxRecord> listFireRetainedDeliveries() const;
    std::vector<event::FireOutboxRecord> listFireLifecycleDeliveries() const;
    std::vector<event::FireOutboxRecord> listPendingFireLifecycleDeliveries(
        const std::optional<std::string>& channel_id = std::nullopt) const;
    bool markFireDeliveryInFlight(const std::string& delivery_key,
                                  std::uint64_t fire_revision);
    bool markFireDeliveryPending(const std::string& delivery_key,
                                 std::uint64_t fire_revision,
                                 const std::string& error);
    bool acknowledgeFireDelivery(const std::string& delivery_key,
                                 std::uint64_t fire_revision);
    bool resetFireInFlightDeliveries();
    [[nodiscard]] bool isSensorBootIdRetired(
        const std::string& source_kind,
        const std::string& sensor_id,
        const std::string& boot_id) const;

    parking::SlotAdmissionResult admitSlotTransitionCommand(
        const parking::SlotTransitionCommand& command,
        std::size_t pending_capacity);
    std::size_t admitDueSlotDeadlines(
        std::int64_t now_epoch_ms,
        std::size_t pending_capacity,
        const std::vector<std::string>& blocked_slots = {});
    std::vector<parking::DurableSlotCommand>
    listRunnableSlotTransitionCommands(std::int64_t now_epoch_ms) const;
    parking::CommittedOccupancyTransition applySlotTransitionCommand(
        const std::string& command_id);
    std::vector<parking::CommittedOccupancyTransition>
    listPendingSlotTransitionEffects(std::int64_t now_epoch_ms) const;
    bool completeSlotTransitionEffects(const std::string& command_id);
    bool deferSlotTransitionEffects(const std::string& command_id,
                                    std::int64_t next_attempt_at_epoch_ms,
                                    const std::string& error) noexcept;
    bool deferSlotTransitionCommand(const std::string& command_id,
                                    std::int64_t next_attempt_at_epoch_ms,
                                    const std::string& error) noexcept;
    std::optional<std::int64_t> nextScheduledSlotDeadlineEpochMs() const;
    std::size_t pendingSlotTransitionCommandCount() const;
    std::size_t pendingSlotTransitionEffectCount() const;
    std::size_t pendingSlotTransitionDrainCount(
        std::int64_t shutdown_cutoff_epoch_ms) const;

    parking::ParkingCorrelationMatch resolveParkingCorrelation(
        const std::string& camera_id,
        const std::string& channel_id,
        const std::string& object_id,
        std::int64_t now_epoch_ms) const;
    parking::BestShotAttachResult attachBestShotIfActive(
        const parking::CommittedCorrelationLease& lease,
        parking::BestShotEvidenceKind kind,
        const std::string& image_ref,
        const std::string& image_path,
        const std::string& plate_text,
        std::int64_t now_epoch_ms);

    /** @brief 서버 시작 시 운영 DB에 안전한 멱등 migration만 적용한다. */
    void migrateRuntimeSchema();
    /** @brief 홀센서 입차의 ACTIVE 세션을 만들고 실제 SQLite ID를 반환한다. */
    std::int64_t createHallSession(const std::string& slot_id,
                                   const std::string& source_id,
                                   const std::string& entry_time);
    /** @brief 활성 세션에 종류별 증거 이미지 한 장만 원자적으로 연결한다. */
    EvidenceInsertResult insertEvidenceImage(
        std::int64_t session_id,
        const std::string& original_path,
        const std::string& evidence_reason,
        const std::string& captured_at,
        const std::string& enhanced_path = {},
        snapshot::NormalizedRoi applied_roi = {},
        std::uint64_t roi_revision = 0);
    /** @brief 이미 저장된 세션 증거 이미지 경로를 조회한다. */
    std::optional<std::string> findEvidenceImagePath(
        std::int64_t session_id,
        const std::string& evidence_reason) const;
    /** @brief 활성 세션에 30/60초 ROI 촬영본을 단계별 최대 한 장 연결한다. */
    EvidenceInsertResult insertHallCaptureImage(
        std::int64_t session_id,
        const std::string& original_path,
        const std::string& enhanced_path,
        const std::string& enhancement_type,
        const std::string& captured_at,
        snapshot::NormalizedRoi applied_roi = {},
        std::uint64_t roi_revision = 0);
    /** @brief OCR 시도 소진을 UNKNOWN으로 한 번만 EVENT_LOG에 기록한다. */
    bool markPlateOcrUnresolved(std::int64_t session_id,
                                const std::string& slot_id,
                                int attempts);

    /** @brief schema와 seed SQL을 적용하며 구형 컬럼을 먼저 호환 마이그레이션한다. */
    void initialize(const std::filesystem::path& schema_file,
                    const std::filesystem::path& seed_file);
    /** @brief VEHICLE의 is_ev/is_phev로 차량 종류를 분류한다. */
    parking_timer::VehicleCategory classifyVehicle(std::string_view car_number) const;
    /** @brief EV/PHEV 장기 점유용 ACTIVE 세션과 최초 이미지를 트랜잭션으로 생성한다. */
    std::int64_t insertParked(const std::string& car_number,
                              const std::string& slot_id,
                              const std::string& parked_at,
                              const std::string& image_path_1);
    /** @brief 아직 ACTIVE인 세션만 VIOLATION으로 조건부 갱신한다. */
    bool markViolation(std::int64_t log_id, const std::string& violation_at,
                       const std::string& image_path_2);
    /** @brief DB 생성 뒤 timer enqueue 실패 시 세션을 보상 종료한다. */
    bool cancelUnscheduled(std::int64_t log_id, const std::string& canceled_at);
    /** @brief slot_id의 활성 세션을 ENDED로 바꾸고 점유시간을 계산한다. */
    std::optional<parking_timer::LogRecord> departActiveBySlot(
        const std::string& slot_id, const std::string& departed_at);
    /** @brief 주차면의 출차되지 않은 최신 세션을 조회한다. */
    std::optional<parking_timer::LogRecord> findActiveBySlot(
        const std::string& slot_id) const;
    /** @brief 불변 session ID로 타이머 읽기 모델을 조회한다. */
    std::optional<parking_timer::LogRecord> findLogById(std::int64_t log_id) const;
    /** @brief 타이머 CLI 표시용 전체 세션을 생성 순서로 반환한다. */
    std::vector<parking_timer::LogRecord> listLogs() const;
    /** @brief 차량번호와 EV/PHEV/NON_EV 문자열 목록을 반환한다. */
    std::vector<std::pair<std::string, std::string>> listVehicles() const;
    /** @brief TIMER_ENTRY로 식별되는 데모 타이머 세션만 정리한다. */
    void clearTimerLogs();

private:
    void executeSqlUnlocked(const std::string& sql);
    static std::string readTextFile(const std::filesystem::path& path);

    bool opened_;
    bool runtime_schema_ready_{};
    bool occupancy_schema_ready_{};
    std::string db_path_;
    mutable std::mutex db_mutex_;
    sqlite3* db_{};
};

}

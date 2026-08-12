PRAGMA foreign_keys = ON;

BEGIN TRANSACTION;

-- 프로젝트 전체에서 사용하는 단일 차량 마스터.
-- 일반 내연기관의 세부 종류는 구분하지 않고 EV/PHEV 여부만 보존한다.
CREATE TABLE IF NOT EXISTS VEHICLE (
    vehicle_id INTEGER PRIMARY KEY AUTOINCREMENT,
    plate_number TEXT UNIQUE NOT NULL,
    is_ev INTEGER NOT NULL DEFAULT 0 CHECK (is_ev IN (0, 1)),
    is_phev INTEGER NOT NULL DEFAULT 0 CHECK (is_phev IN (0, 1)),
    registered_at TEXT DEFAULT CURRENT_TIMESTAMP,
    CHECK (NOT (is_ev = 1 AND is_phev = 1))
);

CREATE TABLE IF NOT EXISTS PARKING_SLOT (
    slot_id TEXT PRIMARY KEY,
    slot_type TEXT NOT NULL CHECK (slot_type IN ('EV_CHARGING', 'NORMAL')),
    status TEXT NOT NULL CHECK (status IN ('VACANT', 'OCCUPIED', 'ERROR')),
    sensor_type TEXT CHECK (sensor_type IN ('CAMERA', 'HALL', 'NONE')),
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS PARKING_SESSION (
    session_id INTEGER PRIMARY KEY AUTOINCREMENT,
    vehicle_id INTEGER,
    slot_id TEXT NOT NULL,
    plate_number TEXT,
    entry_time TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    violation_at TEXT,
    exit_time TEXT,
    duration_sec INTEGER DEFAULT 0,
    status TEXT NOT NULL CHECK (status IN ('ACTIVE', 'ENDED', 'VIOLATION', 'UNKNOWN')),
    FOREIGN KEY (vehicle_id) REFERENCES VEHICLE(vehicle_id),
    FOREIGN KEY (slot_id) REFERENCES PARKING_SLOT(slot_id)
);

CREATE TABLE IF NOT EXISTS IMAGE_LOG (
    image_id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER,
    original_image_path TEXT,
    enhanced_image_path TEXT,
    enhancement_type TEXT,
    evidence_reason TEXT CHECK (
        evidence_reason IS NULL OR evidence_reason IN (
            'OCCUPANCY_START_EVIDENCE',
            'OVERSTAY_EVIDENCE'
        )
    ),
    ocr_result TEXT,
    captured_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (session_id) REFERENCES PARKING_SESSION(session_id)
);

CREATE TABLE IF NOT EXISTS EVENT_LOG (
    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER,
    slot_id TEXT,
    event_type TEXT NOT NULL,
    message TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    handled INTEGER DEFAULT 0 CHECK (handled IN (0, 1)),
    FOREIGN KEY (session_id) REFERENCES PARKING_SESSION(session_id),
    FOREIGN KEY (slot_id) REFERENCES PARKING_SLOT(slot_id)
);

-- Qt REST API에서 변경하는 런타임 설정을 서버 재시작 후에도 복원한다.
CREATE TABLE IF NOT EXISTS SYSTEM_SETTINGS (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS FIRE_ALARM_STATE (
    channel_id TEXT PRIMARY KEY,
    sensor_id TEXT UNIQUE NOT NULL,
    retained_topic TEXT UNIQUE NOT NULL,
    desired_lifecycle TEXT NOT NULL CHECK (
        desired_lifecycle IN ('OPEN', 'ACKNOWLEDGED', 'RESOLVED')
    ),
    active_alarm_id TEXT,
    last_event_id TEXT NOT NULL,
    fire_revision INTEGER NOT NULL CHECK (fire_revision > 0),
    protocol_mode TEXT NOT NULL CHECK (
        protocol_mode IN ('UNSEEN', 'LEGACY', 'VERSIONED')
    ),
    active_boot_id TEXT,
    last_source_sequence TEXT,
    last_signal_json TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS FIRE_MQTT_OUTBOX (
    delivery_key TEXT PRIMARY KEY,
    sink_kind TEXT NOT NULL CHECK (
        sink_kind IN ('RETAINED_STATE', 'LIFECYCLE_EVENT')
    ),
    logical_key TEXT NOT NULL,
    sensor_id TEXT NOT NULL,
    channel_id TEXT NOT NULL,
    event_id TEXT NOT NULL,
    alarm_id TEXT NOT NULL,
    fire_revision INTEGER NOT NULL CHECK (fire_revision > 0),
    topic TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    qos INTEGER NOT NULL DEFAULT 1 CHECK (qos = 1),
    retain INTEGER NOT NULL CHECK (retain IN (0, 1)),
    attempt_count INTEGER NOT NULL DEFAULT 0 CHECK (attempt_count >= 0),
    next_attempt_at TEXT NOT NULL,
    last_error TEXT NOT NULL DEFAULT '',
    delivery_state TEXT NOT NULL CHECK (
        delivery_state IN ('PENDING', 'IN_FLIGHT', 'ACKED')
    ),
    acked_revision INTEGER,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    FOREIGN KEY (channel_id) REFERENCES FIRE_ALARM_STATE(channel_id)
);

CREATE TABLE IF NOT EXISTS SENSOR_RETIRED_BOOT_ID (
    source_kind TEXT NOT NULL CHECK (source_kind IN ('HALL', 'FIRE')),
    sensor_id TEXT NOT NULL,
    boot_id TEXT NOT NULL,
    retired_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (source_kind, sensor_id, boot_id)
);

CREATE INDEX IF NOT EXISTS idx_vehicle_plate ON VEHICLE(plate_number);
CREATE INDEX IF NOT EXISTS idx_session_slot ON PARKING_SESSION(slot_id);
CREATE INDEX IF NOT EXISTS idx_session_vehicle ON PARKING_SESSION(vehicle_id);
CREATE INDEX IF NOT EXISTS idx_session_status ON PARKING_SESSION(status);
CREATE UNIQUE INDEX IF NOT EXISTS ux_parking_session_active_slot
    ON PARKING_SESSION(slot_id)
    WHERE status IN ('ACTIVE', 'VIOLATION') AND exit_time IS NULL;
CREATE INDEX IF NOT EXISTS idx_image_session ON IMAGE_LOG(session_id);
CREATE UNIQUE INDEX IF NOT EXISTS ux_image_evidence_session_reason
    ON IMAGE_LOG(session_id, evidence_reason)
    WHERE session_id IS NOT NULL AND evidence_reason IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_event_session ON EVENT_LOG(session_id);
CREATE INDEX IF NOT EXISTS idx_event_slot ON EVENT_LOG(slot_id);
CREATE INDEX IF NOT EXISTS idx_event_type ON EVENT_LOG(event_type);
CREATE INDEX IF NOT EXISTS idx_event_created_at ON EVENT_LOG(created_at);

COMMIT;

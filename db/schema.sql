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
    occupancy_attempt_id TEXT,
    entry_command_id TEXT,
    exit_command_id TEXT,
    entry_time_epoch_ms INTEGER,
    exit_time_epoch_ms INTEGER,
    hall_confirmed INTEGER NOT NULL DEFAULT 0 CHECK (hall_confirmed IN (0, 1)),
    iva_confirmed INTEGER NOT NULL DEFAULT 0 CHECK (iva_confirmed IN (0, 1)),
    hall_occupied INTEGER NOT NULL DEFAULT 0 CHECK (hall_occupied IN (0, 1)),
    iva_occupied INTEGER NOT NULL DEFAULT 0 CHECK (iva_occupied IN (0, 1)),
    FOREIGN KEY (vehicle_id) REFERENCES VEHICLE(vehicle_id),
    FOREIGN KEY (slot_id) REFERENCES PARKING_SLOT(slot_id)
);

CREATE TABLE IF NOT EXISTS IMAGE_LOG (
    image_id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER,
    original_image_path TEXT,
    enhanced_image_path TEXT,
    enhancement_type TEXT,
    correlation_id TEXT,
    camera_object_id TEXT,
    image_ref TEXT,
    correlation_binding_revision INTEGER,
    roi_x REAL,
    roi_y REAL,
    roi_width REAL,
    roi_height REAL,
    roi_revision INTEGER,
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

-- Qt 앱 로그인 계정. password_hash에는 libsodium Argon2id PHC 문자열만 저장한다.
CREATE TABLE IF NOT EXISTS app_users (
    user_id INTEGER PRIMARY KEY,
    account_id TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    display_name TEXT,
    enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL
);

-- 원문 access token은 반환 직후 폐기하고 SHA-256 digest만 저장한다.
CREATE TABLE IF NOT EXISTS app_sessions (
    session_id INTEGER PRIMARY KEY,
    user_id INTEGER NOT NULL,
    token_hash BLOB NOT NULL UNIQUE,
    created_at_utc INTEGER NOT NULL,
    expires_at_utc INTEGER NOT NULL,
    revoked_at_utc INTEGER,
    FOREIGN KEY(user_id) REFERENCES app_users(user_id)
);

CREATE INDEX IF NOT EXISTS idx_app_sessions_user_active
    ON app_sessions(user_id, expires_at_utc, revoked_at_utc);

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

CREATE TABLE IF NOT EXISTS OCCUPANCY_COMMAND_INBOX (
    command_id TEXT PRIMARY KEY,
    slot_id TEXT NOT NULL,
    source_kind TEXT NOT NULL CHECK (source_kind IN (
        'HALL_OBSERVATION', 'CAMERA_OBSERVATION', 'EXIT_DEADLINE'
    )),
    sensor_id TEXT NOT NULL DEFAULT '',
    source_identity TEXT NOT NULL,
    source_sequence TEXT,
    occurred_at TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    due_at_epoch_ms INTEGER NOT NULL DEFAULT 0,
    admission_ordinal INTEGER NOT NULL UNIQUE,
    status TEXT NOT NULL CHECK (status IN (
        'PENDING_UNPREPARED', 'PENDING_PREPARED',
        'APPLIED', 'REJECTED_INVALID'
    )),
    occupancy_attempt_id TEXT NOT NULL DEFAULT '',
    correlation_id TEXT NOT NULL DEFAULT '',
    observation_generation INTEGER NOT NULL DEFAULT 0,
    deadline_id TEXT NOT NULL DEFAULT '',
    expected_session_id INTEGER,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    next_attempt_at_epoch_ms INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NOT NULL DEFAULT '',
    result_code TEXT NOT NULL DEFAULT '',
    result_session_id INTEGER,
    effect_state TEXT NOT NULL DEFAULT 'NONE' CHECK (effect_state IN (
        'NONE', 'PENDING', 'APPLIED'
    )),
    effect_attempt_count INTEGER NOT NULL DEFAULT 0,
    effect_next_attempt_at_epoch_ms INTEGER NOT NULL DEFAULT 0,
    effect_last_error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (slot_id) REFERENCES PARKING_SLOT(slot_id)
);

CREATE TABLE IF NOT EXISTS PARKING_CORRELATION_BINDING (
    correlation_id TEXT PRIMARY KEY,
    source_command_id TEXT NOT NULL UNIQUE,
    occupancy_attempt_id TEXT NOT NULL,
    session_id INTEGER,
    slot_id TEXT NOT NULL,
    camera_id TEXT NOT NULL,
    video_source_token TEXT NOT NULL,
    rule_name TEXT NOT NULL,
    object_id TEXT NOT NULL,
    channel_id TEXT NOT NULL,
    binding_revision INTEGER NOT NULL CHECK (binding_revision > 0),
    state TEXT NOT NULL CHECK (state IN (
        'PENDING', 'COMMITTED', 'ENDED', 'FAILED', 'EXPIRED', 'QUARANTINED'
    )),
    created_at_epoch_ms INTEGER NOT NULL,
    expires_at_epoch_ms INTEGER NOT NULL,
    updated_at_epoch_ms INTEGER NOT NULL,
    ended_at_epoch_ms INTEGER,
    CHECK (
        state != 'COMMITTED' OR
        (session_id IS NOT NULL AND occupancy_attempt_id != '')
    ),
    UNIQUE (
        occupancy_attempt_id, camera_id, video_source_token, rule_name, object_id
    ),
    FOREIGN KEY (session_id) REFERENCES PARKING_SESSION(session_id),
    FOREIGN KEY (slot_id) REFERENCES PARKING_SLOT(slot_id)
);

CREATE TABLE IF NOT EXISTS OCCUPANCY_SENSOR_SEQUENCE_STATE (
    sensor_id TEXT PRIMARY KEY,
    protocol_mode TEXT NOT NULL CHECK (protocol_mode IN ('LEGACY','VERSIONED')),
    active_boot_id TEXT,
    last_sequence TEXT NOT NULL,
    last_command_id TEXT NOT NULL,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS IVA_SLOT_OBSERVATION_STATE (
    slot_id TEXT PRIMARY KEY,
    occupancy_attempt_id TEXT NOT NULL DEFAULT '',
    active_session_id INTEGER,
    observation_generation INTEGER NOT NULL DEFAULT 0,
    observed_state TEXT NOT NULL CHECK (observed_state IN (
        'UNKNOWN','OCCUPIED','VACANT_PENDING','VACANT'
    )),
    configured_areas_json TEXT NOT NULL DEFAULT '[]',
    area_states_json TEXT NOT NULL DEFAULT '{}',
    last_source_command_id TEXT NOT NULL DEFAULT '',
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (slot_id) REFERENCES PARKING_SLOT(slot_id)
);

CREATE TABLE IF NOT EXISTS OCCUPANCY_EXIT_DEADLINE (
    deadline_id TEXT PRIMARY KEY,
    slot_id TEXT NOT NULL,
    occupancy_attempt_id TEXT NOT NULL,
    expected_session_id INTEGER NOT NULL,
    observation_generation INTEGER NOT NULL,
    due_at_epoch_ms INTEGER NOT NULL,
    occupancy_policy TEXT NOT NULL DEFAULT 'CAMERA_IVA' CHECK (
        occupancy_policy IN ('CAMERA_IVA','HYBRID_OR')
    ),
    state TEXT NOT NULL CHECK (state IN (
        'SCHEDULED','ADMITTED','SUPERSEDED','APPLIED'
    )),
    admitted_command_id TEXT UNIQUE,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (slot_id) REFERENCES PARKING_SLOT(slot_id),
    FOREIGN KEY (expected_session_id) REFERENCES PARKING_SESSION(session_id)
);

CREATE INDEX IF NOT EXISTS idx_vehicle_plate ON VEHICLE(plate_number);
CREATE INDEX IF NOT EXISTS idx_session_slot ON PARKING_SESSION(slot_id);
CREATE INDEX IF NOT EXISTS idx_session_vehicle ON PARKING_SESSION(vehicle_id);
CREATE INDEX IF NOT EXISTS idx_session_status ON PARKING_SESSION(status);
CREATE UNIQUE INDEX IF NOT EXISTS ux_parking_session_active_slot
    ON PARKING_SESSION(slot_id)
    WHERE status IN ('ACTIVE', 'VIOLATION') AND exit_time IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS ux_parking_session_entry_command
    ON PARKING_SESSION(entry_command_id)
    WHERE entry_command_id IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS ux_parking_session_exit_command
    ON PARKING_SESSION(exit_command_id)
    WHERE exit_command_id IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS ux_parking_active_slot
    ON PARKING_SESSION(slot_id)
    WHERE exit_time IS NULL AND status IN ('ACTIVE','VIOLATION');
CREATE INDEX IF NOT EXISTS idx_image_session ON IMAGE_LOG(session_id);
CREATE UNIQUE INDEX IF NOT EXISTS ux_image_evidence_session_reason
    ON IMAGE_LOG(session_id, evidence_reason)
    WHERE session_id IS NOT NULL AND evidence_reason IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS ux_image_bestshot_correlation
    ON IMAGE_LOG(correlation_id, enhancement_type, image_ref)
    WHERE correlation_id IS NOT NULL
      AND enhancement_type IS NOT NULL
      AND image_ref IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_event_session ON EVENT_LOG(session_id);
CREATE INDEX IF NOT EXISTS idx_event_slot ON EVENT_LOG(slot_id);
CREATE INDEX IF NOT EXISTS idx_event_type ON EVENT_LOG(event_type);
CREATE INDEX IF NOT EXISTS idx_event_created_at ON EVENT_LOG(created_at);
CREATE UNIQUE INDEX IF NOT EXISTS ux_fire_outbox_sink_logical
    ON FIRE_MQTT_OUTBOX(sink_kind, logical_key);
CREATE UNIQUE INDEX IF NOT EXISTS ux_fire_lifecycle_event
    ON FIRE_MQTT_OUTBOX(sink_kind, event_id)
    WHERE sink_kind = 'LIFECYCLE_EVENT';
CREATE INDEX IF NOT EXISTS idx_fire_outbox_pending
    ON FIRE_MQTT_OUTBOX(
        sink_kind, delivery_state, channel_id, fire_revision
    );
CREATE UNIQUE INDEX IF NOT EXISTS ux_occupancy_source_identity
    ON OCCUPANCY_COMMAND_INBOX(source_kind, source_identity);
CREATE INDEX IF NOT EXISTS idx_occupancy_effect_pending
    ON OCCUPANCY_COMMAND_INBOX(
        effect_state, slot_id, admission_ordinal,
        effect_next_attempt_at_epoch_ms
    );
CREATE INDEX IF NOT EXISTS idx_occupancy_inbox_runnable
    ON OCCUPANCY_COMMAND_INBOX(
        status, slot_id, admission_ordinal, next_attempt_at_epoch_ms
    );
CREATE UNIQUE INDEX IF NOT EXISTS ux_occupancy_live_deadline_slot
    ON OCCUPANCY_EXIT_DEADLINE(slot_id)
    WHERE state IN ('SCHEDULED','ADMITTED');
CREATE INDEX IF NOT EXISTS idx_parking_correlation_bestshot_lookup
    ON PARKING_CORRELATION_BINDING(
        camera_id, channel_id, object_id, state,
        expires_at_epoch_ms, correlation_id
    );
CREATE INDEX IF NOT EXISTS idx_parking_correlation_session
    ON PARKING_CORRELATION_BINDING(session_id, state, correlation_id);
CREATE TRIGGER IF NOT EXISTS tr_actor_session_identity_insert
BEFORE INSERT ON PARKING_SESSION
WHEN NEW.entry_command_id IS NOT NULL AND (
    NEW.occupancy_attempt_id IS NULL OR
    NEW.occupancy_attempt_id = '' OR
    NEW.entry_time_epoch_ms IS NULL
)
BEGIN
    SELECT RAISE(ABORT, 'actor session identity missing');
END;

COMMIT;

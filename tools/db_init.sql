PRAGMA foreign_keys = ON;

BEGIN TRANSACTION;

CREATE TABLE IF NOT EXISTS VEHICLE (
    vehicle_id INTEGER PRIMARY KEY AUTOINCREMENT,
    plate_number TEXT UNIQUE NOT NULL,
    is_ev INTEGER NOT NULL CHECK (is_ev IN (0, 1)),
    registered_at TEXT DEFAULT CURRENT_TIMESTAMP
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

CREATE INDEX IF NOT EXISTS idx_vehicle_plate ON VEHICLE(plate_number);
CREATE INDEX IF NOT EXISTS idx_session_slot ON PARKING_SESSION(slot_id);
CREATE INDEX IF NOT EXISTS idx_session_vehicle ON PARKING_SESSION(vehicle_id);
CREATE INDEX IF NOT EXISTS idx_session_status ON PARKING_SESSION(status);
CREATE INDEX IF NOT EXISTS idx_image_session ON IMAGE_LOG(session_id);
CREATE INDEX IF NOT EXISTS idx_event_session ON EVENT_LOG(session_id);
CREATE INDEX IF NOT EXISTS idx_event_slot ON EVENT_LOG(slot_id);
CREATE INDEX IF NOT EXISTS idx_event_type ON EVENT_LOG(event_type);
CREATE INDEX IF NOT EXISTS idx_event_created_at ON EVENT_LOG(created_at);

COMMIT;

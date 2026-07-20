PRAGMA foreign_keys = ON;

-- is_ev_table_schema.png에 제시된 차량 마스터.
-- 단순 Boolean 대신 PHEV를 구분할 수 있도록 vehicle_type을 사용한다.
CREATE TABLE IF NOT EXISTS mock_vehicle_master (
    car_number  TEXT PRIMARY KEY NOT NULL,
    vehicle_type TEXT NOT NULL
        CHECK (vehicle_type IN ('EV', 'PHEV', 'GASOLINE', 'DIESEL'))
);

-- log_table_schema.png의 필드를 그대로 옮기고, 타이머 명세의
-- lazy deletion에 필요한 is_canceled만 확장 필드로 추가했다.
CREATE TABLE IF NOT EXISTS timer_log (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    car_number    TEXT NOT NULL,
    zone_id       INTEGER NOT NULL CHECK (zone_id > 0),
    status        TEXT NOT NULL DEFAULT 'PARKED'
        CHECK (status IN ('PARKED', 'VIOLATION', 'DEPARTS')),
    parked_at     TEXT NOT NULL,
    violation_at  TEXT,
    departed_at   TEXT,
    image_path_1  TEXT,
    image_path_2  TEXT,
    is_canceled   INTEGER NOT NULL DEFAULT 0
        CHECK (is_canceled IN (0, 1)),
    FOREIGN KEY (car_number) REFERENCES mock_vehicle_master(car_number)
);

-- 한 구역에는 동시에 하나의 활성 주차 세션만 존재할 수 있다.
CREATE UNIQUE INDEX IF NOT EXISTS ux_timer_log_active_zone
    ON timer_log(zone_id)
    WHERE departed_at IS NULL;

CREATE INDEX IF NOT EXISTS ix_timer_log_car_number
    ON timer_log(car_number);


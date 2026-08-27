-- 통합 테스트 전용 Seed다. 운영과 동일한 EV 4면/일반 4면 구성을 사용한다.
INSERT INTO VEHICLE(plate_number, is_ev, is_reference)
VALUES
    ('123가4567', 1, 1),
    ('234나5678', 1, 1),
    ('345다6789', 0, 1),
    ('456라7890', 0, 1),
    ('12가3456', 1, 1),
    ('34나5678', 1, 1),
    ('99다8888', 0, 1),
    ('77라1234', 0, 1),
    ('342가2670', 1, 1),
    ('294마3087', 1, 1),
    ('429가6789', 1, 1),
    ('383가6890', 0, 1),
    ('315다8504', 0, 1)
ON CONFLICT(plate_number) DO UPDATE SET
    is_ev=excluded.is_ev,
    is_reference=1;

INSERT OR IGNORE INTO PARKING_SLOT(slot_id, slot_type, status, sensor_type)
VALUES
    ('EV01', 'EV_CHARGING', 'VACANT', 'CAMERA'),
    ('EV02', 'EV_CHARGING', 'VACANT', 'CAMERA'),
    ('EV03', 'EV_CHARGING', 'VACANT', 'CAMERA'),
    ('EV04', 'EV_CHARGING', 'VACANT', 'CAMERA'),
    ('P01', 'NORMAL', 'VACANT', 'HALL'),
    ('P02', 'NORMAL', 'VACANT', 'HALL'),
    ('P03', 'NORMAL', 'VACANT', 'HALL'),
    ('P04', 'NORMAL', 'VACANT', 'HALL');

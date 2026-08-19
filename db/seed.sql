INSERT OR IGNORE INTO VEHICLE(plate_number, is_ev, is_phev)
VALUES
    ('123가4567', 1, 0),
    ('234나5678', 0, 1),
    ('345다6789', 0, 0),
    ('456라7890', 0, 0),
    ('12가3456', 1, 0),
    ('34나5678', 1, 0),
    ('99다8888', 0, 0),
    ('77라1234', 0, 0);

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

-- EV 슬롯 기본 ROI(전체 프레임, x=0 y=0 width=1 height=1 revision=1). Qt의
-- ROI 웹 도구로 실제 좌표를 저장하면 ParkingRoiSettingsService가 이 값을
-- 덮어쓴다. 이 기본값은 개발/테스트 중 DB를 초기화해도 캡처가 즉시 동작하도록
-- 하기 위한 것이며, 정확한 크롭 좌표가 아니다.
INSERT OR IGNORE INTO SYSTEM_SETTINGS(key, value)
VALUES
    ('parking_slot_roi.EV01', '0,0,1,1,1'),
    ('parking_slot_roi.EV02', '0,0,1,1,1'),
    ('parking_slot_roi.EV03', '0,0,1,1,1'),
    ('parking_slot_roi.EV04', '0,0,1,1,1');

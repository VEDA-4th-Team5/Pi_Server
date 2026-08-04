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

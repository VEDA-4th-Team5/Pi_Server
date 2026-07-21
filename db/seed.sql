INSERT OR IGNORE INTO VEHICLE(plate_number, is_ev, is_phev)
VALUES
    ('123가4567', 1, 0),
    ('234나5678', 0, 1),
    ('345다6789', 0, 0),
    ('456라7890', 0, 0);

INSERT OR IGNORE INTO PARKING_SLOT(slot_id, slot_type, status, sensor_type)
VALUES
    ('1', 'EV_CHARGING', 'VACANT', 'HALL'),
    ('2', 'EV_CHARGING', 'VACANT', 'HALL'),
    ('3', 'EV_CHARGING', 'VACANT', 'HALL'),
    ('4', 'EV_CHARGING', 'VACANT', 'HALL');

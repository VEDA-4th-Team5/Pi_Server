# DB Schema Proposal (historical — superseded)

> 이 문서가 제안했던 네 항목(ROI 매핑, OCR 상태/신뢰도, 화재 alarm state
> OPEN/ACKNOWLEDGED/RESOLVED, 구조화된 EV 판정)은 모두 구현돼 `db/schema.sql`에
> 있다. 현재 스키마는 5개가 아니라 16개 테이블이며, 최신 테이블 목록과 ERD는
> [`docs/DB_IMPLEMENTATION.md`](DB_IMPLEMENTATION.md)를 참고한다. 이 문서는
> 초기 제안 배경을 남기는 기록으로만 유지한다.

The compatible baseline schema is defined in `db/schema.sql` and contains
`VEHICLE`, `PARKING_SLOT`, `PARKING_SESSION`, `IMAGE_LOG`, and `EVENT_LOG`.

Before changing the schema, add versioned migrations. Candidate additions were:

- slot camera channel and normalized ROI mapping; → `IMAGE_LOG.roi_*`,
  `PARKING_CORRELATION_BINDING`
- OCR status, confidence, and error fields; → `PARKING_SESSION.parking_ocr_confidence`,
  `IMAGE_LOG.ocr_result`
- event alarm state and OPEN/ACKNOWLEDGED/RESOLVED timestamps; → `FIRE_ALARM_STATE`
- structured EV classification state separate from OCR failure. →
  `VEHICLE.is_ev`, `ENTRANCE_RECOGNITION.vision_is_ev`

Multi-row entry/exit operations must use a SQLite transaction. Existing columns
must remain compatible until all C and C++ callers have migrated.

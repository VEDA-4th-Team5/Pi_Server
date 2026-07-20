# DB Schema Proposal

The compatible baseline schema is defined in `tools/db_init.sql` and contains
`VEHICLE`, `PARKING_SLOT`, `PARKING_SESSION`, `IMAGE_LOG`, and `EVENT_LOG`.

Before changing the schema, add versioned migrations. Candidate additions are:

- slot camera channel and normalized ROI mapping;
- OCR status, confidence, and error fields;
- event alarm state and OPEN/ACKNOWLEDGED/RESOLVED timestamps;
- structured EV classification state separate from OCR failure.

Multi-row entry/exit operations must use a SQLite transaction. Existing columns
must remain compatible until all C and C++ callers have migrated.

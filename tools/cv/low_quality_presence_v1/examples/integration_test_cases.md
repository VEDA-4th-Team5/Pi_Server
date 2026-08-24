# Integration Test Cases

## T1. RGB normal quality

- raw JPEG decode succeeds
- `quality_below_threshold=false`
- result is one of `PRESENT/ABSENT/REVIEW`
- existing OCR path is unchanged

## T2. RGB low quality

- raw quality `<0.25`
- low-quality policy is selected
- policy cohort is not changed by rectification output quality

## T3. IR input

- grayscale/IR path is used
- color restoration is not applied
- icon color evidence is not interpreted as restored RGB color

## T4. No candidate

- `presence_decision=REVIEW`
- `reason=no_candidate`
- server process remains alive

## T5. Guard transition

- frozen policy result `ABSENT`
- runtime observability guard flag is true
- final result becomes `REVIEW`, not `PRESENT`

## T6. OCR non-regression

- OCR receives the existing baseline image/crop
- selector candidate crop is not silently substituted

## T7. Model schema mismatch

- feature count/order or coefficient shape mismatch is detected
- CV job is rejected with `model_schema_mismatch`
- no fallback to a different threshold occurs


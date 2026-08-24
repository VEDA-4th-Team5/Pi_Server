# Runtime Contract

## 판정 의미

`presence_decision`은 아이콘/번호판 관측 가능성에 대한 runtime decision이다. 최종 EV/NON_EV 분류가 아니다.

| 값 | 의미 | 후속 처리 |
|---|---|---|
| `PRESENT` | 동결 policy에서 아이콘 존재 evidence가 확인됨 | 기존 OCR/후속 EV 경로에 evidence로 전달 |
| `ABSENT` | 현재 runtime feature에서 존재 evidence가 확인되지 않음 | low-quality guard가 없을 때만 기록 |
| `REVIEW` | 후보 없음, 평면화 실패, low observability, 오류 또는 충돌 | 자동 NON_EV/ABSENT 확정 금지 |

## 동결 규칙

- raw quality threshold: `0.25`
- candidate origins: `geometry`, `character_edge`
- candidate selection: adaptive + component group + coarse reserve, top-3
- rectified target: reference code의 `TARGET_SIZE`를 사용
- spatial side: `left`, `right`
- low-quality cohort: raw input quality 기준
- guard: runtime observable feature만 사용
- human visibility, physical presence GT, plate quad, OCR GT: runtime 입력 금지

모델 feature 순서, mean/scale, coefficient, threshold는 `model_bundle/*.json`의 frozen artifact를 그대로 사용한다. C++ 포트에서 feature 순서를 재정렬하거나 threshold를 새로 튜닝하지 않는다.

## 평가 전용 필드

다음 필드는 서버 runtime 입력으로 넣지 않는다.

- `icon_presence_label`
- `icon_visibility_label`
- `plate_presence_label`
- `plate_visibility_label`
- `plate_quad`
- `ocr_ground_truth`
- `alignment_ssim`의 GT reference
- `exposure_status`

평가에서는 선택 이후 geometry/OCR를 측정하지만, runtime decision을 만들 때 GT를 사용하지 않는다.

## 필드 권장 정의

| 필드 | 정의 |
|---|---|
| `input_quality` | raw input에서 계산한 0~1 품질 점수 |
| `quality_below_threshold` | `input_quality < 0.25` |
| `plate_candidate_found` | candidate가 선택됐는지 여부. 물리적 번호판 존재 GT가 아님 |
| `icon_presence_predicted` | frozen spatial policy의 runtime icon presence 결과 |
| `rectification_success` | runtime geometric rectification이 성공했는지 여부. GT IoU success와 다름 |
| `rectification_method` | `perspective`, `rotated_rect`, `crop_normalized`, `no_candidate` 등 reference 값 |
| `reason` | `frozen_spatial_policy`, `review_runtime_icon_observability_low_confidence`, `no_candidate` 등 |
| `processing_ms` | image decode부터 runtime decision까지의 시간 |

## 오류 처리

```text
image decode 실패       → REVIEW, error=image_decode_failed
candidate 없음          → REVIEW, error=no_candidate
rectification 실패      → REVIEW, error=rectification_failed
model artifact 불일치   → 작업 거부, error=model_schema_mismatch
runtime exception       → REVIEW, error=runtime_exception
```

모델 schema mismatch는 조용히 fallback하지 않는다. 서버 시작 시 model bundle을 검증하고, 실패하면 해당 CV worker만 disabled 상태로 올린다.


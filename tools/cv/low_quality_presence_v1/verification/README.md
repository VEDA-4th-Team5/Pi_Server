# Verification Handoff

## Before server merge

1. Python reference와 C++ port에 동일한 raw JPEG를 입력한다.
2. candidate bbox, rectification method, left/right feature vector, policy cohort, guard flag, final decision을 비교한다.
3. floating-point 차이는 사전에 정한 tolerance 안에서만 허용한다.
4. baseline OCR image path가 바뀌지 않았는지 확인한다.
5. `REVIEW`가 자동 `ABSENT` 또는 `NON_EV`로 변환되지 않는지 확인한다.
6. shadow mode에서 처리 시간, 오류, queue drop을 기록한다.

## 직접 런타임 스모크 테스트

전달 폴더 루트에서 실행한다. `-B`는 전달 패키지 안에 `__pycache__`를 만들지
않기 위한 옵션이다.

```powershell
cd C:\Users\5-13\Downloads\opencv\server_handoff\low_quality_presence_v1

..\..\vehicle_color_prescreen_v1\.venv\Scripts\python.exe -B runtime\build_runtime_template_cache.py `
  --positive-dir ..\..\vehicle_color_prescreen_v1\output\manual_rectified_icon_diagnostics_001\review_images `
  --positive-details ..\..\vehicle_color_prescreen_v1\output\manual_rectified_icon_diagnostics_001\details.csv `
  --negative-dir ..\..\vehicle_color_prescreen_v1\output\inferred_negative_label_review_001\images `
  --output model_bundle\runtime_template_cache.json

..\..\vehicle_color_prescreen_v1\.venv\Scripts\python.exe -B runtime\server_runtime.py `
  --image <input.jpg> `
  --output-dir <output-dir> `
  --source-mode rgb
```

검증된 로컬 예제는 `examples/runtime_smoke/result.json`이다. 현재 예제는
개발용 rectified EV 진단 이미지를 사용했으며, `REVIEW`와
`final_ev_classification=NOT_AVAILABLE`을 확인한다. `raw-a` 예제는
candidate가 없는 입력에서 `error=no_candidate`를 구조화해 반환하는 경계도
확인한다.

현재 `model_bundle/runtime_template_cache.json`은 수동 rectified EV 양성
자료와 inferred-negative 부트스트랩 자료로 만든 개발 캐시다. 실제 서버
활성화 전에는 서버 승인 template와 독립 RGB/IR human-GT holdout으로
재생성·검증해야 한다.

## 독립 성능 검증 전제

신규 paired RGB/IR source, human visibility label, physical presence label, plate quad, OCR GT를 독립 holdout 계약에 맞춰 수집한 뒤에만 Recall/FPR/평면화/OCR 수치를 확정한다.

현재 bundle만으로 서버 운영 성능을 주장하지 않는다.

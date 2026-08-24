# Direct Runtime CLI

## 출력

사진 한 장을 입력하면 다음을 생성한다.

- `result.json`
- `baseline_rectified.jpg`: OCR에 넘길 수 있도록 보존한 baseline 평면화 이미지
- `selected_rectified.jpg`: frozen selector가 선택한 candidate 평면화 이미지
- `candidate_rank_*.jpg`: 평가/디버깅용 top-3 후보 이미지

`result.json`의 `ev_prescreen_decision`은 `EV_CANDIDATE`, `NON_EV_CANDIDATE`, `REVIEW` 중 하나다. 이것은 최종 운영 EV/NON_EV 분류가 아니며, `final_ev_classification`은 의도적으로 `NOT_AVAILABLE`이다.

## 1회 실행

먼저 runtime template cache가 필요하다. 현재 bundle의 bootstrap cache는 개발용 자료에서 생성한다.

```powershell
cd C:\Users\5-13\Downloads\opencv\server_handoff\low_quality_presence_v1

..\..\vehicle_color_prescreen_v1\.venv\Scripts\python.exe runtime\build_runtime_template_cache.py `
  --positive-dir ..\..\vehicle_color_prescreen_v1\output\manual_rectified_icon_diagnostics_001\review_images `
  --positive-details ..\..\vehicle_color_prescreen_v1\output\manual_rectified_icon_diagnostics_001\details.csv `
  --negative-dir ..\..\vehicle_color_prescreen_v1\output\inferred_negative_label_review_001\images `
  --output model_bundle\runtime_template_cache.json

..\..\vehicle_color_prescreen_v1\.venv\Scripts\python.exe runtime\server_runtime.py `
  --image <input.jpg> `
  --output-dir <output-dir> `
  --source-mode rgb
```

서버팀이 자체 승인한 train template을 보유하면 bootstrap cache 대신 그 자료로 다시 생성한다. 현재 bootstrap negative source는 inferred negative이므로 독립 성능 증명이나 운영 승인을 의미하지 않는다.

## Pi_Server 연결

Pi_Server의 `BestShotReceiver` 또는 `EvidenceCaptureWorker`가 raw original JPEG 저장 직후 이 CLI와 동일한 core를 worker thread에서 호출한다. `ocr_input_path`를 기존 `OcrWorker` baseline 입력으로 사용하고, `selected_rectified_path`는 진단/후속 evidence로만 보존한다.


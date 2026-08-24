# 저화질 아이콘/번호판 존재 판단 서버 전달 패키지

## 목적

저화질 입력에서 화질 점수나 OCR 결과만으로 즉시 실패시키지 않고, 아이콘·번호판 후보의 존재 여부를 우선 판단한다. 관측 신뢰도가 낮으면 `REVIEW`로 보존해 자동 `ABSENT` 오판을 줄이는 것이 목적이다.

## 현재 전달 상태

- 상태: `development-only`, `shadow-mode-only`
- 독립 신규 RGB/IR human-GT holdout: 미확보
- 실제 운영 성능 claim: 금지
- 최종 EV/NON_EV 분류: 이 패키지의 책임 범위가 아님
- WiseAI IVA 점유 이벤트: 대체하지 않음
- OCR 입력: 기존 baseline 경로 유지
- 직접 실행부: `runtime/server_runtime.py` 추가
- 출력: `result.json` + baseline/selected 평면화 JPEG

현재 동결 후보는 개발 분할에서만 확인된 후보다. 따라서 서버에는 먼저 결과를 기록만 하는 shadow mode로 연결해야 한다.

## 폴더 구성

```text
low_quality_presence_v1/
├─ README_KO.md
├─ SERVER_INTEGRATION_SPEC.md
├─ RUNTIME_CONTRACT.md
├─ model_bundle/
│  ├─ selector_summary.json
│  ├─ side_model_summary.json
│  ├─ spatial_summary.json
│  ├─ guard_summary.json
│  ├─ default_thresholds.json
│  ├─ runtime_template_cache.json
│  └─ SOURCE_ARTIFACTS.md
├─ python_reference/
│  ├─ README.md
│  └─ ...
├─ runtime/
│  ├─ server_runtime.py
│  ├─ build_runtime_template_cache.py
│  └─ README.md
├─ examples/
│  ├─ presence_response.json
│  ├─ runtime_config.example.json
│  ├─ integration_test_cases.md
│  └─ runtime_smoke/
└─ verification/
   └─ README.md
```

## 서버팀에 전달할 핵심 문장

> 저화질 입력에서 아이콘/번호판 존재 판단을 우선하는 CV 로직을 Pi_Server에 shadow mode로 연동한다. 동일 raw JPEG를 입력으로 받아 `PRESENT/ABSENT/REVIEW`, raw quality, candidate, 평면화 상태, guard reason, 처리 시간을 기록한다. OCR은 기존 baseline crop 경로를 유지하고, 이 결과는 WiseAI 점유 판정이나 최종 EV/NON_EV를 대체하지 않는다. 신규 RGB/IR human-GT 독립 holdout 검증 전까지 운영 판정에는 반영하지 않는다.

## 권장 연결 위치

```text
Pi_Server BestShotReceiver / EvidenceCaptureWorker
  → raw JPEG 저장
  → CvPresenceWorker queue 등록
  → CV 결과 DB/JSON 기록
  → 기존 OcrWorker baseline 경로 실행
```

`REVIEW`는 `ABSENT`, `NON_EV`, 차량 미검출로 변환하지 않는다.

직접 runtime CLI는 `EV_CANDIDATE/NON_EV_CANDIDATE/REVIEW`와 OCR 입력용
`baseline_rectified.jpg`를 반환한다. `final_ev_classification`은 현재
`NOT_AVAILABLE`이며, 독립 holdout 검증 전에는 최종 EV/NON_EV로 승격하지 않는다.

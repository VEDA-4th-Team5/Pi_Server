# Pi_Server 연동 명세

## 1. 권장 구현 방식

Pi_Server는 C++ 서버이며 OpenCV4를 이미 사용하므로 native C++ `CvPresenceWorker`를 권장한다. Python 파일은 C++ 포트의 reference/oracle로만 사용한다.

Python HTTP sidecar는 초기 비교 시험에는 사용할 수 있지만, Raspberry Pi 운영 프로세스에 Python runtime·별도 서비스·IPC 장애를 추가하므로 운영 1차안으로 사용하지 않는다.

## 2. 연결 지점

현재 서버 흐름은 다음과 같다.

```text
BestShotReceiver
  → image download/save
  → parking session / IMAGE_LOG
  → OcrWorker
```

변경 후 흐름은 다음과 같다.

```text
BestShotReceiver 또는 EvidenceCaptureWorker
  → raw original JPEG 저장
  → CvPresenceWorker.enqueue(task)
  → CV 결과 저장 및 callback
  → OcrWorker는 기존 baseline image로 계속 실행
```

권장 소스 연결 대상:

- `Pi_Server/include/bestshot/BestShotReceiver.hpp`
- `Pi_Server/src/bestshot/BestShotReceiver.cpp`
- `Pi_Server/include/ocr/OcrWorker.hpp`
- `Pi_Server/src/ocr/OcrWorker.cpp`

WiseAI IVA의 `IvaEventResolver`, `IvaOccupancyCoordinator`, `ParkingTriggerCoordinator`를 이 로직으로 대체하지 않는다.

## 3. 요청 구조

```json
{
  "request_id": "session-123-image-01",
  "camera_id": "cam01",
  "channel_id": "ch01",
  "slot_id": "EV01",
  "source_mode": "rgb",
  "capture_stage": "bestshot",
  "image_path": "data/bestshots/vehicle/ch01_vehicle_001.jpg",
  "captured_at_utc": "2026-08-20T12:00:00Z",
  "roi_coordinate_system": "server_normalized_slot_roi"
}
```

동일 raw JPEG의 파일 경로를 넘기는 방식이 1차 구현에 적합하다. 원본 bytes를 넘기는 방식으로 변경하더라도 Python reference와 C++가 동일한 JPEG decode 결과를 사용해야 한다.

## 4. 응답 구조

```json
{
  "request_id": "session-123-image-01",
  "model_version": "low_quality_presence_v1_dev",
  "input_quality": 0.18,
  "quality_below_threshold": true,
  "plate_candidate_found": true,
  "icon_presence_predicted": false,
  "presence_decision": "REVIEW",
  "reason": "review_runtime_icon_observability_low_confidence",
  "rectification_success": true,
  "rectification_method": "perspective",
  "ev_prescreen_decision": "REVIEW",
  "final_ev_classification": "NOT_AVAILABLE",
  "baseline_rectified_path": "data/cv/session-123/baseline_rectified.jpg",
  "selected_rectified_path": "data/cv/session-123/selected_rectified.jpg",
  "ocr_input_path": "data/cv/session-123/baseline_rectified.jpg",
  "ocr_route": "baseline",
  "ocr_status": "not_run_by_runtime",
  "processing_ms": 42.3,
  "error": ""
}
```

## 5. 서버 동작 규칙

1. `source_mode=ir`이면 grayscale/IR 처리 규칙을 사용한다.
2. IR에서 사라진 아이콘 색상을 복원한다고 가정하지 않는다.
3. raw input quality `< 0.25`이면 low-quality policy를 선택한다.
4. 평면화 출력 quality로 low-quality cohort를 다시 분류하지 않는다.
5. candidate가 없으면 `REVIEW`와 `no_candidate`를 기록한다.
6. guard가 발동하면 자동 `ABSENT`를 `REVIEW`로 올릴 수 있다.
7. `REVIEW`는 운영자가 확인할 수 있도록 보존한다.
8. OCR은 selector candidate crop이 아니라 baseline crop을 계속 사용한다.
9. CV 실패가 전체 Pi_Server를 종료시키지 않도록 queue 작업 단위 오류로 처리한다.

## 6. 입력 이미지 주의사항

현재 reference는 raw input에서 candidate를 찾는 계약이다. Pi_Server의 BestShot이 전체 프레임인지 차량/번호판 object crop인지 먼저 고정해야 한다. 학습·평가 입력과 다른 crop 의미로 바로 적용하면 결과를 이식했다고 볼 수 없다.

특히 다음 두 경로는 별도 검증해야 한다.

- Camera Snapshot API의 full-frame original
- BestShotReceiver가 내려받은 vehicle/plate image

서버 포트 전에 같은 이미지 파일을 Python reference와 C++ 양쪽에 넣어 candidate bbox와 decision을 비교한다.

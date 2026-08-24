# Pi Server 통합 기록

## 원본

- 전달 파일: `low_quality_presence_v1.zip`
- 원본 SHA-256:
  `6926f9a7389460cc7a028c371862dbc57286d2692aee032558b6ccd0e9b4c01c`
- 최초 무결성 검사는 원본 ZIP을 푼 직후 `verification/SHA256SUMS.md`로
  완료했다.

## Pi Server 수정 사항

- Raspberry Pi의 Python 3.9에서 동작하도록 Python 3.10 전용
  `zip(..., strict=True)` 호출 두 곳을 기존 길이 검사와 일반 `zip()` 조합으로
  바꿨다.
- `runtime/pi_worker.py`를 추가해 C++ 서버와 JSON Lines로 통신한다.
- worker 시작 시 모델, threshold, template cache 계약을 검증한다.
- 검증된 모델과 디코딩된 template cache는 프로세스 수명 동안 재사용한다.
- OpenCV thread 수는 기본 1개이며 OpenCL은 사용하지 않는다.
- timeout, 자식 프로세스 종료, 잘못된 응답은 C++에서 `REVIEW`로 처리한다.

수정 후 파일 무결성은 현재 `verification/SHA256SUMS.md`를 기준으로 확인한다.

## 판정 제한

이 번들은 최종 EV 인증 모델이 아니다. 결과는 다음 사전 판별값이다.

```text
EV_CANDIDATE
NON_EV_CANDIDATE
REVIEW
```

Pi Server 기본 설정은 `ENTRANCE_EV_ANALYSIS_MODE=shadow`다. 따라서 판별 결과를
`ENTRANCE_RECOGNITION`에 감사 데이터로 저장하지만 등록 차량 원장
`VEHICLE.is_ev`를 자동 변경하지 않는다. 독립 실기기 검증 전에는 `enforced`
모드를 사용하지 않는다.

## Raspberry Pi 의존성

```bash
sudo apt install -y python3-opencv python3-numpy
```

별도 Python HTTP 서버나 가상환경은 필요하지 않다. C++ `EntranceEvWorker`가
필요할 때 자식 프로세스 한 개를 직접 관리한다.

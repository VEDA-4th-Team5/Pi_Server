# Qt 조회용 HTTP API

Nginx 없이 C++ `cpp-httplib` 서버가 SQLite 상태와 이미지 파일을 제공한다.

## 설정

- `HTTP_API_ENABLED`: 기본 `true`
- `HTTP_LISTEN_ADDRESS`: 기본 `0.0.0.0`
- `HTTP_PORT`: 기본 `8080`
- `HTTP_TLS_CERT_PATH`, `HTTP_TLS_KEY_PATH`: 둘 다 설정하면 직접 HTTPS
- `HTTP_DATA_ROOT`: 이미지 제공 허용 루트, 기본 `data`
- `HTTP_MAX_IMAGE_MB`: 이미지 최대 응답 크기, 기본 10 MB

## 엔드포인트

- `GET /api/v1/health`
- `GET /api/v1/parking-slots`
- `GET /api/v1/parking-slots/{slot_id}`
- `GET /api/v1/parking-sessions/active`
- `GET /api/v1/parking-sessions/{session_id}/images`
- `GET /api/v1/images/{image_id}/original`
- `GET /api/v1/images/{image_id}/enhanced`
- `GET /api/v1/settings/overstay-threshold`
- `PUT /api/v1/settings/overstay-threshold`

이미지 목록에는 내부 경로 대신 조회 URL만 노출한다. DB 경로가 존재하더라도
`HTTP_DATA_ROOT` 밖의 파일은 응답하지 않는다.

## 장기 점유 기준시간 설정

위반 판정과 `OVERSTAY_EVIDENCE` 촬영은 SQLite `SYSTEM_SETTINGS`의
`overstay_threshold_seconds` 한 값을 함께 사용한다. 기본값은 3600초이고 허용 범위는
60~86400초다. 설정 변경은 현재 활성 세션과 신규 세션 모두에 T0 기준으로 적용된다.

```http
GET /api/v1/settings/overstay-threshold
```

```json
{
  "thresholdSeconds": 3600,
  "thresholdMinutes": 60.0,
  "applyPolicy": "ACTIVE_AND_NEW_SESSIONS"
}
```

```http
PUT /api/v1/settings/overstay-threshold
Content-Type: application/json

{"thresholdSeconds":1800}
```

PUT은 정수만 허용하며 DB 저장에 성공한 뒤 메모리와 활성 예약을 갱신한다. DB 저장이
실패하면 기존 값이 유지된다. 초기 요청서 호환을 위해 `/api/settings/overstay-threshold`
별칭도 같은 동작을 제공한다.

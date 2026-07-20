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

이미지 목록에는 내부 경로 대신 조회 URL만 노출한다. DB 경로가 존재하더라도
`HTTP_DATA_ROOT` 밖의 파일은 응답하지 않는다.

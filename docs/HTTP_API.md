# Qt용 HTTPS/Bearer API

Nginx나 별도 인증 서버 없이 기존 C++ `ParkingHttpServer`가 HTTPS, 로그인,
SQLite 상태와 이미지 파일을 함께 제공한다.

## 설정

- `HTTP_API_ENABLED`: 기본 `true`
- `HTTP_LISTEN_ADDRESS`: 기본 `0.0.0.0`
- `HTTP_PORT`: 기본 `8080`; TLS 사용 여부와 포트 번호는 독립적이다.
- `HTTP_TLS_CERT_PATH`, `HTTP_TLS_KEY_PATH`: 서버 인증서와 private key
- `HTTP_REQUIRE_TLS`: 기본 `true`; non-loopback HTTP listener 시작 금지
- `HTTP_DATA_ROOT`: 이미지 제공 허용 루트, 기본 `data`
- `HTTP_MAX_IMAGE_MB`: 이미지 최대 응답 크기, 기본 10 MB
- `AUTH_SESSION_TTL_SECONDS`: 기본 36000, 허용 범위 28800~43200
- `AUTH_LOGIN_WINDOW_SECONDS`: 기본 300
- `AUTH_LOGIN_MAX_FAILURES`: 기본 5
- `AUTH_LOGIN_COOLDOWN_SECONDS`: 기본 60

인증서와 키 중 하나만 설정하거나, `HTTP_REQUIRE_TLS=true`인 상태에서
`0.0.0.0`에 평문 HTTP를 열려고 하면 서버는 시작에 실패한다. 인증서가 잘못된
경우에도 HTTP로 fallback하지 않는다. `127.0.0.1`과 `::1`은 통합 테스트용
평문 HTTP를 허용한다.

## 공개 API

다음 두 endpoint만 Bearer token 없이 호출할 수 있다.

```http
GET /api/v1/health
```

```json
{"success":true,"status":"ok"}
```

```http
POST /api/v1/auth/login
Content-Type: application/json

{"accountId":"operator","password":"user-entered-password"}
```

성공 응답은 `Cache-Control: no-store`를 포함한다.

```json
{
  "success": true,
  "accessToken": "<opaque-base64url-token>",
  "tokenType": "Bearer",
  "expiresAt": "2026-08-20T21:00:00Z",
  "user": {
    "id": 1,
    "accountId": "operator",
    "displayName": "Parking Operator"
  }
}
```

계정 없음, 비밀번호 오류, 비활성 계정은 모두 401과 동일한 오류를 반환한다.
5분 동안 같은 `(source IP, account ID)` 조합에서 5회 실패하면 이후 60초 동안
429와 `Retry-After`를 반환한다.

## Bearer 인증

health와 login을 제외한 모든 기존 API에는 다음 header가 필요하다.

```http
Authorization: Bearer <accessToken>
```

누락, malformed, 만료, 폐기, 비활성 사용자 token은 모두 다음 응답으로
통일한다.

```json
{"success":false,"error":"authentication required"}
```

access token은 32바이트 CSPRNG 값을 base64url without padding으로 인코딩한다.
원문 token은 로그인 응답에서 한 번만 반환하며 SQLite에는 SHA-256 digest만
저장한다. 세션은 absolute TTL이고 sliding expiration, refresh token,
remember-me는 사용하지 않는다.

```http
POST /api/v1/auth/logout
Authorization: Bearer <accessToken>
```

```json
{"success":true}
```

로그아웃은 현재 세션 하나만 폐기한다. 계정 비활성화와 비밀번호 재설정은 해당
사용자의 모든 활성 세션을 같은 transaction에서 폐기한다.

## 보호 API

- `GET /api/v1/parking-slots`
- `GET /api/v1/parking-slots/{slot_id}`
- `GET /api/v1/parking-sessions/active`
- `GET /api/v1/parking-sessions/{session_id}/images`
- `GET /api/v1/images/{image_id}/original`
- `GET /api/v1/images/{image_id}/enhanced`
- `GET`, `PUT /api/v1/settings/overstay-threshold`
- `GET /api/v1/settings/parking-slots/roi`
- `GET`, `PUT /api/v1/settings/parking-slots/{slot_id}/roi`
- `POST /api/v1/auth/logout`

기존 API의 성공 응답 JSON은 인증 추가 전과 동일하다. 이미지 목록은 같은 HTTPS
origin의 상대 URL을 반환하고, `HTTP_DATA_ROOT` 밖의 파일은 제공하지 않는다.
`overstay-threshold`는 `/api/settings/...`(버전 없는) 경로도 같은 핸들러로
등록돼 있어 구버전 클라이언트와 호환된다.

## 사용자 관리

초기 계정은 migration이나 seed에 포함하지 않는다. 서버와 같은 DB를 지정하고
TTY에서 비밀번호를 두 번 입력해 생성한다. 비밀번호는 4자 이상이어야 한다.

```bash
./cmake-build/app-user --db data/db/parking.db \
  add operator --display-name "Parking Operator"
./cmake-build/app-user --db data/db/parking.db list
./cmake-build/app-user --db data/db/parking.db disable operator
./cmake-build/app-user --db data/db/parking.db enable operator
./cmake-build/app-user --db data/db/parking.db reset-password operator
```

account ID는 trim 후 ASCII lowercase로 저장하며 형식은
`^[a-z0-9][a-z0-9._-]{2,63}$`이다. 비밀번호는 명령 인자로 받지 않으며 평문,
Argon2id PHC 문자열, token digest를 CLI나 서버 로그에 출력하지 않는다.

## SQLite

- `app_users`: account ID, libsodium Argon2id PHC 문자열, 표시 이름, 활성 상태
- `app_sessions`: 사용자 FK, SHA-256 token digest, 생성·만료·폐기 UTC epoch

기존 DB에 additive migration으로 생성되므로 주차·이미지·화재 데이터는 유지된다.
SQLite 연결은 `PRAGMA foreign_keys=ON`을 사용한다.

## TLS 인증서

서버 인증서 SAN에는 Qt에 입력할 실제 IP 또는 hostname이 있어야 한다. 예를 들어
Qt origin이 `https://pi-server.local:8443`이면 SAN에 `pi-server.local`을 넣는다.
주소와 포트는 소스가 아니라 환경 설정에서 관리하며, 장비별로 경로가 다르면
`.env.private`에서 공개 기본값을 덮어쓴다.

현재 시연 Pi에서 자체 서명 인증서를 생성하려면 저장소 루트에서 실행한다.

```bash
./tools/setup_tls.sh 172.20.32.97
```

기존 인증서를 의도적으로 재발급할 때만 두 번째 인자로 `--force`를 사용한다.
private key는 권한 `600`, 인증서 디렉터리는 `700`으로 생성되며 `data/tls/`는
Git 제외 대상이다.

```dotenv
HTTP_PORT=8080
HTTP_TLS_CERT_PATH=data/tls/server.crt
HTTP_TLS_KEY_PATH=data/tls/server.key
```

내부 CA를 사용할 경우 CA private key는 서버 저장소 밖에 보관하고 server key와
인증서만 Pi에 배포한다. Windows에서는 CA 인증서를 `로컬 컴퓨터 > 신뢰할 수 있는
루트 인증 기관`에 설치한다. Qt가 bundled OpenSSL backend를 사용하는 배포라면
Windows 저장소만으로 충분한지 확인하고, 필요하면 동일 CA 파일을 Qt 배포의
신뢰 CA 목록에 추가해야 한다. `sslErrors` 무시는 허용하지 않는다.

검증 예:

```bash
curl --cacert <ca.crt> https://<pi-host>:<port>/api/v1/health
```

로그인 응답 token은 shell history나 테스트 로그에 기록하지 않는다.

## 장기 점유 기준시간

`OVERSTAY_EVIDENCE`와 위반 판정은 SQLite `SYSTEM_SETTINGS`의
`overstay_threshold_seconds`를 함께 사용한다. 허용 범위는 60~86400초다.
인증 추가는 이 설정의 기존 GET/PUT 응답 계약을 변경하지 않는다.

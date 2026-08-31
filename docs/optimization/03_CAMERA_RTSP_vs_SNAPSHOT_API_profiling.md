# 카메라 영상 수신 구조 전환 — RTSP 상시 수신 vs Snapshot API 프로파일링

- 작성일: 2026-08-27
- 대상: Raspberry Pi 4 Model B (8GB) / aarch64 / kernel 6.18.39 / 4코어
- 빌드 커밋: `5236dbd` (`fix/optimization-pi_EVDA-241`) — **동일 바이너리, `.env`만 교체**
- 측정: Before(RTSP) 2026-08-27 18:10~18:22 / After(API) 2026-08-27 17:45~17:49

> **결론: RTSP 상시 수신은 프로세스 CPU 223.6%, Snapshot API는 3.5%. 64배 차이이며
> 절대 증분 220%p는 Pi 4 전체 4코어 중 2.2코어에 해당한다.**

---

## 1. 한눈에 보기

```mermaid
xychart-beta
    title "pi-server 프로세스 CPU (1코어=100%, Pi4는 400%가 상한)"
    x-axis ["RTSP 상시 수신 (Before)", "Snapshot API (After)"]
    y-axis "CPU %" 0 --> 400
    bar [223.56, 3.47]
```

| 지표 | **Before — RTSP on Pi** | **After — Snapshot API** | 차이 |
|---|---|---|---|
| **프로세스 CPU** | **223.56%** | **3.47%** | **64.4배 ↓** (−220%p ≈ 2.2코어) |
| ┗ user | 193.07% | 2.00% | 96배 ↓ |
| ┗ system | 30.50% | 1.47% | 21배 ↓ |
| CPU p50 / p90 / max | 236% / 327% / **361%** | 3% / 5% / 8% | — |
| **RSS** | **429 MB** (408~447) | **82.8 MB** | **5.2배 ↓** |
| 스레드 수 | **40** | 26 | +14 (디코더) |
| CPU 온도 | 54.5 °C | 40.9 °C | +13.6 °C |
| loadavg (1m) | **10.65** | 0.60 | 17.8배 ↓ |
| eth0 rx (시스템 전역) | 769 KB/s | 317 KB/s | +452 KB/s |
| IPC | 0.64 insn/cycle | 0.61 | 유사 |
| cache-miss | 1.79% | 2.34% | 둘 다 정상 |
| throttled | `0x0` | `0x0` | 스로틀 없음 |

**Pi 4의 남은 여유**: Before는 4코어 중 2.2코어를 상시 점유하고 순간 최대 3.6코어(361%)까지 치솟는다.
동일 Pi에서 EV 판정 워커(`pi_worker.py`), MQTT, HTTPS API, LoRa 수신이 함께 돌아야 하므로
**RTSP 방식은 헤드룸이 사실상 없다.** After는 3.5%로 96% 이상의 코어가 남는다.

---

## 2. 실험 설계

`b7dd265`(EVDA-192, 2026-08-10) 이후 두 방식은 **코드 분기가 아니라 런타임 토글**이다.
`src/main.cpp`:

```cpp
const bool rtsp_capture_enabled =
    !rtsp_channels.empty() &&
    (!config.camera_snapshot_api_enabled || config.camera_snapshot_api_rtsp_fallback);
...
if (rtsp_capture_enabled) rtsp_receiver.start();   // ← Before에서만 이 스레드가 뜬다
```

한 번만 빌드하고 `.env.public` 두 줄만 바꿔 각각 실행했다. 다른 코드 변화가 개입하지 않으므로
**CPU 차이 = RTSP 상시 디코딩 부하**로 귀속된다.

| | Arm A — Before (RTSP) | Arm B — After (API, 운영 기본값) |
|---|---|---|
| `CAMERA_SNAPSHOT_API_ENABLED` | `false` | `true` |
| `CAMERA_SNAPSHOT_API_RTSP_FALLBACK` | `true` | `false` |
| 기동 로그 | `RTSP opened: ch01` / `ch02` | `RTSP capture disabled: Camera Snapshot API is the image source` |
| `RtspStreamReceiver` | **기동** (채널당 스레드 1개) | 미기동 |

양쪽 공통 고정: 센서 입력, `CAPTURE_OFFSETS_SEC=30,60`, `PARKING_OCCUPANCY_SOURCE=HYBRID_OR`,
`ENTRANCE_ENABLED=true`, ONVIF IVA, 측정 창(≥120초)·도구.

### 2.1 실제 스트림 사양 (측정으로 확인)

```
$ ffprobe -rtsp_transport tcp "$CAMERA_RTSP_CH1"
codec_name=hevc          ← H.265
width=2592  height=1520  ← 약 3.9 MP
avg_frame_rate=30/1
```

**H.264 1080p가 아니라 HEVC 2592x1520 @ 30fps**다. Pi 4는 HEVC 하드웨어 디코더를
OpenCV/FFmpeg 경로에서 쓰지 않으므로 전량 소프트웨어 디코딩이며, 이것이 223%의 실체다.

`.env.private`에 `CAMERA_RTSP_CH1`과 `CAMERA_RTSP_CH2`가 **동일 URL**로 설정돼 있어
같은 스트림을 2회 디코딩한다 (§6.2 참고).

---

## 3. Before — RTSP 상시 수신

측정 창 131.6초 (`/proc` 1초 샘플 120개), `perf record` 60초 **50,402 샘플**.

### 3.1 CPU 시계열

```mermaid
xychart-beta
    title "Before — RTSP 상시 수신 CPU (10초 이동평균, %)"
    x-axis "경과(초)" [10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120]
    y-axis "CPU %" 0 --> 400
    line [264.2, 228.6, 236.8, 223.3, 263.2, 210.6, 246.0, 238.8, 241.2, 309.3, 271.8, 204.6]
```

이벤트와 무관하게 **상시 200~300%**를 소비한다. 유휴 상태에서도 30fps 프레임이 계속 들어오므로
부하가 내려가는 구간이 없다.

### 3.2 CPU 표본 분포 (120 표본)

```mermaid
xychart-beta
    title "Before — CPU 표본 분포"
    x-axis ["100-150", "150-200", "200-250", "250-300", "300-350", "350-400"]
    y-axis "표본 수" 0 --> 40
    bar [3, 29, 34, 28, 23, 3]
```

100% 미만 표본은 **0개**. 표본의 46%가 250% 이상이다.

### 3.3 라이브러리(DSO)별 CPU — 영상 처리가 전부

```mermaid
%%{init: {"themeVariables": {"pie1": "#ea4335", "pie2": "#9e9e9e", "pie3": "#fbbc04", "pie4": "#4285f4", "pie5": "#34a853"}}}%%
pie showData
    title "Before · DSO별 CPU (50,402 샘플)"
    "libavcodec (HEVC 디코딩)" : 46.17
    "libc (프레임 memcpy)" : 25.45
    "libswscale (색공간 변환)" : 17.77
    "libopencv_imgproc (리사이즈)" : 6.80
    "기타" : 3.81
```

| DSO | 상대 % | **절대 CPU** | 무엇 |
|---|---|---|---|
| `libavcodec` | **46.17%** | **103.2%** | HEVC 소프트웨어 디코딩 |
| `libc` | 25.45% | **56.9%** | 프레임 버퍼 `memcpy` (2592×1520×1.5B × 30fps × 2ch) |
| `libswscale` | **17.77%** | **39.7%** | YUV420 → BGR 색공간 변환 |
| `libopencv_imgproc` | 6.80% | 15.2% | full → preview(640×360) 리사이즈 |
| `libavformat` + `libavutil` + `libtbb` + `videoio` | 2.13% | 4.8% | RTP 디먹싱·스레드풀 |
| `libsqlite3` | 0.24% | **0.54%** | ← After(0.56%)와 **동일** |
| `libcrypto` | 0.19% | 0.42% | TLS |
| `pi-server` 자체 코드 | 0.07% | 0.16% | — |

**영상 파이프라인 합계 = 72.9% 상대 = 163%p 절대.** 나머지(DB·TLS·MQTT·센서)는 After와
사실상 동일한 값이 나와, 두 측정의 비영상 기저가 일치함을 교차검증한다.

### 3.4 스레드 구조 — 어디서 타는가

```
TID       NAME        %CPU    누적시간
1252897   pi-server   79.1    6:09    ← RtspStreamReceiver::captureLoop (ch01)
1252898   pi-server   69.2    6:10    ← RtspStreamReceiver::captureLoop (ch02)
1252924   av:hevc     25.4    1:27    ┐
1252925   av:hevc     24.9    1:27    │
1252935   av:hevc     20.4    1:25    │ FFmpeg HEVC frame-threading
1252936   av:hevc     18.9    1:25    │ 워커 8개 (채널당 4개)
1252926   av:hevc     18.9    1:27    │
1252927   av:hevc     18.4    1:26    │
1252937   av:hevc     17.9    1:25    │
1252934   av:hevc     17.9    1:27    ┘
```

- `captureLoop` 2개(채널당 1개)가 각 70~80%
- FFmpeg가 자동 생성한 `av:hevc` 디코더 워커 **8개**가 각 18~25%
- 스레드 총 40개 (After 26개 대비 **+14개**)

**채널 수에 선형 증가**한다. CH1만 쓰면 대략 절반, CH3까지 3채널이면 300%를 넘겨 Pi 4가 포화된다.

### 3.5 perf stat (20초)

```
    44,970,898,492  task-clock:u        #  2.246 CPUs utilized      → 224.6%
    67,638,736,456  cycles:u            #  1.504 GHz
    43,186,979,699  instructions:u      #  0.64  insn per cycle
    23,574,201,928  cache-references:u  #  524.210 M/sec
       423,098,040  cache-misses:u      #  1.79% of all cache refs
```

`cache-references` 524 M/sec는 After(126 M/sec)의 **4.2배** — 프레임 버퍼를 계속
흘려보내는 스트리밍 워크로드의 특징이다. miss 비율 자체(1.79%)는 낮은데, 순차 접근이라
프리페처가 잘 듣기 때문이며 **적중해도 메모리 대역폭은 그대로 소모된다.**

---

## 4. After — Snapshot API (현 운영)

측정 창 121.8초, `perf record` 90초 773 샘플. 로그: `RTSP capture disabled: Camera Snapshot API is the image source`.

### 4.1 CPU 시계열

```mermaid
xychart-beta
    title "After — Snapshot API CPU (10초 이동평균, %)"
    x-axis "경과(초)" [10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120]
    y-axis "CPU %" 0 --> 10
    line [3.60, 3.40, 3.40, 3.40, 3.40, 3.40, 3.60, 3.40, 3.10, 3.40, 4.60, 3.50]
```

평탄한 3%대. 110초의 4.6%는 스냅샷 API 요청/HTTP 처리 단발 스파이크.

### 4.2 DSO별 CPU

```mermaid
%%{init: {"themeVariables": {"pie1": "#4285f4", "pie2": "#9e9e9e", "pie3": "#ea4335", "pie4": "#fbbc04", "pie5": "#34a853"}}}%%
pie showData
    title "After · DSO별 CPU (773 샘플)"
    "libcrypto (TLS)" : 28.80
    "libc" : 21.28
    "libav* (BestShot 메타데이터)" : 23.02
    "libsqlite3" : 16.05
    "기타" : 10.85
```

| DSO | 상대 % | **절대 CPU** | 무엇 |
|---|---|---|---|
| `libcrypto` | 28.80% | 1.00% | HTTPS API 서버 TLS 핸드셰이크 (`EVP_DigestSignFinal` → RSA) |
| `libc` | 21.28% | 0.74% | malloc·문자열·poll |
| `libav*` | 23.02% | 0.80% | **ENTRANCE BestShot 메타데이터** — 전체 프레임 디코딩 아님 |
| `libsqlite3` | 16.05% | 0.56% | 폴링 쿼리 (EVDA-241 최적화 반영) |
| `pi-server` 자체 | 2.78% | 0.10% | — |

> After에도 `libav*`가 남는 이유는 입구(ENTRANCE) BestShot **메타데이터 스트림** 때문이다.
> 이것은 0.80%p에 불과하며, Before에서 추가되는 163%p의 전체 프레임 디코딩과는 규모가 다르다.

### 4.3 perf stat (20초)

```
       652,994,726  task-clock:u        #  0.033 CPUs utilized   → 3.27%
       342,055,998  instructions:u      #  0.61  insn per cycle
        82,458,550  cache-references:u  #  126.278 M/sec
         1,926,423  cache-misses:u      #  2.34% of all cache refs
```

---

## 5. 왜 이렇게 차이가 나는가

```mermaid
flowchart LR
    subgraph B["Before — RTSP 상시 수신 (223.6%)"]
        direction TB
        B1["카메라<br/>HEVC 2592x1520<br/>30fps × 2ch"] -->|"RTP 상시"| B2["captureLoop ×2<br/>cv::VideoCapture<br/><b>70~80% each</b>"]
        B2 --> B3["libavcodec<br/>HEVC SW 디코딩<br/><b>103%</b>"]
        B3 --> B4["libswscale<br/>YUV→BGR<br/><b>40%</b>"]
        B4 --> B5["imgproc resize<br/>preview 640x360<br/><b>15%</b>"]
        B5 --> B6["FrameBuffer<br/>(이벤트 때만 사용)"]
    end
    subgraph A["After — Snapshot API (3.5%)"]
        direction TB
        A1["이벤트 발생"] --> A2["POST /images/generate"]
        A2 --> A3["GET image_path<br/>JPEG 1장"]
        A3 --> A4["SnapshotStorage 저장"]
    end
    style B2 fill:#fce8e6,stroke:#ea4335,color:#000
    style B3 fill:#fce8e6,stroke:#ea4335,color:#000
    style B4 fill:#fce8e6,stroke:#ea4335,color:#000
    style B6 fill:#fef7e0,stroke:#fbbc04,color:#000
    style A3 fill:#e6f4ea,stroke:#34a853,color:#000
```

핵심은 **버려지는 일의 양**이다. Before는 초당 60프레임(30fps × 2ch)을 디코딩·색변환·리사이즈하지만,
실제로 쓰이는 건 이벤트 순간의 **1프레임**뿐이다. 나머지는 전부 폐기된다.
30초/60초 간격 촬영 기준으로 계산하면 **약 1,800프레임을 디코딩해 1장을 쓴다.**

After는 필요한 순간에 카메라에게 1장을 요청한다. 디코딩은 카메라 SoC(전용 하드웨어)가 수행하고,
Pi는 완성된 JPEG를 받기만 한다.

---

## 6. 측정 중 발견한 문제

### 6.1 RTSP 자격증명 escape 버그 (수정함)

Arm A 첫 기동이 `490 Account Blocked`로 전부 실패했다. 원인:

```bash
# .env.private (수정 전)
CAMERA_RTSP_CH1=rtsp://admin:5hanwha\!@172.20.32.1:554/1/profile2/media.smp
```

`tools/load_env_file.sh`는 **의도적으로 `source`를 쓰지 않고** 따옴표만 벗겨낸다
(CRLF 주입 방지). 따라서 `\!`의 백슬래시가 **값에 그대로 남아** 비밀번호가
`5hanwha\!`(9자)로 전달됐다. 정상값은 `5hanwha!`(8자).

- 반복 인증 실패 → 카메라가 RTSP 계정을 잠금(`490`)
- 같은 계정의 **HTTP Digest(:80)는 정상 200** — `CAMERA_API_PASSWORD`는 따옴표만 쓰고
  백슬래시가 없어 영향 없었음

**수정**: 파일 1행의 `CAMERA_RTSP`와 동일한 컨벤션(작은따옴표, 백슬래시 없음)으로 통일.

```bash
CAMERA_RTSP_CH1='rtsp://admin:5hanwha!@172.20.32.1:554/1/profile2/media.smp'
CAMERA_RTSP_CH2='rtsp://admin:5hanwha!@172.20.32.1:554/1/profile2/media.smp'
```

**부수 효과 — 운영 버그가 함께 해결됨**: 이 잘못된 URL은 `ENTRANCE_ENABLED=true`의
BestShot 메타데이터 경로에서도 쓰이고 있었고, 로그에 `BestShot metadata open failed`가
반복 기록되고 있었다. 수정 후 재기동 로그:

```
[INFO] BestShot metadata ready: ch02
[INFO] BestShot metadata ready: ch01
```

실패 0건. **입구 BestShot 메타데이터 수신이 이 수정으로 복구됐다.**

### 6.2 CH1·CH2가 동일 URL

`.env.private`의 `CAMERA_RTSP_CH1`과 `CAMERA_RTSP_CH2`가 같은 URL(`/1/profile2`)을 가리킨다.
RTSP 모드에서는 **같은 스트림을 2번 디코딩**하게 되어 부하가 2배가 된다.
API 모드에서는 `rtsp_channels`가 수신에 쓰이지 않아 드러나지 않던 문제다.
CH2는 입구 카메라의 별도 채널이어야 하므로 URL 확인이 필요하다.

### 6.3 측정 중 서비스 중단

Arm A 첫 기동 실패(§6.1)로 `initial FULL RTSP frame timeout` → pi-server 종료됐고,
복구 재기동 시 `run_server.sh`가 포그라운드로 동작해 명령 타임아웃에 함께 종료됐다.
**최종적으로 운영 설정(API 모드)으로 복구 완료** (PID 1258042, CPU 3.7%, 스레드 27).
이후 재기동은 `setsid nohup`으로 분리해 수행했다.

---

## 7. 측정 신뢰성과 한계

1. **부하 조건**: 양쪽 모두 차량 진출입이 드문 유휴~저부하. RTSP 상시 디코딩은 유휴에서
   발생하는 비용이므로 이 조건이 타당하다. 실부하 시 OCR·스냅샷 요청 비중은 달라진다.
2. **창 간 편차**: Before는 `/proc` 131초 창 223.56%, `perf stat` 20초 창 224.6%,
   `perf record` 60초 창 281%(50,402 ÷ (299×60))로 측정 창에 따라 220~280% 범위다.
   **본문 대표값은 가장 긴 `/proc` 131초 창(223.56%)**을 쓴다.
3. **After 저표본**: perf record 773 샘플. DSO 백분율에 표본오차가 있으나
   `PROFILING_before_after.md` §9.5(549 샘플)와 집계가 일치해 형상은 신뢰 가능.
   Before는 50,402 샘플로 충분하다.
4. **네트워크 카운터가 전역**: §1의 eth0 값은 Pi 전체(pi_worker.py, SSH 포함)이며
   pi-server 단독이 아니다. 프로세스 단위 비교는 `nethogs -t` 재측정 필요.
5. **채널 수 의존**: Before는 2채널 기준이다. 실제 운영이 CH1·CH2·CH3라면 부하는 더 커진다.
   §6.2의 URL 중복 문제도 함께 정리한 뒤 재측정하는 것이 정확하다.
6. **심볼 스트립**: `libavcodec`/`libcrypto`는 심볼이 없어 개별 함수명 대신
   DSO 집계와 스레드 이름(`av:hevc`)으로 귀속을 판정했다.
7. `perf_event_paranoid=2` → user 공간만 카운트. sys 시간은 `/proc` 기준으로 별도 반영.

---

## 8. 결론

| | Before — RTSP on Pi | After — Snapshot API |
|---|---|---|
| **CPU** | **223.6%** (2.2코어 상시, 최대 3.6코어) | **3.5%** |
| **RSS** | 429 MB | 82.8 MB |
| **스레드** | 40 (captureLoop 2 + av:hevc 8) | 26 |
| **온도** | 54.5 °C | 40.9 °C |
| **병목** | libavcodec 103% + libswscale 40% + memcpy 57% | TLS 1.0% / BestShot 메타 0.8% / SQLite 0.6% |
| **한 일 대비 쓸모** | 초당 60프레임 디코딩 → 이벤트당 1장 사용 | 이벤트당 1장 요청 → 1장 사용 |
| **Pi 4 헤드룸** | 사실상 없음 | 96% 이상 여유 |

전환(EVDA-192, `fb2b0fb`)의 타당성이 수치로 확인된다. **CPU 64배, 메모리 5.2배 절감**이며,
절감분 2.2코어는 현재 EV 판정 워커·OCR·HTTPS API가 사용하고 있다.

추가로 §6.1의 자격증명 버그를 수정해 **입구 BestShot 메타데이터 수신이 복구**됐고,
§6.2의 CH1·CH2 URL 중복은 후속 확인이 필요하다.

---

## 9. 재현 방법

```bash
# ── Arm 전환 (.env.public) ──
# Before: CAMERA_SNAPSHOT_API_ENABLED=false / CAMERA_SNAPSHOT_API_RTSP_FALLBACK=true
# After : CAMERA_SNAPSHOT_API_ENABLED=true  / CAMERA_SNAPSHOT_API_RTSP_FALLBACK=false
setsid nohup ./run_server.sh restart > /tmp/restart.log 2>&1 < /dev/null &   # 포그라운드 종료 방지
sleep 90                                                                     # 안정화

PID=$(pgrep -x pi-server | head -1); HZ=$(getconf CLK_TCK)

# 120초 CPU/RSS 시계열
pc=$(awk '{print $14+$15}' /proc/$PID/stat)
for i in $(seq 1 120); do sleep 1
  pn=$(awk '{print $14+$15}' /proc/$PID/stat)
  m=$(awk '/^VmRSS/{r=$2}/^Threads/{t=$2}END{print r,t}' /proc/$PID/status)
  awk -v d=$((pn-pc)) -v hz=$HZ -v i=$i -v m="$m" 'BEGIN{printf "%d %.2f %s\n",i,d*100/hz,m}'
  pc=$pn
done | tee cpu_ts.csv

perf stat -p $PID -e task-clock,cycles,instructions,cache-references,cache-misses --timeout 20000
perf record -p $PID -g -F 299 -o perf.data -- sleep 60      # ← `-- sleep`로 정상 종료(timeout 금지)
perf report -i perf.data --stdio -g none --no-children --sort dso

top -H -b -n 3 -d 2 -p $PID | sort -k9 -rn | head -12       # 스레드별 CPU
ffprobe -rtsp_transport tcp "$CAMERA_RTSP_CH1"              # 스트림 사양
```

> `timeout 90 perf record`는 SIGTERM으로 헤더를 못 써 `data size field is 0`으로 깨진다.
> 반드시 `-- sleep N` 형태로 실행할 것.

원자료: [`docs/optimization/data/`](./data/)

| 파일 | 내용 |
|---|---|
| `cpu_ts_before_rtsp.csv` / `cpu_ts_after.csv` | 1초 CPU·RSS·스레드 시계열 |
| `window_summary_before_rtsp.txt` / `window_summary_after.txt` | 창 집계(CPU/IO/net/ctxsw) |
| `perf_stat_before_rtsp.txt` / `perf_stat_after.txt` | 하드웨어 카운터 |
| `perf_dso_before_rtsp.txt` / `perf_dso_after.txt` | DSO별 CPU |
| `threads_hot_before_rtsp.txt` / `threads_after.txt` | 스레드별 CPU |

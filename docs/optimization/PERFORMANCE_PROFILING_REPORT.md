# Pi Server 성능 프로파일링 보고서

- 작성일: 2026-08-24
- 대상 바이너리: `cmake-build/pi-server` (Release, `-O3`, not stripped)
- 대상 환경: Raspberry Pi 실기(`aarch64`, kernel `6.18.39+rpt-rpi-v8`)
- 도구: `perf record`/`perf report` (실행 중인 프로세스에 attach), `sqlite3` CLI (`EXPLAIN QUERY PLAN`)
- 작성자: shin75992@gmail.com

## 1. 배경 및 목적

최적화 작업에 앞서 실제 실행 중인 `pi-server`의 CPU 시간이 어디에 쓰이는지 측정한다. VS Code에는 C/C++용 내장 CPU 프로파일러가 없어(디버깅만 지원), 외부 도구(`perf`)로 측정 후 결과를 텍스트로 확인하는 방식을 사용했다.

## 2. 방법

재컴파일이나 재시작 없이, 이미 떠 있는 `pi-server` 프로세스에 `perf`로 attach하여 샘플링했다.

```bash
# 1. 실행 중인 프로세스에 30초간 999Hz로 샘플링 attach
perf record -F 999 -p <PID> -g -o perf.data -- sleep 30

# 2. 함수별 자기실행시간(flat profile)
perf report -i perf.data --stdio -n --sort=overhead,symbol

# 3. 공유 라이브러리(DSO)별 시간 분포
perf report -i perf.data --stdio -n --sort=overhead,dso

# 4. 호출 트리(call graph) - 인터랙티브 TUI
perf report -i perf.data
```

- `perf_event_paranoid=2`였지만 프로세스 소유자 본인이라 attach는 문제없이 동작.
- 30초 측정 동안 999Hz 기준 2,322개 샘플 수집(이론상 최대 29,970개 대비 약 7.7% CPU 사용률 — 서버는 대부분 유휴 상태에서 폴링 중).
- 결과가 의심스러운 지점(SQLite 함수 비중)은 `sqlite3` CLI로 실제 DB(`data/db/parking.db`)에 `EXPLAIN QUERY PLAN`을 돌려 교차검증.

### VS Code에서 결과 보기

`perf.data`는 바이너리라 VS Code로 직접 열 수 없다. `perf report --stdio` 출력을 텍스트 파일로 저장해 VS Code 에디터로 열어보는 방식을 사용했다. 그래픽 콜그래프가 필요하면 `hotspot`(GUI) 또는 `gprof2dot` + Graphviz로 변환 후 이미지 미리보기도 가능(이번 환경엔 미설치).

## 3. 측정 결과

### 3.1 라이브러리(DSO)별 CPU 시간 분포

| 비중 | 대상 |
|---|---|
| 87.60% | `libsqlite3.so.0.8.6` |
| 7.75% | `libc.so.6` |
| 3.67% | `libcrypto.so.3` |
| 0.39% | **`pi-server` 자체 코드** |
| 0.34% | `libstdc++.so.6.0.33` |
| 0.05% | `libcpp-httplib.so.0.18.7` |
| 0.04% | `libmosquitto.so.2.0.21` |

애플리케이션 코드(`parking::`, `database::` 네임스페이스)의 자기실행시간은 개별 함수 기준 0.05% 이하 수준으로, CPU 시간 대부분이 SQLite 내부에서 소모되고 있다.

### 3.2 Flat Profile — 자기실행시간 상위 함수

| 비중 | 샘플 수 | 함수 |
|---|---|---|
| 15.75% | 327 | `sqlite3BtreeTableMoveto` |
| 12.65% | 280 | `sqlite3VdbeExec` |
| 3.31% | 74 | `sqlite3VdbeOneByteSerialTypeLen` |
| 2.55% | 51 | `sqlite3VdbeRecordCompareWithSkip` |
| 2.12% | 47 | `sqlite3Parser` |
| 1.93% | 40 | `sqlite3VdbeMemFromBtreeZeroOffset` |
| 1.57% | 35 | `sqlite3GetVarint` |
| 1.13% | 23 | `sqlite3PcacheFetchFinish` |
| 1.09% | 23 | `sqlite3BtreePayloadSize` |

### 3.3 Call Graph — 핫스팟 호출 경로

```
parking::SlotTransitionActor::run()          (기본 50ms 주기 폴링 루프)
 └─ processEffects(now_epoch_ms)
     └─ database::SessionTransitionStore::listPendingEffects(now_epoch_ms)
         └─ database::EventDatabase::listPendingSlotTransitionEffects(now_epoch_ms)
             └─ sqlite3_step → sqlite3VdbeExec → sqlite3BtreeTableMoveto  (self 15.75%)
```

`SlotTransitionActor::run()`은 진행 상황(`progressed`)이 없을 때만 `retryDelay`(기본 **50ms**)만큼 대기하고, 매 반복마다 `processIngress` → `processRunnable`(루프) → `processDueDeadlines` → `processEffects` 순으로 여러 SQL 쿼리를 실행한다. 이 중 `listPendingSlotTransitionEffects`가 가장 비싼 쿼리로 확인됐다.

## 4. 근본 원인 분석

`listPendingSlotTransitionEffects`가 실행하는 쿼리:

```sql
SELECT ... FROM OCCUPANCY_COMMAND_INBOX c
WHERE c.status='APPLIED' AND c.effect_state='PENDING'
  AND c.effect_next_attempt_at_epoch_ms<=?
  AND NOT EXISTS (
    SELECT 1 FROM OCCUPANCY_COMMAND_INBOX earlier
    WHERE earlier.slot_id=c.slot_id
      AND earlier.status='APPLIED' AND earlier.effect_state='PENDING'
      AND earlier.admission_ordinal<c.admission_ordinal
  )
ORDER BY c.admission_ordinal;
```

`OCCUPANCY_COMMAND_INBOX`(현재 2,161행)에는 이 쿼리에 정확히 맞는 인덱스가 이미 존재한다.

```sql
CREATE INDEX idx_occupancy_effect_pending
    ON OCCUPANCY_COMMAND_INBOX(effect_state, slot_id, admission_ordinal, effect_next_attempt_at_epoch_ms);
```

그런데 `EXPLAIN QUERY PLAN` 결과, 쿼리 플래너는 이 인덱스 대신 `idx_occupancy_inbox_runnable`(`status` 기준)을 선택하고 있었다.

```
QUERY PLAN
|--SEARCH c USING INDEX idx_occupancy_inbox_runnable (status=?)
|--CORRELATED SCALAR SUBQUERY 1
|  `--SEARCH earlier USING INDEX idx_occupancy_inbox_runnable (status=? AND slot_id=? AND admission_ordinal<?)
`--USE TEMP B-TREE FOR ORDER BY
```

이 경우 `status='APPLIED'`인 전체 행(2,161개)을 스캔한 뒤 `effect_state='PENDING'` 조건으로 걸러내야 하므로, 측정 시점처럼 `effect_state='PENDING'`인 행이 0개(`APPLIED/APPLIED` 54건, `APPLIED/NONE` 2,107건)여도 매번 풀스캔 비용(실측 **약 7ms/회**)이 든다.

**원인**: DB에 `ANALYZE`가 한 번도 실행되지 않아 `sqlite_stat1` 테이블 자체가 없었다. 통계 정보가 없으니 플래너가 두 인덱스 중 더 적합한 쪽을 판단하지 못하고 `status` 인덱스를 선택한 것이다. 코드 전체를 검색해도 `ANALYZE`나 `PRAGMA optimize`를 호출하는 곳이 없다.

검증을 위해 `data/db/parking.db`에 직접 `ANALYZE OCCUPANCY_COMMAND_INBOX;`를 실행한 결과, 플래너가 즉시 올바른 인덱스로 전환됐다.

```
QUERY PLAN
|--SEARCH c USING INDEX idx_occupancy_effect_pending (effect_state=?)
|--CORRELATED SCALAR SUBQUERY 1
|  `--SEARCH earlier USING INDEX idx_occupancy_effect_pending (effect_state=? AND slot_id=? AND admission_ordinal<?)
`--USE TEMP B-TREE FOR ORDER BY
```

동일 쿼리 실행 시간: **약 7ms → 2ms 미만**으로 감소.

> ⚠️ 본 조사 과정에서 실 운영 DB(`data/db/parking.db`)에 `ANALYZE`를 실행했다. 데이터 행은 전혀 변경되지 않고 통계 테이블(`sqlite_stat1`)만 생성/갱신됐다.

## 5. 영향

이 쿼리는 `SlotTransitionActor::run()`의 매 반복(진행 상황이 없어도 최소 50ms 주기)마다 호출되므로, 서버가 완전히 유휴 상태여도 이 풀스캔 비용이 상시 누적된다. 30초 측정 구간에서 SQLite가 전체 CPU 시간의 87.6%를 차지한 주된 원인으로 판단된다.

## 6. 권장 조치

1. **DB 연결 오픈 시점(또는 스키마 마이그레이션 직후)에 `PRAGMA optimize;` 실행 추가.** 재시작 없이도 최신 통계를 유지할 수 있어 가장 간단하고 안전한 수정.
2. 대안으로 마이그레이션/시드 스크립트(`db/schema.sql`, `db/seed.sql`) 실행 후 `ANALYZE;`를 명시적으로 호출.
3. 조치 후 동일한 `perf record`/`EXPLAIN QUERY PLAN` 절차로 재측정하여 `libsqlite3.so` 비중 및 `sqlite3BtreeTableMoveto` 자기실행시간이 감소했는지 확인 권장.

## 7. 한계

- 이번 측정은 실 트래픽(센서 이벤트) 없이 서버가 폴링만 하는 상태를 반영한다. 실제 부하가 걸리면 `effect_sink` 처리(웹훅/알림 등)나 `processIngress` 경로 비중이 달라질 수 있다.
- `perf`는 x86이 아닌 Pi 실기(aarch64)에서 직접 측정했으므로 배포 환경과 동일한 수치다(별도 환경 보정 불필요).
- 인덱스 선택 문제는 구조적 결함이라 부하 유무와 무관하게 항상 재현된다.

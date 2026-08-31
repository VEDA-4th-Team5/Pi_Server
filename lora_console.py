#!/usr/bin/env python3
"""STM1 <-> Pi LoRa 콘솔 (개발/시연용).

수신: 프레임 디코딩 + CRC 검증 + 페이로드 해석 (v1.0 / v1.1 동시 지원)
송신: ALERT 명령을 고정점 헤더 + 프레임으로 인코딩해 전송
종료: Ctrl-C 또는 q -> 최종 송수신 리포트

사용:
    python3 lora_console.py
"""

import os
import sys
import time
import threading

# ---------------------------------------------------------------- 설정

DEV        = "/dev/serial0"
BAUD       = 115200

STM1_ADDR  = (0x00, 0x01)   # 고정점 목적지
CHANNEL    = 0x1E           # 채널 30 = 922.9 MHz

FRAME_VER  = 0x02           # v1.1
TYPE_SENSOR = 0x01
TYPE_ALERT  = 0x02
TYPE_HB     = 0x03

SOF = b"\xAA\x55"

# ---------------------------------------------------------------- 색

class C:
    R = "\033[0m"
    DIM = "\033[2m"
    B = "\033[1m"
    RED = "\033[31m"
    GRN = "\033[32m"
    YEL = "\033[33m"
    BLU = "\033[34m"
    MAG = "\033[35m"
    CYN = "\033[36m"
    GRY = "\033[90m"


def crc16(data):
    """CRC-16/CCITT-FALSE. init 0xFFFF, poly 0x1021, 반전/최종XOR 없음."""
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def build_frame(msg_type, seq, payload, version=FRAME_VER):
    p = payload.encode() if isinstance(payload, str) else payload
    body = bytes([version, msg_type]) + seq.to_bytes(4, "big") \
        + len(p).to_bytes(2, "big") + p
    return SOF + body + crc16(body).to_bytes(2, "big")


# ---------------------------------------------------------------- 상태

class Stats:
    def __init__(self):
        self.t0 = time.time()
        self.rx_bytes = 0
        self.rx_frames = 0
        self.rx_crc_bad = 0
        self.rx_junk = 0            # 프레임으로 인식 못 한 바이트
        self.tx_frames = 0
        self.tx_bytes = 0
        self.streams = {}           # node -> {first,last,lost,frames}
        self.hall = {}              # (node,sensor) -> state
        self.flame = {}             # (node,sensor) -> (state, energy)
        self.ver_seen = set()

    def note_rx(self, node, seq):
        s = self.streams.setdefault(node, {"first": None, "last": None,
                                           "lost": 0, "frames": 0, "resets": 0})
        s["frames"] += 1
        if s["last"] is not None:
            if seq > s["last"]:
                s["lost"] += seq - s["last"] - 1
            elif s["last"] - seq > 1000:
                # 규격: seq 가 크게 감소 = 노드 재부팅
                s["resets"] += 1
                s["first"] = seq
        if s["first"] is None:
            s["first"] = seq
        s["last"] = seq


stats = Stats()
plock = threading.Lock()
running = True


def out(msg):
    with plock:
        sys.stdout.write("\r\033[K" + msg + "\n")
        sys.stdout.write(PROMPT)
        sys.stdout.flush()


PROMPT = C.GRY + "lora> " + C.R


# ---------------------------------------------------------------- 페이로드 해석

def render_payload(text):
    """페이로드를 사람이 읽기 좋게. v1.0/v1.1 둘 다 받는다."""
    f = text.split(":")

    if f[0] == "SENSOR":
        # v1.1: SENSOR:STM1:HALL01:OCCUPIED:142
        # v1.0: SENSOR:HALL01:OCCUPIED:142
        if len(f) >= 5:
            node, sensor, state, seq = f[1], f[2], f[3], f[4]
        else:
            node, sensor, state, seq = "-", f[1], f[2], f[3]
        stats.hall[(node, sensor)] = state
        col = C.YEL if state == "OCCUPIED" else C.GRN
        return node, seq, "%-6s %s%-9s%s %s" % (sensor, col, state, C.R,
                                                C.GRY + "#" + seq + C.R)

    if f[0] == "FIRE":
        # v1.1: FIRE:STM1:FLAME01:DETECTED:144:43.70
        # v1.0: FIRE:FLAME01:DETECTED:12
        energy = None
        if len(f) >= 5:
            node, sensor, state, seq = f[1], f[2], f[3], f[4]
            if len(f) >= 6:
                energy = f[5]
        else:
            node, sensor, state, seq = "-", f[1], f[2], f[3]
        stats.flame[(node, sensor)] = (state, energy)
        col = C.RED + C.B if state == "DETECTED" else C.GRY
        e = ("  energy=" + C.MAG + energy + C.R) if energy is not None else ""
        return node, seq, "%-6s %s%-9s%s %s%s" % (sensor, col, state, C.R,
                                                  C.GRY + "#" + seq + C.R, e)

    return "-", None, C.GRY + text + C.R


# ---------------------------------------------------------------- 수신 스레드

def reader(fd):
    buf = b""
    while running:
        try:
            d = os.read(fd, 256)
        except BlockingIOError:
            time.sleep(0.005)
            continue
        if not d:
            continue
        stats.rx_bytes += len(d)
        buf += d

        while True:
            i = buf.find(SOF)
            if i < 0:
                if len(buf) > 1:
                    stats.rx_junk += len(buf) - 1
                    buf = buf[-1:]
                break
            if i > 0:
                stats.rx_junk += i
                buf = buf[i:]
            if len(buf) < 12:
                break

            ver, mtype = buf[2], buf[3]
            plen = int.from_bytes(buf[8:10], "big")
            if ver not in (0x01, 0x02) or mtype not in (1, 2, 3) \
               or plen == 0 or plen > 200:
                stats.rx_junk += 2
                buf = buf[2:]
                continue

            total = 12 + plen
            if len(buf) < total:
                break

            f = buf[:total]
            buf = buf[total:]
            seq = int.from_bytes(f[4:8], "big")
            want = int.from_bytes(f[-2:], "big")
            got = crc16(f[2:-2])
            stats.ver_seen.add(ver)

            if want != got:
                stats.rx_crc_bad += 1
                out("%s◀ CRC BAD%s  seq=%d  계산 %04X != 수신 %04X"
                    % (C.RED, C.R, seq, got, want))
                continue

            stats.rx_frames += 1
            text = f[10:-2].decode("utf-8", errors="replace")
            node, _, rendered = render_payload(text)
            stats.note_rx(node, seq)
            out("%s◀%s %s%-4s%s v%d  %s"
                % (C.CYN, C.R, C.B, node, C.R, ver, rendered))

    return


# ---------------------------------------------------------------- 송신

tx_seq = 0


def send_alert(fd, sensor, on):
    global tx_seq
    tx_seq += 1
    payload = "ALERT:%s:LED:%s:%d" % (sensor, "ON" if on else "OFF", tx_seq)
    frame = build_frame(TYPE_ALERT, tx_seq, payload)
    pkt = bytes([STM1_ADDR[0], STM1_ADDR[1], CHANNEL]) + frame
    os.write(fd, pkt)
    stats.tx_frames += 1
    stats.tx_bytes += len(pkt)
    out("%s▶%s %s  %s(frame %dB, +3B 목적지)%s"
        % (C.YEL, C.R, payload, C.GRY, len(frame), C.R))


def send_raw(fd, text):
    global tx_seq
    tx_seq += 1
    frame = build_frame(TYPE_ALERT, tx_seq, text)
    pkt = bytes([STM1_ADDR[0], STM1_ADDR[1], CHANNEL]) + frame
    os.write(fd, pkt)
    stats.tx_frames += 1
    stats.tx_bytes += len(pkt)
    out("%s▶%s %s  %s(raw)%s" % (C.YEL, C.R, text, C.GRY, C.R))


# ---------------------------------------------------------------- 리포트

def report():
    dur = time.time() - stats.t0
    W = 66
    print("\n" + C.B + "═" * W)
    print(" LoRa 세션 리포트")
    print("═" * W + C.R)
    print("  기간          %.1f 초" % dur)
    vers = ", ".join("v%d" % v for v in sorted(stats.ver_seen)) or "-"
    print("  프레임 버전   %s" % vers)

    print("\n" + C.B + "── 수신 (STM → Pi)" + C.R)
    print("  총 바이트     %d" % stats.rx_bytes)
    print("  정상 프레임   %d" % stats.rx_frames)
    bad = C.RED if stats.rx_crc_bad else C.GRN
    print("  CRC 실패      %s%d%s" % (bad, stats.rx_crc_bad, C.R))
    junk = C.YEL if stats.rx_junk else C.GRN
    print("  버려진 바이트 %s%d%s  (프레임 밖 잡음)" % (junk, stats.rx_junk, C.R))

    if stats.streams:
        print("\n  " + C.DIM + "노드    프레임   seq 범위        유실        재시작" + C.R)
        for node, s in sorted(stats.streams.items()):
            span = (s["last"] - s["first"] + 1) if s["first"] is not None else 0
            rate = 100.0 * s["lost"] / span if span else 0.0
            lc = C.GRN if s["lost"] == 0 else C.RED
            print("  %-6s  %5d   %6d~%-6d  %s%4d (%4.1f%%)%s  %d"
                  % (node, s["frames"], s["first"], s["last"],
                     lc, s["lost"], rate, C.R, s["resets"]))
    else:
        print("  " + C.RED + "(수신 프레임 없음)" + C.R)

    print("\n" + C.B + "── 송신 (Pi → STM)" + C.R)
    print("  프레임        %d" % stats.tx_frames)
    print("  바이트        %d" % stats.tx_bytes)

    if stats.hall or stats.flame:
        print("\n" + C.B + "── 마지막 상태" + C.R)
        for (node, sensor), st in sorted(stats.hall.items()):
            col = C.YEL if st == "OCCUPIED" else C.GRN
            print("  %-4s %-8s %s%s%s" % (node, sensor, col, st, C.R))
        for (node, sensor), (st, en) in sorted(stats.flame.items()):
            col = C.RED + C.B if st == "DETECTED" else C.GRY
            e = ("  energy=" + en) if en else ""
            print("  %-4s %-8s %s%s%s%s" % (node, sensor, col, st, C.R, e))

    # duty 참고치 (프레임 13.5ms 가정, 법정 한도 2%)
    if dur > 0:
        occupancy = stats.rx_frames * 13.5 / (dur * 1000.0) * 100.0
        col = C.GRN if occupancy < 1.0 else (C.YEL if occupancy < 2.0 else C.RED)
        print("\n" + C.B + "── 전파 점유(추정)" + C.R)
        print("  STM 송신 duty %s%.3f%%%s  (법정 한도 2%%)" % (col, occupancy, C.R))

    print(C.B + "═" * W + C.R)


# ---------------------------------------------------------------- 메인

HELP = """
  {b}명령{r}
    1 2 3 4        해당 슬롯 LED 토글 (HALL01~04)
    on 2 / off 2   명시적으로 켜기/끄기
    raw <문자열>   임의 문자열을 프레임으로 전송
    s              현재까지 통계 요약
    h              이 도움말
    q              종료 (Ctrl-C 도 동일)
""".format(b=C.B, r=C.R)


def main():
    global running

    os.system("stty -F %s %d raw -echo" % (DEV, BAUD))
    fd = os.open(DEV, os.O_RDWR | os.O_NONBLOCK)

    print(C.B + "LoRa 콘솔" + C.R + "  %s @ %d  목적지 STM1(0x%02X%02X) ch%d"
          % (DEV, BAUD, STM1_ADDR[0], STM1_ADDR[1], CHANNEL))
    print(HELP)

    t = threading.Thread(target=reader, args=(fd,), daemon=True)
    t.start()

    led = {1: False, 2: False, 3: False, 4: False}

    try:
        while True:
            try:
                line = input(PROMPT).strip()
            except EOFError:
                break
            if not line:
                continue

            parts = line.split(None, 1)
            cmd = parts[0].lower()
            arg = parts[1] if len(parts) > 1 else ""

            if cmd in ("q", "quit", "exit"):
                break
            elif cmd == "h":
                print(HELP)
            elif cmd == "s":
                report()
            elif cmd in ("1", "2", "3", "4"):
                n = int(cmd)
                led[n] = not led[n]
                send_alert(fd, "HALL%02d" % n, led[n])
            elif cmd in ("on", "off") and arg.strip().isdigit():
                n = int(arg.strip())
                if 1 <= n <= 4:
                    led[n] = (cmd == "on")
                    send_alert(fd, "HALL%02d" % n, led[n])
                else:
                    print("  슬롯은 1~4")
            elif cmd == "raw" and arg:
                send_raw(fd, arg)
            else:
                print("  알 수 없는 명령. h 로 도움말")
    except KeyboardInterrupt:
        pass
    finally:
        running = False
        time.sleep(0.05)
        os.close(fd)
        report()


if __name__ == "__main__":
    main()

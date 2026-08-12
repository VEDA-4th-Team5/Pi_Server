# pi-server systemd 서비스

`run_server.sh`를 systemd 유닛으로 감싸서 재부팅 자동 기동/크래시 자동 재시작을 제공합니다.

## 설치

```bash
sudo cp deploy/pi-server.service /etc/systemd/system/pi-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now pi-server
```

## 운영

```bash
sudo systemctl status pi-server      # 상태 확인
sudo systemctl restart pi-server     # 재시작 (Jenkins 배포 단계에서 사용)
sudo systemctl stop pi-server        # 정지
journalctl -u pi-server -f           # 실시간 로그 (tmux 세션 생존 여부와 무관)
journalctl -u pi-server -n 200       # 최근 로그
```

파일 로그(`data/logs/pi-server.log`)는 `run_server.sh`가 그대로 남기므로 기존 로그 수집/백업 방식과 호환됩니다.

## 동작 방식

- `ExecStart`는 인자 없이 `run_server.sh`를 실행합니다. 프로세스 생존 여부 체크나 `restart` 인자 처리는 수동 실행용이고, systemd가 시작/정지/재시작을 직접 관리하므로 필요 없습니다.
- 정지 시 systemd가 유닛 cgroup 전체에 `SIGTERM`을 보내므로 `pi-server` 바이너리가 직접 신호를 받아 (`main.cpp`의 시그널 핸들러) 정상 종료됩니다. RTSP 정리에 최대 30초가량 걸릴 수 있어 `TimeoutStopSec=60`으로 여유를 뒀습니다.
- `Restart=on-failure`이므로 `pi-server`가 비정상 종료하면 (스크립트의 `set -o pipefail` 덕분에 파이프라인 종료 코드가 제대로 전파됨) 5초 후 자동 재시작하며, 그 과정에서 `run_server.sh`가 증분 빌드를 다시 수행합니다.

## Jenkins 연동

배포 파이프라인의 deploy 단계는 다음 한 줄이면 됩니다.

```bash
sudo systemctl restart pi-server
```

Jenkins 실행 계정이 비밀번호 없이 이 명령만 쓸 수 있도록 `/etc/sudoers.d/`에 최소 권한만 추가하는 걸 권장합니다 (예: `visudo -f /etc/sudoers.d/jenkins-pi-server`).

```
jenkins ALL=(root) NOPASSWD: /usr/bin/systemctl restart pi-server, /usr/bin/systemctl status pi-server
```

이 sudoers 설정은 시스템 권한 변경이라 직접 적용하지 않았습니다. 필요하면 알려주세요.

#!/usr/bin/env bash
# 이 Raspberry Pi를 Jenkins SSH build agent로 쓸 수 있게 최초 1회 세팅한다.
# 새 팀원이 자기 Pi를 agent로 등록하기 전에, 그 Pi에서 직접 실행해야 한다.
#
# 사용법:
#   chmod +x tools/setup_jenkins_agent.sh
#   ./tools/setup_jenkins_agent.sh
#
# 반복 실행해도 안전하다(이미 되어있는 항목은 건너뜀).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CURRENT_USER="$(whoami)"
SERVICE_NAME="pi-server"

# Jenkins 컨트롤러(Windows PC, Docker) 전용 공개키. 컨트롤러가 바뀌면 이 값만 교체.
CONTROLLER_PUBKEY="ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINQMnL5i+SypT5lYBwzJ/CA4wr5A0jcuvCtmPEVk1Dnw jenkins-controller@windows-pc"

echo "== 1. Java 설치 확인 (agent의 remoting.jar 실행에 필요) =="
if ! command -v java >/dev/null 2>&1; then
    sudo apt update
    sudo apt install -y openjdk-21-jre-headless
else
    echo "이미 설치됨: $(java -version 2>&1 | head -1)"
fi

echo "== 2. Jenkins 컨트롤러 SSH 공개키 등록 =="
mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
touch "$HOME/.ssh/authorized_keys"
if ! grep -qF "$CONTROLLER_PUBKEY" "$HOME/.ssh/authorized_keys"; then
    echo "$CONTROLLER_PUBKEY" >> "$HOME/.ssh/authorized_keys"
    echo "공개키 추가됨"
else
    echo "이미 등록되어 있음"
fi
chmod 600 "$HOME/.ssh/authorized_keys"

echo "== 3. sudoers NOPASSWD 규칙 등록 (systemctl restart/status ${SERVICE_NAME}) =="
SUDOERS_FILE="/etc/sudoers.d/jenkins-${SERVICE_NAME}"
SUDOERS_LINE="${CURRENT_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl restart ${SERVICE_NAME}, /usr/bin/systemctl status ${SERVICE_NAME}"
if ! sudo test -f "$SUDOERS_FILE" || ! sudo grep -qF "$SUDOERS_LINE" "$SUDOERS_FILE"; then
    echo "$SUDOERS_LINE" | sudo tee "$SUDOERS_FILE" >/dev/null
    sudo chmod 440 "$SUDOERS_FILE"
    if ! sudo visudo -c -f "$SUDOERS_FILE" >/dev/null 2>&1; then
        echo "오류: sudoers 문법 검증 실패, 롤백합니다" >&2
        sudo rm -f "$SUDOERS_FILE"
        exit 1
    fi
    echo "등록됨: $SUDOERS_FILE"
else
    echo "이미 등록되어 있음"
fi

echo "== 4. systemd 서비스 유닛 등록 (WorkingDirectory=${REPO_DIR}) =="
UNIT_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
if [[ ! -f "$UNIT_FILE" ]]; then
    sudo tee "$UNIT_FILE" >/dev/null <<EOF
[Unit]
Description=Pi Server (parking/fire/IVA main service)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${CURRENT_USER}
Group=${CURRENT_USER}
WorkingDirectory=${REPO_DIR}
ExecStart=${REPO_DIR}/run_server.sh
Restart=on-failure
RestartSec=5
KillSignal=SIGTERM
TimeoutStopSec=60

[Install]
WantedBy=multi-user.target
EOF
    sudo systemctl daemon-reload
    sudo systemctl enable "${SERVICE_NAME}"
    echo "서비스 유닛 생성 및 활성화 완료: $UNIT_FILE"
else
    echo "이미 존재함: $UNIT_FILE (건드리지 않음 — 다른 경로를 쓰고 있을 수 있으니 직접 확인)"
fi

echo "== 5. Jenkins agent 작업 디렉토리 준비 =="
mkdir -p "$HOME/jenkins-agent"

echo
echo "완료. Jenkins 관리자에게 아래 정보를 전달해서 agent 노드로 등록해달라고 하세요:"
echo "  - Host: $(hostname -I | awk '{print $1}')"
echo "  - User: ${CURRENT_USER}"
echo "  - Remote FS root: $HOME/jenkins-agent"
echo "  - Label: pi-native (또는 원하는 라벨)"

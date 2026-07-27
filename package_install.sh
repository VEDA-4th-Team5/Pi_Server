#!/bin/bash
# Pi_Server 빌드 환경 셋업 스크립트 (Debian/Ubuntu 계열: Raspberry Pi OS, WSL Ubuntu 등)
#
# 사용법:
#   chmod +x package_install.sh
#   ./package_install.sh
#
# 설치 후 빌드:
#   cmake -B build && make -C build -j4
# 테스트:
#   ctest --test-dir build
set -e

sudo apt update

# 빌드 도구
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config

# 라이브러리 의존성 (CMakeLists.txt 기준)
sudo apt install -y \
    libopencv-dev \
    libavformat-dev libavcodec-dev libavutil-dev \
    libcurl4-openssl-dev \
    libcpp-httplib-dev \
    libmosquitto-dev \
    libsqlite3-dev \
    nlohmann-json3-dev

# MQTT 브로커 (로컬에서 fire-pipeline-tool 등 실행/테스트용)
sudo apt install -y \
    mosquitto \
    mosquitto-clients

echo
echo "== 설치 완료. 다음 명령으로 빌드하세요:"
echo "   cmake -B build && make -C build -j4"

# AGENTS.md

## Project Rule

이 프로젝트는 Hanwha Vision CCTV 기반 스마트 주차/관제 시스템의 Raspberry Pi 서버 파트이다.

## Hard Constraints

- Python은 절대 사용하지 않는다.
- 모든 서버 코드는 C 또는 C++로 작성한다.
- Raspberry Pi 서버는 C++ 기반으로 구현한다.
- STM32 펌웨어는 C 기반으로 구현한다.
- Qt 클라이언트는 C++/Qt 기반으로 구현한다.
- Python 예제, Python 스크립트, Python 기반 테스트 코드는 제안하지 않는다.
- OpenSDK는 초기 MVP에서 사용하지 않는다.
- 카메라 내부 앱 개발이 아니라 외부 Raspberry Pi 서버 기반 구조로 진행한다.
- 카메라 영상은 RTSP로 수신한다.
- 카메라 이벤트는 MQTT로 수신한다.
- MQTT Broker는 Raspberry Pi의 Mosquitto를 사용한다.
- 데이터 저장은 SQLite를 사용한다.
- 영상 처리 및 프레임 처리는 OpenCV C++ API를 사용한다.
- GUI 없는 Raspberry Pi 서버 환경을 고려하여 `cv::imshow()`는 기본 사용하지 않는다.
- 서버는 headless 환경에서 동작해야 한다.

## Current Camera / Network Context

- Camera model: Hanwha Vision PNO-A9081R
- Camera IP: 172.20.35.76
- RTSP profile candidate:
  - `rtsp://<USER>:<PASSWORD>@172.20.35.76:554/profile2/media.smp`
  - `rtsp://<USER>:<PASSWORD>@172.20.35.76:554/profile1/media.smp`
- 비밀번호는 코드나 Git 저장소에 직접 커밋하지 않는다.
- 카메라 계정 정보는 환경변수 또는 별도 로컬 설정 파일로 관리한다.

## Confirmed

- RTSP 스트림 연결은 성공했다.
- MQTT 클라이언트 설정을 통해 카메라에서 Raspberry Pi Mosquitto로 이벤트 발행이 성공했다.
- 수신된 MQTT topic 예시:
  - `E4:30:22:EB:D7:F9/onvif-ej/VideoSource/MotionAlarm/&VideoSourceToken-0`
  - `E4:30:22:EB:D7:F9/onvif-ej/VideoAnalytics/tnssamsung:MotionDetection/&VideoSourceToken-0`
  - `E4:30:22:EB:D7:F9/onvif-ej/Device/tns1:Trigger/tnssamsung:DigitalInput/&0/1`

## Target Architecture

Hanwha Camera  
→ MQTT event publish  
→ Raspberry Pi Mosquitto  
→ C++ pi-server subscribe  
→ SQLite log 저장  
→ Snapshot / 3~5초 clip 저장  
→ Qt 관제 클라이언트로 재발행  
→ STM32 LED/Buzzer/지자기센서 연동

## Development Direction

1. MQTT 단독 수신 테스트
2. RTSP frame 수신 테스트
3. headless snapshot 저장
4. MQTT 이벤트 발생 시 최신 frame snapshot 저장
5. 최근 3~5초 영상 clip 저장
6. SQLite 이벤트 로그 저장
7. Qt 클라이언트로 정리된 topic 재발행
8. STM32 UART 또는 MQTT 연동

## Do Not Suggest

- Python 코드
- Flask/FastAPI 서버
- Python OpenCV 테스트
- Jupyter Notebook
- OpenSDK `.cap` 앱 개발을 초기 구현으로 제안
- GUI 필수 구조

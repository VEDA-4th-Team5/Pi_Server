#!/usr/bin/env bash
[ -f ./.env.fire.local ] || ./tools/fire_setup.sh
# 로그 켜고 끄기 설정. 없으면 템플릿에서 만들어 둔다(값은 전부 주석 설명 포함).
[ -f ./.env.log.local ] || [ ! -f ./.env.log.example ] || cp ./.env.log.example ./.env.log.local

set -a
source ./.env.camera.local
source ./.env.fire.local
[ -f ./.env.gemini.local ] && source ./.env.gemini.local
[ -f ./.env.iva.local ] && source ./.env.iva.local
[ -f ./.env.log.local ] && source ./.env.log.local
set +a

exec ./build/pi-server

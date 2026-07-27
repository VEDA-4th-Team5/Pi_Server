#!/usr/bin/env bash
set -a
source ./.env.camera.local
source ./.env.fire.local
[ -f ./.env.gemini.local ] && source ./.env.gemini.local
[ -f ./.env.iva.local ] && source ./.env.iva.local
set +a

exec ./build/pi-server

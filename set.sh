#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

[[ -f ./.env.fire.local ]] || ./tools/fire_setup.sh

exec "$ROOT_DIR/run_server.sh" "$@"

#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

set -a
[[ -f ./.env.public ]] && source ./.env.public
[[ -f ./.env.private ]] && source ./.env.private
set +a

if [[ ! -f ./cmake-build/CMakeCache.txt ]]; then
    cmake -S . -B cmake-build
fi

cmake --build cmake-build --target check_coordinates -j4

exec ./cmake-build/check_coordinates \
    --web \
    --port 8091

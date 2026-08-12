#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

channel_number="${1:-1}"
if [[ ! "$channel_number" =~ ^[1-4]$ ]]; then
    echo "Usage: ./capture_frame.sh <1|2|3|4> [output.jpg]" >&2
    exit 1
fi

set -a
[[ -f ./.env.public ]] && source ./.env.public
[[ -f ./.env.private ]] && source ./.env.private
set +a

required=(
    CAMERA_OPEN_API_BASE
    CAMERA_IMAGE_BASE
    CAMERA_IMAGE_SERVER_PORT
    CAMERA_API_USERNAME
    CAMERA_API_PASSWORD
)
for key in "${required[@]}"; do
    if [[ -z "${!key:-}" ]]; then
        echo "error: $key is not configured" >&2
        exit 1
    fi
done

api_channel=$((channel_number - 1))
timestamp="$(date +%Y%m%d_%H%M%S)"
output_path="${2:-data/fullframes/ch${channel_number}_${timestamp}.jpg}"
mkdir -p "$(dirname "$output_path")"

start_status="$(
    curl -sS --digest \
        -u "${CAMERA_API_USERNAME}:${CAMERA_API_PASSWORD}" \
        --connect-timeout 3 --max-time 10 \
        -o /dev/null -w '%{http_code}' \
        -X POST "${CAMERA_OPEN_API_BASE}/startserver" \
        -H 'Content-Type: application/json' \
        -d "{\"port\":${CAMERA_IMAGE_SERVER_PORT}}"
)"
if [[ "$start_status" != 200 && "$start_status" != 202 ]]; then
    echo "error: camera image server start failed (HTTP $start_status)" >&2
    exit 1
fi

response="$(
    curl -sS --digest \
        -u "${CAMERA_API_USERNAME}:${CAMERA_API_PASSWORD}" \
        --connect-timeout 3 --max-time 30 \
        -X POST "${CAMERA_OPEN_API_BASE}/images/generate" \
        -H 'Content-Type: application/json' \
        -d "{\"channel\":${api_channel},\"outputs\":[{\"id\":\"original\",\"type\":\"original\"}]}"
)"

image_path="$(
    printf '%s' "$response" \
        | grep -oE '"image_path"[[:space:]]*:[[:space:]]*"[^"]+"' \
        | head -1 \
        | sed -E 's/^.*"([^"]+)"$/\1/'
)"
if [[ -z "$image_path" ]]; then
    echo "error: camera response did not contain image_path" >&2
    exit 1
fi

curl -fsS \
    --connect-timeout 3 --max-time 15 \
    "${CAMERA_IMAGE_BASE}${image_path}" \
    -o "$output_path"

if [[ ! -s "$output_path" ]]; then
    echo "error: downloaded image is empty" >&2
    exit 1
fi

echo "saved: $output_path"

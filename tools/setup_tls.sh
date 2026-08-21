#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TLS_DIR="$ROOT_DIR/data/tls"
PRIMARY_IP="${1:-}"
FORCE="${2:-}"

if [[ -z "$PRIMARY_IP" ]]; then
    PRIMARY_IP="$(hostname -I | tr ' ' '\n' | grep -m1 -E '^[0-9]+(\.[0-9]+){3}$' || true)"
fi

if [[ ! "$PRIMARY_IP" =~ ^[0-9]+(\.[0-9]+){3}$ ]]; then
    echo "Usage: $0 <pi-ipv4> [--force]" >&2
    exit 2
fi

CERT_PATH="$TLS_DIR/server.crt"
KEY_PATH="$TLS_DIR/server.key"

if [[ ( -e "$CERT_PATH" || -e "$KEY_PATH" ) && "$FORCE" != "--force" ]]; then
    echo "TLS certificate already exists. Use --force to replace it." >&2
    exit 1
fi

command -v openssl >/dev/null 2>&1 || {
    echo "openssl is required" >&2
    exit 1
}

mkdir -p "$TLS_DIR"
chmod 700 "$TLS_DIR"
umask 077

HOST_NAME="$(hostname | tr -cd 'A-Za-z0-9.-')"
SAN="IP:$PRIMARY_IP,IP:127.0.0.1,DNS:localhost"
if [[ -n "$HOST_NAME" ]]; then
    SAN="$SAN,DNS:$HOST_NAME"
fi

openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 365 \
    -keyout "$KEY_PATH" \
    -out "$CERT_PATH" \
    -subj "/CN=$PRIMARY_IP/O=VEDA Smart Parking Demo" \
    -addext "subjectAltName=$SAN" \
    -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
    -addext "extendedKeyUsage=serverAuth" >/dev/null 2>&1

chmod 600 "$KEY_PATH"
chmod 644 "$CERT_PATH"

openssl x509 -in "$CERT_PATH" -noout -checkend 86400 >/dev/null

echo "TLS certificate created: $CERT_PATH"
echo "TLS private key created: $KEY_PATH (mode 600)"
echo "The default .env.public paths already point to these generated files."
echo "Qt origin: https://$PRIMARY_IP:8080"
echo "Trust server.crt in Qt/Windows; do not disable certificate validation."

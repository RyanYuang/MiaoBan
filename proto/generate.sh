#!/usr/bin/env bash
# Generate nanopb C sources from proto/oye/device/v1/device.proto
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "${ROOT}/main/ble/generated"
PROTO="${ROOT}/proto/oye/device/v1/device.proto"
OUT="${ROOT}/main/ble/generated"
OPTS="${ROOT}/proto/oye/device/v1/device.options"

mkdir -p "${OUT}"

if command -v nanopb_generator >/dev/null 2>&1; then
  nanopb_generator -I "${ROOT}/proto" -D "${OUT}" "${PROTO}" -f "${OPTS}"
elif python3 -m nanopb_generator -h >/dev/null 2>&1; then
  python3 -m nanopb_generator -I "${ROOT}/proto" -D "${OUT}" "${PROTO}" -f "${OPTS}"
else
  echo "Install nanopb: pip install nanopb" >&2
  exit 1
fi

echo "Generated nanopb files in ${OUT}"

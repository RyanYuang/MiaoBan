#!/usr/bin/env bash
# Apply MiaoBan-local patches to 78/esp-ml307 after IDF Component Manager download.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPONENT_DIR="${ROOT}/managed_components/78__esp-ml307"
PATCH_FILE="${ROOT}/patches/78__esp-ml307/voice-chat-tcp-websocket.patch"

if [[ ! -d "${COMPONENT_DIR}" ]]; then
    exit 0
fi

if [[ ! -f "${PATCH_FILE}" ]]; then
    echo "apply_esp_ml307_patches: missing ${PATCH_FILE}" >&2
    exit 1
fi

cd "${COMPONENT_DIR}"

# The dependency is freshly downloaded into managed_components/, so the safest
# idempotence check is to look for the features introduced by our local patch.
if grep -q "void ShutdownTransport();" include/web_socket.h \
    && grep -q "Reply Pong:" src/web_socket.cc \
    && grep -q "kTcpReceiveTaskStackBytes" src/esp/esp_tcp.cc; then
    exit 0
fi

if patch --forward -p0 --dry-run -s < "${PATCH_FILE}" 2>/dev/null; then
    patch --forward -p0 < "${PATCH_FILE}"
    echo "apply_esp_ml307_patches: applied voice-chat-tcp-websocket.patch"
    exit 0
fi

echo "apply_esp_ml307_patches: patch does not apply cleanly (esp-ml307 version changed?)" >&2
exit 1

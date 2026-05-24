# Component patches

Patches for dependencies downloaded into `managed_components/` (gitignored).

## 78/esp-ml307 (~3.6.5, commit `ab4de7c`)

| File | Purpose |
|------|---------|
| `78__esp-ml307/voice-chat-tcp-websocket.patch` | Non-blocking TCP, WebSocket handshake/Pong fixes for voice-chat WS |

Applied automatically on `idf.py build` / `reconfigure` via `scripts/apply_esp_ml307_patches.sh`.

To apply manually after `idf.py reconfigure`:

```bash
./scripts/apply_esp_ml307_patches.sh
```

To refresh the patch after editing files under `managed_components/78__esp-ml307/`:

```bash
# From a clean upstream checkout at the pinned commit (see component idf_component.yml repository_info)
diff -Naur upstream/src/esp/esp_tcp.cc managed_components/78__esp-ml307/src/esp/esp_tcp.cc
# ... repeat for src/web_socket.cc and include/web_socket.h, paths relative to component root
```

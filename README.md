# PowerMesh — Off-Grid Mesh Rescue Node

ESP32-S3 + SA618 433 MHz radio node. A phone with no internet connects to the node's WiFi hotspot and sends SOS / GPS / chat / compressed photo messages over an RF mesh.

**Live Wokwi simulation:** https://wokwi.com/projects/474510804154140673

## Quick demo (Wokwi free tier, 2 min)

1. https://wokwi.com → **New Project → ESP32-S3**
2. Paste `diagram.json` (Diagram → JSON tab) and `sketch.ino`
3. Install libs from `libraries.txt`, hit ▶
4. Serial monitor: `PowerMesh v2 online`, `SD mounted`, softAP `PowerMesh_Rescue` at `192.168.4.1`
5. Move battery pot → OLED voltage/bar animate
6. Press **SOS** button → `[RF TX]` in serial, message logged to SD

## Important files

| File | Purpose |
|------|---------|
| `sketch.ino` | Main Arduino firmware (captive portal, OLED, SD, RF mesh) |
| `diagram.json` | Wokwi circuit (single node, free tier) |
| `wokwi.toml` | Wokwi CLI config (port forward `localhost:9080` → node `:80`) |
| `libraries.txt` | Arduino libraries to install |
| `run-cli.sh` | Build + simulate with Wokwi CLI (no VS Code) |
| `prod.py` | Python server mirror — run locally to test without hardware |
| `PINOUT.md` | ESP32-S3 pin connections |
| `BOM.csv` | Hardware bill of materials (~₹4,150/prototype) |

## How it works

- Node creates hotspot `PowerMesh_Rescue` (192.168.4.1) with captive portal
- Portal: send SOS / chat / image, with GPS
- Messages over Serial1 to SA618 433 MHz radio: **CRC16 + TTL 3-hop mesh + ACK + dedup**
- Every TX/RX appended to SD `/queue.log` (store-and-forward)
- OLED shows battery %, voltage, RSSI, TX/RX/forward counts

## Security

Your Wokwi CI token must never be committed or pasted in chat/issues.
- `.env` is gitignored — keep the token there only
- Rotate any token pasted in chat: https://wokwi.com/dashboard/tokens
- Add as GitHub secret `WOKWI_CLI_TOKEN` for CI

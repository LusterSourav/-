#!/usr/bin/env bash
# PowerMesh — No VS Code needed. Pure CLI (Wokwi CLI + Arduino CLI)
# Works on macOS / Linux / GitHub Codespaces. Requires Node 20+ and arduino-cli.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

# 1) Check token (NEVER hardcode — use env var)
if [ -z "$WOKWI_CLI_TOKEN" ]; then
  if [ -f "$HERE/.env" ]; then
    echo "[i] Loading .env"
    set -a; source "$HERE/.env"; set +a
  fi
fi
if [ -z "$WOKWI_CLI_TOKEN" ]; then
  echo "ERROR: WOKWI_CLI_TOKEN not set."
  echo "  export WOKWI_CLI_TOKEN=wok_xxx   # from https://wokwi.com/dashboard/tokens"
  echo "  or: echo 'WOKWI_CLI_TOKEN=wok_xxx' > $HERE/.env"
  exit 1
fi
echo "[✓] WOKWI_CLI_TOKEN present (${#WOKWI_CLI_TOKEN} chars) — not printing value"

# 2) Install tools if missing (macOS brew fallback)
need_install=false
command -v arduino-cli >/dev/null 2>&1 || need_install=true
command -v npx >/dev/null 2>&1 || { echo "Need Node 20+ (https://nodejs.org)"; exit 1; }

if $need_install; then
  echo "[i] Installing arduino-cli..."
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
  export PATH="$PWD/bin:$PATH"
fi

# 3) Install ESP32 core + libs (once)
if [ ! -d "$HOME/.arduino15/packages/esp32" ]; then
  echo "[i] Installing ESP32 core..."
  arduino-cli config init || true
  arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json || true
  arduino-cli core update-index
  arduino-cli core install esp32:esp32
fi
echo "[i] Installing libs..."
arduino-cli lib install "Adafruit GFX Library" "Adafruit SSD1306" "ArduinoJson" 2>&1 | tail -5 || true
arduino-cli lib install "ESP Async WebServer" 2>&1 | tail -5 || true
arduino-cli lib install "AsyncTCP" 2>&1 | tail -5 || true

# 4) Compile
echo "[i] Compiling sketch.ino for ESP32-S3..."
mkdir -p "$HERE/build"
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=16M,PSRAM=enabled "$HERE/sketch.ino" --build-path "$HERE/build" || \
arduino-cli compile --fqbn esp32:esp32:esp32s3 "$HERE" --build-path "$HERE/build"
ls -lh "$HERE/build"/*.bin "$HERE/build"/*.elf 2>&1 | head -20

# 5) Run Wokwi headless with private gateway (no VS Code)
echo ""
echo "[i] Starting Wokwi sim..."
echo "    Portal will be at http://localhost:9080  (from wokwi.toml [[net.forward]])"
echo "    Press Ctrl+C to stop"
echo ""
# wokwi.toml already has [[net.forward]] from=localhost:9080 to=target:80
# wokwi-cli is installed from https://github.com/wokwi/wokwi-cli/releases (brew/npm not needed)
if ! command -v wokwi-cli >/dev/null 2>&1; then
  echo "ERROR: wokwi-cli not found in PATH."
  echo "  Install: curl -L https://wokwi.com/ci/install.sh | sh"
  exit 1
fi
# Run sim (uses diagram.json + build/*.elf via wokwi.toml)
wokwi-cli . --timeout 0 2>&1 | cat

#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${SOFLE_SYNC_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/sofle-pico-sync}"
BASE_URL="${SOFLE_SYNC_BASE_URL:-https://raw.githubusercontent.com/IanBurwell/qmk_firmware/ib-sofle-pico/keyboards/sofle_pico/keymaps/default/host_sync}"
BASE_URL="${BASE_URL%/}"
SKIP_UDEV="${SOFLE_SYNC_SKIP_UDEV:-0}"
SKIP_SYSTEMD="${SOFLE_SYNC_SKIP_SYSTEMD:-0}"
OS_NAME="$(uname -s)"

TMP_DIR="$(mktemp -d)"
SCRIPT_PATH="${BASH_SOURCE[0]:-}"
cleanup() {
    rm -rf "$TMP_DIR"
    if [[ "$SCRIPT_PATH" == "/tmp/s.sh" ]]; then
        rm -f /tmp/s.sh
    fi
}
trap cleanup EXIT

if ! command -v python3 >/dev/null 2>&1; then
    echo "[sofle-sync] ERROR: python3 is required (Python 3.9+)." >&2
    exit 1
fi

PYTHON_BIN="$(command -v python3)"
if ! "$PYTHON_BIN" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)'; then
    echo "[sofle-sync] ERROR: Python 3.9+ is required (found $("$PYTHON_BIN" --version 2>&1))." >&2
    exit 1
fi

fetch_file() {
    local url="$1"
    local out="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -H "Cache-Control: no-cache" "$url" -o "$out"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$out" --header="Cache-Control: no-cache" "$url"
    else
        "$PYTHON_BIN" -c 'import sys, urllib.request; req = urllib.request.Request(sys.argv[1], headers={"Cache-Control": "no-cache"}); data = urllib.request.urlopen(req, timeout=15).read(); open(sys.argv[2], "wb").write(data)' "$url" "$out"
    fi
}

sha256_file() {
    local path="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$path" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$path" | awk '{print $1}'
    else
        "$PYTHON_BIN" -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$path"
    fi
}

echo "[sofle-sync] Installing/updating Sofle Pico sync service in $INSTALL_DIR"
echo "[sofle-sync] Using Python: $PYTHON_BIN"

mkdir -p "$INSTALL_DIR"

FILES=(
    "sofle_sync.py"
    "sofle_flash.py"
    "99-sofle-pico-hidraw.rules"
    "sofle-pico-sync.service"
    "README.md"
    "ha_config.example.json"
    "audio_config.example.json"
)

ANY_CHANGED=0
CACHE_BUST="$(date +%s)"
for file in "${FILES[@]}"; do
    tmp_file="$TMP_DIR/$file"
    dest_file="$INSTALL_DIR/$file"
    fetch_file "${BASE_URL}/${file}?cb=${CACHE_BUST}" "$tmp_file"
    new_hash="$(sha256_file "$tmp_file")"
    if [[ ! -f "$dest_file" ]]; then
        mv "$tmp_file" "$dest_file"
        echo "  [new]     $file"
        ANY_CHANGED=1
    else
        old_hash="$(sha256_file "$dest_file")"
        if [[ "$old_hash" != "$new_hash" ]]; then
            cp "$tmp_file" "$dest_file"
            rm -f "$tmp_file"
            echo "  [updated] $file"
            ANY_CHANGED=1
        else
            rm -f "$tmp_file"
            echo "  [current] $file"
        fi
    fi
done

chmod +x "$INSTALL_DIR/sofle_sync.py" "$INSTALL_DIR/sofle_flash.py"

UDEV_CHANGED=0
SERVICE_CHANGED=0

if [[ "$OS_NAME" == "Linux" ]]; then
    if [[ "$SKIP_UDEV" != "1" ]]; then
        UDEV_SRC="$INSTALL_DIR/99-sofle-pico-hidraw.rules"
        UDEV_DEST="/etc/udev/rules.d/99-sofle-pico-hidraw.rules"
        if [[ -f "$UDEV_DEST" ]] && cmp -s "$UDEV_SRC" "$UDEV_DEST"; then
            echo "  [current] $UDEV_DEST"
        elif command -v sudo >/dev/null 2>&1 && command -v udevadm >/dev/null 2>&1; then
            echo "[sofle-sync] Installing udev rule (requires sudo if prompted)..."
            if sudo cp "$UDEV_SRC" "$UDEV_DEST" && sudo udevadm control --reload-rules && sudo udevadm trigger; then
                echo "  [udev]    Installed $UDEV_DEST"
                UDEV_CHANGED=1
            else
                echo "  [warn]    Could not install $UDEV_DEST; hidraw access may require manual udev setup." >&2
            fi
        else
            echo "  [warn]    sudo or udevadm missing; install $UDEV_SRC to $UDEV_DEST manually." >&2
        fi
    fi

    if [[ "$SKIP_SYSTEMD" != "1" ]]; then
        UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
        UNIT_DEST="$UNIT_DIR/sofle-pico-sync.service"
        UNIT_TMP="$TMP_DIR/sofle-pico-sync.service"
        cat > "$UNIT_TMP" <<EOF
[Unit]
Description=Sofle Pico Dual-USB Split Sync Bridge
After=default.target

[Service]
Type=simple
ExecStart=${PYTHON_BIN} ${INSTALL_DIR}/sofle_sync.py
Restart=always
RestartSec=2

[Install]
WantedBy=default.target
EOF
        mkdir -p "$UNIT_DIR"
        if [[ -f "$UNIT_DEST" ]] && cmp -s "$UNIT_TMP" "$UNIT_DEST"; then
            echo "  [current] $UNIT_DEST"
        else
            cp "$UNIT_TMP" "$UNIT_DEST"
            echo "  [unit]    Updated $UNIT_DEST"
            SERVICE_CHANGED=1
        fi

        if command -v systemctl >/dev/null 2>&1 && systemctl --user list-units >/dev/null 2>&1; then
            if [[ "$SERVICE_CHANGED" -eq 1 ]]; then
                systemctl --user daemon-reload
            fi
            systemctl --user enable sofle-pico-sync.service >/dev/null
            if [[ "$ANY_CHANGED" -eq 1 || "$SERVICE_CHANGED" -eq 1 || "$UDEV_CHANGED" -eq 1 ]] || ! systemctl --user is-active --quiet sofle-pico-sync.service; then
                systemctl --user restart sofle-pico-sync.service
                echo "[sofle-sync] sofle-pico-sync.service enabled and running."
            else
                echo "[sofle-sync] Already up to date and running (sofle-pico-sync.service)."
            fi
        else
            echo "[sofle-sync] systemd --user not active; start manually with:"
            echo "  nohup \"$PYTHON_BIN\" \"$INSTALL_DIR/sofle_sync.py\" >/dev/null 2>&1 &"
        fi
    fi
elif [[ "$OS_NAME" == "Darwin" ]]; then
    echo "[sofle-sync] Files downloaded to $INSTALL_DIR."
    echo "[sofle-sync] On macOS, install hidapi and start the service with:"
    echo "  pip3 install hidapi && \"$PYTHON_BIN\" \"$INSTALL_DIR/sofle_sync.py\" --backend hidapi"
fi

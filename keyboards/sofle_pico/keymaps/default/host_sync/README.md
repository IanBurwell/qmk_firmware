# Sofle Pico Dual-USB Sync Service (Linux & Windows)

Lightweight host relay that synchronizes **`_LOWER` / `_RAISE` / `_ADJUST` layers (~1–2 ms latency)**, **modifier OLED badges (`S C A G`)**, **GP24 OLED/RGB lights toggle**, and **RGB Matrix profile/HSV/speed** between both halves of your Sofle Pico when plugged in via two USB cables (no TRRS cable).

- **Works automatically**: When TRRS is plugged in (`0:TRRS`), QMK's native split link handles everything and the service stays idle. When TRRS is unplugged and both USB cables are plugged in, the OLED header switches from `0:N/C` → `0:WAIT` → `0:SYNC` once both halves are paired.
- **No VIA conflict**: Uses custom Raw HID command `0xF0`; normal VIA configuration continues to work untouched.
- **Standalone fallback**: If plugged into a machine without the service (`0:N/C` / `0:WAIT`), both halves still work as independent USB keyboards with fixed Left/Right RGB initialization and native OS cross-half typing modifiers (`Shift`/`Ctrl`/`Alt`/`GUI`).

---

## Single-Command Dual-Board Flashing (`sofle_flash.py`)

When both halves are plugged in via USB, you can update both boards sequentially with a single command (zero external dependencies; works cleanly even while `sofle_sync.py` is running in the background):

- **Flash existing `.uf2` build** (Windows PowerShell, WSL bash, or native Linux):
  ```powershell
  python.exe keyboards\sofle_pico\keymaps\default\host_sync\sofle_flash.py
  ```
  ```bash
  python3 keyboards/sofle_pico/keymaps/default/host_sync/sofle_flash.py
  ```

- **Compile + flash both boards in one command** (`--build`):
  ```powershell
  python.exe keyboards\sofle_pico\keymaps\default\host_sync\sofle_flash.py --build
  ```
  ```bash
  python3 keyboards/sofle_pico/keymaps/default/host_sync/sofle_flash.py --build
  ```

- **Useful options**:
  - `--side {both,left,right}`: flash only one half or both (default: `both`, Left then Right).
  - `--uf2 PATH`: flash a custom `.uf2` file instead of `sofle_pico_default.uf2`.
  - `--verbose` / `-v`: print probe, ACK, and reboot progress details.

> **One-time bootstrap:** If a half is still running older firmware from before remote bootloader support (`0x03`/`0x04`) was flashed, `sofle_flash.py` will prompt you once to hold `BOOT` while plugging that half in and will then flash it automatically. Every update after that is completely hands-free.

---

## Linux Setup (Pure Python Stdlib, No `pip` Required)

1. Install the udev rule so your user can access `/dev/hidraw*` without root:
   ```bash
   sudo cp keyboards/sofle_pico/keymaps/default/host_sync/99-sofle-pico-hidraw.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules && sudo udevadm trigger
   ```
   *(Unplug and replug the USB cables if the keyboards were already attached.)*

2. Test interactively:
   ```bash
   python3 keyboards/sofle_pico/keymaps/default/host_sync/sofle_sync.py --verbose
   ```

3. Enable auto-start at login via `systemd --user`:
   ```bash
   mkdir -p ~/.config/systemd/user
   cp keyboards/sofle_pico/keymaps/default/host_sync/sofle-pico-sync.service ~/.config/systemd/user/
   systemctl --user daemon-reload
   systemctl --user enable --now sofle-pico-sync.service
   ```

---

## Windows Setup (Pure Python Stdlib, No Packages Required)

1. Ensure Python 3.9+ (`python.exe`) is installed (uses built-in Windows HID APIs via `ctypes` — no `pip install` packages required).

2. Test in PowerShell:
   ```powershell
   python.exe keyboards\sofle_pico\keymaps\default\host_sync\sofle_sync.py --verbose
   ```
   *(Optional fallback: `pip install hidapi` and run with `--backend hidapi` if needed.)*

3. Run silently in the background on login:
   - Press `Win + R`, run `shell:startup`, and create a shortcut pointing to `sofle_sync_windows.vbs` (or configure a Task Scheduler task running `pythonw.exe <path>\sofle_sync.py` at logon).

---

## Home Assistant Hex Lights (`_ADJUST + F`)

Pressing the Left home-row index key (`F`, rendered as a light bulb icon on the OLED) while `_ADJUST` is active (either by holding `LOWER + RAISE` or while in RGB Info OLED mode) sends a Raw HID host action packet (`RAW_MSG_HOST_ACTION = 0x05`, action `1`) over USB to `sofle_sync.py`, which fires an asynchronous `POST` request to `https://home-assistant.zuul-diphda.ts.net/` without blocking key syncing. This works in both Dual-USB mode and TRRS mode whenever `sofle_sync.py` is running on the host connected to the master half.

### 1. Add a Webhook trigger to `automation.toggle_hex_lights`

In Home Assistant (`Settings -> Automations & scenes -> toggle_hex_lights`):

1. Keep your existing toggle action for `light.h6061`.
2. Add a trigger: **Add trigger -> Other triggers -> Webhook**.
3. Set **Webhook ID** to `toggle_hex_lights` (or copy the generated ID into a local gitignored `ha_config.json` — see below).
4. Click the gear icon next to the Webhook trigger:
   - Ensure **`POST`** is enabled.
   - **Uncheck** *"Only accessible from the local network"* if Home Assistant receives requests through Tailscale (`100.64.0.0/10`), then click **Save**.

### 2. Local config & overrides (never committed)

Copy `ha_config.example.json` to `ha_config.json` (gitignored via `host_sync/.gitignore`) if you want a custom webhook ID or REST API fallback:

```json
{
  "base_url": "https://home-assistant.zuul-diphda.ts.net",
  "webhook_id": "toggle_hex_lights",
  "entity_id": "light.h6061",
  "timeout_s": 5.0
}
```

- **REST API fallback (optional)**: set `"webhook_id": ""` and `"token": "<Long-Lived Access Token>"` (with `"entity_id": "light.h6061"` or `"automation.toggle_hex_lights"`) in `ha_config.json` to call Home Assistant's REST `/api/services/...` endpoint instead of a webhook.
- **Environment variables**: `SOFLE_HA_CONFIG`, `SOFLE_HA_URL`, `SOFLE_HA_WEBHOOK_ID`, `SOFLE_HA_TOKEN`, `SOFLE_HA_ENTITY_ID`, `SOFLE_HA_TIMEOUT`.
- **CLI flags**: `--ha-config`, `--ha-url`, `--ha-webhook-id`.

### 3. Test from the command line

Verify connectivity from your PC before or after flashing:

```bash
python3 keyboards/sofle_pico/keymaps/default/host_sync/sofle_sync.py --test-hex-toggle --verbose
```

```powershell
python.exe keyboards\sofle_pico\keymaps\default\host_sync\sofle_sync.py --test-hex-toggle --verbose
```

---

## Windows Audio Output Switcher (`_ADJUST + D`)

Pressing the Left home-row middle key (`D`, next to `F`) while `_ADJUST` is active (either by holding `LOWER + RAISE` or while in RGB Info OLED mode) sends `HOST_ACTION_AUDIO_SWAP` (`0x02`) over Raw HID to `sofle_sync.py`. On Windows, `sofle_sync.py` uses pure Python `ctypes` Core Audio COM (`IPolicyConfig` / `IPolicyConfigVista`, zero `pip` packages or console popups) to swap **Default Playback** (`console` + `multimedia`, leaving Default Communications unchanged) between `"Wireless Headset"` and `"Speakers"`. Works in both Dual-USB and TRRS modes whenever `sofle_sync.py` is running on the host connected to the master half.

On the OLED `_ADJUST` screen, `F` renders as a **light bulb** icon and `D` dynamically switches between a **headphones** icon (when Windows is currently on **Speakers**) and a **speaker** icon (when currently on **Wireless Headset**, syncing live via `MSG_HOST_STATUS = 0x06` in both Dual-USB and TRRS modes).

### Verify & inspect devices on Windows

```powershell
python.exe keyboards\sofle_pico\keymaps\default\host_sync\sofle_sync.py --list-audio-devices
python.exe keyboards\sofle_pico\keymaps\default\host_sync\sofle_sync.py --test-audio-swap --verbose
```

### Optional customization

Copy `audio_config.example.json` to `audio_config.json` (gitignored via `host_sync/.gitignore`) to adjust device names or roles:

```json
{
  "devices": [
    "Wireless Headset",
    "Speakers"
  ],
  "roles": [
    "console",
    "multimedia"
  ]
}
```

- **Environment variables**: `SOFLE_AUDIO_CONFIG`, `SOFLE_AUDIO_DEVICES` (comma-separated), `SOFLE_AUDIO_ROLES` (comma-separated: `console`, `multimedia`, `communications`).
- **CLI flags**: `--audio-config`, `--audio-devices "Wireless Headset,Speakers"`, `--audio-include-comms` (also switches Default Communications).

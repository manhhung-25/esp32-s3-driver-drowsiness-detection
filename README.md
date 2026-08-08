# Driver Drowsiness Detection — ESP32-S3-N16R8 (ESP-WHO + ESP-DL v3 + PFLD)

A fully **on-device (edge AI)** driver-drowsiness monitoring system built for the
**ESP32-S3-N16R8** (16 MB flash + 8 MB octal PSRAM), using **ESP-WHO** as a base and
**ESP-DL v3** for inference. Face landmark analysis (PFLD 68 points) runs **on the chip** —
no cloud, no external AI service is required.

> [!IMPORTANT]
> This documentation focuses on the **edge model** running on the ESP32
> (PicoDet face detection + PFLD 68-point landmarks). The optional Android app and the
> Flask server are described only briefly — deploy them yourself if needed.

---

## 1. Overview

The device captures a 240×240 RGB565 frame, detects the driver's face, localizes
98→68 facial landmarks, and derives drowsiness metrics completely on-device:

```
camera ──► PicoDet (face detect) ──► PFLD 68 landmarks ──► EAR / MAR / pitch
   ──► PERCLOS + state machine (AWAKE → PRE_DROWSY → DROWSY → MICROSLEEP)
   ──► buzzer (PWM) + warning LED
```

- **Face detection:** ESP-DL `espdet_pico_224_224_face.espdl` (PicoDet, 224×224 input).
- **Landmarks:** `pfld68.espdl` — PFLD, 68 points, iBUG/300-W mapping
  (eyes 36–41 / 42–47, nose 27–35, mouth 48–59).
- **Metrics:** EAR (Eye Aspect Ratio, Soukupová & Čech 2016), MAR (Mouth Aspect Ratio),
  head-pitch deviation, blink/yawn rates, PERCLOS (medical standard).
- **State machine:** with hysteresis, triggers buzzer/LED.

Edge-only: **no Google/cloud model** is required for detection.

---

## 2. Features

- 🧠 **Edge AI** — PicoDet face detection + PFLD 68 landmark, both pre-quantized
  `.espdl` models embedded in RODATA.
- 👁️ **EAR** — standard 6-point Euclidean formula, roll-invariant.
- 👄 **MAR** — mouth-open detection with roll derotation (yawn detection).
- 🙇 **Head pitch** deviation → nodding detection.
- ⏱️ **PERCLOS** (medical standard) + fatigue score + state machine
  `AWAKE → PRE_DROWSY → DROWSY → MICROSLEEP`.
- 🚨 **GPIO alarms** — buzzer (PWM via LEDC) + LED patterns per state.
- ⚡ **Dual-core** — camera on Core 0, AI + alarm on Core 1.
- 🛠️ **Runtime console** — `drowsy get/set/stats/reset` (no rebuild to tune thresholds).
- 🌐 **Web interface** — MJPEG stream, `/status`, live overlay, admin config.
- 📶 **WiFi modes** — AP (default), STA (router/Internet), OFF.
- 📦 **OTA firmware update** — flash new firmware over the air from the app / curl.

---

## 3. Repository layout

```text
driver_drowsiness_detection/
├── CMakeLists.txt                  # Standalone project (Component Registry deps)
├── partitions.csv                  # OTA layout: otadata + ota_0 + ota_1
├── sdkconfig.defaults(.esp32s3)    # 16 MB flash, Octal PSRAM 80 MHz, CPU 240 MHz
├── sdkconfig                        # Current build config (.esp32s3 target)
├── main/
│   ├── app_main.cpp                # Dual-core pipeline + console
│   ├── camera_utils.cpp            # Camera init (self-declared pins, no esp32-camera Kconfig)
│   ├── face_pipeline.cpp           # PicoDet face detection (largest face)
│   ├── landmark_pfld.cpp           # PFLD inference + EAR/MAR/pitch (iBUG mapping)
│   ├── drowsiness_detector.cpp     # State machine + PERCLOS + fatigue score
│   ├── alarm_control.cpp           # Buzzer (LEDC PWM) + LED patterns
│   ├── console_cmds.cpp            # `drowsy` command (get/set/stats/reset)
│   ├── web_server.cpp              # HTTP: /, /stream, /status, /admin, /api/alarm, /api/ota
│   ├── config_store.cpp            # NVS device config (thresholds, WiFi)
│   ├── report_client.cpp           # Periodic JSON report to Flask server (STA only)
│   ├── Kconfig.projbuild           # ALL thresholds + camera/Buzzer/LED hints (menuconfig)
│   └── www/                        # Embedded HTML pages (index.html, admin.html)
├── model/                          # Vendored `.espdl` models (already included)
│   ├── espdet_pico_224_224_face.espdl   # Face detect (PicoDet 224×224, ~480 KB)
│   └── pfld68.espdl                     # PFLD 68 landmarks (RODATA)
├── tools/                          # Host-side tools (quantization, verification, video)
│   ├── quantize_pfld.py            # ESP-PPQ: ONNX → .espdl
│   ├── export_pfld_onnx.py         # PyTorch checkpoint → ONNX
│   ├── export_pyfeat_pfld68.py     # py-feat/68 export
│   ├── verify_pfld_mapping.py      # verify landmark mapping on host
│   ├── annotate_landmark_video.py  # annotate landmarks on a video (host)
│   ├── check_calib.py, setup_quant_env.sh, quantize_pfld_espdl.md
└── partitions.csv                  # OTA partition table (16 MB)
```

---

## 4. Requirements

| Component   | Version      | Notes                                             |
|-------------|--------------|---------------------------------------------------|
| ESP-IDF     | **≥ 5.3**    | esp-dl v3.3 requires IDF ≥ 5.3                    |
| Python      | 3.8 – 3.12   | only for host tools (quantization, video annotation) |
| esp-dl      | ^3.3.0       | via `main/idf_component.yml` (registry, auto-fetch) |
| esp32-camera| ^2           | via registry                                       |
| espressif/mdns | ^1        | via registry                                       |

```bash
cd ~/esp/esp-idf
git checkout release/v5.3 && git submodule update --init --recursive
./install.sh esp32s3 && . ./export.sh
```

> [!WARNING]
> This example uses **esp-dl v3 from the IDF Component Registry**, not the esp-who
> submodule esp-dl v2 — so it does **not** interfere with the other esp-who examples.
> To return to an older IDF: `git checkout release/v5.0 && . ./export.sh` (not recommended).

---

## 5. Models — already vendored

The required models are **already present** in `model/` and are baked into the firmware
at build time (RODATA). **No download step is needed.**

| File | Type | Used by |
|---|---|---|
| `model/espdet_pico_224_224_face.espdl` | Face detection (PicoDet) | `face_pipeline.cpp` |
| `model/pfld68.espdl` | PFLD 68 landmarks (iBUG) | `landmark_pfld.cpp` |

Each model is embedded using `target_add_aligned_binary_data(...)` in `main/CMakeLists.txt`,
so it ships inside the application binary.

> [!NOTE]
> If you want to use the **98-point PFLD** instead, select it in menuconfig
> (`DROWSY_PFLD_98PT`) and put the quantized model at
> `model/pfld_landmarks_98.espdl`. The 68-point mapping is the default.

---

## 6. Build / Flash / Monitor

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3                    # once
idf.py menuconfig                            # optional: tune pins/thresholds
idf.py build
idf.py -p /dev/ttyACM0 flash                 # full flash (see OTA note below)
idf.py -p /dev/ttyACM0 monitor               # serial console (USB-Serial/JTAG)
```

> [!IMPORTANT]
> The partition table is **OTA layout** (`ota_0`/`ota_1` + `otadata`). The **first** flash
> must be a **full flash** (`flash`, not `app-flash`) so the bootloader + otadata + slot are
> written correctly. After that, subsequent updates can go through OTA.

---

## 7. Menuconfig reference (`Drowsiness Detection Configuration`)

All parameters are available in `idf.py menuconfig` → **Drowsiness Detection
Configuration** (from `main/Kconfig.projbuild`).

| Option | Default | Meaning |
|---|---|---|
| Camera board preset | ESP32-S3-EYE | S3-EYE / AI-Thinker / Custom (own 15 pins) |
| Frame size | 240×240 | or QVGA 320×240 |
| Camera RGB565 BE | n | enable if skin color is swapped (green/purple) |
| Buzzer GPIO | 14 | free on S3-EYE |
| LED GPIO | 21 | free on S3-EYE |
| EAR blink th | 200 (0.20) | EAR below = eye closed |
| EAR drowsy th | 160 (0.16) | sustained deep close |
| MAR yawn th | 500 (0.50) | mouth open = yawn |
| Microsleep ms | 1200 | sustained deep-close → emergency |
| PERCLOS th | 400 (40%) | % closed in window → DROWSY |
| Window ms | 60000 | PERCLOS sliding window |
| Blink rate high | 20 | blinks/min contribute to fatigue |
| Pitch dev th | 120 (0.12) | deviation from baseline → nodding |
| Console enable | y | runtime `drowsy` commands |
| Web enable | y | HTTP UI + stream + admin |
| WiFi mode | AP | AP / STA / OFF |
| AP SSID / pass | Drowsy_AP / 12345678 | hotspot (open or WPA2) |
| WiFi SSID / pass (STA) | – | router credentials |
| Admin pass | admin | for `/admin` |
| HTTP port | 80 | page + stream + capture |
| JPEG quality | 45 | stream quality |
| Stream skip | 3 | send 1/N frames |
| Report enable (needs STA) | y | POST state to server |
| Report URL | – | e.g. `http://x:5000/api/report` |
| **AI on device** | n | **see section “Source of state”** |

### Source-of-truth (AI) configuration

The `DROWSY_AI_ON_DEVICE` switch decides **who detects drowsiness**:

| `AI_ON_DEVICE` | Behavior |
|---|---|
| `n` (default) | The **phone/app** is the source. ESP32 only streams frames + drives buzzer/LED when the app sends `POST /api/alarm`. No on-device face/landmap work → lightest CPU/RAM. |
| `y`            | ESP32 runs the full **edge pipeline** itself (face detect + PFLD + state machine) and drives the buzzer/LED directly. Use when no phone is present. |

The console `drowsy` commands (`get/set/stats/reset`) **also exist** and work at runtime.

---

## 8. Runtime console

```text
drowsy get                     # show all thresholds
drowsy set ear_blink_th 0.22   # change threshold & save (NVS)
drowsy set perclos_th 0.45
drowsy stats                   # PERCLOS, blink/min, yawn/min, fatigue
drowsy reset                   # reset baseline pitch + counters
```

### Calibration procedure (light-dependent)

1. Sit normally; look straight → log should show `ear ≈ 0.28 – 0.35`.
2. Blink 3× → `blink/min` rises, no DROWSY.
3. Close eyes for 2 s → must enter **MICROSLEEP** (buzzer continuous + LED on).
4. Yawn → `yawn/min` rises.
5. Nod head down → `pitch_dev` rises.
6. If EAR is noisy in dark/glasses: increase `--calib-steps` when quantizing, try
   `num_of_bits=16`, or lower resolution.

---

## 9. Web interface & API

The HTTP server (port 80) exposes:

| Endpoint | Method | Purpose |
|---|---|---|
| `/` | GET | live dashboard page |
| `/stream` | GET | MJPEG stream |
| `/capture` | GET | single JPEG |
| `/status` | GET | JSON metrics (EAR, MAR, PERCLOS, …) |
| `/admin` | GET | admin panel (password) |
| `/admin/api/config` | GET | view / change config (+ `?pass=`), reboot WiFi |
| `/admin/api/reboot` | GET | reboot device |
| `/admin/api/log` | GET | recent log ring |
| `/api/alarm` | POST | body `{"state": 0..3}` → buzzer/LED (when `AI_ON_DEVICE=off`) |
| `/api/ota` | POST | firmware `.bin` → OTA update |

---

## 10. WiFi modes

| Mode | Behavior |
|---|---|
| **AP** | device hosts `Drowsy_AP` (`12345678`) → open `http://192.168.4.1` |
| **STA** | device joins a router; get IP from serial log (`WiFi connected: 192.168.x.x`) or via mDNS `drowsy.local` |
| **OFF** | WiFi disabled |

- Switching AP↔STA at runtime is done through the app (`DeviceApi.setDeviceWifi`) or
  `/admin/api/config?pass=...&wifi_mode=...`.
- If STA fails to connect **after 5 retries**, the firmware **automatically returns to AP**
  mode so the device can never become unreachable.

---

## 11. OTA firmware update

OTA is enabled by the OTA partition layout:

```text
nvs,       … 0x6000
phy_init,  … 0x1000
otadata,   … 0x2000
ota_0,     … 0x3C0000   (≈3.75 MB)
ota_1,     … 0x3C0000
```

- **First flash** must be full (`flash`) — see §6.
- New images are pushed to `POST /api/ota` (raw binary, `application/octet-stream`).
- `idf.py size` will report whether the current build fits in a single OTA slot
  (≈3.75 MB, models included).

Example (curl):

```bash
curl -v -X POST http://192.168.4.1/api/ota \
     -H "Content-Type: application/octet-stream" \
     --data-binary @build/driver_drowsiness_detection.bin
```

---

## 12. Algorithms

### Pipeline

```text
Frame RGB565 240×240
  └─ PicoDet (face detect) ─► largest face [x0,y0,x1,y1]
       └─ PFLD 68 (crop → 112×112 → normalize → inference)
            ├─ EAR = (|p2−p6| + |p3−p5|) / (2·|p1−p4|)   // Soukupová & Čech 2016
            ├─ MAR = (height/width) of mouth after −roll derotation
            └─ pitch = geometric ratio (nose/mouth), clamped [−1, 1]
                 └─ PERCLOS + fatigue score + state machine → buzzer/LED
```

- **EAR**: standard Euclidean 6-point formula; **roll-invariant** (no explicit
  derotate needed for the eye).
- **MAR**: height/width of the mouth bounding box **after** rotating points by −roll.
- **PERCLOS**: % of closed-eye samples in a sliding window (default 60 s).
- **Blink/yawn rates**: real rates within the window.
- **fatigue_score** =
  `0.40·PERCLOS + 0.20·yawn + 0.20·pitch_dev + 0.20·blink_rate`.

### State machine (hysteresis)

```text
AWAKE(0) ─► PRE_DROWSY(1) ─► DROWSY(2) ─► MICROSLEEP(3)
   ▲               │              │              │
   └───────────────┴──────────────┴──────────────┘ exit conditions
```

- Enter **MICROSLEEP** when the eye is deeply closed (`EAR < ear_drowsy`) continuously
  for `microsleep_ms` (default 1200 ms).
- Exit **MICROSLEEP → AWAKE** when `EAR >= ear_blink` **AND** `MAR <= mar_th`
  (eyes open + mouth not open).
- Exit **DROWSY → AWAKE** when `EAR >= ear_blink`.
- (No dependence on a fragile “eye-open `since`” timer — thresholds are derived directly
  from the live EAR/MAR.)

---

## 13. Troubleshooting / FAQ

| Symptom | Cause / fix |
|---|---|
| `Camera init failed 0x...` | wrong camera pins → choose preset or declare Custom (menuconfig) |
| Build error: missing `.espdl` | models are vendored — check `model/`; else re-run quantization tool |
| FPS low (< 3) | EAR closed → PFLD runs every frame; check `DROWSY_AI_ON_DEVICE`, cache/CPU config |
| EAR always ~0 or noisy | landmark mapping wrong → run `tools/verify_pfld_mapping.py` |
| Skin colors swapped (purple/green) | enable `DROWSY_CAM_RGB565_BE` (byte-order) |
| Buzzer silent | check buzzer GPIO + driver current (needs transistor if > 5 mA) |
| STA won't connect | router 5 GHz? ESP32 is 2.4 GHz only. 5. check SSID/pass. |
| mDNS / auto-IP fails on Android | use AP fixed IP `192.168.4.1`; mDNS unreliable on no-internet N/W |
| OTA fails | first flash must be full; check `idf.py size` fits slot; use `-p /dev/ttyACM0` |

---

## 14. Optional companions (briefly)

Two optional companion components exist in this repo:

- **Android app** (`android/`) — a Jetpack Compose app that can:
  - connect to the device (stream, metrics, thresholds),
  - run its own (optional) AI (MediaPipe-based) for higher FPS on the phone,
  - send alarms / notifications locally,
  - then sync local log to a server via `SyncWorker`.
- **Admin server** (`server/`) — a Flask + SQLite dashboard that accepts
  `/api/report` from devices and `/api/sync` from the app; shows live state, history, GPS map.

Deploy/run the app and the server **yourself** — this README focuses on the **edge model**.

---

## 15. Licenses

- Example code: MIT (like ESP-WHO).
- Face-detect model: MIT (Espressif).
- PFLD model: you must verify the license of your source model (WFLW used for research).
  The `pfld68.espdl` included here was quantized from a PyTorch/ONNX origin.
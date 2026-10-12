# M5Gemini

M5Gemini is a voice-driven AI assistant for the ESP32-S3 powered M5 Cardputer mini PC. It uses Google Gemini's Live API for real-time Speech-to-Speech (S2S) conversation over a single WebSocket connection. The application uses the M5 Cardputer's built-in microphone and speaker for hands-free interaction.

## Features

- Real-time voice conversation powered by Google Gemini Live API (Speech-to-Speech).
- Half-duplex audio: microphone and speaker alternate automatically via server-side VAD.
- Streaming text transcription displayed during playback.
- Configurable model, voice, and system instructions.
- Automatic session reconnection on transient errors; error dialogs for critical failures.
- Settings management compatible with M5Apps installer.

## Prerequisites

- ESP-IDF v5.4 or later
- M5 Cardputer hardware
- WiFi internet connection (2.4GHz) for API access
- Google AI API Key (https://aistudio.google.com/apikey)

## Getting Started

### 1. Clone the Repository

```bash
git clone https://github.com/d4rkmen/M5Gemini.git
cd M5Gemini
```

### 2. Configuration

Configuration settings (WiFi credentials, API keys) are compatible with M5Apps.

- **Configuration Methods:**

  1.  **Manual Configuration on Device:**
      - Press `ESC` to enter Settings menu
      - Navigate using arrow keys and Enter to select
      - Configure WiFi, API key, model, voice, and other settings
      - Settings are automatically saved to NVS storage and persist across reinstalls
  2.  **Import Configuration File:** - Create a `settings.txt` file with your configuration - Example `settings.txt`:

  ```ini
    wifi-enabled=true
    wifi-ssid=YOUR_WIFI_SSID
    wifi-pass=YOUR_WIFI_PASS
    wifi-static_ip=false
    wifi-ip=192.168.88.101
    wifi-mask=255.255.255.0
    wifi-gw=192.168.1.1
    wifi-dns=8.8.8.8
    system-brightness=100
    system-volume=70
    system-boot_sound=true
    gemini-api_key=YOUR_GEMINI_API_KEY
    gemini-model=gemini-2.0-flash-live-001
    gemini-voice=Kore
    gemini-volume=255
    gemini-rules=you are conversational AI assistant running on M5 CardPuter. give short direct and a quite funny answer.
  ```

  - Copy file to SD card root as `/sdcard/settings.txt`
  - In Settings menu select: `Import (SD card)`

### 3. Build the Project

```bash
idf.py build
```

### 4. Flash onto M5Cardputer

Connect your M5 Cardputer via USB and run:

```bash
idf.py -p [Your-Serial-Port] flash monitor
```

(Replace `[Your-Serial-Port]` with the correct port, e.g., `COM3` on Windows or `/dev/ttyUSB0` on Linux).

## Usage

- From the start screen press `ESC` to enter Settings or `ENTER` to start a voice session.
- During a session the keyboard is used for control:
  - `ENTER` — interrupt (skip playback or send during listening)
  - `ESC` — stop session and return to start screen
  - Arrow keys — scroll chat history

[![YouTube video](https://img.youtube.com/vi/NF_7Dyx2gLY/0.jpg)](https://www.youtube.com/watch?v=NF_7Dyx2gLY)

[![S2S Demo](https://img.youtube.com/vi/3MHIc3bUb94/0.jpg)](https://youtube.com/shorts/3MHIc3bUb94?si=4pxLwXzcZXVly9VV)

## M5StickS3

This fork also runs on the M5StickS3 (ESP32-S3-PICO-1, ES8311 codec, M5PM1 power chip, two buttons).

### Build and flash

The StickS3 has its own configuration file; the default `sdkconfig` stays the Cardputer one.

```bash
idf.py -B build_sticks3 -D SDKCONFIG=sdkconfig.sticks3 build
idf.py -B build_sticks3 -D SDKCONFIG=sdkconfig.sticks3 -p [Your-Serial-Port] flash
```

Use `app-flash` instead of `flash` for later updates: `flash` also rewrites the settings partition and clears the saved WiFi networks and API key.

If the stick was put into download mode by hand (long press on the side button), single-click the side button after flashing to start the firmware.

### Buttons

`A` is the front button, `B` is the side button.

| Press    | Action                             |
| -------- | ---------------------------------- |
| `A`      | OK / start session / interrupt     |
| `B`      | Move down / scroll                 |
| Hold `A` | Move up                            |
| Hold `B` | Back / open settings / stop session |

### Web setup

There is no keyboard, so text settings are edited from a browser: `Settings` → `Web setup`.

- Not connected to WiFi yet: the stick starts its own access point with a one-off password and shows a QR code to join it. Press `A` to switch the QR code to the page address (`http://192.168.4.1/`).
- Already connected: the page is served on the current network at the address shown on screen. It has no password, and is only reachable while the Web setup screen is open.

Hold `B` to leave; WiFi restarts with the saved settings.

Up to 8 WiFi networks can be saved. With more than one, the stick scans and joins the strongest saved network in range, and looks for another when the current one keeps failing.

### Differences from the Cardputer

- No SD card: settings import/export and screenshots are not available.
- Numbers are picked from a list of steps on the device; exact values can be entered in Web setup.
- A battery indicator is shown on the main screen (on the Cardputer too).
- `Settings` → `Power off` switches the device off through the power chip.

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues.

## License

This project is licensed under the GNU General Public License

## Acknowledgements

- Google AI (Gemini Live API)
- M5Stack (M5Unified library)
- LovyanGFX (Display)
- mjson (Embedded JSON parser)

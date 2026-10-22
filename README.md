# OLED Desk Companion

An ESP32 (ESP32-WROOM-32 dev board) with a 1.3" OLED that sits on your desk and shows:

- **a big animated clock** with rolling digits and the date,
- **expressive eyes** that play little scenes (happy, sleepy, angry, dizzy…),
- **Claude mode**: while Claude Code is working on a reply on your PC, Claude "thinking" animations and your **5-hour and weekly usage**.

A small Python script on the PC (`tools/pc_link.py`) sends the ESP32 the time, whether Claude Code is working, and your usage **over WiFi**, so the OLED only needs USB for power. The script starts automatically in the background when you log in.

```
Claude not working:  eyes ──► time ──► eyes ──► time ...
Claude working:      Claude animation ──► usage ──► Claude animation ──► time ──► ...
```

An open chat that is sitting idle doesn't count: Claude mode starts when you send a message and ends when the reply is done.

---

## Contents

1. [Hardware](#hardware)
2. [Wiring](#wiring)
3. [Software setup](#software-setup)
4. [Running it all the time](#running-it-all-the-time)
5. [What's on the screen](#whats-on-the-screen)
6. [How the PC link works](#how-the-pc-link-works)
7. [Customising](#customising)
8. [Project structure](#project-structure)
9. [Memory notes](#memory-notes)
10. [Troubleshooting](#troubleshooting)
11. [Demos (earlier versions)](#demos-earlier-versions)

---

## Hardware

| Part | Notes |
|------|-------|
| ESP32-WROOM-32 dev board ("ESP32 DevKit V1" / "ESP32-DevKitC") | 2.4 GHz WiFi, 520 KB RAM, 4 MB flash |
| 1.3" I2C OLED, 128×64 | SH1106 controller (most 1.3" modules). 0.96" modules usually use SSD1306 — see [Troubleshooting](#troubleshooting) |
| USB cable | Powers the ESP32 and is used for uploading. Any USB charger works once it's set up. |

## Wiring

| OLED pin | ESP32 |
|----------|-------|
| VCC | **3V3** |
| GND | GND |
| SDA | **GPIO21** (marked `D21` or `21`) |
| SCL | **GPIO22** (marked `D22` or `22`) |

Use 3V3, not 5V/VIN. The ESP32's pins are 3.3 V only, and most OLED modules run fine on 3.3 V.

Pin order printed on OLED modules varies (`GND VCC SCL SDA` vs `VCC GND SCL SDA`) — follow the labels on your board, not the position.

## Software setup

### 1. Firmware (PlatformIO)

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE** extension.
2. Open this folder in VS Code.
3. **Enter your WiFi:** open `include/wifi_secrets.h` and fill in `WIFI_SSID` and `WIFI_PASSWORD`. The ESP32 only joins **2.4 GHz** networks. The file is in `.gitignore`. If it's missing (for example after a fresh clone), copy `include/wifi_secrets.example.h` to `include/wifi_secrets.h`.
4. Plug in the ESP32 with a **data** USB cable. Many cables only carry power: the board lights up, but Windows never sees it. A new port should appear in Device Manager → Ports, e.g. `Silicon Labs CP210x USB to UART Bridge (COM7)` or `USB-SERIAL CH340 (COM7)`. If an "Unknown device" with a warning icon appears instead, install the driver for the chip printed next to the board's USB socket (CP210x or CH340). Put that port in `platformio.ini` (`upload_port` / `monitor_port`), because otherwise PlatformIO may pick one of the PC's Bluetooth COM ports.
5. Click **✓ Build**, then **→ Upload** in the PlatformIO toolbar. If the upload hangs at `Connecting....`, hold the **BOOT** button on the board until it starts writing.

The first build downloads the ESP32 toolchain, which takes a few minutes. The only library, [U8g2](https://github.com/olikraus/u8g2), is installed automatically.

After it boots, the bottom line of the screen shows `connecting to WiFi...` and then `waiting @ 192.168.x.x` (the ESP32's IP) until the PC script sends the time.

### 2. PC script (Python)

Requires Python 3 on Windows. It only uses the standard library, so there's nothing to install.

```powershell
python tools\pc_link.py                       # run it once in a terminal to try it
python tools\pc_link.py --host 192.168.1.42   # if the name lookup doesn't work (see below)
```

It finds the ESP32 **by name**: the ESP32 answers to `claude-oled.local` over mDNS, which Windows 10/11 resolves by itself. That means you don't need to know its IP, and the IP can change. The script repeats the lookup every minute. The PC and the ESP32 just have to be on the same network (PC on Ethernet with ESP32 on WiFi from the same router is fine). You should see something like:

```
Sending to claude-oled.local:4210 over UDP
13:29:38 sending to 192.168.0.106
13:29:40 Claude sessions working: 1
13:29:41 usage: 5h 56%, week 18%
```

It doesn't broadcast to the whole network by default because many routers, including this project's, don't pass broadcasts on to WiFi devices. To try broadcasting anyway, use `--host 255.255.255.255`.

Without the script the ESP32 shows the eyes with *"waiting @ <its IP>"* underneath.

## Running it all the time

Install it as a background task that starts every time you log in:

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_autostart.ps1
```

This creates a Windows Task Scheduler task called **"OLED PC Link"** (for your user only, no admin rights needed). It:

- runs `pc_link.py` with `pythonw.exe`, so there's **no window**;
- writes its messages to **`tools/pc_link.log`** (rotated at ~200 KB);
- keeps sending if the ESP32 is unplugged, the network drops, the PC sleeps, or anything unexpected happens, and Task Scheduler restarts it if it ever exits.

| To… | Run |
|-----|-----|
| Remove auto-start | `powershell -ExecutionPolicy Bypass -File tools\install_autostart.ps1 -Uninstall` |
| Stop it for now | `schtasks /End /TN "OLED PC Link"` |
| Start it again | `schtasks /Run /TN "OLED PC Link"` |

The link no longer uses the COM port, so you can upload firmware and open PlatformIO's Serial Monitor while it runs. On boot the Serial Monitor prints `WiFi connected, IP ..., name claude-oled.local`, then `time synced from PC` once the first time arrives.

If you pass `--host` to use a fixed IP in the background task, add it to the `-Argument` in `install_autostart.ps1`. Also give the ESP32 a fixed (reserved) address in your router so the IP doesn't change.

## What's on the screen

The screen "blinks" between pages: black bars close in from the top and bottom, then open on the next page.

### Clock

Big 12-hour `H:MM` with the seconds beside the minutes, `AM`/`PM` under the seconds, and the date cut out of a white bar along the bottom:

```
 ┌────────────────────────────┐
 │   9:34 56                  │   each digit rolls up when it changes
 │        PM                  │   the colon blinks every second
 │█████ SAT 17 OCT 2026 ██████│   no leading zero: the tens digit rolls away at 1:00
 └────────────────────────────┘
```

Shown for 10 seconds at a time. The time comes from the PC every 10 seconds; between syncs the ESP32 counts on its own.

### Eyes (Claude not working)

Two rounded eyes play random scenes back to back for 10–20 seconds (never the same scene twice in a row):

| Scene | What happens |
|-------|--------------|
| Look around | glances left and right, blinks |
| Happy | `^ ^` eyes with a wink |
| Sleepy | eyelids droop, nod off, jolt awake |
| Surprised | big round eyes with a shine |
| Suspicious | narrow side-eye |
| Angry | lids slant towards the nose, quick shake |
| Dizzy | fast shake, then `X X` eyes |
| Star-struck | eyes turn into twinkling stars |
| Confused | one eye squints, a `?` appears |
| Thinking | looks up with `...`, then an "aha!" |
| Laughing | happy eyes bouncing |
| Eye roll | eyes sweep up and around |
| Cross-eyed | both eyes look inwards |
| Nap | closed eyes, floating `Z`s, wakes with a start |
| Scanner | narrow eyes sweeping side to side |

### Claude animations (Claude working)

One of these plays for 6 seconds each time (random, never the same twice in a row):

| Animation | What happens |
|-----------|--------------|
| **Spinner** | Claude Code's thinking spinner morphing `· ✢ ✳ ✶ ✻ ✽`, with a verb such as `PONDERING...` |
| **Ray wave** | a wave runs round Claude's spark, each ray stretching in turn |
| **Writing** | a small spinning spark types out lines of "code" that scroll up |
| **Shimmer word** | a big thinking verb with a bright band sweeping across it |
| **Assemble** | rays fly in from the edges, snap into the spark, spin, then burst out |
| **Terminal** | Claude Code's status line: a prompt types in, then a spinner, a shimmering verb and a live `(12s ↓ 1.4k tokens)` counter |
| **Typewriter** | a big verb types itself out next to a spark, holds with a blinking cursor, then backspaces into the next one |
| **Coin flip** | the spark spins in 3D like a tossed coin over its shadow, with small stars twinkling around it |
| **Bounce** | the spark bounces with squash and stretch, kicking up dust when it lands |
| **Oscilloscope** | a "brainwave" trace with a sweeping beam, a graticule and a frequency readout |

Verbs: THINKING, PONDERING, NOODLING, CLAUDING, BREWING, MUSING, COGITATING, CONJURING.

The OLED can only show one colour, so the animations also use **light**: they change the whole screen's brightness (the display's contrast setting) as they play, and some add a soft dithered "grey" glow. The coin brightens and glints when it faces you, Bounce flashes on landing, Assemble lights up when the spark snaps together, Spinner gets brighter with the bigger glyphs, and Typewriter blips on each keystroke. Every other page stays at the normal brightness (`CONTRAST_NORMAL`).

### Usage page (Claude working)

Two rings: **5-hour limit** on the left, **weekly limit** on the right. The percentage in each ring counts up when the page appears, and every 2 seconds the labels swap with the time until each limit resets (e.g. `2H31M`). If the usage can't be read, the rings show `--`.

## How the PC link works

`tools/pc_link.py` sends plain text lines in UDP packets to port **4210** of `claude-oled.local` (the ESP32). Each packet holds one or more whole lines, each ending in `\n`. The ESP32 also accepts the same lines over USB serial at 115200 baud, which is handy for testing from the Serial Monitor:

| Line | Meaning | How often |
|------|---------|-----------|
| `T1792240000,77` | local time as seconds since 1970 (time zone already applied) | every 10 s, sent at the start of a second |
| `C1,56,18,151,1082,90` | `C<working>,<5h %>,<week %>,<5h reset in minutes>,<week reset in minutes>` | every 2 s |

The last number on each line is a **checksum**: the sum of the line's characters before it, mod 256. The ESP32 ignores any line that doesn't add up, or whose time is before 2020. To test by hand in the Serial Monitor, compute it with `python -c "l='T1792240000'; print(l + ',' + str(sum(l.encode()) % 256))"`.

A usage value of `255` means "unknown". If the ESP32 hears no `C` line for 15 seconds it treats Claude as not working.

**More than one PC.** Several PCs on the same network can each run `pc_link.py` at once (up to 4). The ESP32 keeps each PC's status apart, by IP address. Claude mode shows when Claude is working on **any** of them, and the usage page shows the numbers of a PC where Claude is working. A PC that goes quiet for 15 seconds drops out. The time comes from whichever PC sent it last, so keep their clocks right. The script uses Windows-only calls to check processes, so each PC must run Windows.

**Detecting when Claude is working.** Each running Claude Code session (terminal or VS Code extension) keeps a file `~/.claude/sessions/<pid>.json`, and Claude Code updates its `"status"` field as the session changes state:

| `status` | Meaning | Counts as working? |
|----------|---------|--------------------|
| `busy` | working on a reply (thinking, running tools, writing) | yes |
| `waiting` | paused on you, e.g. a permission prompt | no |
| `idle` | reply finished (or interrupted), waiting for your next message | no |

The script counts the `busy` files whose process is still alive, so a file left behind by a crashed session is ignored. This field isn't a documented API either; if a Claude Code update renames it, Claude mode stops appearing, and the [troubleshooting](#troubleshooting) entry below shows how to check.

**Reading usage.** The 5-hour and weekly numbers come from the same endpoint Claude Code's `/usage` command uses (`https://api.anthropic.com/api/oauth/usage`), using your saved Claude Code login from `~/.claude/.credentials.json`.

- The login token is only ever sent to Anthropic's API, never anywhere else, and is never printed or logged.
- It's checked at most every 3 minutes, and only while Claude is working. Between checks the reset countdowns tick down locally.
- This is **not an official, documented API**. If a Claude Code update changes it, the usage rings show `--` and everything else keeps working.

## Customising

All the settings are constants near the top of `src/main.cpp`:

| Setting | Default | What it does |
|---------|---------|--------------|
| `CLOCK_MIN_S` / `CLOCK_MAX_S` | 10 / 10 | seconds the clock stays up (picked at random between the two) |
| `EYES_MIN_S` / `EYES_MAX_S` | 10 / 20 | seconds of eye scenes |
| `CLAUDE_MS` | 6000 | length of one Claude animation (ms) |
| `USAGE_MS` | 5000 | how long the usage page stays up (ms) |
| `ROLL_MS` | 450 | speed of the digit roll (ms) |
| `LID_MS` | 300 | speed of the screen "blink" between pages (ms) |
| `CLAUDE_TIMEOUT_MS` | 15000 | no status from the PC for this long = not working |
| `I2C_CLOCK` | 400000 | OLED bus speed; use `100000` if the screen flickers |
| `HOSTNAME` | `claude-oled` | the ESP32's network name: the PC finds it as `claude-oled.local` (change `ESP32_HOST` in `tools/pc_link.py` too) |
| `LINK_PORT` | 4210 | UDP port to listen on (change `LINK_PORT` in `tools/pc_link.py` too) |

Other easy changes:

- **Remove an eye scene:** delete its name from the `ACTS[]` list (or list one twice to make it more common).
- **Change the verbs:** edit the `VERBS` string (words separated by `\0`, uppercase, max ~10 letters).
- **Script timing:** `TIME_EVERY`, `STATUS_EVERY` and `USAGE_EVERY` at the top of `tools/pc_link.py`.

## Project structure

```
platformio.ini            board (esp32dev) and library
src/main.cpp              the firmware
include/
  wifi_secrets.h          your WiFi name and password (not in git)
  wifi_secrets.example.h  template for it
tools/
  pc_link.py              PC → ESP32 link over WiFi: time, Claude activity, usage
  install_autostart.ps1   installs/removes the "OLED PC Link" background task
  pc_link.log             log written by the background task (not in git)
demos/
  animations.cpp          earlier "crazy animations" demo
```

### Firmware layout (`src/main.cpp`)

| Section | Purpose |
|---------|---------|
| Timekeeping | local time from the PC + `millis()`, calendar date calculation |
| Messages from the PC | joins WiFi, reads UDP packets and USB serial, parses the `T` and `C` lines |
| Clock page | rolling digits, blinking colon, date bar |
| Eyes | keyframe "acts" the eyes glide between, and their drawing styles |
| Claude's spark | the 12-ray spark built from tapered "rays" (triangles with round tips) |
| Claude animations | the five animations, each split into *prepare* (maths, once per frame) and *draw* |
| Usage page | the two progress rings |
| Page sequencing | which page comes next, and the blink transitions |

Each frame, `update()` advances timers and works out the picture once, then `render()` draws it.

## Memory notes

The ESP32 has plenty of room (520 KB RAM, 4 MB flash), so the display uses a **full 1 KB frame buffer** (`U8G2_..._F_HW_I2C`) and each frame is drawn in one pass. Logic still lives in `update()` and drawing in `render()`. That split came from the Uno version, which drew in strips, and it keeps frames cheap.


## Troubleshooting

| Problem | Fix |
|---------|-----|
| Screen stays blank | Check wiring: SDA→GPIO21, SCL→GPIO22, VCC→3V3, VCC/GND not swapped. Most modules are at I2C address `0x3C`; some use `0x3D` (change `setI2CAddress` in `setup()`). |
| Image shifted, garbled, or a stripe of noise on one edge | Your module has an SSD1306 chip: replace the display line with `U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);` |
| Screen freezes, flickers or blanks out | Usually a loose wire or unsoldered header pins. Press the wires in, solder the header, keep jumpers short, and try `I2C_CLOCK = 100000`. |
| Stuck on "connecting to WiFi..." | Check `WIFI_SSID` / `WIFI_PASSWORD` in `include/wifi_secrets.h` (both are case-sensitive), and that the network is 2.4 GHz. Many routers have a combined 2.4/5 GHz name, which works. A separate 5 GHz-only name doesn't. |
| Stuck on "waiting @ 192.168.x.x" | The ESP32 is on WiFi but no packets arrive. Check the link is running (`schtasks /Run /TN "OLED PC Link"`, `tools/pc_link.log`). If it is and the log never shows `sending to <the IP on screen>`, the name lookup isn't working (some VPNs block mDNS), so run it with `--host <the IP on screen>`. Also check that "client/AP isolation" is off in the router. |
| Upload hangs at `Connecting....` | Hold the **BOOT** button on the ESP32 until the upload starts. |
| Usage rings show `--` | The usage couldn't be read (offline, login expired, or the endpoint changed). Open Claude Code once to refresh the login; check `tools/pc_link.log` for the error. |
| Never switches to Claude mode | Claude mode only shows while Claude is working on a reply, not while a chat sits idle. Send a message, then look in `~/.claude/sessions/`: the session's `.json` file should say `"status":"busy"` until the reply finishes, and `tools/pc_link.log` should print `Claude sessions working: 1`. |
| Upload picks the wrong COM port | Set `upload_port` / `monitor_port` in `platformio.ini`, e.g. `upload_port = COM7`. |

## Demos (earlier versions)

The `demos/` folder keeps an earlier firmware, written for the **Arduino Uno**. PlatformIO only builds `src/`, so to try it, copy it over `src/main.cpp` (keep a copy of the current one first) and upload.

- **`animations.cpp`** — a standalone loop of six effects: warp starfield, dithered plasma, spinning 3D cubes, fireworks, Matrix rain and a hypnotic tunnel. No PC needed.

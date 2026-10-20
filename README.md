# OLED Desk Companion

An Arduino Uno with a 1.3" OLED that sits on your desk and shows:

- **a big animated clock** with rolling digits and the date,
- **expressive eyes** that play little scenes (happy, sleepy, angry, dizzy…),
- **Claude mode**: while Claude Code is working on a reply on your PC, Claude "thinking" animations and your **5-hour and weekly usage**.

A small Python script on the PC (`tools/pc_link.py`) feeds the Arduino the time, whether Claude Code is working, and your usage. It starts automatically in the background when you log in.

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
| Arduino Uno (or Nano) | ATmega328P, 2 KB RAM, 32 KB flash |
| 1.3" I2C OLED, 128×64 | SH1106 controller (most 1.3" modules). 0.96" modules usually use SSD1306 — see [Troubleshooting](#troubleshooting) |
| USB cable | Powers the Arduino and carries data from the PC |

## Wiring

| OLED pin | Arduino |
|----------|---------|
| VCC | 5V (or 3.3V) |
| GND | GND |
| SDA | **A4** |
| SCL | **A5** |

Pin order printed on OLED modules varies (`GND VCC SCL SDA` vs `VCC GND SCL SDA`) — follow the labels on your board, not the position.

## Software setup

### 1. Firmware (PlatformIO)

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE** extension.
2. Open this folder in VS Code.
3. Plug in the Arduino. The project is set to **COM5** in `platformio.ini` (`upload_port` / `monitor_port`) — change it if Windows gave your board a different port (Device Manager → Ports).
4. Click **✓ Build**, then **→ Upload** in the PlatformIO toolbar.

The only library, [U8g2](https://github.com/olikraus/u8g2), is installed automatically.

### 2. PC script (Python)

Requires Python 3 on Windows.

```powershell
pip install pyserial
python tools\pc_link.py          # run it once in a terminal to try it
```

It finds the Arduino by itself (or pass `--port COM5`). You should see something like:

```
Connected to COM5
13:29:40 Claude sessions working: 1
13:29:41 usage: 5h 56%, week 18%
```

Without the script the Arduino shows the eyes with *"waiting for PC time"* underneath.

## Running it all the time

Install it as a background task that starts every time you log in:

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_autostart.ps1
```

This creates a Windows Task Scheduler task called **"OLED PC Link"** (for your user only, no admin rights needed). It:

- runs `pc_link.py` with `pythonw.exe`, so there's **no window**;
- writes its messages to **`tools/pc_link.log`** (rotated at ~200 KB);
- keeps retrying if the Arduino is unplugged, the PC sleeps, or anything unexpected happens, and Task Scheduler restarts it if it ever exits.

| To… | Run |
|-----|-----|
| Remove auto-start | `powershell -ExecutionPolicy Bypass -File tools\install_autostart.ps1 -Uninstall` |
| Stop it for now | `schtasks /End /TN "OLED PC Link"` |
| Start it again | `schtasks /Run /TN "OLED PC Link"` |

**Uploading firmware while it runs:** nothing to do. `tools/pio_pause_link.py` is hooked into PlatformIO: it stops the task before an upload, waits until the COM port is free, and restarts the task afterwards (even if the upload failed).

Only one program can use the COM port at a time, so while the link is running you can't also open PlatformIO's Serial Monitor.

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

Shown for 10 seconds at a time. The time comes from the PC every 10 seconds; between syncs the Arduino counts on its own.

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

Verbs: THINKING, PONDERING, NOODLING, CLAUDING, BREWING, MUSING, COGITATING, CONJURING.

### Usage page (Claude working)

Two rings: **5-hour limit** on the left, **weekly limit** on the right. The percentage in each ring counts up when the page appears, and every 2 seconds the labels swap with the time until each limit resets (e.g. `2H31M`). If the usage can't be read, the rings show `--`.

## How the PC link works

`tools/pc_link.py` sends plain text lines over USB serial at 115200 baud:

| Line | Meaning | How often |
|------|---------|-----------|
| `T1792240000` | local time as seconds since 1970 (time zone already applied) | every 10 s, sent at the start of a second |
| `C1,56,18,151,1082` | `C<working>,<5h %>,<week %>,<5h reset in minutes>,<week reset in minutes>` | every 2 s |

A usage value of `255` means "unknown". If the Arduino hears no `C` line for 15 seconds it treats Claude as not working.

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

Other easy changes:

- **Remove an eye scene:** delete its name from the `ACTS[]` list (or list one twice to make it more common).
- **Change the verbs:** edit the `VERBS` string (words separated by `\0`, uppercase, max ~10 letters).
- **Script timing:** `TIME_EVERY`, `STATUS_EVERY` and `USAGE_EVERY` at the top of `tools/pc_link.py`.

## Project structure

```
platformio.ini            board, library, COM port, upload hook, RAM-saving build flags
src/main.cpp              the firmware
tools/
  pc_link.py              PC → Arduino link: time, Claude activity, usage
  install_autostart.ps1   installs/removes the "OLED PC Link" background task
  pio_pause_link.py       PlatformIO hook: pauses the link during uploads
  pc_link.log             log written by the background task
  status_sender.py        PC script for the old status-face demo (see below)
  requirements.txt        Python packages (pyserial; pynput is only for status_sender.py)
demos/
  animations.cpp          earlier "crazy animations" demo
  status_face.cpp         earlier typing/mouse status face
```

### Firmware layout (`src/main.cpp`)

| Section | Purpose |
|---------|---------|
| Timekeeping | local time from the PC + `millis()`, calendar date calculation |
| Serial from the PC | parses the `T` and `C` lines |
| Clock page | rolling digits, blinking colon, date bar |
| Eyes | keyframe "acts" the eyes glide between, and their drawing styles |
| Claude's spark | the 12-ray spark built from tapered "rays" (triangles with round tips) |
| Claude animations | the five animations, each split into *prepare* (maths, once per frame) and *draw* |
| Usage page | the two progress rings |
| Page sequencing | which page comes next, and the blink transitions |

Each frame, `update()` advances timers and works out the picture once, then `render()` draws it.

## Memory notes

The Uno is tight: **2 KB RAM** and **32 KB flash**. A few choices keep it stable:

- **Page mode display buffer.** The OLED is drawn in 4 strips through a 256-byte buffer (`U8G2_..._2_HW_I2C`) instead of a full 1 KB buffer. An earlier full-buffer version crashed from running out of stack. Because of this, drawing code must not change state — that's why logic lives in `update()` and drawing in `render()`.
- **Smaller serial buffers** (`-DSERIAL_RX_BUFFER_SIZE=32 -DSERIAL_TX_BUFFER_SIZE=16` in `platformio.ini`).
- **Few fonts.** Only three fonts are used: Logisoso 32 (digits only), Logisoso 16, and 5×7. The sleeping `Z`s are drawn with lines rather than extra fonts.
- **No `sprintf`/`strtoul`.** Small hand-written helpers replace them, saving over 1 KB of flash.

Current usage is about **67% RAM** and **89% flash**. There's room for small tweaks; bigger additions would need something removed or a board with more memory (Arduino Mega, Nano Every, ESP32 — the code is plain Arduino + U8g2 and ports easily).

Frame rates: eyes ~20 fps, clock and Claude animations ~12 fps.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| Screen stays blank | Check wiring: SDA→A4, SCL→A5, VCC/GND not swapped. Most modules are at I2C address `0x3C`; some use `0x3D` (change `setI2CAddress` in `setup()`). |
| Image shifted, garbled, or a stripe of noise on one edge | Your module has an SSD1306 chip: replace the display line with `U8G2_SSD1306_128X64_NONAME_2_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);` |
| Screen freezes, flickers or blanks out | Usually a loose wire or unsoldered header pins. Press the wires in, solder the header, keep jumpers short, and try `I2C_CLOCK = 100000`. |
| Shows "waiting for PC time" | The PC link isn't running: `schtasks /Run /TN "OLED PC Link"`, or check `tools/pc_link.log`. |
| Upload fails with "Access is denied" on COM5 | Something else has the port open — usually a Serial Monitor or a `pc_link.py` started by hand in a terminal. Close it and upload again. |
| Usage rings show `--` | The usage couldn't be read (offline, login expired, or the endpoint changed). Open Claude Code once to refresh the login; check `tools/pc_link.log` for the error. |
| Never switches to Claude mode | Claude mode only shows while Claude is working on a reply, not while a chat sits idle. Send a message, then look in `~/.claude/sessions/`: the session's `.json` file should say `"status":"busy"` until the reply finishes, and `tools/pc_link.log` should print `Claude sessions working: 1`. |
| Different COM port | Change `upload_port` / `monitor_port` in `platformio.ini`. The PC script auto-detects. |

## Demos (earlier versions)

The `demos/` folder keeps earlier versions of the firmware. PlatformIO only builds `src/`, so to try one, copy it over `src/main.cpp` (keep a copy of the current one first) and upload.

- **`animations.cpp`** — a standalone loop of six effects: warp starfield, dithered plasma, spinning 3D cubes, fireworks, Matrix rain and a hypnotic tunnel. No PC needed.
- **`status_face.cpp`** — a face that reacts to your keyboard and mouse: focused eyes while typing, eyes that follow the mouse, winks on clicks, sleeping when you're away. Needs `tools/status_sender.py` (`pip install pyserial pynput`), which counts key presses but never records which keys. These demos were written for the older full-buffer display mode and may need trimming to fit alongside newer code.

"""Link between this PC and the ESP32 OLED: time, Claude activity and usage.

Sends UDP packets over WiFi to port 4210 of the ESP32:
  T<unix seconds>,<checksum>                        local time, every 10 s
  C<working>,<5h %>,<week %>,<5h reset min>,<week reset min>,<checksum>   every 2 s

<checksum> is the sum of the line's characters before it, mod 256; the OLED
ignores lines that don't add up.

<working> counts the Claude Code sessions busy on a reply right now. Each
session keeps ~/.claude/sessions/<pid>.json up to date with a "status" of
"busy", "idle" or "waiting" (waiting on you, e.g. a permission prompt); only
"busy" sessions whose process is still running count. A chat that is open but
idle doesn't.

Usage limits come from the same endpoint Claude Code's /usage command uses,
read with your saved Claude Code login (only sent to Anthropic's API, at most
every few minutes, and only while Claude is working). Neither the session files
nor that endpoint are an official API, so a future Claude Code update could
change them.

Usage:
    python tools/pc_link.py                      # finds the ESP32 as claude-oled.local
    python tools/pc_link.py --host 192.168.1.42  # or send to a fixed IP

The ESP32 answers to claude-oled.local (mDNS, built into Windows 10/11), so
its IP can change freely. The OLED shows its IP until the first time sync
arrives.

To run it all the time in the background, see tools/install_autostart.ps1.
When started without a console (pythonw), messages go to tools/pc_link.log.
"""

import argparse
import asyncio
import ctypes
import json
import socket
import sys
import time
import traceback
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ESP32_HOST = "claude-oled.local"  # the ESP32's mDNS name (HOSTNAME in src/main.cpp)
LINK_PORT = 4210   # UDP port the ESP32 listens on (LINK_PORT in src/main.cpp)

TIME_EVERY = 10    # seconds between time syncs
STATUS_EVERY = 2   # seconds between Claude status updates
USAGE_EVERY = 180  # seconds between usage checks
LOGIN_CHECK_EVERY = 30  # while the login is expired, re-read the (local) credentials file this often
BACKOFF_MAX = 1800      # longest wait after repeated HTTP 429 errors

CLAUDE_DIR = Path.home() / ".claude"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"

kernel32 = ctypes.windll.kernel32
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259


def process_running(pid):
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    try:
        code = ctypes.c_ulong()
        return bool(kernel32.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


def count_working():
    """Claude Code sessions busy on a reply whose process is still running."""
    count = 0
    for f in (CLAUDE_DIR / "sessions").glob("*.json"):
        try:
            info = json.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError):  # missing, or caught mid-write
            continue
        pid = info.get("pid")
        if info.get("status") == "busy" and isinstance(pid, int) and process_running(pid):
            count += 1
    return count


def minutes_until(iso):
    if not iso:
        return 0
    reset = datetime.fromisoformat(iso)
    return max(0, int((reset - datetime.now(timezone.utc)).total_seconds() // 60))


class LoginExpired(Exception):
    """The saved Claude Code login has expired; sending it would only get HTTP 401."""


def fetch_usage():
    """Returns (5h %, week %, 5h reset minutes, week reset minutes)."""
    cred = json.loads((CLAUDE_DIR / ".credentials.json").read_text(encoding="utf-8"))
    oauth = cred["claudeAiOauth"]
    expires = oauth.get("expiresAt")  # milliseconds since 1970
    if isinstance(expires, (int, float)) and expires / 1000 <= time.time():
        raise LoginExpired(datetime.fromtimestamp(expires / 1000).strftime("%d %b %H:%M"))
    token = oauth["accessToken"]
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": "Bearer " + token,
        "anthropic-beta": "oauth-2025-04-20",
        "Content-Type": "application/json",
    })
    with urllib.request.urlopen(req, timeout=10) as r:
        data = json.load(r)
    five, week = data.get("five_hour") or {}, data.get("seven_day") or {}
    return (round(five.get("utilization") or 0), round(week.get("utilization") or 0),
            minutes_until(five.get("resets_at")), minutes_until(week.get("resets_at")))


def local_unix_time():
    """Seconds since 1970 in local time (the Arduino doesn't know time zones)."""
    now = time.time()
    return int(now + time.localtime(now).tm_gmtoff)


def with_checksum(line):
    """Adds ",<sum of the characters mod 256>": the ESP32 drops lines garbled on the way."""
    return f"{line},{sum(line.encode()) % 256}"


class LinkDown(Exception):
    """Sending failed; the link reconnects before the next message."""


class UdpLink:
    """The ESP32, as UDP packets over WiFi."""

    def __init__(self, host):
        self.host = host
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.targets, self.targets_at = [], 0.0

    @staticmethod
    def broadcast_addresses():
        """With several network adapters (WSL, Hyper-V, VPN), Windows sends 255.255.255.255
        out of only one of them, often the wrong one. So also send to each local network's
        own broadcast address (assuming the usual /24, e.g. 192.168.0.255), which Windows
        routes out of the right adapter."""
        try:
            ips = {a[4][0] for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)}
        except OSError:
            ips = set()
        nets = {ip.rsplit(".", 1)[0] + ".255" for ip in ips if not ip.startswith(("127.", "169.254."))}
        return ["255.255.255.255"] + sorted(nets)

    def find_targets(self):
        """Looks the ESP32 up by name (it answers to claude-oled.local over mDNS). If that
        fails, keeps the last address that worked; with none yet, broadcasts, which only
        reaches it if the router passes broadcasts on to WiFi devices (many don't)."""
        if self.host == "255.255.255.255":
            return self.broadcast_addresses()
        try:
            return [socket.gethostbyname(self.host)]
        except OSError:
            return self.targets or self.broadcast_addresses()

    async def current_targets(self):
        if time.time() - self.targets_at > 60:  # recheck every minute: its IP can change
            targets = await asyncio.to_thread(self.find_targets)  # a failed lookup takes seconds
            if targets != self.targets:
                print(f"{time.strftime('%H:%M:%S')} sending to {', '.join(targets)}")
            self.targets, self.targets_at = targets, time.time()
        return self.targets

    async def connect(self):
        print(f"Sending to {self.host}:{LINK_PORT} over UDP")

    async def send(self, data):
        sent, error = 0, None
        for target in await self.current_targets():
            try:
                self.sock.sendto(data, (target, LINK_PORT))
                sent += 1
            except OSError as e:  # e.g. that adapter is down
                error = e
        if not sent:  # no network at all (WiFi off, PC just woke up)
            raise LinkDown(error)

    async def reconnect(self):
        pass  # nothing to reconnect: the next send simply tries again


async def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=ESP32_HOST,
                        help=f"the ESP32's name or IP address, or 255.255.255.255 to broadcast (default: {ESP32_HOST})")
    args = parser.parse_args()

    link = UdpLink(args.host)
    await link.connect()
    send_problem = None  # last send error logged, so a repeating one is only logged once
    last_time = next_usage = 0.0
    usage = None  # (5h %, week %, 5h reset min, week reset min), fetched at time `usage_at`
    usage_at = 0.0
    backoff = USAGE_EVERY  # wait after the next HTTP 429; doubles each time, up to BACKOFF_MAX
    usage_problem = None   # last problem logged, so a repeating one is only logged once
    last_working = None

    while True:
        # Wake at the start of each second so the OLED's seconds tick in step
        await asyncio.sleep(1 - time.time() % 1)
        now = time.time()
        lines = []

        if now - last_time >= TIME_EVERY:
            lines.append(f"T{local_unix_time()}")
            last_time = now

        if int(now) % STATUS_EVERY == 0:
            working = count_working()
            if working != last_working:
                print(f"{time.strftime('%H:%M:%S')} Claude sessions working: {working}")
                last_working = working

            if working and now >= next_usage:
                next_usage = now + USAGE_EVERY
                problem = None
                try:
                    usage = await asyncio.to_thread(fetch_usage)
                    usage_at = now
                    backoff = USAGE_EVERY
                    print(f"{time.strftime('%H:%M:%S')} usage: 5h {usage[0]}%, week {usage[1]}%")
                except LoginExpired as e:
                    # Nothing is sent; check the file again soon so a refreshed login is picked up
                    next_usage = now + LOGIN_CHECK_EVERY
                    problem = (f"Claude Code login expired at {e}, not checking usage. "
                               "Run `claude` in a terminal (or use Claude Code) to refresh it.")
                except urllib.error.HTTPError as e:
                    if e.code == 429:
                        try:
                            wait = int(e.headers.get("Retry-After") or 0)
                        except ValueError:
                            wait = 0
                        wait = min(max(wait, backoff), BACKOFF_MAX)
                        backoff = min(backoff * 2, BACKOFF_MAX)
                        next_usage = now + wait
                        problem = f"usage rate-limited (HTTP 429), waiting {wait // 60} min"
                    elif e.code == 401:
                        problem = ("Claude Code login rejected (HTTP 401). "
                                   "Run `claude` in a terminal (or use Claude Code) to refresh it.")
                    else:
                        problem = f"couldn't read usage: {e}"
                except Exception as e:  # keep showing the last numbers
                    problem = f"couldn't read usage: {e}"
                # 429 waits change each time, so always log those; others only when they change
                if problem and (problem != usage_problem or problem.startswith("usage rate")):
                    print(f"{time.strftime('%H:%M:%S')} {problem}")
                usage_problem = problem

            if usage:
                gone = int((now - usage_at) // 60)  # count the reset timers down between checks
                five, week = usage[0], usage[1]
                five_min, week_min = max(0, usage[2] - gone), max(0, usage[3] - gone)
            else:
                five = week = 255  # unknown: the OLED shows "--"
                five_min = week_min = 0
            lines.append(f"C{working},{five},{week},{five_min},{week_min}")

        if not lines:
            continue
        try:
            await link.send("".join(with_checksum(line) + "\n" for line in lines).encode())
            send_problem = None
        except LinkDown as e:  # network down: keep trying
            if str(e) != send_problem:
                print(f"{time.strftime('%H:%M:%S')} lost the link: {e}")
                send_problem = str(e)
            await link.reconnect()
            last_time = 0  # resend the time as soon as the link is back


def log_to_file_if_hidden():
    """pythonw has no console, so send messages to a small log file instead."""
    if sys.stdout is None:
        log = Path(__file__).with_name("pc_link.log")
        if log.exists() and log.stat().st_size > 200_000:
            log.replace(log.with_suffix(".old.log"))
        sys.stdout = sys.stderr = open(log, "a", encoding="utf-8", buffering=1)


if __name__ == "__main__":
    log_to_file_if_hidden()
    while True:
        try:
            asyncio.run(main())
        except KeyboardInterrupt:
            break
        except Exception:  # never give up when running unattended: log it and start over
            print(f"{time.strftime('%H:%M:%S')} unexpected error, restarting in 10 s:")
            traceback.print_exc()
            time.sleep(10)

# PlatformIO hook: the background PC link holds the serial port, which would
# make uploads fail. So stop its task just before uploading, wait until the
# port is really free, and start the task again when PlatformIO finishes
# (even if the upload failed). Does nothing if the task isn't installed.
import atexit
import subprocess
import time

import serial

Import("env")  # noqa: F821  (provided by PlatformIO)

TASK = "OLED PC Link"


def schtasks(*args):
    return subprocess.run(["schtasks", *args, "/TN", TASK],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def wait_for_port(port, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        try:
            serial.Serial(port).close()
            return
        except serial.SerialException:
            time.sleep(0.3)


def pause_link(source, target, env):
    if schtasks("/End"):
        atexit.register(schtasks, "/Run")  # restart it once PlatformIO is done
        port = env.subst("$UPLOAD_PORT")
        if port:
            wait_for_port(port)


env.AddPreAction("upload", pause_link)  # noqa: F821

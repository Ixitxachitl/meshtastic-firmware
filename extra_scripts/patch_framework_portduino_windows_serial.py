#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
import filecmp
import os
import shutil

Import("env")

# framework-portduino's LinuxSerial (cores/portduino/linux/LinuxSerial.{h,cpp}) ships a
# Windows stub: Serial1.setPath() is accepted but begin()/read()/write() are all no-ops,
# so `GPS: SerialPath: COMn` in config.yaml silently does nothing on native-windows(-tft).
# Until that lands upstream in meshtastic/framework-portduino, drop in a real Win32
# CreateFile/DCB/COMMTIMEOUTS backend by overwriting the two files with our patched
# copies before compiling. Idempotent: skips the copy once the files already match.

if env["PIOENV"].startswith("native-windows"):
    framework_dir = env.PioPlatform().get_package_dir("framework-portduino")
    if not framework_dir:
        raise RuntimeError("native-windows needs framework-portduino, but PlatformIO hasn't installed it yet")

    patch_root = os.path.join(env["PROJECT_DIR"], "extra_scripts", "patches", "framework-portduino")
    target_root = os.path.join(framework_dir)

    for relpath in (
        os.path.join("cores", "portduino", "linux", "LinuxSerial.h"),
        os.path.join("cores", "portduino", "linux", "LinuxSerial.cpp"),
    ):
        src = os.path.join(patch_root, relpath)
        dst = os.path.join(target_root, relpath)
        if not os.path.isfile(dst):
            raise RuntimeError(f"native-windows serial patch: expected {dst} in framework-portduino, but it's missing")
        if not os.path.isfile(src) or not filecmp.cmp(src, dst, shallow=False):
            shutil.copyfile(src, dst)
            print(f"Patched framework-portduino: {relpath} (Windows CreateFile/DCB serial backend)")

#!/usr/bin/env python3
"""Turn userPrefs.jsonc into a force-included header.

bin/platformio-custom.py passes these as -D flags; going through a header
instead keeps values with quotes, braces and commas intact under CMake.
"""

import json
import re
import sys

src, dst = sys.argv[1], sys.argv[2]

with open(src, "r") as f:
    text = f.read()

# Same JSONC handling as bin/platformio-custom.py: drop // comments, then parse.
text = re.sub(r"//.*$", "", text, flags=re.MULTILINE)
prefs = json.loads(text)

lines = ["// Generated from userPrefs.jsonc - do not edit.", "#pragma once", ""]
for key, value in prefs.items():
    if (
        value.startswith("{")
        or value.lstrip("-").replace(".", "").isdigit()
        or value in ("true", "false")
        or value.startswith("meshtastic_")
    ):
        literal = value
    else:
        literal = '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
    lines.append(f"#ifndef {key}")
    lines.append(f"#define {key} {literal}")
    lines.append("#endif")

with open(dst, "w") as f:
    f.write("\n".join(lines) + "\n")

#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
import os

Import("env")

# PNGdec vendors a copy of zlib's inflate.c. Its inflateSetDictionary() declares
# dictLength as `uint`, a BSD/glibc typedef that MinGW's headers don't provide -
# every other zlib symbol in the same file correctly uses zlib's own `uInt`.
# Building native-windows-tft (which pulls in PNGdec via device-ui_base) fails
# with "unknown type name 'uint'" until this one-line typo is fixed upstream.
# Patch the installed libdep in place before compiling; idempotent since it only
# rewrites the line when the bad signature is still present.

if env["PIOENV"].startswith("native-windows"):
    inflate_c = os.path.join(env["PROJECT_LIBDEPS_DIR"], env["PIOENV"], "PNGdec", "src", "inflate.c")
    if os.path.isfile(inflate_c):
        bad = "int ZEXPORT inflateSetDictionary(z_streamp strm, const Bytef *dictionary, uint dictLength)"
        good = "int ZEXPORT inflateSetDictionary(z_streamp strm, const Bytef *dictionary, uInt dictLength)"
        with open(inflate_c, "r") as f:
            contents = f.read()
        if bad in contents:
            with open(inflate_c, "w") as f:
                f.write(contents.replace(bad, good))
            print("Patched PNGdec/src/inflate.c: uint -> uInt in inflateSetDictionary() (MinGW has no bare uint)")

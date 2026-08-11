#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
import os
import shutil
import subprocess

Import("env")


def _first_existing(*candidates):
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate
    return None


def _cmake_path(path):
    # CMake writes -D values straight into generated .cmake cache files as quoted
    # cmake-language strings, where backslash is an escape char. A raw Windows path
    # like C:\msys64\mingw32\bin\gcc.exe round-trips to "Invalid character escape
    # '\m'" the moment CMake re-parses its own CMakeCCompiler.cmake. Forward slashes
    # are accepted natively on Windows and sidestep the whole escaping problem.
    return path.replace("\\", "/")


def _ensure_local_argp(project_dir, msys_bin, msys_label, build_subdir):
    argp_root = os.path.join(project_dir, "argp-standalone")
    argp_include = os.path.join(argp_root, "include", "argp-standalone")
    argp_header = os.path.join(argp_include, "argp.h")
    argp_build_dir = os.path.join(argp_root, build_subdir)
    argp_lib = os.path.join(argp_build_dir, "src", "libargp-standalone.a")
    if not os.path.isfile(argp_header):
        return None, None

    if not os.path.isfile(argp_lib):
        cmake = _first_existing(
            shutil.which("cmake"),
            r"C:\Program Files\CMake\bin\cmake.exe",
        )
        ninja = _first_existing(
            shutil.which("ninja"),
            os.path.join(msys_bin, "ninja.exe"),
            r"C:\mingw64\bin\ninja.exe",
        )
        gcc = os.path.join(msys_bin, "gcc.exe")
        gxx = os.path.join(msys_bin, "g++.exe")
        if not (cmake and ninja and os.path.isfile(gcc) and os.path.isfile(gxx)):
            raise RuntimeError(
                f"native-windows needs argp-standalone, but its local checkout is not built and CMake/Ninja/{msys_label} GCC were not found"
            )

        subprocess.check_call(
            [
                cmake,
                "-S",
                argp_root,
                "-B",
                argp_build_dir,
                "-G",
                "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={_cmake_path(ninja)}",
                f"-DCMAKE_C_COMPILER={_cmake_path(gcc)}",
                f"-DCMAKE_CXX_COMPILER={_cmake_path(gxx)}",
                "-DCMAKE_BUILD_TYPE=Release",
            ],
            cwd=project_dir,
        )
        subprocess.check_call([cmake, "--build", argp_build_dir], cwd=project_dir)

    if os.path.isfile(argp_lib):
        return argp_include, argp_lib
    return argp_include, None


def _patch_yamlcpp_missing_cstdint(yamlcpp_root):
    # yaml-cpp 0.8.0's emitterutils.cpp uses uint16_t/uint32_t without including <cstdint>,
    # relying on some other header to drag it in transitively. That happens to hold on the
    # UCRT64 toolchain (hence no pacman package needs this patch - see below) but not on
    # MINGW32's GCC 16, which fails the whole native-windows-tft-i686 build over a missing
    # header from an upstream file we don't otherwise touch.
    path = os.path.join(yamlcpp_root, "src", "emitterutils.cpp")
    if not os.path.isfile(path):
        return
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    if "#include <cstdint>" in content:
        return
    patched = content.replace("#include <algorithm>\n", "#include <algorithm>\n#include <cstdint>\n", 1)
    if patched != content:
        with open(path, "w", encoding="utf-8") as f:
            f.write(patched)


def _ensure_local_yamlcpp_i686(project_dir, msys_bin):
    # MSYS2 doesn't package yaml-cpp for the 32-bit MINGW32 environment (only 248 mingw32
    # packages exist at all vs. ~3900 for UCRT64), so native-windows-tft-i686 builds it from
    # an upstream checkout the same way native-windows builds argp-standalone above.
    yamlcpp_root = os.path.join(project_dir, "yaml-cpp-i686")
    yamlcpp_include = os.path.join(yamlcpp_root, "include")
    yamlcpp_header = os.path.join(yamlcpp_include, "yaml-cpp", "yaml.h")
    yamlcpp_build_dir = os.path.join(yamlcpp_root, "build")
    yamlcpp_lib = os.path.join(yamlcpp_build_dir, "libyaml-cpp.a")
    if not os.path.isfile(yamlcpp_header):
        return None, None

    if not os.path.isfile(yamlcpp_lib):
        cmake = _first_existing(
            shutil.which("cmake"),
            r"C:\Program Files\CMake\bin\cmake.exe",
        )
        ninja = _first_existing(
            shutil.which("ninja"),
            os.path.join(msys_bin, "ninja.exe"),
            r"C:\mingw64\bin\ninja.exe",
        )
        gcc = os.path.join(msys_bin, "gcc.exe")
        gxx = os.path.join(msys_bin, "g++.exe")
        if not (cmake and ninja and os.path.isfile(gcc) and os.path.isfile(gxx)):
            raise RuntimeError(
                "native-windows-tft-i686 needs yaml-cpp-i686 built, but CMake/Ninja/MINGW32 GCC were not found"
            )

        _patch_yamlcpp_missing_cstdint(yamlcpp_root)
        subprocess.check_call(
            [
                cmake,
                "-S",
                yamlcpp_root,
                "-B",
                yamlcpp_build_dir,
                "-G",
                "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={_cmake_path(ninja)}",
                f"-DCMAKE_C_COMPILER={_cmake_path(gcc)}",
                f"-DCMAKE_CXX_COMPILER={_cmake_path(gxx)}",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DYAML_CPP_BUILD_TESTS=OFF",
                "-DYAML_CPP_BUILD_TOOLS=OFF",
                "-DYAML_CPP_BUILD_CONTRIB=OFF",
                "-DYAML_BUILD_SHARED_LIBS=OFF",
                "-DYAML_CPP_INSTALL=OFF",
            ],
            cwd=project_dir,
        )
        subprocess.check_call([cmake, "--build", yamlcpp_build_dir], cwd=project_dir)

    if os.path.isfile(yamlcpp_lib):
        return yamlcpp_include, yamlcpp_lib
    return yamlcpp_include, None


if env["PIOENV"].startswith("native-windows"):
    project_dir = env["PROJECT_DIR"]
    # native-windows-tft-i686 targets 32-bit Windows via MSYS2's MINGW32 environment
    # (i686-w64-mingw32); every other native-windows* env uses the 64-bit UCRT64 one.
    # These are separate MSYS2 environments/shells with their own package sets, not
    # just separate compiler flags - see the prereq comments on each env in
    # variants/native/portduino/platformio.ini.
    is_i686 = env["PIOENV"].endswith("-i686")
    msys_root = r"C:\msys64\mingw32" if is_i686 else r"C:\msys64\ucrt64"
    msys_label = "MINGW32" if is_i686 else "UCRT64"
    msys_bin = os.path.join(msys_root, "bin")
    msys_lib = os.path.join(msys_root, "lib")
    msys_include = os.path.join(msys_root, "include")
    if not os.path.isdir(msys_bin) or not os.path.isdir(msys_lib):
        raise RuntimeError(f"{env['PIOENV']} requires the MSYS2 {msys_label} toolchain at {msys_root}")

    env.PrependENVPath("PATH", msys_bin)
    # env.PrependENVPath only updates SCons' own env["ENV"] dict, used when SCons spawns
    # build actions. It's invisible to plain subprocess.run() calls elsewhere in the build -
    # notably platform-native's arduino.py framework loader, which probes `$CC -E -xc -` to
    # find the real <string.h> for its case-shadowing shim (see the "pre:" comment on this
    # script's entry in platformio.ini for why that probe needs mingw32/bin ahead of
    # ucrt64/bin on the *real* PATH for -i686, not just SCons' copy of it).
    os.environ["PATH"] = msys_bin + os.pathsep + os.environ.get("PATH", "")
    env.Replace(
        AR=os.path.join(msys_bin, "ar.exe"),
        AS=os.path.join(msys_bin, "as.exe"),
        CC=os.path.join(msys_bin, "gcc.exe"),
        CXX=os.path.join(msys_bin, "g++.exe"),
        GDB=os.path.join(msys_bin, "gdb.exe"),
        RANLIB=os.path.join(msys_bin, "ranlib.exe"),
    )
    env.AppendUnique(LIBPATH=[msys_lib])
    env.AppendUnique(CPPPATH=[msys_include])
    # Keep the 32-bit argp-standalone build in its own directory: its CMakeCache.txt
    # pins the MINGW32 compiler paths, and reusing "build" would collide with (or
    # silently reuse the wrong-arch output of) the UCRT64 build done for the other
    # native-windows* envs.
    argp_build_subdir = "build-i686" if is_i686 else "build"
    argp_include, argp_lib = _ensure_local_argp(project_dir, msys_bin, msys_label, argp_build_subdir)
    if argp_include and argp_lib:
        env.AppendUnique(CPPPATH=[argp_include])
        env.AppendUnique(LIBPATH=[os.path.dirname(argp_lib)])
        env.AppendUnique(LIBS=["argp-standalone"])
    elif os.path.isfile(os.path.join(msys_include, "argp.h")) and os.path.isfile(os.path.join(msys_lib, "libargp.a")):
        env.AppendUnique(CPPPATH=[msys_include])
        env.AppendUnique(LIBS=["argp"])
    else:
        raise RuntimeError(
            f"native-windows requires argp. Keep a buildable argp-standalone checkout at <repo>/argp-standalone or install argp.h/libargp.a into {msys_root}."
        )

    if is_i686:
        # yaml-cpp has no MINGW32 package (see _ensure_local_yamlcpp_i686) - unlike argp above,
        # there's no "install it into the MSYS2 prefix" fallback, so a missing checkout is fatal.
        yamlcpp_include, yamlcpp_lib = _ensure_local_yamlcpp_i686(project_dir, msys_bin)
        if yamlcpp_include and yamlcpp_lib:
            env.AppendUnique(CPPPATH=[yamlcpp_include])
            env.AppendUnique(LIBPATH=[os.path.dirname(yamlcpp_lib)])
        else:
            raise RuntimeError(
                "native-windows-tft-i686 requires yaml-cpp, which MSYS2 doesn't package for MINGW32. "
                "Clone https://github.com/jbeder/yaml-cpp into <repo>/yaml-cpp-i686 - this script builds "
                "it automatically from there on the next run."
            )

    # Let the MSYS2 toolchain pick its default runtime/import libraries. Forcing a
    # fully static CRT on Windows conflicts with Portduino's itoa shim and drags in
    # the wrong wide-char CRT symbol flavor for libstdc++.

    # Embed meshtastic.ico (the Android app's launcher icon) as the .exe's file/taskbar
    # icon. Built eagerly here - same pattern as _ensure_local_argp() above - so the
    # object file already exists by the time SCons links the program.
    windres_dir = os.path.join(project_dir, "src", "platform", "portduino", "windows")
    rc_path = os.path.join(windres_dir, "meshtastic.rc")
    build_dir = env.subst("$BUILD_DIR")
    res_obj = os.path.join(build_dir, "meshtastic_rc.o")
    if os.path.isfile(rc_path):
        os.makedirs(build_dir, exist_ok=True)
        windres = os.path.join(msys_bin, "windres.exe")
        subprocess.check_call([windres, "-I", windres_dir, rc_path, "-O", "coff", "-o", res_obj])
        env.Append(LINKFLAGS=[res_obj])

    if env["PIOENV"] in ("native-windows-tft", "native-windows-tft-i686"):
        # GUI subsystem: no console window is auto-created at all (double-clicking meshtasticd.exe
        # from Explorer opens only the SDL window, nothing to hide/minimize after the fact).
        # PortduinoGlue.cpp's portduinoWindowsConsoleAttachToParent() reattaches to the launching
        # terminal's console when there is one, so the dev workflow (running from a shell) is
        # unaffected. Not applied to plain native-windows: that build is a headless daemon with no
        # window at all, so its console must never be suppressed.
        env.Append(LINKFLAGS=["-mwindows"])

    def _bundle_runtime_dlls(target, source, env):
        # meshtasticd.exe dynamically links against MSYS2-provided DLLs (SDL2, curl, ssl, crypto,
        # libstdc++, libwinpthread, ...) - it is NOT statically linked (see the comment above about
        # Portduino's itoa shim). Without this, running the exe outside a shell that happens to have
        # the matching MSYS2 bin dir on PATH fails with "not a valid Win32 application": Windows'
        # DLL search finds a same-named but wrong-architecture DLL first (e.g. the 64-bit UCRT64
        # SDL2.dll shadowing the 32-bit MINGW32 one for native-windows-tft-i686, since UCRT64 is
        # commonly on PATH already but MINGW32 usually isn't). Copying the exact DLLs this toolchain
        # produced next to the exe makes it self-contained regardless of the caller's PATH.
        #
        # Unlike PlatformIO's generic embedded-target builders (which define a "buildprog" alias -
        # see nrf52_lto.py), the meshtastic/platform-native package's builder/main.py just calls
        # env.Program(env.subst("$PROGPATH"), ...) directly with no alias. AddPostAction must
        # target that exact same $PROGPATH string, or it silently attaches to a node that's never
        # part of the actual build DAG (observed: no error, the action just never runs).
        prog_path = env.subst("$PROGPATH")
        if not os.path.isfile(prog_path):
            return
        dest_dir = os.path.dirname(prog_path)
        objdump = os.path.join(msys_bin, "objdump.exe")
        if not os.path.isfile(objdump):
            return

        to_scan = [prog_path]
        scanned = set()
        copied = set()
        while to_scan:
            current = to_scan.pop()
            if current in scanned:
                continue
            scanned.add(current)
            try:
                out = subprocess.check_output([objdump, "-p", current], stderr=subprocess.DEVNULL, text=True)
            except (subprocess.CalledProcessError, OSError):
                continue
            for line in out.splitlines():
                line = line.strip()
                if not line.startswith("DLL Name:"):
                    continue
                dll_name = line.split(":", 1)[1].strip()
                if dll_name in copied:
                    continue
                src = os.path.join(msys_bin, dll_name)
                if not os.path.isfile(src):
                    continue # not MSYS2-provided (Windows system DLL, or a virtual API-set DLL) - skip
                shutil.copyfile(src, os.path.join(dest_dir, dll_name))
                copied.add(dll_name)
                to_scan.append(src) # this DLL may itself depend on further MSYS2 DLLs

        if copied:
            print(f"windows_link_flags.py: bundled {len(copied)} runtime DLL(s) next to {os.path.basename(prog_path)}")

    env.AddPostAction("$PROGPATH", _bundle_runtime_dlls)

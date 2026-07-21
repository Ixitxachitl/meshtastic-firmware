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


def _ensure_local_argp(project_dir):
    argp_root = os.path.join(project_dir, "argp-standalone")
    argp_include = os.path.join(argp_root, "include", "argp-standalone")
    argp_header = os.path.join(argp_include, "argp.h")
    argp_build_dir = os.path.join(argp_root, "build")
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
            r"C:\msys64\ucrt64\bin\ninja.exe",
            r"C:\mingw64\bin\ninja.exe",
        )
        gcc = r"C:\msys64\ucrt64\bin\gcc.exe"
        gxx = r"C:\msys64\ucrt64\bin\g++.exe"
        if not (cmake and ninja and os.path.isfile(gcc) and os.path.isfile(gxx)):
            raise RuntimeError(
                "native-windows needs argp-standalone, but its local checkout is not built and CMake/Ninja/UCRT64 GCC were not found"
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
                f"-DCMAKE_MAKE_PROGRAM={ninja}",
                f"-DCMAKE_C_COMPILER={gcc}",
                f"-DCMAKE_CXX_COMPILER={gxx}",
                "-DCMAKE_BUILD_TYPE=Release",
            ],
            cwd=project_dir,
        )
        subprocess.check_call([cmake, "--build", argp_build_dir], cwd=project_dir)

    if os.path.isfile(argp_lib):
        return argp_include, argp_lib
    return argp_include, None


if env["PIOENV"].startswith("native-windows"):
    project_dir = env["PROJECT_DIR"]
    ucrt_bin = r"C:\msys64\ucrt64\bin"
    ucrt_lib = r"C:\msys64\ucrt64\lib"
    ucrt_include = r"C:\msys64\ucrt64\include"
    if not os.path.isdir(ucrt_bin) or not os.path.isdir(ucrt_lib):
        raise RuntimeError("native-windows requires the MSYS2 UCRT64 toolchain at C:\\msys64\\ucrt64")

    env.PrependENVPath("PATH", ucrt_bin)
    env.Replace(
        AR=os.path.join(ucrt_bin, "ar.exe"),
        AS=os.path.join(ucrt_bin, "as.exe"),
        CC=os.path.join(ucrt_bin, "gcc.exe"),
        CXX=os.path.join(ucrt_bin, "g++.exe"),
        GDB=os.path.join(ucrt_bin, "gdb.exe"),
        RANLIB=os.path.join(ucrt_bin, "ranlib.exe"),
    )
    env.AppendUnique(LIBPATH=[ucrt_lib])
    env.AppendUnique(CPPPATH=[ucrt_include])
    argp_include, argp_lib = _ensure_local_argp(project_dir)
    if argp_include and argp_lib:
        env.AppendUnique(CPPPATH=[argp_include])
        env.AppendUnique(LIBPATH=[os.path.dirname(argp_lib)])
        env.AppendUnique(LIBS=["argp-standalone"])
    elif os.path.isfile(r"C:\msys64\ucrt64\include\argp.h") and os.path.isfile(r"C:\msys64\ucrt64\lib\libargp.a"):
        argp_include = r"C:\msys64\ucrt64\include"
        env.AppendUnique(CPPPATH=[argp_include])
        env.AppendUnique(LIBS=["argp"])
    else:
        raise RuntimeError(
            "native-windows requires argp. Keep a buildable argp-standalone checkout at <repo>/argp-standalone or install argp.h/libargp.a into C:\\msys64\\ucrt64."
        )

    # Let the UCRT64 toolchain pick its default runtime/import libraries.
    # Forcing a fully static CRT on Windows conflicts with Portduino's itoa shim
    # and drags in the wrong wide-char CRT symbol flavor for libstdc++.

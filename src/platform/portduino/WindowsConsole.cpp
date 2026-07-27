#if defined(ARCH_PORTDUINO) && defined(_WIN32)

// This build links with the Windows (GUI) subsystem (see windows_link_flags.py's -mwindows for
// native-windows-tft), so the OS never auto-creates a console window for this process at all -
// there's nothing to hide/minimize after the fact. Isolated TU, same reasoning as
// WindowsMacAddr.cpp: NOUSER (set repo-wide so Arduino's PinMode doesn't collide with winuser.h's
// INPUT struct) would otherwise strip AttachConsole/AllocConsole out of <windows.h>.
#undef WIN32_LEAN_AND_MEAN
#undef NOUSER
#undef NOGDI

#include <windows.h>

#include <cstdio>

static bool consoleAttached = false;

static void reopenStdio()
{
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    freopen("CONIN$", "r", stdin);
}

// Call once, as early as possible. Reattaches to the launching terminal's console (dev workflow:
// running meshtasticd.exe from an existing shell) so output appears there exactly as it would with
// the console subsystem. Returns false when there's no parent console to attach to - e.g.
// double-clicked from Explorer - in which case the process stays silent/windowless until/unless
// portduinoWindowsConsoleAllocIfNeeded() is called.
bool portduinoWindowsConsoleAttachToParent()
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        reopenStdio();
        consoleAttached = true;
    }
    return consoleAttached;
}

// Opens a fresh console window on demand (General.ShowConsole in config.yaml). No-op if a parent
// console is already attached. Log lines written before this call has nothing to attach to are
// lost, same tradeoff any GUI-subsystem app makes when it defers console allocation.
void portduinoWindowsConsoleAllocIfNeeded()
{
    if (consoleAttached)
        return;
    if (AllocConsole()) {
        reopenStdio();
        consoleAttached = true;
    }
}

#endif // ARCH_PORTDUINO && _WIN32

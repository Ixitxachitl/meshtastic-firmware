//
// Created by kevinh on 9/1/20.
//

#include "LinuxSerial.h"
#include <string>

#include <stdio.h>

// LinuxSerial drives a real UART: POSIX termios on Linux/macOS, Win32
// CreateFile/DCB/COMMTIMEOUTS on Windows. SimSerial, the console log sink the
// headless builds use, is portable and stays common.
#ifndef _WIN32

#include <fcntl.h> // Contains file controls like O_RDWR
#include <errno.h> // Error integer and strerror() function
#include <termios.h> // Contains POSIX terminal control definitions
#include <unistd.h> // write(), read(), close()
#include <sys/ioctl.h>

struct termios tty;
#else // _WIN32
// Isolated to this one TU (never included via LinuxSerial.h) so <windows.h>
// doesn't collide with the Arduino API elsewhere - same reasoning as
// WindowsMacAddr.cpp.
#include <windows.h>
#endif // !_WIN32

namespace arduino {
    LinuxSerial Serial1;
    SimSerial Serial;

#ifdef _WIN32
    namespace {
        // Bare port numbers only address COM1-9; COM10+ requires the \\.\ prefix.
        // Applying it unconditionally is harmless for COM1-9 too, and lets users
        // pass either "COM8" or an already-qualified path in config.yaml.
        std::string qualifyComPath(const std::string &p) {
            if (p.rfind("\\\\.\\", 0) == 0)
                return p;
            return "\\\\.\\" + p;
        }
    }

    void LinuxSerial::begin(unsigned long baudrate, uint16_t config) {
        std::string fullPath = qualifyComPath(path);
        HANDLE h = CreateFileA(fullPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "LinuxSerial: failed to open %s (error %lu)\n", fullPath.c_str(), GetLastError());
            return;
        }

        // SetCommState requires a fully-populated DCB; seed it from the driver's
        // current state rather than zero-initializing, then override just what we need.
        DCB dcb = {};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(h, &dcb)) {
            fprintf(stderr, "LinuxSerial: GetCommState(%s) failed (error %lu)\n", fullPath.c_str(), GetLastError());
            CloseHandle(h);
            return;
        }

        dcb.BaudRate = static_cast<DWORD>(baudrate);
        dcb.ByteSize = 8;
        dcb.StopBits = ONESTOPBIT;
        dcb.Parity = NOPARITY;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE; // matches the POSIX side's CRTSCTS-disabled config
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE; // most USB-serial GPS dongles need DTR asserted to transmit
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fAbortOnError = FALSE;

        if (!SetCommState(h, &dcb)) {
            fprintf(stderr, "LinuxSerial: SetCommState(%s) failed (error %lu)\n", fullPath.c_str(), GetLastError());
            CloseHandle(h);
            return;
        }

        // This specific combination makes ReadFile return immediately with whatever
        // is already buffered (even zero bytes), matching the POSIX side's VMIN=0/
        // VTIME=0 non-blocking read so available()/read() never stall the loop.
        COMMTIMEOUTS timeouts = {};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 0;
        SetCommTimeouts(h, &timeouts);

        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        hComm = h;
    }

    int LinuxSerial::setPath(std::string serialPath) {
        if (serialPath != "")
            path = serialPath;
        return 0;
    }

    void LinuxSerial::end() {
        if (hComm) {
            CloseHandle((HANDLE)hComm);
            hComm = nullptr;
        }
    }

    int LinuxSerial::available(void) {
        if (!hComm)
            return 0;
        COMSTAT stat;
        DWORD errors;
        if (!ClearCommError((HANDLE)hComm, &errors, &stat))
            return 0;
        return static_cast<int>(stat.cbInQue);
    }

    int LinuxSerial::peek(void) { return -1; }

    int LinuxSerial::read(void) {
        if (!hComm)
            return -1;
        uint8_t c = 0;
        DWORD bytesRead = 0;
        if (!ReadFile((HANDLE)hComm, &c, 1, &bytesRead, nullptr) || bytesRead == 0)
            return -1;
        return c;
    }

    void LinuxSerial::flush(void) {
        if (hComm)
            FlushFileBuffers((HANDLE)hComm);
    }

    size_t LinuxSerial::write(uint8_t c) {
        if (!hComm)
            return 0;
        DWORD bytesWritten = 0;
        if (!WriteFile((HANDLE)hComm, &c, 1, &bytesWritten, nullptr))
            return 0;
        return bytesWritten;
    }

    LinuxSerial::operator bool() {
        // Returns true if the port is open and ready for use
        return hComm != nullptr;
    }

#else // !_WIN32
    // https://blog.mbedded.ninja/programming/operating-systems/linux/linux-serial-ports-using-c-cpp/

    void LinuxSerial::begin(unsigned long baudrate, uint16_t config) {
        serial_port = open(path.c_str(), O_RDWR);
        tcgetattr(serial_port, &tty);

        tty.c_cflag &= ~PARENB; // Clear parity bit, disabling parity (most common)
        tty.c_cflag &= ~CSTOPB; // Clear stop field, only one stop bit used in communication (most common)
        tty.c_cflag &= ~CSIZE; // Clear all bits that set the data size
        tty.c_cflag |= CS8; // 8 bits per byte (most common)
        tty.c_cflag &= ~CRTSCTS; // Disable RTS/CTS hardware flow control (most common)
        tty.c_cflag |= CREAD | CLOCAL; // Turn on READ & ignore ctrl lines (CLOCAL = 1)

        tty.c_lflag &= ~ICANON;
        tty.c_lflag &= ~ECHO; // Disable echo
        tty.c_lflag &= ~ECHOE; // Disable erasure
        tty.c_lflag &= ~ECHONL; // Disable new-line echo
        tty.c_lflag &= ~ISIG; // Disable interpretation of INTR, QUIT and SUSP
        tty.c_iflag &= ~(IXON | IXOFF | IXANY); // Turn off s/w flow ctrl
        tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL); // Disable any special handling of received bytes

        tty.c_oflag &= ~OPOST; // Prevent special interpretation of output bytes (e.g. newline chars)
        tty.c_oflag &= ~ONLCR; // Prevent conversion of newline to carriage return/line feed
        // tty.c_oflag &= ~OXTABS; // Prevent conversion of tabs to spaces (NOT PRESENT ON LINUX)
        // tty.c_oflag &= ~ONOEOT; // Prevent removal of C-d chars (0x004) in output (NOT PRESENT ON LINUX)

        tty.c_cc[VTIME] = 0;    // don't wait
        tty.c_cc[VMIN] = 0;

        speed_t speed;
        switch(baudrate)
        {
#ifdef B1200
            case 1200:
                speed = B1200;
                break;
#endif
#ifdef B2400
            case 2400:
                speed = B2400;
                break;
#endif
#ifdef B4800
            case 4800:
                speed = B4800;
                break;
#endif
#ifdef B9600
            case 9600:
                speed = B9600;
                break;
#endif
#ifdef B19200
            case 19200:
                speed = B19200;
                break;
#endif
#ifdef B38400
            case 38400:
                speed = B38400;
                break;
#endif
#ifdef B57600
            case 57600:
                speed = B57600;
                break;
#endif
#ifdef B115200
            case 115200:
                speed = B115200;
                break;
#endif
#ifdef B230400
            case 230400:
                speed = B230400;
                break;
#endif
#ifdef B460800
            case 460800:
                speed = B460800;
                break;
#endif
#ifdef B500000
            case 500000:
                speed = B500000;
                break;
#endif
#ifdef B576000
            case 576000:
                speed = B576000;
                break;
#endif
#ifdef B921600
            case 921600:
                speed = B921600;
                break;
#endif
#ifdef B1000000
            case 1000000:
                speed = B1000000;
                break;
#endif
#ifdef B1152000
            case 1152000:
                speed = B1152000;
                break;
#endif
#ifdef B1500000
            case 1500000:
                speed = B1500000;
                break;
#endif
#ifdef B2000000
            case 2000000:
                speed = B2000000;
                break;
#endif
#ifdef B2500000
            case 2500000:
                speed = B2500000;
                break;
#endif
#ifdef B3000000
            case 3000000:
                speed = B3000000;
                break;
#endif
#ifdef B3500000
            case 3500000:
                speed = B3500000;
                break;
#endif
#ifdef B4000000
            case 4000000:
                speed = B4000000;
                break;
#endif
            default:
                speed = baudrate;
                break;
        }

        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);
        tcsetattr(serial_port, TCSANOW, &tty);

    }

    int LinuxSerial::setPath(std::string serialPath) {
        if (serialPath != "")
            path = serialPath;
        return 0;
    }

    void LinuxSerial::end() {
        if (serial_port != -1)
            close(serial_port);
    }

    int LinuxSerial::available(void) {
        int bytes;
        int ret = ioctl(serial_port, FIONREAD, &bytes);
        if (ret == -1) {
            // ioctl failed, likely due to calling available on an invalid file descriptor (EBADF)
            return 0;
        }
        return bytes;
    }

    int LinuxSerial::peek(void) {
        return -1;
    }

    int LinuxSerial::read(void) {
        int buf = 0;
        ::read(serial_port, &buf, 1);
        return buf;
    }

    void LinuxSerial::flush(void) {
    }

    size_t LinuxSerial::write(uint8_t c) {
        ::write(serial_port, &c, 1);
        return 1;
    }

    LinuxSerial::operator bool() {
        // Returns true if the port is ready for use
        return serial_port != -1;
    }

#endif // !_WIN32

    //simulated serial for log output:
    void SimSerial::begin(unsigned long baudrate, uint16_t config) {
        // Ignore baudrate and config on linux (for now)
        // FIXME open file descriptor
    }

    void SimSerial::end() {
        // FIXME - close file descriptor
    }

    int SimSerial::available(void) {
        return 0;
    }

    int SimSerial::peek(void) {
        return -1;
    }

    int SimSerial::read(void) {
        return -1;
    }

    void SimSerial::flush(void) {
    }

    size_t SimSerial::write(uint8_t c) {
        putchar(c);
        return 1;
    }

    SimSerial::operator bool() {
        // Returns true if the port is ready for use
        return true;
    }
}

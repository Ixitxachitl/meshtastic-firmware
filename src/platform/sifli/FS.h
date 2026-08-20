#pragma once

// Arduino's filesystem names, mapped onto the Zephyr LittleFS wrapper.
//
// Libraries written against Arduino cores take an `fs::FS &` and call
// open/exists/remove/mkdir on it, which is the interface InternalFileSystem
// already presents.

#include "Arduino.h" // String, which Arduino's FS.h also brings along
#include "InternalFileSystem.h"

namespace fs
{
using File = Adafruit_LittleFS_Namespace::File;
using FS = Adafruit_LittleFS_Namespace::InternalFileSystem;
} // namespace fs

// Arduino cores hoist these into the global namespace, and libraries written
// against them use the unqualified names.
using fs::File;
using fs::FS;

// Open modes are plain fopen-style strings in this filesystem wrapper.
#ifndef FILE_READ
#define FILE_READ "r"
#endif
#ifndef FILE_WRITE
#define FILE_WRITE "w"
#endif
#ifndef FILE_APPEND
#define FILE_APPEND "a"
#endif

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

#pragma once

// Arduino's LittleFS name for the internal filesystem. Cores expose a global
// object of that name; here it is the InternalFileSystem instance the platform
// already creates.

#include "FS.h"
#include "InternalFileSystem.h"

#define LittleFS InternalFS

#pragma once

#include "targetver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace videoplayer {

class App final {
public:
    int Run(HINSTANCE instance, PWSTR commandLine, int showCommand);
};

}  // namespace videoplayer

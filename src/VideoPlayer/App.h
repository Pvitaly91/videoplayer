#pragma once

#include "targetver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace videoplayer {

inline std::wstring FirstFileArgument(const int argc, wchar_t* const* const argv) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return {};
    }
    return argv[1];
}

class App final {
public:
    int Run(HINSTANCE instance, PWSTR commandLine, int showCommand);
};

}  // namespace videoplayer

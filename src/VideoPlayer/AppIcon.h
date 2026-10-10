#pragma once

#include "targetver.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Resource.h"

namespace videoplayer {

// Resource-backed shared handles live for the process and must not be destroyed.
inline HICON LoadPlayerIcon(HINSTANCE instance, int width, int height) noexcept {
    HICON icon = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_VIDEOPLAYER), IMAGE_ICON, width, height, LR_SHARED));
    if (icon == nullptr) {
        icon = static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION,
            IMAGE_ICON, width, height, LR_SHARED));
    }
    return icon;
}

}  // namespace videoplayer

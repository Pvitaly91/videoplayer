#pragma once

#include "targetver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

#include <array>

namespace videoplayer {

inline constexpr std::array<const char*, 3> kPrivateLibVlcArguments{
    "--ignore-config",
    "--no-media-library",
    "--no-video-title-show",
};

inline constexpr DWORD kPrivateOpenDialogFlags =
    OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
    OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT |
    OFN_ALLOWMULTISELECT;

// Export writes only the explicitly selected output. The save dialog itself
// must not add that path to Windows Recent Items or mutate process CWD.
inline constexpr DWORD kPrivateSaveDialogFlags =
    OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;

}  // namespace videoplayer

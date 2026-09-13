#pragma once

#include "ExportTypes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace videoplayer {

bool HasActiveExportDialog(HWND owner) noexcept;
void ShowExportDialog(HWND owner, HINSTANCE instance, ExportSnapshot snapshot);

}  // namespace videoplayer

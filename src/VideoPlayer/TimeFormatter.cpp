#include "TimeFormatter.h"

#include <iomanip>
#include <sstream>

namespace videoplayer {

std::wstring FormatTime(const std::int64_t milliseconds) {
    const std::int64_t totalSeconds = milliseconds > 0 ? milliseconds / 1000 : 0;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t seconds = totalSeconds % 60;

    std::wostringstream text;
    text << std::setfill(L'0');
    if (hours > 0) {
        text << hours << L':' << std::setw(2) << minutes << L':' << std::setw(2) << seconds;
    } else {
        text << std::setw(2) << minutes << L':' << std::setw(2) << seconds;
    }
    return text.str();
}

}  // namespace videoplayer

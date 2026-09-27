#include "clock.h"
#include <cstdio>

std::string formatClock(const std::tm& local, int format) {
    char text[32];
    if (format == 12) {
        const int hour = local.tm_hour % 12;
        std::snprintf(text, sizeof(text), "%d:%02d %s", hour ? hour : 12, local.tm_min,
                      local.tm_hour < 12 ? "AM" : "PM");
    } else if (format == 24) {
        std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
    } else return "";
    return text;
}

std::string localClock(int format) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (!localtime_r(&now, &local)) return "--:--";
    return formatClock(local, format);
}

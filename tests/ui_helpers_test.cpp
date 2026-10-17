// Exact pixel conversion and settings-file regression tests. No SteamVR/device required.
#include "config.h"
#include "draw.h"
#include "json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

// The previous conversion algorithm, retained here as an independent reference.
void referenceRgba(cairo_surface_t* surface, std::vector<uint8_t>& out) {
    static const auto table = [] {
        std::vector<uint8_t> values(256 * 256);
        for (unsigned a = 0; a < 256; ++a) {
            for (unsigned c = 0; c < 256; ++c) {
                values[a * 256 + c] = a == 0 ? 0 : static_cast<uint8_t>(std::min(255u, (c * 255 + a / 2) / a));
            }
        }
        return values;
    }();
    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface), height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const uint8_t* data = cairo_image_surface_get_data(surface);
    out.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* dst = out.data();
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(data + static_cast<size_t>(y) * stride);
        for (int x = 0; x < width; ++x) {
            const uint32_t p = row[x], a = p >> 24;
            const uint8_t* line = table.data() + a * 256;
            *dst++ = line[(p >> 16) & 255];
            *dst++ = line[(p >> 8) & 255];
            *dst++ = line[p & 255];
            *dst++ = static_cast<uint8_t>(a);
        }
    }
}

void pixels() {
    // Every channel/alpha combination, including invalid premultiplied channels
    // (the public helper previously saturated these rather than wrapping them).
    cairo_surface_t* image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 256, 256);
    CHECK(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);
    auto* data = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(image));
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned c = 0; c < 256; ++c) data[a * 256 + c] = (a << 24) | (c << 16) | ((255 - c) << 8) | ((c + a) & 255);
    }
    cairo_surface_mark_dirty(image);
    std::vector<uint8_t> actual, expected;
    referenceRgba(image, expected);
    surfaceToRgba(image, actual);
    CHECK(actual == expected);
    cairo_surface_destroy(image);

    // Respect Cairo's row stride and overwrite an already-sized/reused buffer.
    constexpr int width = 13, height = 17, stride = 64;
    std::vector<uint32_t> padded(stride / 4 * height, 0xdeadbeef);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned a = (y * width + x) % 256;
            padded[y * stride / 4 + x] = (a << 24) | (a << 16) | ((a / 2) << 8) | (a / 3);
        }
    }
    image = cairo_image_surface_create_for_data(reinterpret_cast<uint8_t*>(padded.data()), CAIRO_FORMAT_ARGB32,
                                                width, height, stride);
    CHECK(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);
    referenceRgba(image, expected);
    surfaceToRgba(image, actual);
    CHECK(actual == expected && actual.size() == width * height * 4);
    cairo_surface_destroy(image);
}

void configuration(const std::string& directory) {
    const std::string path = directory + "/config.json";
    Config config;
    config.visible = config.updateCheck = config.wristFade = false;
    config.language = Language::En;
    config.attachment = Attachment::RightWrist;
    config.posX = -0.1234; config.posY = 0.5432; config.posZ = -2.314;
    config.yawDeg = -123.4; config.pitchDeg = 45.67; config.rollDeg = 12.34;
    config.leftWrist = {-0.1234, 0.2345, -0.3456, -45.67, 123.4, -23.45};
    config.rightWrist = {0.3456, -0.2345, 0.1234, -123.4, 23.45, 45.67};
    config.wristFadeEndDeg = 84.32; config.clockFormat = 12;
    config.widthM = 0.4567; config.alpha = 0.7654;
    config.updateIntervalMs = 1234; config.graphSeconds = 123;
    config.fontPath = "Japanese-日本語\\font\".ttf";
    // Every ASCII control character, including embedded NUL, must be escaped and round-trip.
    for (char c = 0; c < 32; ++c) config.boldFontPath += c;
    config.boldFontPath += "\\\"font.ttf";
    const struct { const char* name; double Thresholds::*member; double value; } fields[] = {
        {"fps_warn_ratio", &Thresholds::fpsWarnRatio, 0.87}, {"fps_crit_ratio", &Thresholds::fpsCritRatio, 0.42},
        {"frame_warn_ratio", &Thresholds::frameWarnRatio, 1.234}, {"frame_crit_ratio", &Thresholds::frameCritRatio, 2.345},
        {"reproj_warn_pct", &Thresholds::reprojWarnPct, 12.34}, {"reproj_crit_pct", &Thresholds::reprojCritPct, 23.45},
        {"temp_warn_c", &Thresholds::tempWarnC, 56.78}, {"temp_crit_c", &Thresholds::tempCritC, 78.91},
        {"battery_warn_pct", &Thresholds::batteryWarnPct, 34.56}, {"battery_crit_pct", &Thresholds::batteryCritPct, 12.34},
        {"power_warn_w", &Thresholds::powerWarnW, 23.45}, {"power_crit_w", &Thresholds::powerCritW, 34.56},
        {"cpu_warn_pct", &Thresholds::cpuWarnPct, 56.78}, {"cpu_crit_pct", &Thresholds::cpuCritPct, 67.89},
        {"gpu_warn_pct", &Thresholds::gpuWarnPct, 78.91}, {"gpu_crit_pct", &Thresholds::gpuCritPct, 89.12},
        {"wifi_warn_dbm", &Thresholds::wifiWarnDbm, -45.67}, {"wifi_crit_dbm", &Thresholds::wifiCritDbm, -89.12},
        {"controller_warn_pct", &Thresholds::controllerWarnPct, 45.67},
        {"controller_crit_pct", &Thresholds::controllerCritPct, 23.45},
    };
    for (const auto& field : fields) config.thresholds.*field.member = field.value;
    std::string error;
    CHECK(saveConfig(path, config, error));
    CHECK(!std::filesystem::exists(path + ".tmp"));
    Config loaded;
    std::vector<std::string> warnings;
    CHECK(loadConfig(path, loaded, warnings, error) && warnings.empty());
    CHECK(loaded.visible == config.visible && loaded.updateCheck == config.updateCheck && loaded.wristFade == config.wristFade);
    CHECK(loaded.language == config.language && loaded.attachment == config.attachment);
    CHECK(loaded.posX == config.posX && loaded.posY == config.posY && loaded.posZ == config.posZ);
    CHECK(loaded.yawDeg == config.yawDeg && loaded.pitchDeg == config.pitchDeg && loaded.rollDeg == config.rollDeg);
    CHECK(!(loaded.leftWrist != config.leftWrist) && !(loaded.rightWrist != config.rightWrist));
    CHECK(loaded.wristFadeEndDeg == config.wristFadeEndDeg && loaded.clockFormat == config.clockFormat);
    CHECK(loaded.widthM == config.widthM && loaded.alpha == config.alpha);
    CHECK(loaded.updateIntervalMs == config.updateIntervalMs && loaded.graphSeconds == config.graphSeconds);
    CHECK(loaded.fontPath == config.fontPath && loaded.boldFontPath == config.boldFontPath);
    for (const auto& field : fields) CHECK(loaded.thresholds.*field.member == field.value);

    // A failed flush must preserve the original file and remove the temporary link.
    CHECK(::symlink("/dev/full", (path + ".tmp").c_str()) == 0);
    loaded.alpha = 0.1;
    CHECK(!saveConfig(path, loaded, error));
    CHECK(!std::filesystem::exists(path + ".tmp"));
    CHECK(loadConfig(path, loaded, warnings, error) && loaded.alpha == config.alpha);
}

// Optional developer measurement; never a timing assertion in CTest.
void benchmark() {
    for (const auto dimensions : {std::pair<int, int>{512, 460}, {1600, 738}}) {
        for (int kind = 0; kind < 3; ++kind) {
            const int width = dimensions.first, height = dimensions.second;
            cairo_surface_t* image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
            auto* data = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(image));
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    unsigned a = 255;
                    if (kind == 1) a = static_cast<unsigned>((x + y) % 254 + 1);
                    if (kind == 2) a = x > 8 && x < width - 8 && y > 8 && y < height - 8 ? 255 : (x + y) % 256;
                    data[y * width + x] = (a << 24) | (a << 16) | ((a / 2) << 8) | (a / 3);
                }
            }
            cairo_surface_mark_dirty(image);
            std::vector<uint8_t> actual, expected;
            referenceRgba(image, expected);
            surfaceToRgba(image, actual);
            CHECK(actual == expected);
            constexpr int repeats = 200;
            const auto measure = [&](auto convert) {
                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < repeats; ++i) convert(image, actual);
                return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / repeats;
            };
            // Alternate order across five samples; report the median to reduce noise.
            std::vector<double> oldTimes, newTimes;
            for (int sample = 0; sample < 5; ++sample) {
                if (sample % 2 == 0) { oldTimes.push_back(measure(referenceRgba)); newTimes.push_back(measure(surfaceToRgba)); }
                else { newTimes.push_back(measure(surfaceToRgba)); oldTimes.push_back(measure(referenceRgba)); }
            }
            std::sort(oldTimes.begin(), oldTimes.end()); std::sort(newTimes.begin(), newTimes.end());
            std::printf("RGBA %dx%d %s: reference=%.3f us current=%.3f us ratio=%.4f (%d repeats x5)\n",
                        width, height, kind == 0 ? "opaque" : kind == 1 ? "translucent" : "opaque-with-edges",
                        oldTimes[2], newTimes[2], newTimes[2] / oldTimes[2], repeats);
            cairo_surface_destroy(image);
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    char directory[] = "/tmp/frame-ui-helpers-XXXXXX";
    if (::mkdtemp(directory) == nullptr) return 2;
    try {
        pixels();
        configuration(directory);
        if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) benchmark();
        std::filesystem::remove_all(directory);
        std::puts("Exhaustive RGBA conversion, padded stride, settings round-trip and atomic-write failure tests passed.");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s (artifacts: %s)\n", error.what(), directory);
        return 1;
    }
}

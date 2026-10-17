// Synthetic /proc/stat files only; no device or host sensor access.
#include "sensors.h"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

struct SensorsTestAccess {
    static void setStat(Sensors& sensors, int fd) { sensors.procStat_.fd = ::dup(fd); }
    static void setPowerSources(Sensors& sensors, int mainFd, int otherFd) {
        sensors.powers_ = {{"", "max34417_10/vph", ::dup(mainFd)}, {"", "max34417_11/apc0", ::dup(otherFd)}};
        // The test exercises only synthetic power files, never actual networking or /proc.
        sensors.lastWifiRead_ = sensors.lastGpuRead_ = std::numeric_limits<double>::max();
    }
    static void refreshPower(Sensors& sensors) { sensors.lastPowerRead_ = -1; }
    static void freezePower(Sensors& sensors) { sensors.lastPowerRead_ = std::numeric_limits<double>::max(); }
    static void setDescriptionSources(Sensors& sensors, int fd) {
        sensors.cpuTemps_ = {{"", "cpu0-thermal", -1}, {"", "cpu1-thermal", -1}};
        sensors.gpuTemps_ = {{"", "gpuss-thermal", -1}};
        sensors.gpuFreq_ = {"/synthetic/gpu/cur_freq", "gpu", ::dup(fd)};
        sensors.powers_ = {{"/synthetic/power1_input", "max34417_10/vph", -1}};
        sensors.cpuCooling_ = {{"", "cpufreq-cpu0", -1}};
        sensors.gpuCooling_ = {{"", "devfreq-gpu", -1}};
    }
    static SensorSample cpu(Sensors& sensors) {
        SensorSample sample;
        sensors.readCpuUsage(sample);
        return sample;
    }
};

namespace {

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void near(double actual, double expected) {
    check(std::isfinite(actual) && std::fabs(actual - expected) < 1e-9, "unexpected sensor value");
}

struct StatFile {
    int fd;
    StatFile() {
        char path[] = "/tmp/frame-perf-stat-XXXXXX";
        fd = ::mkstemp(path);
        check(fd >= 0, "mkstemp failed");
        ::unlink(path);
    }
    ~StatFile() { ::close(fd); }
    void write(const std::string& text) {
        check(::ftruncate(fd, 0) == 0, "truncate failed");
        check(::pwrite(fd, text.data(), text.size(), 0) == static_cast<ssize_t>(text.size()), "write failed");
    }
};

void usageAndTopology() {
    StatFile stat;
    Sensors sensors;
    SensorsTestAccess::setStat(sensors, stat.fd);
    stat.write("cpu 100 10 20 800 50 5 15 0 0 0\n"
               "cpu0 50 5 10 400 25 2 8 0 0 0\n"
               "cpu1 50 5 10 400 25 3 7 0 0 0\n"
               "intr 12345\n");
    auto sample = SensorsTestAccess::cpu(sensors);
    check(std::isnan(sample.cpuUsagePct) && std::isnan(sample.cpuMaxCorePct), "first sample needs a baseline");

    // Guest counters are already included in user/nice and must not be counted twice.
    const std::string second = "cpu 250 10 20 850 50 5 15 0 100000 100000\n"
                               "cpu0 150 5 10 400 25 2 8 0 100000 100000\n"
                               "cpu1 100 5 10 450 25 3 7 0 0 0\n";
    stat.write(second);
    sample = SensorsTestAccess::cpu(sensors);
    near(sample.cpuUsagePct, 75);
    near(sample.cpuMaxCorePct, 100);
    sample = SensorsTestAccess::cpu(sensors);
    near(sample.cpuUsagePct, 0);
    near(sample.cpuMaxCorePct, 0);

    // A changed CPU count needs a fresh baseline rather than mixing different rows.
    stat.write("cpu 150 5 10 400 25 2 8 0\ncpu0 150 5 10 400 25 2 8 0\n");
    sample = SensorsTestAccess::cpu(sensors);
    check(std::isnan(sample.cpuUsagePct) && std::isnan(sample.cpuMaxCorePct), "CPU hotplug needs a baseline");
    stat.write("");
    sample = SensorsTestAccess::cpu(sensors);
    check(std::isnan(sample.cpuUsagePct), "empty reads must be unavailable");
    stat.write("cpu 175 5 10 475 25 2 8 0\ncpu0 175 5 10 475 25 2 8 0\n");
    sample = SensorsTestAccess::cpu(sensors);
    near(sample.cpuUsagePct, 25);
    near(sample.cpuMaxCorePct, 25);
}

void repeatedSamples() {
    StatFile stat;
    Sensors sensors;
    SensorsTestAccess::setStat(sensors, stat.fd);
    for (unsigned sampleIndex = 0; sampleIndex < 200; ++sampleIndex) {
        const auto busy = std::to_string(50 + sampleIndex * 25);
        const auto idle = std::to_string(50 + sampleIndex * 75);
        std::string text;
        for (unsigned row = 0; row < 9; ++row) {
            text += "cpu";
            if (row != 0) text += std::to_string(row - 1);
            text += " " + busy + " 0 0 " + idle + " 0 0 0 0\n";
        }
        stat.write(text);
        const auto sample = SensorsTestAccess::cpu(sensors);
        if (sampleIndex == 0) {
            check(std::isnan(sample.cpuUsagePct), "first repeated sample needs a baseline");
        } else {
            near(sample.cpuUsagePct, 25);
            near(sample.cpuMaxCorePct, 25);
        }
    }
}

void description() {
    StatFile stat;
    Sensors sensors;
    SensorsTestAccess::setDescriptionSources(sensors, stat.fd);
    check(sensors.describe() ==
              "CPU 温度: 2 個 cpu0-thermal cpu1-thermal\n"
              "GPU 温度: 1 個 gpuss-thermal\n"
              "DDR 温度: （見つからない）\n"
              "CPU クロック: 0 policy / 大コア （見つからない）\n"
              "GPU クロック: /synthetic/gpu/cur_freq\n"
              "消費電力: 1 ch\n  max34417_10/vph -> /synthetic/power1_input\n"
              "ファン: （見つからない）\n"
              "バッテリー: （見つからない）\n"
              "充電器: （見つからない）\n"
              "iio 温度: （見つからない） / （見つからない） / （見つからない）\n"
              "熱制限: CPU 1 個 cpufreq-cpu0 / GPU 1 個 devfreq-gpu",
          "sensor description changed");
}

void optionalPowerChannels() {
    StatFile mainRail, otherRail;
    mainRail.write("12500000\n");
    otherRail.write("3000000\n");
    Sensors sensors;
    SensorsTestAccess::setPowerSources(sensors, mainRail.fd, otherRail.fd);
    auto sample = sensors.read();
    near(sample.powerMainW, 12.5);
    near(sample.powerSumW, 15.5);
    check(sample.powerChannels.size() == 2, "default read omitted power diagnostics");

    mainRail.write("13500000\n");
    otherRail.write("3500000\n");
    SensorsTestAccess::refreshPower(sensors);
    sample = sensors.read(false);
    near(sample.powerMainW, 13.5);
    near(sample.powerSumW, 17);
    check(sample.powerChannels.empty(), "aggregate-only read copied power diagnostics");

    // Skipping the result copy must preserve the cache for subsequent diagnostic reads.
    mainRail.write("14500000\n");
    SensorsTestAccess::freezePower(sensors);
    sample = sensors.read();
    near(sample.powerMainW, 13.5);
    near(sample.powerSumW, 17);
    check(sample.powerChannels.size() == 2, "aggregate-only read discarded cached diagnostics");
    check(sample.powerChannels[0].chip == "max34417_10" && sample.powerChannels[0].label == "vph",
          "main power metadata changed");
    near(sample.powerChannels[0].watts, 13.5);
    check(sample.powerChannels[1].chip == "max34417_11" && sample.powerChannels[1].label == "apc0",
          "other power metadata changed");
    near(sample.powerChannels[1].watts, 3.5);
    sample = sensors.read(false);
    near(sample.powerMainW, 13.5);
    near(sample.powerSumW, 17);
    check(sample.powerChannels.empty(), "cached aggregate-only read copied power diagnostics");
}

}  // namespace

int main() {
    try {
        usageAndTopology();
        repeatedSamples();
        description();
        optionalPowerChannels();
        std::puts("sensor CPU tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

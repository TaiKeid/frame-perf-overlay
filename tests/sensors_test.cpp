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
    check(std::isfinite(actual) && std::fabs(actual - expected) < 1e-9, "unexpected CPU usage");
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

}  // namespace

int main() {
    try {
        usageAndTopology();
        repeatedSamples();
        std::puts("sensor CPU tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

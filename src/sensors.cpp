// /proc と /sys からの読み取りの実装。すべて O_RDONLY で開き、書き込みはしない。
#include "sensors.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

namespace {

/**
 * ディレクトリの中の名前を並べて返す（. と .. を除く）。
 * @param dir ディレクトリのパス
 * @param prefix この文字列で始まる名前だけにする（空ならすべて）
 * @return 名前の一覧（自然順: hwmon2 < hwmon10）
 */
std::vector<std::string> listDir(const std::string& dir, const std::string& prefix) {
    std::vector<std::string> names;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return names;
    while (const dirent* entry = ::readdir(d)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (!prefix.empty() && name.compare(0, prefix.size(), prefix) != 0) continue;
        names.push_back(name);
    }
    ::closedir(d);
    // 末尾の数字を数値として比べる（hwmon2 と hwmon10 の順番を正しくする）
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        const size_t ia = a.find_last_not_of("0123456789") + 1;
        const size_t ib = b.find_last_not_of("0123456789") + 1;
        const std::string headA = a.substr(0, ia);
        const std::string headB = b.substr(0, ib);
        if (headA != headB || ia == a.size() || ib == b.size()) return a < b;
        return std::atol(a.c_str() + ia) < std::atol(b.c_str() + ib);
    });
    return names;
}

/**
 * 小さなテキストファイルを 1 回だけ読む（走査時の name / type / label 用）。
 * @param path パス
 * @return 中身（末尾の改行は除く）。読めなければ空
 */
std::string readSmallFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    char buffer[256];
    const ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    if (n <= 0) return "";
    std::string text(buffer, static_cast<size_t>(n));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

/**
 * ファイルがあるか調べる。
 * @param path パス
 * @return あれば true
 */
bool fileExists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

/**
 * 文字列が指定の文字列で始まるか。
 * @param text 調べる文字列
 * @param prefix 先頭
 * @return 始まっていれば true
 */
bool startsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

/** fan1_input を回転数（rpm）にするための割る数（SteamOS 同梱のファン制御の設定と同じ換算）。 */
constexpr double kFanRpmScale = 2.0;

}  // namespace

Sensors::~Sensors() {
    closeAll();
}

Sensors::Source Sensors::openSource(const std::string& path, const std::string& name) {
    Source source;
    source.path = path;
    source.name = name;
    source.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    return source;
}

void Sensors::closeAll() {
    const auto closeOne = [](Source& s) {
        if (s.fd >= 0) ::close(s.fd);
        s.fd = -1;
    };
    for (auto* list : {&cpuTemps_, &gpuTemps_, &cpuFreqs_, &powers_}) {
        for (auto& s : *list) closeOne(s);
        list->clear();
    }
    for (auto* list : {&cpuCooling_, &gpuCooling_}) {
        for (auto& s : *list) closeOne(s);
        list->clear();
    }
    for (Source* s : {&ddrTemp_, &cpuBigFreq_, &gpuFreq_, &fan_, &batCapacity_, &batVoltage_, &batCurrent_,
                      &batTemp_, &batStatus_, &chargerOnline_, &procStat_, &memInfo_, &displayTemp_, &exhaustTemp_,
                      &heatsinkTemp_}) {
        closeOne(*s);
    }
}

void Sensors::discover() {
    closeAll();

    // 温度: thermal_zone の type で分類する（番号は起動ごとに変わりうる）
    const std::string thermalDir = "/sys/class/thermal/";
    for (const auto& zone : listDir(thermalDir, "thermal_zone")) {
        const std::string base = thermalDir + zone;
        const std::string type = readSmallFile(base + "/type");
        if (startsWith(type, "cpu")) {
            cpuTemps_.push_back(openSource(base + "/temp", type));
        } else if (startsWith(type, "gpuss")) {
            gpuTemps_.push_back(openSource(base + "/temp", type));
        } else if (startsWith(type, "ddr") && ddrTemp_.fd < 0) {
            ddrTemp_ = openSource(base + "/temp", type);
        }
    }

    // hwmon: name で探す（消費電力 max34417_*、ファン）
    const std::string hwmonDir = "/sys/class/hwmon/";
    for (const auto& hw : listDir(hwmonDir, "hwmon")) {
        const std::string base = hwmonDir + hw;
        const std::string name = readSmallFile(base + "/name");
        if (startsWith(name, "max34417")) {
            for (int ch = 1; ch <= 8; ++ch) {
                const std::string input = base + "/power" + std::to_string(ch) + "_input";
                if (!fileExists(input)) continue;
                std::string label = readSmallFile(base + "/power" + std::to_string(ch) + "_label");
                if (label.empty()) label = "power" + std::to_string(ch);
                if (label == "unused") continue;
                powers_.push_back(openSource(input, name + "/" + label));
            }
        } else if (fan_.fd < 0 && fileExists(base + "/fan1_input")) {
            fan_ = openSource(base + "/fan1_input", name);
        }
    }
    // チップ名の順に並べる（表示を毎回同じ順にする）
    std::stable_sort(powers_.begin(), powers_.end(),
                     [](const Source& a, const Source& b) { return a.name.substr(0, a.name.find('/')) <
                                                                   b.name.substr(0, b.name.find('/')); });

    // CPU クロック: cpufreq の policy ごと
    const std::string cpufreqDir = "/sys/devices/system/cpu/cpufreq/";
    for (const auto& policy : listDir(cpufreqDir, "policy")) {
        cpuFreqs_.push_back(openSource(cpufreqDir + policy + "/scaling_cur_freq", policy));
    }
    // 大コア（いちばん番号の大きい CPU）
    for (int cpu = 15; cpu >= 0; --cpu) {
        const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq";
        if (fileExists(path)) {
            cpuBigFreq_ = openSource(path, "cpu" + std::to_string(cpu));
            break;
        }
    }

    // GPU クロック: devfreq の名前に gpu を含むもの
    const std::string devfreqDir = "/sys/class/devfreq/";
    for (const auto& dev : listDir(devfreqDir, "")) {
        if (dev.find("gpu") == std::string::npos) continue;
        gpuFreq_ = openSource(devfreqDir + dev + "/cur_freq", dev);
        const std::string maxText = readSmallFile(devfreqDir + dev + "/max_freq");
        if (!maxText.empty()) gpuMaxFreqMHz_ = std::atof(maxText.c_str()) / 1e6;
        break;
    }

    // バッテリーと充電器: power_supply の type / 名前で探す
    const std::string psDir = "/sys/class/power_supply/";
    for (const auto& ps : listDir(psDir, "")) {
        const std::string base = psDir + ps;
        const std::string type = readSmallFile(base + "/type");
        if (type == "Battery" && batCapacity_.fd < 0) {
            batCapacity_ = openSource(base + "/capacity", ps);
            batVoltage_ = openSource(base + "/voltage_now", ps);
            batCurrent_ = openSource(base + "/current_now", ps);
            batTemp_ = openSource(base + "/temp", ps);
            batStatus_ = openSource(base + "/status", ps);
        } else if (ps.find("charger") != std::string::npos && chargerOnline_.fd < 0) {
            chargerOnline_ = openSource(base + "/online", ps);
        }
    }

    // iio の温度: name で vadc を探す（device 番号は変わりうる）
    const std::string iioDir = "/sys/bus/iio/devices/";
    for (const auto& dev : listDir(iioDir, "iio:device")) {
        const std::string base = iioDir + dev + "/";
        if (readSmallFile(base + "name") != "c400000.spmi:pmic@0:vadc@9000") continue;
        // display_right は -17℃ などおかしな値なので読まない
        displayTemp_ = openSource(base + "in_temp_display_left_therm_input", "display_left");
        exhaustTemp_ = openSource(base + "in_temp_pcba_at_fan_exhaust_input", "fan_exhaust");
        heatsinkTemp_ = openSource(base + "in_temp_pcba_at_heat_sink_fins_input", "heat_sink_fins");
        break;
    }

    // 熱による制限: cooling_device の type で CPU と GPU を分ける
    const std::string coolingDir = "/sys/class/thermal/";
    for (const auto& dev : listDir(coolingDir, "cooling_device")) {
        const std::string base = coolingDir + dev + "/";
        const std::string type = readSmallFile(base + "type");
        if (startsWith(type, "cpufreq-cpu")) {
            cpuCooling_.push_back(openSource(base + "cur_state", type));
        } else if (startsWith(type, "devfreq-") && type.find("gpu") != std::string::npos) {
            gpuCooling_.push_back(openSource(base + "cur_state", type));
        }
    }

    procStat_ = openSource("/proc/stat", "stat");
    memInfo_ = openSource("/proc/meminfo", "meminfo");
    prevBusy_.clear();
    prevTotal_.clear();
    lastPowerRead_ = -1.0;
    lastBatteryRead_ = -1.0;
    lastWifiRead_ = -1.0;
    lastIioRead_ = -1.0;
    lastGpuRead_ = -1.0;
    cached_ = SensorSample();
}

long Sensors::readRaw(const Source& source, char* buffer, size_t size) {
    if (source.fd < 0 || size == 0) return -1;
    // sysfs / procfs は先頭（offset 0）から読み直すと最新の値が作り直される
    const ssize_t n = ::pread(source.fd, buffer, size - 1, 0);
    if (n < 0) return -1;
    buffer[n] = '\0';
    return static_cast<long>(n);
}

bool Sensors::readLong(const Source& source, long long& value) {
    char buffer[64];
    if (readRaw(source, buffer, sizeof(buffer)) <= 0) return false;
    char* end = nullptr;
    value = std::strtoll(buffer, &end, 10);
    return end != buffer;
}

std::string Sensors::readText(const Source& source) {
    char buffer[64];
    if (readRaw(source, buffer, sizeof(buffer)) <= 0) return "";
    std::string text(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
}

double Sensors::maxTempC(const std::vector<Source>& sources) {
    double best = kNoValue;
    for (const auto& s : sources) {
        long long milli = 0;
        if (!readLong(s, milli)) continue;
        const double c = static_cast<double>(milli) / 1000.0;
        if (std::isnan(best) || c > best) best = c;
    }
    return best;
}

void Sensors::readCpuUsage(SensorSample& sample) {
    // cpu 行は先頭にあるので 4KB で足りる（後ろの intr 行などは読まない）
    char buffer[4096];
    if (readRaw(procStat_, buffer, sizeof(buffer)) <= 0) return;

    std::vector<uint64_t> busy;
    std::vector<uint64_t> total;
    const char* line = buffer;
    while (line != nullptr && std::strncmp(line, "cpu", 3) == 0) {
        // cpu  user nice system idle iowait irq softirq steal ...
        const char* p = line + 3;
        while (*p != ' ' && *p != '\0') ++p;  // "cpu" の後ろの番号を飛ばす
        uint64_t fields[10] = {};
        int count = 0;
        char* end = nullptr;
        while (count < 10) {
            const unsigned long long v = std::strtoull(p, &end, 10);
            if (end == p) break;
            fields[count++] = v;
            p = end;
        }
        if (count >= 5) {
            uint64_t all = 0;
            for (int i = 0; i < std::min(count, 8); ++i) all += fields[i];  // guest は user に含まれるので除く
            const uint64_t idle = fields[3] + fields[4];                    // idle + iowait
            busy.push_back(all - idle);
            total.push_back(all);
        }
        const char* next = std::strchr(line, '\n');
        line = next != nullptr ? next + 1 : nullptr;
    }

    if (!prevTotal_.empty() && prevTotal_.size() == total.size()) {
        double maxCore = kNoValue;
        for (size_t i = 0; i < total.size(); ++i) {
            const double dt = static_cast<double>(total[i] - prevTotal_[i]);
            const double db = static_cast<double>(busy[i] - prevBusy_[i]);
            const double pct = dt > 0 ? 100.0 * db / dt : 0.0;
            if (i == 0) {
                sample.cpuUsagePct = pct;
            } else if (std::isnan(maxCore) || pct > maxCore) {
                maxCore = pct;
            }
        }
        sample.cpuMaxCorePct = maxCore;
    }
    prevBusy_ = std::move(busy);
    prevTotal_ = std::move(total);
}

void Sensors::readMemory(SensorSample& sample) {
    char buffer[2048];
    if (readRaw(memInfo_, buffer, sizeof(buffer)) <= 0) return;
    long long totalKb = -1;
    long long availableKb = -1;
    const char* line = buffer;
    while (line != nullptr && *line != '\0') {
        if (std::strncmp(line, "MemTotal:", 9) == 0) totalKb = std::atoll(line + 9);
        if (std::strncmp(line, "MemAvailable:", 13) == 0) availableKb = std::atoll(line + 13);
        const char* next = std::strchr(line, '\n');
        line = next != nullptr ? next + 1 : nullptr;
    }
    if (totalKb > 0 && availableKb >= 0) {
        sample.memTotalGiB = static_cast<double>(totalKb) / (1024.0 * 1024.0);
        sample.memUsedGiB = static_cast<double>(totalKb - availableKb) / (1024.0 * 1024.0);
    }
}

void Sensors::readPower(SensorSample& sample) {
    sample.powerMainW = kNoValue;
    sample.powerSumW = kNoValue;
    sample.powerChannels.clear();
    long long v = 0;
    double sum = 0.0;
    bool any = false;
    for (const auto& s : powers_) {
        PowerChannel ch;
        const size_t slash = s.name.find('/');
        ch.chip = s.name.substr(0, slash);
        ch.label = s.name.substr(slash + 1);
        if (readLong(s, v)) {
            ch.watts = static_cast<double>(v) / 1e6;  // µW → W
            sum += ch.watts;
            any = true;
            if (ch.label == "vph" && std::isnan(sample.powerMainW)) sample.powerMainW = ch.watts;
        }
        sample.powerChannels.push_back(ch);
    }
    if (any) sample.powerSumW = sum;
}

void Sensors::readBattery(SensorSample& sample) {
    long long v = 0;
    sample.batteryPct = readLong(batCapacity_, v) ? static_cast<double>(v) : kNoValue;
    sample.batteryVoltageV = readLong(batVoltage_, v) ? static_cast<double>(v) / 1e6 : kNoValue;     // µV → V
    sample.batteryCurrentMa = readLong(batCurrent_, v) ? static_cast<double>(v) / 1000.0 : kNoValue;  // µA → mA
    sample.batteryTempC = readLong(batTemp_, v) ? static_cast<double>(v) / 10.0 : kNoValue;           // 0.1℃ → ℃
    sample.batteryStatus = readText(batStatus_);
    sample.chargerOnline = readLong(chargerOnline_, v) && v != 0;
}

void Sensors::readIioTemps(SensorSample& sample) {
    long long v = 0;
    sample.displayTempC = readLong(displayTemp_, v) ? static_cast<double>(v) / 1000.0 : kNoValue;  // m℃ → ℃
    sample.exhaustTempC = readLong(exhaustTemp_, v) ? static_cast<double>(v) / 1000.0 : kNoValue;
    sample.heatsinkTempC = readLong(heatsinkTemp_, v) ? static_cast<double>(v) / 1000.0 : kNoValue;
}

bool Sensors::anyCooling(const std::vector<Source>& sources) {
    for (const auto& s : sources) {
        long long state = 0;
        if (readLong(s, state) && state > 0) return true;
    }
    return false;
}

double Sensors::monotonicSeconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

SensorSample Sensors::read() {
    SensorSample sample;
    long long v = 0;

    readCpuUsage(sample);
    readMemory(sample);

    for (const auto& s : cpuFreqs_) {
        if (!readLong(s, v)) continue;
        const double mhz = static_cast<double>(v) / 1000.0;  // kHz → MHz
        if (std::isnan(sample.cpuFreqMHz) || mhz > sample.cpuFreqMHz) sample.cpuFreqMHz = mhz;
    }
    if (readLong(cpuBigFreq_, v)) sample.cpuBigFreqMHz = static_cast<double>(v) / 1000.0;
    if (readLong(gpuFreq_, v)) sample.gpuFreqMHz = static_cast<double>(v) / 1e6;  // Hz → MHz
    sample.gpuMaxFreqMHz = gpuMaxFreqMHz_;

    sample.cpuTempC = maxTempC(cpuTemps_);
    sample.gpuTempC = maxTempC(gpuTemps_);
    if (readLong(ddrTemp_, v)) sample.ddrTempC = static_cast<double>(v) / 1000.0;

    // fan1_input はタコパルスの周期数で、1 回転 = 2 周期。Valve の SteamOS に入っている
    // ファン制御の設定も 2 で割って回転数にしているので、それに合わせる
    if (readLong(fan_, v)) sample.fanRpm = static_cast<double>(v) / kFanRpmScale;

    // 電力とバッテリーは読むのが重い（I2C。電力は 1ch あたり CPU 約 0.1ms）うえに値の更新も遅いので、
    // 間隔をあけて読み、あいだは前回の値を使う
    const double now = monotonicSeconds();
    if (lastPowerRead_ < 0 || now - lastPowerRead_ >= kPowerIntervalSec - 0.1) {
        lastPowerRead_ = now;
        readPower(cached_);
    }
    if (lastBatteryRead_ < 0 || now - lastBatteryRead_ >= kBatteryIntervalSec - 0.1) {
        lastBatteryRead_ = now;
        readBattery(cached_);
    }
    if (lastWifiRead_ < 0 || now - lastWifiRead_ >= kWifiIntervalSec - 0.1) {
        lastWifiRead_ = now;
        cached_.wifi = wifiLink_.read(now);
    }
    if (lastIioRead_ < 0 || now - lastIioRead_ >= kIioIntervalSec - 0.1) {
        lastIioRead_ = now;
        readIioTemps(cached_);
    }
    sample.wifi = cached_.wifi;
    sample.displayTempC = cached_.displayTempC;
    sample.exhaustTempC = cached_.exhaustTempC;
    sample.heatsinkTempC = cached_.heatsinkTempC;

    // GPU 使用率: ゲーム中は fdinfo の読み直しが 1 回 1.6ms ほどかかるので 2 秒おき
    if (lastGpuRead_ < 0 || now - lastGpuRead_ >= kGpuIntervalSec - 0.1) {
        lastGpuRead_ = now;
        cached_.gpuBusyPct = gpuUsage_.update(now);
    }
    sample.gpuBusyPct = cached_.gpuBusyPct;

    // 毎回読む軽いもの: 熱による制限（1 回 0.03ms 程度）
    sample.throttleCpu = anyCooling(cpuCooling_);
    sample.throttleGpu = anyCooling(gpuCooling_);
    sample.powerMainW = cached_.powerMainW;
    sample.powerSumW = cached_.powerSumW;
    sample.powerChannels = cached_.powerChannels;
    sample.batteryPct = cached_.batteryPct;
    sample.batteryVoltageV = cached_.batteryVoltageV;
    sample.batteryCurrentMa = cached_.batteryCurrentMa;
    sample.batteryTempC = cached_.batteryTempC;
    sample.batteryStatus = cached_.batteryStatus;
    sample.chargerOnline = cached_.chargerOnline;

    return sample;
}

std::string Sensors::describe() const {
    std::ostringstream out;
    const auto state = [](const Source& s) { return s.fd >= 0 ? s.path : std::string("（見つからない）"); };
    out << "CPU 温度: " << cpuTemps_.size() << " 個";
    for (const auto& s : cpuTemps_) out << " " << s.name;
    out << "\nGPU 温度: " << gpuTemps_.size() << " 個";
    for (const auto& s : gpuTemps_) out << " " << s.name;
    out << "\nDDR 温度: " << state(ddrTemp_);
    out << "\nCPU クロック: " << cpuFreqs_.size() << " policy / 大コア " << state(cpuBigFreq_);
    out << "\nGPU クロック: " << state(gpuFreq_);
    out << "\n消費電力: " << powers_.size() << " ch";
    for (const auto& s : powers_) out << "\n  " << s.name << " -> " << s.path;
    out << "\nファン: " << state(fan_);
    out << "\nバッテリー: " << state(batCapacity_);
    out << "\n充電器: " << state(chargerOnline_);
    out << "\niio 温度: " << state(displayTemp_) << " / " << state(exhaustTemp_) << " / " << state(heatsinkTemp_);
    out << "\n熱制限: CPU " << cpuCooling_.size() << " 個";
    for (const auto& s : cpuCooling_) out << " " << s.name;
    out << " / GPU " << gpuCooling_.size() << " 個";
    for (const auto& s : gpuCooling_) out << " " << s.name;
    return out.str();
}

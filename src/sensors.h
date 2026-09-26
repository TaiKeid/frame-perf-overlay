// /proc と /sys からの読み取り（読むだけ。書き込みは一切しない）。
#pragma once

#include "gpu_usage.h"
#include "wifi_link.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

/** 値が取れなかったことを表す NaN。 */
constexpr double kNoValue = std::numeric_limits<double>::quiet_NaN();

/**
 * 消費電力センサーの 1 チャンネル分（max34417 の power*_input）。
 */
struct PowerChannel {
    std::string chip;    ///< hwmon の name（max34417_10 など）
    std::string label;   ///< power*_label（vph、apc0 など）
    double watts = kNoValue;
};

/**
 * 1 回の読み取り結果。取れなかった値は kNoValue（NaN）。
 */
struct SensorSample {
    // CPU
    double cpuUsagePct = kNoValue;     ///< 全体の使用率
    double cpuMaxCorePct = kNoValue;   ///< 最も忙しいコアの使用率
    double cpuFreqMHz = kNoValue;      ///< いちばん速いクラスタの現在クロック
    double cpuBigFreqMHz = kNoValue;   ///< cpu7（大コア）の現在クロック
    // GPU
    double gpuFreqMHz = kNoValue;
    double gpuMaxFreqMHz = kNoValue;
    double gpuBusyPct = kNoValue;      ///< GPU 使用率の目安（DRM fdinfo の drm-engine-gpu から）
    // 温度（℃）
    double cpuTempC = kNoValue;        ///< cpu*-thermal / cpuss*-thermal の最大
    double gpuTempC = kNoValue;        ///< gpuss-*-thermal の最大
    double ddrTempC = kNoValue;
    double batteryTempC = kNoValue;
    double displayTempC = kNoValue;    ///< ディスプレイ（左）付近（iio vadc）
    double exhaustTempC = kNoValue;    ///< ファンの排気付近（iio vadc）
    double heatsinkTempC = kNoValue;   ///< ヒートシンクのフィン付近（iio vadc）
    // 熱による制限（cooling_device の cur_state > 0）
    bool throttleCpu = false;
    bool throttleGpu = false;
    // Steam Link の直通回線
    WifiInfo wifi;
    // 電力（W）
    double powerMainW = kNoValue;      ///< 主電源レール（vph）
    double powerSumW = kNoValue;       ///< 全チャンネルの単純合計（レールが重なっている可能性あり）
    std::vector<PowerChannel> powerChannels;
    // バッテリー
    double batteryPct = kNoValue;
    double batteryVoltageV = kNoValue;
    double batteryCurrentMa = kNoValue;  ///< current_now（符号はドライバのまま）
    std::string batteryStatus;           ///< Charging / Discharging / Not charging / Full
    bool chargerOnline = false;
    // その他
    double fanRpm = kNoValue;          ///< 回転数（fan1_input / 2）
    double memUsedGiB = kNoValue;
    double memTotalGiB = kNoValue;
};

/**
 * センサーの場所を起動時に一度だけ探し、以後は開いたままのファイルを読み直すクラス。
 */
class Sensors {
public:
    Sensors() = default;
    ~Sensors();
    Sensors(const Sensors&) = delete;
    Sensors& operator=(const Sensors&) = delete;

    /**
     * hwmon・thermal_zone・power_supply などを走査して、読むファイルを開いておく。
     * 走査はここだけ（起動時の 1 回）。
     */
    void discover();

    /**
     * 開いてあるファイルを読み直して値を返す。CPU 使用率は前回との差から出すので初回は NaN。
     * 重いものは間隔をあけて実際に読み、あいだは前回の値を返す:
     * 電力・直通回線・GPU 使用率は 2 秒おき、バッテリー・iio の温度は 5 秒おき（GPU 使用率の fd 探しは 10 秒おき）。
     * @return 読み取り結果
     */
    SensorSample read();

    /**
     * 見つかったセンサーの一覧（--print 用）。
     * @return 人が読むための複数行の文字列
     */
    std::string describe() const;

private:
    /** 開いたままにする読み取り専用ファイル。 */
    struct Source {
        std::string path;
        std::string name;  ///< 表示・分類用の名前
        int fd = -1;
    };

    std::vector<Source> cpuTemps_;
    std::vector<Source> gpuTemps_;
    Source ddrTemp_;
    std::vector<Source> cpuFreqs_;     ///< policy*/scaling_cur_freq
    Source cpuBigFreq_;
    Source gpuFreq_;
    double gpuMaxFreqMHz_ = kNoValue;
    std::vector<Source> powers_;       ///< name = "chip/label"
    Source fan_;
    Source batCapacity_, batVoltage_, batCurrent_, batTemp_, batStatus_;
    Source chargerOnline_;
    Source procStat_;
    Source memInfo_;
    Source displayTemp_, exhaustTemp_, heatsinkTemp_;  ///< iio の温度（m℃）
    std::vector<Source> cpuCooling_;                   ///< cooling_device（cpufreq-cpu*）の cur_state
    std::vector<Source> gpuCooling_;                   ///< cooling_device（devfreq-*gpu）の cur_state
    GpuUsage gpuUsage_;
    WifiLink wifiLink_;

    static constexpr double kPowerIntervalSec = 2.0;    ///< 電力を読む間隔（センサー自体の更新も 1.5〜2 秒おき）
    static constexpr double kBatteryIntervalSec = 5.0;  ///< バッテリーを読む間隔
    static constexpr double kWifiIntervalSec = 2.0;     ///< 直通回線を読む間隔
    static constexpr double kGpuIntervalSec = 2.0;      ///< GPU 使用率を読む間隔（ゲーム中は 1 回 1.6ms ほど）
    static constexpr double kIioIntervalSec = 5.0;      ///< iio の温度を読む間隔（ADC の変換で 1 回数 ms かかる）
    double lastPowerRead_ = -1.0;
    double lastBatteryRead_ = -1.0;
    double lastWifiRead_ = -1.0;
    double lastIioRead_ = -1.0;
    double lastGpuRead_ = -1.0;
    SensorSample cached_;              ///< 間隔をあけて読む値（電力・バッテリー・直通回線・iio 温度）の前回値

    std::vector<uint64_t> prevBusy_;   ///< /proc/stat の前回値（[0] が全体、以降コアごと）
    std::vector<uint64_t> prevTotal_;

    /**
     * ファイルを読み取り専用で開いて Source を作る。
     * @param path パス
     * @param name 名前
     * @return 開けなかったときは fd = -1 の Source
     */
    static Source openSource(const std::string& path, const std::string& name);

    /**
     * 開いてあるファイルを先頭から読み直す。
     * @param source 読むファイル
     * @param buffer 書き込み先
     * @param size buffer の大きさ
     * @return 読んだバイト数。失敗時は -1
     */
    static long readRaw(const Source& source, char* buffer, size_t size);

    /**
     * 整数ひとつのファイルを読む。
     * @param source 読むファイル
     * @param value 書き込み先
     * @return 読めたら true
     */
    static bool readLong(const Source& source, long long& value);

    /**
     * 1 行の文字列ファイルを読む（末尾の改行は除く）。
     * @param source 読むファイル
     * @return 読んだ文字列。失敗時は空
     */
    static std::string readText(const Source& source);

    /**
     * 温度ファイル群の最大値を℃で返す（m℃ 単位を想定）。
     * @param sources 温度ファイル群
     * @return 最大値。ひとつも読めなければ NaN
     */
    static double maxTempC(const std::vector<Source>& sources);

    /**
     * /proc/stat から CPU 使用率を計算する。
     * @param sample 書き込み先
     */
    void readCpuUsage(SensorSample& sample);

    /**
     * /proc/meminfo からメモリ使用量を読む。
     * @param sample 書き込み先
     */
    void readMemory(SensorSample& sample);

    /**
     * 消費電力の全チャンネルを読む。
     * @param sample 書き込み先（電力の項目だけ書き換える）
     */
    void readPower(SensorSample& sample);

    /**
     * バッテリーと充電器の状態を読む。
     * @param sample 書き込み先（バッテリーの項目だけ書き換える）
     */
    void readBattery(SensorSample& sample);

    /**
     * iio のディスプレイ・排気・ヒートシンクの温度を読む。
     * @param sample 書き込み先（その 3 項目だけ書き換える）
     */
    void readIioTemps(SensorSample& sample);

    /**
     * cooling_device 群のどれかが制限中（cur_state > 0）か。
     * @param sources cur_state のファイル群
     * @return 1 つでも 0 より大きければ true
     */
    static bool anyCooling(const std::vector<Source>& sources);

    /**
     * 単調増加の時計で今の時刻を秒で返す。
     * @return 秒
     */
    static double monotonicSeconds();

    /** 開いたファイルをすべて閉じる。 */
    void closeAll();
};

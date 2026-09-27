// Frame Perf Overlay: Steam Frame の状態（フレーム時間・温度・電力など）を HMD 基準の小さなパネルに出す。
// 読むだけのアプリ。sysfs への書き込み・sudo・カメラへのアクセスはしない。
#include "autostart.h"
#include "config.h"
#include "draw.h"
#include "i18n.h"
#include "panel.h"
#include "settings_panel.h"
#include "theme.h"
#include "sensors.h"
#include "update_check.h"  // vendor/frame-updater/cpp
#include "vr_overlay.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// バージョンは CMakeLists.txt の project(VERSION) から入る
#ifndef FRAME_PERF_OVERLAY_VERSION
#define FRAME_PERF_OVERLAY_VERSION "unknown"
#endif

namespace {

volatile std::sig_atomic_t gStopRequested = 0;
volatile std::sig_atomic_t gToggleRequested = 0;  ///< SIGUSR1（2 つ目の起動から）でパネルの表示を切り替える

/** 設定パネルの「アプリを終了」で終わったときの終了コード（systemd の RestartPreventExitStatus= に書く）。 */
constexpr int kExitCodeUserQuit = 3;

constexpr int kThumbnailSize = 256;         ///< ダッシュボードのサムネイルの一辺（px）
constexpr double kSettingsPollSec = 0.05;       ///< 設定パネルを操作している間、マウスのイベントを確かめる間隔
constexpr double kSettingsIdlePollSec = 0.1;    ///< 設定パネルが見えているだけ（ポインターが動いていない）ときの間隔
constexpr double kSettingsActiveHoldSec = 3.0;  ///< 最後のマウスのイベントから、この秒数は操作中とみなす
constexpr double kSlowCheckSec = 0.5;       ///< SteamVR のイベント・設定ファイル・見えているかなどを確かめる最短の間隔
constexpr double kControllerIntervalSec = 10.0;  ///< コントローラーの電池を読む間隔
constexpr double kAutostartRefreshSec = 3.0;     ///< 設定パネルが見えている間に自動起動の状態を読み直す間隔

/** コマンドラインの内容。 */
struct Options {
    enum class Mode { Overlay, Print, DumpPng, DumpSettingsPng, DumpFrameTimings, ContrastReport, Help, Version };
    Mode mode = Mode::Overlay;
    std::string configPath;
    std::string pngPath;
    std::string thumbnailPngPath;
    int thumbnailSize = 256;  ///< --thumbnail-png で書き出す一辺（px）
    int printCount = 5;
    double dumpSeconds = 4.0;
    double timingSeconds = 5.0;
    bool timingSecondsGiven = false;
    bool secondsGiven = false;
    bool fakeThrottle = false;
    std::string language;     ///< 空でなければ PNG の書き出しで設定の言語の代わりに使う（ja / en）
    bool previewQuit = false; ///< --dump-settings-png で「もう一度押すと終了」の状態を描く
    bool forceSettingsVisible = false;  ///< 診断用: 設定パネルが見えているものとして 50ms の周期で回す
    std::string previewAutostart;  ///< --dump-settings-png で自動起動をこの状態として描く（enabled / disabled / notinstalled / busy / failed）
    std::string previewUpdate;  ///< --dump-settings-png で新しい版の確認をこの状態として描く
                                 ///< （uptodate / available / manual / confirm / installing / installed / checkfailed / installfailed）
    bool fakeFrames = false;
    bool fakeWarnings = false;
    double fakeFps = 0.0;
    std::string fakeWifi;  ///< 空でなければ直通回線をダミーにする（dBm の数字か none）
    bool verbose = false;
};

/**
 * SIGTERM / SIGINT を受けたら止める印をつける。
 * @param signal 受けたシグナル（使わない）
 */
void onSignal(int /*signal*/) {
    gStopRequested = 1;
}

/**
 * SIGUSR1 を受けたら、パネルの表示を切り替える印をつける（2 つ目の起動が送ってくる）。
 * @param signal 受けたシグナル（使わない）
 */
void onToggleSignal(int /*signal*/) {
    gToggleRequested = 1;
}

/**
 * SIGTERM / SIGINT で行儀よく終われるようにし、SIGUSR1 で表示を切り替えられるようにする。
 */
void installSignalHandlers() {
    struct sigaction toggle {};
    toggle.sa_handler = onToggleSignal;
    sigemptyset(&toggle.sa_mask);
    sigaction(SIGUSR1, &toggle, nullptr);

    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

/**
 * 単調増加の時計で今の時刻を秒で返す。
 * @return 秒
 */
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/**
 * このスレッドが使った CPU 時間を秒で返す（--verbose の処理時間の内訳用）。
 * @return 秒
 */
double cpuSeconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

/**
 * 止める印がつくまで、または指定時間が経つまで待つ。
 * @param seconds 待つ秒数
 */
void sleepInterruptible(double seconds) {
    const double end = nowSeconds() + seconds;
    while (!gStopRequested && !gToggleRequested) {
        const double left = end - nowSeconds();
        if (left <= 0) break;
        std::this_thread::sleep_for(std::chrono::duration<double>(std::fmin(left, 0.5)));
    }
}

/**
 * 今の日時を "YYYY-MM-DD HH:MM:SS" で返す。
 * @return 日時の文字列
 */
std::string localTimeText() {
    const std::time_t t = std::time(nullptr);
    std::tm tm {};
    localtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

/**
 * 数値を文字列にする。NaN は "--"。
 * @param value 値
 * @param decimals 小数点以下の桁数
 * @return 文字列
 */
std::string fmt(double value, int decimals) {
    if (std::isnan(value)) return "--";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

/** 使い方を表示する。 */
void printUsage() {
    std::printf(
        "使い方: frame-perf-overlay [オプション]\n"
        "  （なし）             SteamVR のオーバーレイとして常駐する（SteamVR が無ければ待つ）\n"
        "  --print [--count N]  OpenVR なしで、読み取った値を 1 秒おきに N 回（既定 5）表示して終わる\n"
        "  --dump-png PATH      OpenVR なしでパネル画像を PNG に書き出して終わる\n"
        "      --seconds S      書き出す前に S 秒（既定 4）値を集めてグラフに入れる\n"
        "      --fake-frames    フレーム時間とコントローラーにダミーの値を入れる（見た目の確認用）\n"
        "      --fake-warnings  警告色の確認用に、熱で制限中・電波弱・電池少などのダミーの値を入れる\n"
        "      --fake-fps N     fps が落ちたときの見た目用に、アプリの fps をダミーで N 付近にする\n"
        "      --fake-wifi D    直通回線をダミーにする（D は電波の dBm、none で未接続。home:D で家の Wi-Fi）\n"
        "      --fake-throttle  SteamVR がアプリを半分に抑えている状態（72Hz で 36 fps、再投影 50%%）をダミーで出す\n"
        "  --dump-settings-png PATH  OpenVR なしで設定パネル（ダッシュボード）の画像を PNG に書き出して終わる\n"
        "      --thumbnail-png PATH  あわせてダッシュボードのサムネイル（ランチャーのアイコンと同じ絵）も書き出す\n"
        "      --thumbnail-size N    そのサムネイルの一辺（既定 256）\n"
        "      --preview-quit   「もう一度押すと終了」の状態で描く\n"
        "      --preview-autostart S  自動起動をこの状態で描く（enabled / disabled / notinstalled / busy / failed）\n"
        "      --preview-update S     新しい版の確認をこの状態で描く（uptodate / available / manual / confirm /\n"
        "                             installing / installed / checkfailed / installfailed。既定は uptodate）\n"
        "  --language ja|en     PNG の書き出しで、設定の言語の代わりにこの言語で描く\n"
        "  --config PATH        設定ファイル（既定 ~/.config/frame-perf-overlay/config.json）\n"
        "  --verbose            オーバーレイ中、数秒ごとに値を標準エラーに出す\n"
        "  --force-settings-visible  診断用: 設定パネルが見えているものとして 50ms の周期で回す（負荷の計測用）\n"
        "  --contrast-report    画面の文字色・部品の色と背景の組み合わせごとに、WCAG のコントラスト比と合否を出す\n"
        "  --dump-frame-timings [S]  診断用: SteamVR につないで、届いたフレームの生のタイミングを S 秒（既定 5）出して終わる\n"
        "                       （Background 型で読むだけなので、常駐しているインスタンスと同時に使える。--seconds S でも可）\n"
        "  --version            バージョンを表示して終わる\n");
}

/**
 * コマンドラインを読む。
 * @param argc 引数の数
 * @param argv 引数
 * @param options 書き込み先
 * @return 正しく読めたら true
 */
bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = i + 1 < argc;
        if (arg == "--print") {
            options.mode = Options::Mode::Print;
        } else if (arg == "--count" && hasNext) {
            options.printCount = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--dump-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.pngPath = argv[++i];
        } else if (arg == "--dump-settings-png" && hasNext) {
            options.mode = Options::Mode::DumpSettingsPng;
            options.pngPath = argv[++i];
        } else if (arg == "--dump-frame-timings") {
            options.mode = Options::Mode::DumpFrameTimings;
            if (hasNext && argv[i + 1][0] != '-') {
                options.timingSeconds = std::fmax(0.5, std::atof(argv[++i]));
                options.timingSecondsGiven = true;
            }
        } else if (arg == "--thumbnail-png" && hasNext) {
            options.thumbnailPngPath = argv[++i];
        } else if (arg == "--thumbnail-size" && hasNext) {
            options.thumbnailSize = std::max(16, std::min(1024, std::atoi(argv[++i])));
        } else if (arg == "--seconds" && hasNext) {
            options.dumpSeconds = std::fmax(0.0, std::atof(argv[++i]));
            options.secondsGiven = true;
        } else if (arg == "--fake-frames") {
            options.fakeFrames = true;
        } else if (arg == "--fake-fps" && hasNext) {
            options.fakeFps = std::fmax(1.0, std::atof(argv[++i]));
        } else if (arg == "--language" && hasNext) {
            options.language = argv[++i];
            Language check;
            if (!parseLanguage(options.language, check)) {
                std::fprintf(stderr, "--language は ja か en です: %s\n", options.language.c_str());
                return false;
            }
        } else if (arg == "--preview-autostart" && hasNext) {
            options.previewAutostart = argv[++i];
        } else if (arg == "--preview-update" && hasNext) {
            options.previewUpdate = argv[++i];
        } else if (arg == "--contrast-report") {
            options.mode = Options::Mode::ContrastReport;
        } else if (arg == "--force-settings-visible") {
            options.forceSettingsVisible = true;
        } else if (arg == "--preview-quit") {
            options.previewQuit = true;
        } else if (arg == "--fake-throttle") {
            options.fakeThrottle = true;
        } else if (arg == "--fake-wifi" && hasNext) {
            options.fakeWifi = argv[++i];
        } else if (arg == "--fake-warnings") {
            options.fakeWarnings = true;
        } else if (arg == "--config" && hasNext) {
            options.configPath = argv[++i];
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--version") {
            options.mode = Options::Mode::Version;
        } else if (arg == "--help" || arg == "-h") {
            options.mode = Options::Mode::Help;
        } else {
            std::fprintf(stderr, "知らない引数です: %s\n", arg.c_str());
            return false;
        }
    }
    if (options.configPath.empty()) options.configPath = defaultConfigPath();
    // --dump-frame-timings --seconds S の書き方も受け付ける
    if (options.mode == Options::Mode::DumpFrameTimings && options.secondsGiven && !options.timingSecondsGiven) {
        options.timingSeconds = std::fmax(0.5, options.dumpSeconds);
    }
    return true;
}

/**
 * 読み取り結果を端末向けに書き出す（--print 用）。
 * @param s 読み取り結果
 */
void printSample(const SensorSample& s) {
    std::printf("  CPU    使用率 %s%%（最大コア %s%%） 最速クラスタ %sMHz  大コア %sMHz\n", fmt(s.cpuUsagePct, 1).c_str(),
                fmt(s.cpuMaxCorePct, 1).c_str(), fmt(s.cpuFreqMHz, 0).c_str(), fmt(s.cpuBigFreqMHz, 0).c_str());
    std::printf("  GPU    使用率 %s%%（目安）  %sMHz（最大 %sMHz）\n", fmt(s.gpuBusyPct, 1).c_str(),
                fmt(s.gpuFreqMHz, 0).c_str(), fmt(s.gpuMaxFreqMHz, 0).c_str());
    std::printf("  温度   CPU %s℃  GPU %s℃  DDR %s℃  電池 %s℃\n", fmt(s.cpuTempC, 1).c_str(), fmt(s.gpuTempC, 1).c_str(),
                fmt(s.ddrTempC, 1).c_str(), fmt(s.batteryTempC, 1).c_str());
    std::printf("         画面 %s℃  排気 %s℃  放熱 %s℃   熱制限 CPU %s / GPU %s\n", fmt(s.displayTempC, 1).c_str(),
                fmt(s.exhaustTempC, 1).c_str(), fmt(s.heatsinkTempC, 1).c_str(), s.throttleCpu ? "中" : "なし",
                s.throttleGpu ? "中" : "なし");
    if (!s.wifi.interfaceUp) {
        std::printf("  無線   インターフェースなし（直通回線の AP も、家の Wi-Fi の子機も見つからない）\n");
    } else if (!s.wifi.connected) {
        std::printf("  無線   未接続（直通回線に相手がいない・家の Wi-Fi にもつながっていない）\n");
    } else {
        // 直通回線の相手は PC、家の Wi-Fi の相手はつないでいる AP
        std::printf("  %s 受信 %s Mbps  送信 %s Mbps  リンク 送 %s / 受 %s Mbps  電波 %s dBm\n",
                    s.wifi.homeWifi ? "Wi-Fi " : "直通  ", fmt(s.wifi.rxMbps, 2).c_str(), fmt(s.wifi.txMbps, 2).c_str(),
                    fmt(s.wifi.txLinkMbps, 1).c_str(), fmt(s.wifi.rxLinkMbps, 1).c_str(),
                    fmt(s.wifi.signalDbm, 0).c_str());
    }
    std::printf("  電力   vph %sW  全ch計 %sW\n        ", fmt(s.powerMainW, 3).c_str(), fmt(s.powerSumW, 3).c_str());
    std::string chip;
    for (const auto& ch : s.powerChannels) {
        if (ch.chip != chip) {
            if (!chip.empty()) std::printf("\n        ");
            std::printf("%s:", ch.chip.c_str());
            chip = ch.chip;
        }
        std::printf(" %s=%s", ch.label.c_str(), fmt(ch.watts, 3).c_str());
    }
    std::printf("\n  ファン %srpm\n", fmt(s.fanRpm, 0).c_str());
    std::printf("  電池   %s%%  %sV  %smA  %s（充電器 %s）\n", fmt(s.batteryPct, 0).c_str(),
                fmt(s.batteryVoltageV, 3).c_str(), fmt(s.batteryCurrentMa, 0).c_str(), s.batteryStatus.c_str(),
                s.chargerOnline ? "あり" : "なし");
    std::printf("  メモリ %s / %s GiB\n", fmt(s.memUsedGiB, 2).c_str(), fmt(s.memTotalGiB, 2).c_str());
}

/**
 * --print: OpenVR なしで値を数回表示する。
 * @param options コマンドライン
 * @return 終了コード
 */
int runPrint(const Options& options) {
    Sensors sensors;
    sensors.discover();
    std::printf("== 見つかったセンサー ==\n%s\n", sensors.describe().c_str());
    sensors.read();  // CPU 使用率の基準を作る
    for (int i = 1; i <= options.printCount && !gStopRequested; ++i) {
        sleepInterruptible(1.0);
        const SensorSample s = sensors.read();
        std::printf("== [%d/%d] %s ==\n", i, options.printCount, localTimeText().c_str());
        printSample(s);
        std::fflush(stdout);
    }
    return 0;
}

/**
 * 見た目の確認用のダミーのフレーム時間を作る。
 * @param t 時刻（秒）
 * @param fps 0 より大きければ、アプリの fps をこの値のあたりにする（fps が落ちたときの見た目用）
 * @return それらしく揺れる値
 */
FrameStats fakeFrameStats(double t, double fps) {
    FrameStats f;
    f.valid = true;
    f.haveTimes = true;
    f.displayHz = 90.0;
    f.targetMs = 1000.0 / 90.0;
    // fps: 指定がなければ 90 付近で、ときどき少し落ちる
    f.appFps = fps > 0 ? fps + 1.5 * std::sin(t * 2.1) : 90.0 - (std::fmod(t, 7.0) < 0.6 ? 6.0 : 0.3 * std::fabs(std::sin(t)));
    f.gpuMs = 8.0 + 1.5 * std::sin(t * 1.3) + (std::fmod(t, 7.0) < 0.6 ? 4.0 : 0.0);
    f.gpuMaxMs = f.gpuMs + 1.0;
    f.cpuMs = 5.0 + 1.0 * std::sin(t * 0.7 + 1.0);
    f.cpuMaxMs = f.cpuMs + 0.8;
    f.frames = 45;
    f.reprojectedPct = f.gpuMs > f.targetMs ? 12.0 : 0.0;
    if (fps > 0) {
        // fps が落ちている画像: GPU が重く、再投影が多い状態にする
        f.gpuMs = 14.5 + 1.0 * std::sin(t * 1.3);
        f.reprojectedPct = 100.0 * (1.0 - fps / 90.0);
    }
    f.dropped = 0;
    return f;
}

/**
 * 見た目の確認用のダミーのコントローラーの電池（左は充電中）。
 * @param low 電池少の状態にするか
 * @return 左右の電池
 */
ControllerStatus fakeControllers(bool low) {
    ControllerStatus status;
    status.left.present = true;
    status.left.pct = low ? 8.0 : 80.0;
    status.left.charging = true;
    status.right.present = !low;  // 電池少の画像では右手を「つながっていない」にする
    status.right.pct = 75.0;
    return status;
}

/**
 * 警告色の確認用に、読み取り結果の一部をダミーの悪い値に置き換える。
 * @param sample 書き換える読み取り結果
 */
void applyFakeWarnings(SensorSample& sample) {
    sample.throttleCpu = true;
    sample.throttleGpu = true;
    sample.gpuBusyPct = 97.0;
    sample.cpuTempC = 86.0;
    sample.batteryPct = 12.0;
    sample.batteryStatus = "Discharging";
    sample.chargerOnline = false;
    sample.wifi.interfaceUp = true;
    sample.wifi.connected = true;
    sample.wifi.signalDbm = -81.0;
    sample.wifi.rxMbps = 42.5;
    sample.wifi.txMbps = 1.2;
    sample.wifi.txLinkMbps = 288.2;
    sample.wifi.rxLinkMbps = 216.1;
}

/**
 * 見た目の確認用に、SteamVR がアプリを半分に抑えている状態（Half-Life: Alyx で実際に見えた値）にする。
 * @param frame 書き換えるフレームの集計
 * @param t 時刻（秒）
 */
void applyFakeThrottle(FrameStats& frame, double t) {
    frame.displayHz = 72.0;
    frame.targetMs = 1000.0 / 72.0;
    frame.appFps = 36.0 - (std::fmod(t, 6.0) < 0.5 ? 2.0 : 0.0);
    frame.gpuMs = 22.0 + 1.0 * std::sin(t * 1.3);
    frame.cpuMs = 4.4 + 0.5 * std::sin(t * 0.7);
    frame.reprojectedPct = 50.0;
    frame.dropped = 0;
    frame.throttledFrames = 1;
}

/**
 * 見た目の確認用に、直通回線をダミーの状態にする。
 * @param sample 書き換える読み取り結果
 * @param value 電波の dBm（例: "-65"。直通回線）、"home:-47"（家の Wi-Fi）、または "none"（未接続）
 */
void applyFakeWifi(SensorSample& sample, const std::string& value) {
    sample.wifi = WifiInfo();
    sample.wifi.interfaceUp = true;
    if (value == "none") return;  // ステーションなし = 未接続
    sample.wifi.connected = true;
    const bool home = value.rfind("home:", 0) == 0;
    sample.wifi.homeWifi = home;
    sample.wifi.signalDbm = std::atof(value.c_str() + (home ? 5 : 0));
    sample.wifi.rxMbps = 187.4;
    sample.wifi.txMbps = 2.1;
    sample.wifi.txLinkMbps = 1152.8;
    sample.wifi.rxLinkMbps = 960.7;
}

/**
 * 更新チェッカーの設定を作る（スクリプトの場所は install.sh が置いた ~/.local/share/frame-perf-overlay/）。
 * @return 設定
 */
frame_updater::UpdaterConfig makeUpdaterConfig() {
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    std::string dataHome;
    if (xdgData != nullptr && xdgData[0] != '\0') {
        dataHome = xdgData;
    } else {
        const char* home = std::getenv("HOME");
        dataHome = std::string(home != nullptr ? home : ".") + "/.local/share";
    }
    frame_updater::UpdaterConfig cfg;
    cfg.script = dataHome + "/frame-perf-overlay/frame-update.sh";
    cfg.app = "frame-perf-overlay";
    cfg.repo = "sasaken1102r/frame-perf-overlay";
    cfg.currentVersion = FRAME_PERF_OVERLAY_VERSION;
    cfg.assetPattern = "frame-perf-overlay-{version}.tar.gz";
    return cfg;
}

/**
 * 見た目の確認用に、新しい版の確認の状態をダミーで作る（--preview-update 用）。
 * @param name 状態の名前（uptodate / available / manual / confirm / installing / installed / checkfailed / installfailed）
 * @return ダミーの状態（知らない名前なら uptodate）
 */
frame_updater::UpdateStatus fakeUpdateStatus(const std::string& name) {
    using frame_updater::UpdateState;
    frame_updater::UpdateStatus s;
    s.current = FRAME_PERF_OVERLAY_VERSION;
    s.latest = FRAME_PERF_OVERLAY_VERSION;
    s.checkedAt = std::time(nullptr);
    if (name == "available" || name == "confirm") {
        s.state = UpdateState::Available;
        s.latest = std::string(FRAME_PERF_OVERLAY_VERSION) + "-preview";
        s.installable = true;
        s.url = "https://github.com/sasaken1102r/frame-perf-overlay/releases/tag/v" + s.latest;
    } else if (name == "manual") {
        s.state = UpdateState::Available;
        s.latest = std::string(FRAME_PERF_OVERLAY_VERSION) + "-preview";
        s.installable = false;
        s.reason = "no-checksums";
    } else if (name == "installing") {
        s.state = UpdateState::Installing;
        s.version = s.latest;
        s.step = "download";
    } else if (name == "installed") {
        s.state = UpdateState::Installed;
        s.version = s.latest;
    } else if (name == "checkfailed") {
        s.state = UpdateState::CheckFailed;
        s.error = "network";
    } else if (name == "installfailed") {
        s.state = UpdateState::InstallFailed;
        s.version = s.latest;
        s.error = "checksum-mismatch";
    } else {
        s.state = UpdateState::UpToDate;
    }
    return s;
}

/**
 * --dump-png: OpenVR なしで値を集めてパネルを描き、PNG に書き出す。
 * @param options コマンドライン
 * @return 終了コード
 */
int runDumpPng(const Options& options) {
    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    if (!options.language.empty()) parseLanguage(options.language, config.language);
    Sensors sensors;
    sensors.discover();
    FontSet fonts;
    fonts.load(config.fontPath, config.boldFontPath);
    PanelRenderer renderer(fonts);
    PanelState state;

    sensors.read();  // CPU 使用率の基準
    const double interval = config.updateIntervalMs / 1000.0;
    const double start = nowSeconds();
    do {
        sleepInterruptible(interval);
        const double t = nowSeconds();
        const bool fake = options.fakeFrames || options.fakeWarnings || options.fakeFps > 0 || options.fakeThrottle;
        FrameStats frame = fake ? fakeFrameStats(t - start, options.fakeFps) : FrameStats();
        if (options.fakeThrottle) applyFakeThrottle(frame, t - start);
        SensorSample sample = sensors.read();
        if (options.fakeWarnings) applyFakeWarnings(sample);
        if (!options.fakeWifi.empty()) applyFakeWifi(sample, options.fakeWifi);
        state.update(t, sample, frame, fake, config.graphSeconds);
        if (fake) state.controllers = fakeControllers(options.fakeWarnings);
    } while (!gStopRequested && nowSeconds() - start < options.dumpSeconds);

    renderer.render(state, config);
    if (!renderer.writePng(options.pngPath)) {
        std::fprintf(stderr, "PNG を書き出せませんでした: %s\n", options.pngPath.c_str());
        return 1;
    }
    std::printf("PNG を書き出しました: %s（%dx%d）\n", options.pngPath.c_str(), renderer.width(), renderer.height());
    return 0;
}

/**
 * --dump-settings-png: OpenVR なしで設定パネル（とサムネイル）を描いて PNG に書き出す。
 * @param options コマンドライン
 * @return 終了コード
 */
int runDumpSettingsPng(const Options& options) {
    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    if (!options.language.empty()) parseLanguage(options.language, config.language);
    FontSet fonts;
    fonts.load(config.fontPath, config.boldFontPath);
    SettingsPanel panel(fonts);
    if (options.previewQuit) panel.armQuitForPreview();
    // 自動起動: ふだんは実際の状態を読む（読むだけ）。見た目の確認用に状態を指定することもできる
    Autostart autostart;
    AutostartStatus autostartView = autostart.status();
    const std::string& preview = options.previewAutostart;
    if (preview == "enabled") autostartView = {AutostartStatus::State::Enabled, false, false};
    if (preview == "disabled") autostartView = {AutostartStatus::State::Disabled, false, false};
    if (preview == "notinstalled") autostartView = {AutostartStatus::State::NotInstalled, false, false};
    if (preview == "busy") autostartView = {AutostartStatus::State::Enabled, true, false};
    if (preview == "failed") autostartView = {AutostartStatus::State::Enabled, false, true};
    const frame_updater::UpdateStatus updateView = fakeUpdateStatus(options.previewUpdate);
    if (options.previewUpdate == "confirm") panel.armUpdateConfirmForPreview();
    panel.render(config, autostartView, updateView);
    if (!panel.writePng(options.pngPath)) {
        std::fprintf(stderr, "PNG を書き出せませんでした: %s\n", options.pngPath.c_str());
        return 1;
    }
    std::printf("PNG を書き出しました: %s（%dx%d）\n", options.pngPath.c_str(), panel.width(), panel.height());
    if (!options.thumbnailPngPath.empty()) {
        std::vector<uint8_t> rgba;
        renderThumbnail(fonts, options.thumbnailSize, rgba, options.thumbnailPngPath);
        std::printf("サムネイルを書き出しました: %s（%dx%d）\n", options.thumbnailPngPath.c_str(), options.thumbnailSize,
                    options.thumbnailSize);
    }
    return 0;
}

/**
 * --dump-frame-timings: SteamVR に Background 型でつないで（起動していなければ待たずに終わる）、生のフレームタイミングを出す。
 * オーバーレイも Vulkan も作らないので、常駐しているインスタンスと同時に使える。
 * @param options コマンドライン
 * @return 終了コード
 */
int runDumpFrameTimings(const Options& options) {
    VrOverlay vr;
    std::string message;
    if (!vr.connectReadOnly(message)) {
        std::fprintf(stderr, "[VR] 接続できません: %s\n", message.c_str());
        return 1;
    }
    vr.dumpFrameTimings(options.timingSeconds, gStopRequested);
    vr.shutdown();
    return 0;
}

/**
 * 設定パネルの操作を設定に反映し、性能パネルに当てて、設定ファイルに保存する。
 * @param action 押されたボタンの操作
 * @param config 今の設定（書き換える）
 * @param watcher 設定ファイルの監視役（自分の書き込みを読み直さないよう知らせる）
 * @param vr オーバーレイ
 * @param angleStepDeg 向きのボタンの刻み（度。設定パネルの「1° ずつ / 5° ずつ」）
 */
void handleSettingsAction(SettingsAction action, Config& config, ConfigWatcher& watcher, VrOverlay& vr,
                          double angleStepDeg = 1.0) {
    if (!applySettingsAction(action, config, angleStepDeg)) return;
    vr.applyConfig(config);
    std::string error;
    if (saveConfig(watcher.path(), config, error)) {
        watcher.noteSaved();
    } else {
        std::fprintf(stderr, "[設定] 保存に失敗: %s\n", error.c_str());
    }
}

/**
 * 常駐のロックファイルのパス（$XDG_RUNTIME_DIR の下。無ければ /tmp にユーザーごと）。
 * @return パス
 */
std::string lockFilePath() {
    // Steam から・systemd から・SSH から起動しても同じ場所になるよう、XDG_RUNTIME_DIR が無ければ /run/user/<uid> を使う
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] != '\0') return std::string(runtime) + "/frame-perf-overlay.lock";
    const std::string userRuntime = "/run/user/" + std::to_string(::getuid());
    if (::access(userRuntime.c_str(), W_OK) == 0) return userRuntime + "/frame-perf-overlay.lock";
    return "/tmp/frame-perf-overlay-" + std::to_string(::getuid()) + ".lock";
}

/**
 * 常駐のロックを取る。取れたら自分の PID を書いて、ファイルを開いたままにする（終われば OS がロックを外す）。
 * 取れなければ、ロックを持っている常駐側の PID を返す。
 * @param lockFd 取れたときのファイル（開いたままにする）の書き込み先
 * @param holderPid 取れなかったときの、常駐側の PID の書き込み先（読めなければ 0）
 * @return ロックを取れた（またはロックを使えないので、そのまま起動してよい）なら true
 */
bool acquireInstanceLock(int& lockFd, pid_t& holderPid) {
    lockFd = -1;
    holderPid = 0;
    const std::string path = lockFilePath();
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "[起動] ロックファイル %s を開けないので、二重起動の確認をせずに起動します\n", path.c_str());
        return true;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        const std::string pid = std::to_string(::getpid()) + "\n";
        if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, pid.data(), pid.size(), 0) < 0) {
            std::fprintf(stderr, "[起動] ロックファイルに PID を書けませんでした\n");
        }
        lockFd = fd;
        return true;
    }
    // 常駐側がロックを取った直後で、まだ PID を書いていないことがあるので少し待って読み直す
    for (int attempt = 0; attempt < 10 && holderPid <= 0; ++attempt) {
        char buffer[32] = {};
        const ssize_t n = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) holderPid = static_cast<pid_t>(std::atol(buffer));
        if (holderPid <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::close(fd);
    return false;
}

/**
 * このアプリの systemd サービス（frame-perf-overlay.service）として起動されたかどうか。
 * INVOCATION_ID は Steam（これもサービス）から＋で起動した子にも引き継がれるので使えない。
 * 自分の cgroup がこのアプリのユニットかどうかで見分ける。
 * @return サービスとして動いていれば true
 */
bool runningAsService() {
    std::ifstream file("/proc/self/cgroup");
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("/frame-perf-overlay.service") != std::string::npos) return true;
    }
    return false;
}

/**
 * 表示の切り替えの知らせ（SIGUSR1）が来ていたら、ダッシュボードの「オン / オフ」と同じように切り替えて保存する。
 * @param config 今の設定（書き換える）
 * @param watcher 設定ファイルの監視役
 * @param vr オーバーレイ（つながっていなければ設定だけ変える）
 * @return 切り替えたら true
 */
bool handleToggleRequest(Config& config, ConfigWatcher& watcher, VrOverlay& vr) {
    if (!gToggleRequested) return false;
    gToggleRequested = 0;
    handleSettingsAction(config.visible ? SettingsAction::ShowOff : SettingsAction::ShowOn, config, watcher, vr);
    std::fprintf(stderr, "[起動] 2 つ目の起動から知らせが来たので、パネルを%sにしました\n", config.visible ? "表示" : "非表示");
    return true;
}

/**
 * オーバーレイとして常駐する。SteamVR が無ければ数秒おきに待ち、終了の知らせで静かに終わる。
 * すでに常駐していれば、そちらに表示の切り替えを知らせてすぐ終わる（VR_Init はしない）。
 * @param options コマンドライン
 * @return 終了コード
 */
int runOverlay(const Options& options) {
    int lockFd = -1;
    pid_t holderPid = 0;
    bool locked = acquireInstanceLock(lockFd, holderPid);
    if (!locked && runningAsService()) {
        // サービスは表示を切り替えない（Restart=always で切り替えが繰り返されるため）。手で起動したものが終わるのを待つ
        std::fprintf(stderr, "[起動] サービスの外で常駐しています（PID %d）。終わるまで待ちます\n",
                     static_cast<int>(holderPid));
        while (!locked && !gStopRequested) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            locked = acquireInstanceLock(lockFd, holderPid);
        }
        if (!locked) return 0;
        std::fprintf(stderr, "[起動] 常駐が終わったので起動します\n");
    }
    if (!locked) {
        if (holderPid > 0 && ::kill(holderPid, SIGUSR1) == 0) {
            std::fprintf(stderr, "[起動] すでに常駐しています（PID %d）。パネルの表示を切り替えて終わります\n",
                         static_cast<int>(holderPid));
            return 0;
        }
        std::fprintf(stderr, "[起動] すでに常駐しているようですが、知らせを送れませんでした（PID %d）\n",
                     static_cast<int>(holderPid));
        return 1;
    }

    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    Sensors sensors;
    sensors.discover();
    FontSet fonts;
    fonts.load(config.fontPath, config.boldFontPath);
    PanelRenderer renderer(fonts);
    SettingsPanel settings(fonts);
    Autostart autostart;  // 自動起動（systemd ユーザーサービス）の状態と切り替え
    frame_updater::UpdateChecker updater(makeUpdaterConfig());  // 新しい版の確認・インストール
    std::uint64_t lastUpdaterRevision = updater.revision();
    PanelState state;
    VrOverlay vr;
    vr.setVerbose(options.verbose);

    // SteamVR を待つ
    std::string lastMessage;
    while (!gStopRequested) {
        std::string message;
        const VrOverlay::ConnectResult result =
            vr.connect(renderer.width(), renderer.height(), settings.width(), settings.height(), message);
        if (result == VrOverlay::ConnectResult::Ok) break;
        if (message != lastMessage) {
            if (result == VrOverlay::ConnectResult::NotRunning) {
                std::fprintf(stderr, "[VR] SteamVR が起動していないので待ちます（3 秒おきに再試行）\n");
            } else {
                std::fprintf(stderr, "[VR] 接続に失敗: %s（3 秒後に再試行）\n", message.c_str());
            }
            lastMessage = message;
        }
        sleepInterruptible(3.0);
        handleToggleRequest(config, watcher, vr);  // SteamVR を待っている間も切り替えは受け付ける
    }
    if (gStopRequested) return 0;
    std::fprintf(stderr, "[VR] SteamVR につながりました\n");
    vr.applyConfig(config);
    if (vr.hasSettings()) {
        std::vector<uint8_t> thumbnail;
        renderThumbnail(fonts, kThumbnailSize, thumbnail);
        vr.submitThumbnail(thumbnail.data(), kThumbnailSize);
    }
    sensors.read();  // CPU 使用率の基準

    double nextPanel = nowSeconds();
    double lastVerbose = 0.0;
    double lastControllers = -1.0;
    bool settingsDirty = true;
    bool settingsVisible = false;
    bool settingsWasVisible = false;
    double lastAutostartRefresh = 0.0;
    double nextSlowCheck = 0.0;
    double lastPointerEvent = -1e9;  // 最後にマウスのイベントが来た時刻（操作中は細かく確かめる）
    bool userQuit = false;
    unsigned loopCount = 0;  // --verbose 用: 前回の表示からループが回った回数
    while (!gStopRequested && !userQuit) {
        ++loopCount;
        // 細かい周期（設定パネルが見えている間の 50ms）では設定パネルのイベントだけ見て、
        // それ以外の確認は 0.5 秒おきにする（見えていないときはループ自体が性能パネルの更新間隔で回る）
        const double loopStart = nowSeconds();
        const bool slowCheck = loopStart >= nextSlowCheck;
        if (slowCheck) {
            nextSlowCheck = loopStart + kSlowCheckSec;
            settingsDirty |= handleToggleRequest(config, watcher, vr);
            if (watcher.reloadIfChanged(config)) {
                vr.applyConfig(config);
                fonts.load(config.fontPath, config.boldFontPath);
                settingsDirty = true;
            }
            // 新しい版の確認・インストール。GitHub に行くのはスクリプト側のキャッシュが切れたときだけ
            updater.tick(config.updateCheck);
            if (updater.revision() != lastUpdaterRevision) {
                lastUpdaterRevision = updater.revision();
                settingsDirty = true;
            }
        }
        const VrEvents events = vr.pollEvents(slowCheck);
        // SteamVR 自体の終了（VREvent_Quit）は今までどおり終了コード 0。
        // ダッシュボードのアイコンの「閉じる」（VREvent_OverlayClosed）はユーザーの終了なので、設定パネルの
        // 「アプリを終了」と同じく終了コード 3（systemd でも起動し直さない）
        if (events.quit) break;
        if (events.closeRequested) {
            std::fprintf(stderr, "[VR] ダッシュボードの「閉じる」で終了します\n");
            userQuit = true;
            break;
        }
        if (slowCheck && !vr.steamVrAlive()) {
            std::fprintf(stderr, "[VR] vrserver がいなくなったので終了します\n");
            break;
        }

        // 設定パネル（ダッシュボード）への操作
        if (!events.pointer.empty()) lastPointerEvent = loopStart;
        for (const PointerInput& input : events.pointer) {
            switch (input.type) {
                case PointerInput::Type::Move: settingsDirty |= settings.pointerMove(input.x, input.y); break;
                case PointerInput::Type::Down: {
                    const SettingsAction action = settings.pointerDown(input.x, input.y, nowSeconds());
                    if (action == SettingsAction::Quit) {
                        std::fprintf(stderr, "[VR] 設定パネルの「アプリを終了」で終了します\n");
                        userQuit = true;
                    } else if (action == SettingsAction::AutostartOn || action == SettingsAction::AutostartOff) {
                        // systemctl --user enable / disable を子プロセスで始めるだけ（終わるのは待たない）
                        autostart.request(action == SettingsAction::AutostartOn);
                    } else if (action == SettingsAction::UpdateCheckNow) {
                        updater.checkNow();  // ［確認］: update_check が off でも動く
                    } else if (action == SettingsAction::UpdateConfirmYes || action == SettingsAction::UpdateRetry) {
                        updater.install();  // すぐ戻る。以後は状態ファイルを読んで進み具合を表示する
                    } else if (action == SettingsAction::UpdateDismiss) {
                        updater.dismiss();
                    } else {
                        handleSettingsAction(action, config, watcher, vr, settings.angleStepDeg());
                    }
                    settingsDirty = true;
                    break;
                }
                case PointerInput::Type::Up: settingsDirty |= settings.pointerUp(); break;
                case PointerInput::Type::Leave: settingsDirty |= settings.pointerLeave(); break;
            }
        }
        if (userQuit) break;
        if (slowCheck) {
            settingsDirty |= settings.tick(loopStart);  // 「もう一度押すと終了」の期限切れ
            settingsDirty |= autostart.poll();          // systemctl が終わっていれば回収して結果を反映
            // 設定パネルが実際に見えているか（ダッシュボード全体が開いているだけでは見えている扱いにしない）
            const SettingsVisibility visibility = vr.settingsVisibility();
            const bool nowVisible = visibility.visible() || options.forceSettingsVisible;
            if (nowVisible != settingsVisible) {
                std::fprintf(stderr, "[設定パネル] %s（ダッシュボード %s・このタブ %s・オーバーレイ %s%s）\n",
                             nowVisible ? "見えました。マウスを 100ms（操作中は 50ms）おきに確かめます" : "隠れました",
                             visibility.dashboardVisible ? "開" : "閉", visibility.activeTab ? "選択中" : "非選択",
                             visibility.overlayVisible ? "表示" : "非表示",
                             options.forceSettingsVisible ? "・--force-settings-visible" : "");
            }
            settingsVisible = nowVisible;
        }
        // 設定パネルは見えているときだけ、変化があったときだけ描く
        if (slowCheck && settingsVisible) {
            // 自動起動の状態（リンクの有無）は、見え始めたときと数秒おきに読み直す（外で変えられても追いつく）
            const double t = nowSeconds();
            if (!settingsWasVisible || t - lastAutostartRefresh >= kAutostartRefreshSec) {
                lastAutostartRefresh = t;
                if (autostart.refresh()) {
                    const AutostartStatus::State state = autostart.status().state;
                    std::fprintf(stderr, "[自動起動] 状態が変わりました: %s\n",
                                 state == AutostartStatus::State::Enabled
                                     ? "有効"
                                     : (state == AutostartStatus::State::Disabled ? "無効" : "未インストール"));
                    settingsDirty = true;
                }
            }
        }
        if (settingsVisible && (settingsDirty || !settingsWasVisible)) {
            settings.render(config, autostart.status(), updater.status());
            vr.submitSettings(settings.toRgba().data());
            settingsDirty = false;
        }
        settingsWasVisible = settingsVisible;

        // 性能パネル（更新間隔ごと）
        double now = nowSeconds();
        if (now >= nextPanel) {
            if (config.visible) {
                // 処理ごとの CPU 時間（--verbose で表示）
                const double c0 = cpuSeconds();
                const SensorSample sample = sensors.read();
                const double c1 = cpuSeconds();
                const FrameStats frame = vr.readFrameStats();
                // コントローラーの電池はゆっくりしか変わらないので 10 秒おき
                if (lastControllers < 0 || now - lastControllers >= kControllerIntervalSec) {
                    lastControllers = now;
                    state.controllers = vr.readControllers();
                }
                const double c2 = cpuSeconds();
                state.update(now, sample, frame, true, config.graphSeconds);
                renderer.render(state, config);
                const double c3 = cpuSeconds();
                const std::vector<uint8_t>& rgba = renderer.toRgba();
                const double c4 = cpuSeconds();
                const bool sent = vr.submitPanel(rgba.data());
                const double c5 = cpuSeconds();
                if (options.verbose && now - lastVerbose >= 2.0) {
                    lastVerbose = now;
                    std::fprintf(stderr,
                                 "[値] ループ %u 回 | fps %s frames=%u GPU %s/%sms CPU %s/%sms 目標 %sms(%sHz) 再投影 %s%% 落ち %u | "
                                 "GPU使用率 %s%% CPU %s℃ GPU %s℃ vph %sW 熱制限 %d/%d | 無線 %s ↓%sMbps %sdBm | "
                                 "コン L%s R%s | ダッシュボード %s 設定パネル %s | 送信 %s | CPU時間 ms: 読取 %.2f VR %.2f 描画 %.2f 変換 %.2f 送信 %.2f\n",
                                 loopCount, fmt(frame.appFps, 1).c_str(), frame.frames, fmt(frame.gpuMs, 2).c_str(),
                                 fmt(frame.gpuMaxMs, 2).c_str(),
                                 fmt(frame.cpuMs, 2).c_str(), fmt(frame.cpuMaxMs, 2).c_str(),
                                 fmt(frame.targetMs, 2).c_str(), fmt(frame.displayHz, 1).c_str(),
                                 fmt(frame.reprojectedPct, 1).c_str(), frame.dropped,
                                 fmt(sample.gpuBusyPct, 1).c_str(), fmt(sample.cpuTempC, 1).c_str(),
                                 fmt(sample.gpuTempC, 1).c_str(), fmt(sample.powerMainW, 2).c_str(),
                                 sample.throttleCpu ? 1 : 0, sample.throttleGpu ? 1 : 0,
                                 !sample.wifi.connected ? "未接続" : (sample.wifi.homeWifi ? "Wi-Fi" : "直通"),
                                 fmt(sample.wifi.rxMbps, 1).c_str(),
                                 fmt(sample.wifi.signalDbm, 0).c_str(),
                                 state.controllers.left.present ? fmt(state.controllers.left.pct, 0).c_str() : "-",
                                 state.controllers.right.present ? fmt(state.controllers.right.pct, 0).c_str() : "-",
                                 vr.dashboardOpen() ? "開" : "閉", settingsVisible ? "見えている" : "見えていない",
                                 sent ? "OK" : "失敗", (c1 - c0) * 1e3, (c2 - c1) * 1e3, (c3 - c2) * 1e3,
                                 (c4 - c3) * 1e3, (c5 - c4) * 1e3);
                    loopCount = 0;
                }
            }
            // 次の更新（遅れたときは詰めて追いかけない）
            nextPanel += config.updateIntervalMs / 1000.0;
            if (nextPanel < now) nextPanel = now;
        }

        // 待つ: 設定パネルが見えている間はマウスに素早く応えるため 50ms、それ以外は次の性能パネルの更新まで
        now = nowSeconds();
        const double wait = nextPanel - now;
        // 見えているだけのときは 100ms、ポインターが動いている間（最後のイベントから 3 秒）は 50ms
        const double pollSec = now - lastPointerEvent < kSettingsActiveHoldSec ? kSettingsPollSec : kSettingsIdlePollSec;
        sleepInterruptible(settingsVisible ? std::fmin(wait, pollSec) : wait);
    }

    // SIGTERM / SIGINT・SteamVR の終了・vrserver の消滅・「アプリを終了」のどれでも同じ終了処理を通す
    vr.shutdown();
    std::fprintf(stderr, "[VR] 終了しました\n");
    if (lockFd >= 0) ::close(lockFd);
    // ユーザーが終了したときは、systemd（Restart=always）に起動し直させないよう決まった終了コードにする
    return userQuit ? kExitCodeUserQuit : 0;
}

}  // namespace

/**
 * エントリーポイント。
 * @param argc 引数の数
 * @param argv 引数
 * @return 終了コード
 */
int main(int argc, char** argv) {
    // journald でも行ごとにすぐ出るようにする
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    installSignalHandlers();

    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return 2;
    }
    switch (options.mode) {
        case Options::Mode::Help: printUsage(); return 0;
        case Options::Mode::Version: std::printf("frame-perf-overlay %s\n", FRAME_PERF_OVERLAY_VERSION); return 0;
        case Options::Mode::ContrastReport: return printContrastReport();
        case Options::Mode::Print: return runPrint(options);
        case Options::Mode::DumpPng: return runDumpPng(options);
        case Options::Mode::DumpSettingsPng: return runDumpSettingsPng(options);
        case Options::Mode::DumpFrameTimings: return runDumpFrameTimings(options);
        case Options::Mode::Overlay: break;
    }
    return runOverlay(options);
}

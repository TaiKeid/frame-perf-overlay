// 設定（~/.config/frame-perf-overlay/config.json）の読み込みと変更の監視。
#pragma once

#include "i18n.h"

#include <ctime>
#include <string>
#include <vector>

/**
 * 色を変えるしきい値。warn で黄色、crit で赤。
 */
struct Thresholds {
    double fpsWarnRatio = 0.95;    ///< アプリの fps がリフレッシュレートのこの割合を下回ったら黄
    double fpsCritRatio = 0.75;    ///< 同じく赤
    double frameWarnRatio = 0.9;   ///< GPU フレーム時間が目標時間のこの倍率を超えたら黄
    double frameCritRatio = 1.0;   ///< 同じく赤
    double reprojWarnPct = 5.0;    ///< 再投影の割合（%）
    double reprojCritPct = 20.0;
    double tempWarnC = 70.0;       ///< CPU・GPU 温度（℃）
    double tempCritC = 80.0;
    double batteryWarnPct = 30.0;  ///< バッテリー残量（%）。これ以下で色が変わる
    double batteryCritPct = 15.0;
    double powerWarnW = 13.0;      ///< 本体の消費電力（vph、W）
    double powerCritW = 16.0;
    double cpuWarnPct = 85.0;      ///< CPU 使用率（最も忙しいコア、%）
    double cpuCritPct = 97.0;
    double gpuWarnPct = 85.0;      ///< GPU 使用率の目安（%）
    double gpuCritPct = 95.0;
    double wifiWarnDbm = -70.0;    ///< 直通回線の電波（dBm）。これ以下で色が変わる
    double wifiCritDbm = -78.0;
    double controllerWarnPct = 20.0;  ///< コントローラーの電池（%）。これ以下で色が変わる
    double controllerCritPct = 10.0;
};

/**
 * アプリの設定一式。ファイルが無いときはこの既定値で動く。
 */
enum class Attachment { Head, LeftWrist, RightWrist };

struct WristPose {
    double x = 0.0, y = 0.05, z = 0.08;  // Controller-local meters.
    double pitch = -90.0, yaw = 0.0, roll = 0.0;  // Degrees; Rz * Ry * Rx.
    bool operator!=(const WristPose& p) const {
        return x != p.x || y != p.y || z != p.z || pitch != p.pitch || yaw != p.yaw || roll != p.roll;
    }
};

struct Config {
    Attachment attachment = Attachment::Head;
    WristPose leftWrist, rightWrist;
    bool wristFade = true;
    double wristFadeEndDeg = 75.0;  // Smooth fade across the preceding 30 degrees.
    int clockFormat = 24;  // 0 = hidden, 12 or 24; headset local time.

    bool visible = true;            ///< false でパネルを隠す（読み取りと描画も止める）
    bool updateCheck = true;        ///< false で新しい版の自動確認を止める（手動の［確認］は常に使える）
    Language language = systemLanguage();  ///< 画面の文言の言語（"ja" / "en"。既定は Frame のシステム言語）
    double posX = -0.15;            ///< HMD 基準の位置（m）。右が +x
    double posY = -0.12;            ///< 上が +y
    double posZ = -0.50;            ///< 前が -z
    double yawDeg = 0.0;            ///< 左右の向き（度）。正でパネルの面が右（+x）を向く（-180〜180）
    double pitchDeg = 0.0;          ///< 上下の傾き（度）。正でパネルの面が上（+y）を向く（-90〜90）
    double rollDeg = 0.0;           ///< 画面内の回転（度）。正で面の側から見て反時計回り（-180〜180）
    double widthM = 0.20;           ///< パネルの幅（m）
    double alpha = 0.9;             ///< パネル全体の不透明度（0〜1）
    int updateIntervalMs = 500;     ///< 更新間隔（ms）
    int graphSeconds = 30;          ///< グラフに出す秒数
    std::string fontPath = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
    std::string boldFontPath = "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc";
    Thresholds thresholds;
};

/**
 * 既定の設定ファイルの場所を返す（$XDG_CONFIG_HOME か ~/.config の下）。
 * @return 設定ファイルのパス
 */
std::string defaultConfigPath();
const char* attachmentName(Attachment attachment);
WristPose& selectedWrist(Config& config);
const WristPose& selectedWrist(const Config& config);

/**
 * 設定ファイルを読む。ファイルが無いときは既定値を返して成功扱いにする。
 * 値の範囲外は丸め、知らないキーは warnings に入れる。
 * @param path 設定ファイルのパス
 * @param out 読んだ設定の書き込み先（失敗時は変更しない）
 * @param warnings 気づいたこと（知らないキー、範囲外の値など）の追加先
 * @param error 読めなかったときの理由
 * @return 成功（またはファイルが無い）なら true
 */
bool loadConfig(const std::string& path, Config& out, std::vector<std::string>& warnings, std::string& error);

/**
 * 設定をファイルに保存する（同じフォルダに一時ファイルを書いてから置き換える）。
 * フォルダが無ければ作る。書くのはこの設定ファイルだけ。
 * @param path 設定ファイルのパス
 * @param config 保存する設定
 * @param error 失敗したときの理由
 * @return 保存できたら true
 */
bool saveConfig(const std::string& path, const Config& config, std::string& error);

/**
 * 表示まわり（表示の有無・位置・向き・幅・透明度）だけ既定値に戻す。言語・しきい値・フォントはそのまま。
 * @param config 書き換える設定
 */
void resetDisplaySettings(Config& config);

/**
 * パネルの回転行列（HMD 基準）を作る。性能パネルの変換は「位置 × この回転」。
 * パネル自身の軸で見て yaw → pitch → roll の順に回す（内因的な Y-X-Z の順）。
 * 行列では R = Ry(yaw) · Rx(−pitch) · Rz(roll)（右手系、列ベクトル）。pitch だけ符号を反転しているのは、
 * 設定の pitch を「正で面が上を向く」にそろえるため（X 軸まわりの右手の回転は正で面が下を向く）。
 * 回転なしのとき、パネルの表（+z 側）は頭の方（HMD の +z = 後ろ）を向く。
 * @param config 設定（yawDeg / pitchDeg / rollDeg を使う）
 * @param r 書き込み先（r[行][列]）
 */
void panelRotation(const Config& config, double r[3][3]);

/**
 * 今の位置のまま、パネルの表が頭（HMD 基準の原点）を向くように yaw と pitch を決める。roll は今の値のまま残す
 * （ユーザーが合わせた傾きを消さない。Rz(roll) は (0,0,1) を動かさないので、roll があっても表の向きは同じ）。
 * 表の向き（法線）は R·(0,0,1) = (sin yaw·cos pitch, sin pitch, cos yaw·cos pitch) なので、
 * これが「パネルから原点への向き」−pos / |pos| と一致するように
 * yaw = atan2(−x, −z)、pitch = atan2(−y, √(x² + z²)) とする（0.1° 単位に丸める）。
 * @param config 書き換える設定
 */
void faceHead(Config& config);

/**
 * 今の向きが faceHead() で決まる向きとほぼ同じか（設定パネルの ✓ 用）。
 * @param config 設定
 * @return yaw・pitch の差が 0.25° 未満なら true（roll は問わない）
 */
bool isFacingHead(const Config& config);

/**
 * 設定ファイルの更新時刻を見て、変わっていたら読み直すための小さな監視役。
 */
class ConfigWatcher {
public:
    /**
     * @param path 監視する設定ファイルのパス
     */
    explicit ConfigWatcher(std::string path);

    /**
     * 最初の読み込み。ファイルが無い・壊れているときは既定値を使う。
     * @return 読み込んだ設定
     */
    Config loadInitial();

    /**
     * 更新時刻が変わっていたら読み直す。stat を 1 回呼ぶだけなので毎回呼んでよい。
     * @param config 現在の設定。変わったときだけ書き換える
     * @return 設定を読み直して書き換えたら true
     */
    bool reloadIfChanged(Config& config);

    /**
     * 自分で設定ファイルを書いた直後に呼ぶ。今のファイルの状態を覚えて、次の reloadIfChanged で読み直さないようにする。
     */
    void noteSaved();

    /** @return 監視しているパス */
    const std::string& path() const { return path_; }

private:
    std::string path_;
    bool existed_ = false;
    struct timespec mtime_ {};
    off_t size_ = 0;

    /**
     * 今のファイルの状態（有無・更新時刻・サイズ）を取る。
     * @param exists ファイルがあるか
     * @param mtime 更新時刻
     * @param size サイズ
     */
    void statFile(bool& exists, struct timespec& mtime, off_t& size) const;

    /**
     * 読み込んでログを出す共通処理。
     * @param config 書き込み先（失敗時は変えない）
     * @return 読めたら true
     */
    bool loadAndLog(Config& config) const;
};

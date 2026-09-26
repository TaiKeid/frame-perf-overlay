// OpenVR との接続、HMD 基準の性能パネル、ダッシュボードの設定パネル、フレーム時間の取得。
#pragma once

#include "config.h"
#include "history.h"
#include "vk_texture.h"

#include <cstdint>
#include <string>
#include <vector>

/** ダッシュボードの設定パネルへのポインター操作（座標は画像の左上が原点の px）。 */
struct PointerInput {
    enum class Type { Move, Down, Up, Leave };
    Type type;
    double x = 0.0;
    double y = 0.0;
};

/**
 * 設定パネル（ダッシュボードのこのアプリのタブ）が見えているかの材料。
 */
struct SettingsVisibility {
    bool dashboardVisible = false;  ///< IVROverlay::IsDashboardVisible()（ダッシュボード全体が開いている）
    bool activeTab = false;         ///< IVROverlay::IsActiveDashboardOverlay(設定パネル)（このアプリのタブが選ばれている）
    bool overlayVisible = false;    ///< IVROverlay::IsOverlayVisible(設定パネル)

    /**
     * 実際に見えているか。ダッシュボード全体が開いているだけでは見えている扱いにしない。
     * @return 3 つとも true なら true
     */
    bool visible() const { return dashboardVisible && activeTab && overlayVisible; }
};

/** pollEvents() の結果。 */
struct VrEvents {
    bool quit = false;                  ///< SteamVR から終了を求められた（VREvent_Quit。SteamVR 自体が終わる）
    bool closeRequested = false;        ///< ダッシュボードのアイコンの「閉じる」が押された（VREvent_OverlayClosed）
    std::vector<PointerInput> pointer;  ///< 設定パネルへの操作（届いた順）
};

/**
 * OpenVR のオーバーレイアプリとしての接続をまとめたクラス。
 * SteamVR の設定は変えない（オーバーレイを作って出すだけ）。
 * 画像は SetOverlayRaw ではなく Vulkan の画像を SetOverlayTexture で渡す。
 */
class VrOverlay {
public:
    /** connect() の結果。 */
    enum class ConnectResult {
        Ok,          ///< つながった
        NotRunning,  ///< SteamVR が起動していない（待って再試行する）
        Error,       ///< それ以外の失敗
    };

    VrOverlay();
    ~VrOverlay();
    VrOverlay(const VrOverlay&) = delete;
    VrOverlay& operator=(const VrOverlay&) = delete;

    /**
     * SteamVR が起動していればオーバーレイアプリとしてつなぎ、性能パネル・Vulkan・設定パネルを用意する。
     * 起動していないときに SteamVR を立ち上げてしまわないよう、先に Background 型で有無を確かめる。
     * @param panelWidth 性能パネルの画像の幅（px）
     * @param panelHeight 性能パネルの画像の高さ（px）
     * @param settingsWidth 設定パネルの画像の幅（px）
     * @param settingsHeight 設定パネルの画像の高さ（px）
     * @param message 失敗したときの理由
     * @return 接続結果
     */
    ConnectResult connect(int panelWidth, int panelHeight, int settingsWidth, int settingsHeight,
                          std::string& message);

    /**
     * 診断用: Background 型でだけつなぐ（オーバーレイも Vulkan も作らないので、常駐しているインスタンスと
     * ぶつからない。SteamVR が起動していなければ起動させずに失敗する）。
     * @param message 失敗したときの理由
     * @return つながったら true
     */
    bool connectReadOnly(std::string& message);

    /**
     * 安全な順番で片付ける: 隠す → テクスチャを外す → オーバーレイを消す → コンポジタの数フレーム分待つ
     * → VR_Shutdown → Vulkan の画像とデバイスを壊す。各 API の戻り値はログに出す。
     */
    void shutdown();

    /**
     * 位置・幅・透明度・表示の有無を性能パネルに反映する。
     * @param config 反映する設定
     */
    void applyConfig(const Config& config);

    /**
     * たまったイベントを処理する。SteamVR の終了（VREvent_Quit）には AcknowledgeQuit_Exiting で応える。
     * @param includeSystem false なら設定パネルのイベント（マウス・閉じる）だけ見る（設定パネルが見えている間の
     *                      細かい周期用）。true なら SteamVR 全体と性能パネルのイベントも見る
     * @return 終了要求と設定パネルへの操作
     */
    VrEvents pollEvents(bool includeSystem = true);

    /**
     * 接続時に見つけた vrserver のプロセスがまだ生きているか。
     * @return 生きている（または確かめられない）なら true
     */
    bool steamVrAlive() const;

    /**
     * 設定パネル（ダッシュボードのこのアプリのタブ）が今見えているかの材料を読む（IPC 3 回）。
     * @return 材料。visible() で判定する
     */
    SettingsVisibility settingsVisibility() const;

    /**
     * SteamVR のダッシュボードが開いているか（--verbose の診断用）。
     * @return 開いていれば true
     */
    bool dashboardOpen() const;

    /** @return 設定パネル（ダッシュボード）を作れていれば true */
    bool hasSettings() const { return dashboardHandle_ != 0; }

    /**
     * ダッシュボードのサムネイル画像を送る（接続直後に 1 回）。
     * @param rgba 非乗算済み RGBA
     * @param size 一辺の px
     * @return 送れたら true
     */
    bool submitThumbnail(const uint8_t* rgba, int size);

    /**
     * 前回の呼び出しからのフレーム時間を集計する。
     * @return 集計結果（取れなければ valid = false）
     */
    FrameStats readFrameStats();

    /**
     * 左右のコントローラーの電池を読む（呼ぶのは 10 秒おき程度で十分）。
     * @return 左右の電池。つながっていない側は present = false
     */
    ControllerStatus readControllers();

    /**
     * 診断用: 新しく届いたフレームの生のタイミング（フレーム番号・表示回数・再投影の印など）を
     * 0.5 秒おきに標準出力へ書き出す。
     * @param seconds 書き出す秒数
     * @param stop 途中でやめる印（SIGINT などで 1 になる）
     */
    void dumpFrameTimings(double seconds, const volatile int& stop);

    /**
     * 性能パネルの画像を送る。
     * @param rgba 非乗算済み RGBA 8bit（connect で指定した大きさ）
     * @return 送れたら true
     */
    bool submitPanel(const uint8_t* rgba);

    /**
     * 設定パネルの画像を送る。
     * @param rgba 非乗算済み RGBA 8bit（connect で指定した大きさ）
     * @return 送れたら true
     */
    bool submitSettings(const uint8_t* rgba);

private:
    bool connected_ = false;
    uint64_t panelHandle_ = 0;      ///< vr::VROverlayHandle_t（性能パネル）
    uint64_t dashboardHandle_ = 0;  ///< ダッシュボードの設定パネル
    uint64_t thumbnailHandle_ = 0;  ///< ダッシュボードのサムネイル
    int settingsHeight_ = 0;
    int vrserverPid_ = -1;
    uint32_t lastFrameIndex_ = 0;
    bool haveFrameIndex_ = false;   ///< 前回の readFrameStats でフレームを受け取った
    double displayHz_ = 0.0;
    double hzCheckedAt_ = -1.0;
    bool readOnly_ = false;         ///< connectReadOnly でつないだ（オーバーレイも Vulkan もない）
    std::vector<uint8_t> timingBuffer_;  ///< Compositor_FrameTiming の配列用
    std::string lastPanelError_;
    std::string lastSettingsError_;

    VulkanContext vulkan_;
    OverlayTexture panelTexture_;
    OverlayTexture settingsTexture_;
    OverlayTexture thumbnailTexture_;

    /**
     * ダッシュボードの設定パネルとサムネイルを作る。失敗しても性能パネルは動かす。
     * @param width 設定パネルの幅（px）
     * @param height 設定パネルの高さ（px）
     */
    void createDashboard(int width, int height);

    /**
     * /proc を一度だけ走査して vrserver の PID を探す。
     * @return 見つかった PID。無ければ -1
     */
    static int findVrserverPid();

    /**
     * HMD のリフレッシュレートを取る（5 秒に 1 回だけ問い合わせる）。
     * @param now 現在時刻（秒）
     * @return Hz。取れなければ 0
     */
    double displayHz(double now);
};

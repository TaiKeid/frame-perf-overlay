// SteamVR ダッシュボードに出す設定パネル（描画・ボタンの当たり判定・操作の反映）。
#pragma once

#include "autostart.h"
#include "config.h"
#include "update_check.h"  // vendor/frame-updater/cpp（CMake の include dir で見つかる）

#include <cstdint>
#include <string>
#include <vector>

class FontSet;
struct Pen;
struct UiText;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/** 設定パネルのボタンが表す操作。 */
enum class SettingsAction {
    None,
    ShowOn,
    ShowOff,
    LanguageJa,
    LanguageEn,
    PresetLeftBottom,
    PresetCenterBottom,
    PresetRightBottom,
    PresetLeftTop,
    PresetRightTop,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    MoveNear,
    MoveFar,
    YawLeft,      ///< 面を左へ向ける（yaw を減らす）
    YawRight,     ///< 面を右へ向ける（yaw を増やす）
    PitchUp,      ///< 面を上へ向ける（pitch を増やす）
    PitchDown,    ///< 面を下へ向ける（pitch を減らす）
    RollLeft,     ///< 画面内で左に回す（面を見て反時計回り。roll を増やす）
    RollRight,    ///< 画面内で右に回す（面を見て時計回り。roll を減らす）
    FaceMe,       ///< 今の位置のまま、面を頭に向ける（roll はそのまま）
    FaceForward,  ///< 回転なし（0, 0, 0）に戻す
    AngleStep1,   ///< 向きのボタンの刻みを 1° に（設定パネルの中だけで扱う）
    AngleStep5,   ///< 向きのボタンの刻みを 5° に（設定パネルの中だけで扱う）
    SizeDown,
    SizeUp,
    AlphaDown,
    AlphaUp,
    Reset,
    Quit,  ///< アプリを終了（2 回目の押下で確定したときだけ返る）
    AutostartOn,   ///< 自動起動を有効に（systemctl --user enable）
    AutostartOff,  ///< 自動起動を無効に（systemctl --user disable）
    UpdateCheckNow,     ///< ［確認］: 24 時間のキャッシュを無視してその場で確かめる（呼び出し側で扱う）
    UpdateInstall,      ///< ［更新］の 1 回目: 確認の表示に切り替えるだけ
    UpdateConfirmYes,   ///< 確認の「更新する」: 実際にインストールを始める（呼び出し側で扱う）
    UpdateConfirmNo,    ///< 確認の「やめる」: 確認を取り消す
    UpdateRetry,        ///< 失敗のあとの「もう一度」（呼び出し側で扱う）
    UpdateDismiss,      ///< 「入れました」/「失敗しました」の表示を閉じる（呼び出し側で扱う）
};

/**
 * 操作を設定に反映する（Quit と None は何もしない）。
 * @param action 操作
 * @param config 書き換える設定
 * @param angleStepDeg 向きのボタン（左右・上下）の刻み（度。1 か 5）。その刻みの目盛りに寄せて動かす
 * @return 設定が変わったら true
 */
bool applySettingsAction(SettingsAction action, Config& config, double angleStepDeg = 1.0);

/**
 * 設定パネルの画像を描き、レーザーポインターの位置からボタンを判定する係。
 */
class SettingsPanel {
public:
    /** 「アプリを終了」を 1 回押してから、確定の 2 回目を待つ秒数。 */
    static constexpr double kQuitConfirmSec = 3.0;

    /**
     * @param fonts 使うフォント（このオブジェクトより長く生きていること）
     */
    explicit SettingsPanel(const FontSet& fonts);
    ~SettingsPanel();
    SettingsPanel(const SettingsPanel&) = delete;
    SettingsPanel& operator=(const SettingsPanel&) = delete;

    /**
     * 今の設定と、ポインターが乗っている・押しているボタンの状態でパネルを描く。
     * @param config 今の設定（言語もここから）
     * @param autostart 自動起動の今の状態
     * @param update 新しい版の確認・インストールの今の状態
     */
    void render(const Config& config, const AutostartStatus& autostart, const frame_updater::UpdateStatus& update);

    /**
     * ポインターが動いた。
     * @param x 左端からの px
     * @param y 上端からの px
     * @return 乗っているボタンが変わった（描き直しが要る）なら true
     */
    bool pointerMove(double x, double y);

    /**
     * ボタンが押された。「アプリを終了」は 1 回目で確認の表示に変わり、kQuitConfirmSec 秒以内の 2 回目で Quit を返す。
     * @param x 左端からの px
     * @param y 上端からの px
     * @param now 現在時刻（秒、単調増加）
     * @return 押されたボタンの操作（ボタンの外・終了の 1 回目なら None）
     */
    SettingsAction pointerDown(double x, double y, double now);

    /**
     * ボタンが離された。
     * @return 押している表示を消した（描き直しが要る）なら true
     */
    bool pointerUp();

    /**
     * ポインターがパネルから外れた。
     * @return 描き直しが要るなら true
     */
    bool pointerLeave();

    /**
     * 時間で変わる表示（終了の確認の期限切れ）を進める。
     * @param now 現在時刻（秒、単調増加）
     * @return 描き直しが要るなら true
     */
    bool tick(double now);

    /**
     * 描いた画像を OpenVR 用の非乗算済み RGBA にして返す。
     * @return width() * height() * 4 バイト
     */
    const std::vector<uint8_t>& toRgba();

    /**
     * 描いた画像を PNG で保存する。
     * @param path 保存先
     * @return 保存できたら true
     */
    bool writePng(const std::string& path) const;

    /**
     * 見た目の確認用に「もう一度押すと終了」の状態にする（--dump-settings-png 用）。
     */
    void armQuitForPreview();

    /**
     * 見た目の確認用に、更新の確認（「%s に更新しますか？」）の状態にする（--dump-settings-png 用）。
     */
    void armUpdateConfirmForPreview();

    /** @return 画像の幅（px） */
    int width() const;
    /** @return 画像の高さ（px） */
    int height() const;

    /**
     * 向きのボタン（左右・上下）の今の刻み。設定ファイルには保存せず、起動するたびに 1° から始まる。
     * @return 刻み（度。1 か 5）
     */
    double angleStepDeg() const { return angleStepDeg_; }

private:
    /** ボタン 1 個。文言は描くときに言語の表から引く。 */
    struct Button {
        SettingsAction action;
        double x, y, w, h;
    };

    const FontSet& fonts_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    std::vector<uint8_t> rgba_;
    std::vector<Button> buttons_;
    SettingsAction hover_ = SettingsAction::None;
    bool autostartInstalled_ = true;  ///< 最後に描いたときユニットがあったか（無ければ自動起動のボタンは押せない）
    SettingsAction pressed_ = SettingsAction::None;
    bool quitArmed_ = false;
    double quitArmedUntil_ = 0.0;
    bool updateConfirmArmed_ = false;  ///< 「更新する」を 1 回押して、確認の表示を出している間
    double angleStepDeg_ = 1.0;        ///< 向きのボタンの刻み（度）。「1° ずつ / 5° ずつ」で切り替える（起動中だけ覚える）

    /** 左右と向きのカードのボタンの配置を作る（起動時に 1 回。下の段は render() のたびに置き直す）。 */
    void layoutButtons();

    /**
     * 座標にあるボタンを探す。
     * @param x 左端からの px
     * @param y 上端からの px
     * @return ボタンの操作。無ければ None
     */
    SettingsAction hitTest(double x, double y) const;

    /**
     * ボタンの文言を言語の表から引く。
     * @param action ボタンの操作
     * @param text 言語の表
     * @return 文言
     */
    std::string labelOf(SettingsAction action, const UiText& text) const;

    /**
     * 操作からボタンを探す。
     * @param action 操作
     * @return ボタン。無ければ nullptr
     */
    const Button* findButton(SettingsAction action) const;

    /**
     * 下の段・更新の帯のボタンを置く（文言の幅が言語や状態で変わるので、描くたびに位置を決め直す）。
     * 既にあるボタンは位置を置き直すだけ、無ければ足す。
     * @param action 操作
     * @param x 左
     * @param y 上
     * @param w 幅
     * @param h 高さ
     */
    void placeFooterButton(SettingsAction action, double x, double y, double w, double h);

    /**
     * 2 択のセグメント切り替え（丸い枠のピルの中で、選択中がアクセントの塗り ＋ ✓）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param left 左の操作
     * @param right 右の操作
     * @param selected 選択中（0 = 左、1 = 右、-1 = どちらでもない）
     * @param usable 押せるか（押せないときは枠なし・薄い文字）
     */
    void drawSegmented(const Pen& pen, const UiText& text, SettingsAction left, SettingsAction right, int selected,
                       bool usable) const;

    /**
     * 1 つのボタン（位置・微調整・− / ＋・既定に戻す）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param action 操作
     * @param selected 選ばれている（位置のボタン）ならアクセントの塗り ＋ ✓
     * @param size 文字の大きさ（入りきらなければ小さくする）
     * @param rotateIcon 回す向きの絵（-1 = 文言の左に反時計回り ⟲、+1 = 文言の右に時計回り ⟳、0 = なし）。
     *                   フォントに ⟲ ⟳ の字が無いので線で描く
     * @param check selected のとき ✓ を付けるか（false なら塗りだけの強調ボタン。更新の帯の［更新する］）
     */
    void drawButton(const Pen& pen, const UiText& text, SettingsAction action, bool selected, double size,
                    int rotateIcon = 0, bool check = true) const;

    /**
     * 見出しと、右上の状態のピル（● パネル表示中 / ○ パネル非表示）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     * @return 状態のピルの左端の x（新しい版の確認の帯はこの左に置く）
     */
    double drawHeader(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 左のカード「パネル」（表示・大きさ・透明度・既定に戻す）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     */
    void drawPanelCard(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 右のカード「位置」（位置の 3×2・微調整の十字・近く / 遠く・いまの位置）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     */
    void drawPositionCard(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 下の横長のカード「向き」（左右・上下の向きの十字と、その上の角の左に回す / 右に回す、刻みの 1° / 5°、
     * 自分に向ける、正面向き、いまの向き）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     */
    void drawFacingCard(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 下の段（言語・自動起動・終了）と、いちばん下の 1 行の説明を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     * @param autostart 自動起動の今の状態
     */
    void drawFooter(const Pen& pen, const UiText& text, const Config& config, const AutostartStatus& autostart);

    /**
     * 見出しの行の、見出しと状態のピルの間に出す、新しい版の確認・インストールの帯（いつも見えている 1 行 ＋ 右のボタン）。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     * @param update 新しい版の確認・インストールの今の状態
     * @param left 帯の左端の x
     * @param right 帯の右端の x
     */
    void drawUpdateBar(const Pen& pen, const UiText& text, const Config& config, const frame_updater::UpdateStatus& update,
                       double left, double right);
};

/**
 * ダッシュボードの下に並ぶサムネイル（アイコン）を描く。言語によらず同じ（「Perf」の文字とグラフ）。
 * @param fonts 使うフォント
 * @param size 一辺の px
 * @param rgba 非乗算済み RGBA の書き込み先
 * @param pngPath 空でなければ PNG にも保存する
 */
void renderThumbnail(const FontSet& fonts, int size, std::vector<uint8_t>& rgba, const std::string& pngPath = "");

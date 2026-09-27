// 設定パネルの実装。
#include "settings_panel.h"

#include "draw.h"
#include "i18n.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// 1200×826 を幅 2.8m で出す（1px あたりの大きさは前の 1024px / 2.4m とほぼ同じ）
constexpr int kWidth = 1200;
constexpr int kHeight = 826;
constexpr double kPad = 28;          // パネルの外側の余白
constexpr double kButtonH = 68;      // ボタンの高さ（レーザーで押しやすい大きさ。前と同じ）
constexpr double kCardY = 84;        // 左右のカードの上端
constexpr double kCardH = 424;
constexpr double kCardPad = 20;      // カードの中の余白
constexpr double kLeftCardX = kPad;  // 左のカード「パネル」
constexpr double kLeftCardW = 560;
constexpr double kRightCardX = 612;  // 右のカード「位置」
constexpr double kRightCardW = 560;
// 左のカードの行
constexpr double kRowControlX = kLeftCardX + 172;  // ボタンの左端（左は行の見出し）
constexpr double kRowY0 = kCardY + 64;             // 1 行目のボタンの上端
constexpr double kRowStep = 84;
constexpr double kSegmentW = 260;                  // 2 択のセグメント切り替えの幅
constexpr double kStepButtonW = 84;                // − / ＋
constexpr double kValueW = 120;                    // − と ＋ の間の値
// 右のカード
constexpr double kPresetX = kRightCardX + kCardPad;
constexpr double kPresetY = kCardY + 64;
constexpr double kPresetW = 166;
constexpr double kPresetGap = 11;
constexpr double kNudgeY = kPresetY + 2 * kButtonH + kPresetGap + 40;  // 微調整の 1 段目
constexpr double kArrowW = 106;
constexpr double kDepthX = 982;                    // 近く / 遠く
constexpr double kDepthW = 170;
// 下の横長のカード「向き」
constexpr double kCardGap = 24;                    // カードどうしの間（左右のカードの間と同じ）
constexpr double kFacingCardY = kCardY + kCardH + kCardGap;
constexpr double kFacingCardH = 152;
constexpr double kFacingCardX = kPad;
constexpr double kFacingCardW = kWidth - kPad * 2;
constexpr double kFacingRowY = kFacingCardY + 64;  // ボタンの上端（ほかのカードと同じ）
constexpr double kFacingArrowW = 150;              // ← 左向き / 右向き → / ↑ 上向き / ↓ 下向き
constexpr double kFacingGroupGap = 28;             // 左右・上下・自分に向けるの組の間
constexpr double kFaceMeW = 215;
// 下の段
constexpr double kFooterY = kFacingCardY + kFacingCardH + kCardGap;
constexpr double kFooterSegmentW = 240;
constexpr double kQuitW = 230;

constexpr double kNudgeM = 0.02;       // 上下左右の微調整（m）
constexpr double kDepthStepM = 0.05;   // 前後の微調整（m）
constexpr double kPresetDistance = 0.5;  // プリセットの座標はこの距離での値
constexpr double kAngleStepDeg = 1.0;    // 向きの 1 回の変化（度）

/** 位置のプリセット（距離 0.5m での x, y）。 */
struct Preset {
    SettingsAction action;
    double x, y;
};
constexpr Preset kPresets[] = {
    {SettingsAction::PresetLeftBottom, -0.15, -0.12},  {SettingsAction::PresetCenterBottom, 0.0, -0.16},
    {SettingsAction::PresetRightBottom, 0.15, -0.12},  {SettingsAction::PresetLeftTop, -0.15, 0.12},
    {SettingsAction::PresetRightTop, 0.15, 0.12},
};

/**
 * 1mm 単位に丸める（小数の誤差を設定ファイルに残さない）。
 * @param meters 値（m）
 * @return 丸めた値
 */
double roundMm(double meters) {
    return std::round(meters * 1000.0) / 1000.0;
}

/**
 * パネルまでの距離に合わせて、プリセットの座標を伸び縮みさせる倍率。
 * @param config 設定
 * @return 倍率（z = -0.5m で 1）
 */
double distanceScale(const Config& config) {
    return std::max(0.1, -config.posZ) / kPresetDistance;
}

/**
 * 今の位置がプリセットと同じか。
 * @param preset プリセット
 * @param config 設定
 * @return 同じなら true
 */
bool matchesPreset(const Preset& preset, const Config& config) {
    const double scale = distanceScale(config);
    return std::fabs(config.posX - preset.x * scale) < 0.004 && std::fabs(config.posY - preset.y * scale) < 0.004;
}

/**
 * 符号つきの cm 表記にする（例: −15）。
 * @param meters 値（m）
 * @return 文字列
 */
std::string signedCm(double meters) {
    char text[32];
    const long cm = std::lround(meters * 100.0);
    std::snprintf(text, sizeof(text), "%s%ld", cm < 0 ? "−" : "", std::labs(cm));
    return text;
}

/**
 * 角度を 1° 刻みの次の目盛りへ進める（例: 16.7° から増やすと 17°、減らすと 16°）。範囲の外には出さない。
 * @param degrees 今の角度（度）
 * @param direction +1 で増やす、-1 で減らす
 * @param limit 範囲（±limit）
 * @return 新しい角度（度）
 */
double stepAngle(double degrees, int direction, double limit) {
    const double steps = degrees / kAngleStepDeg;
    const double next = direction > 0 ? std::floor(steps + 1e-6) + 1 : std::ceil(steps - 1e-6) - 1;
    return std::clamp(next * kAngleStepDeg, -limit, limit);
}

/**
 * 符号つきの度の表記にする（例: −17°）。
 * @param degrees 値（度）
 * @return 文字列
 */
std::string signedDegrees(double degrees) {
    char text[32];
    const long whole = std::lround(degrees);
    std::snprintf(text, sizeof(text), "%s%ld°", whole < 0 ? "−" : "", std::labs(whole));
    return text;
}

/**
 * 回転なし（0, 0, 0）か。
 * @param config 設定
 * @return 3 つとも 0 なら true
 */
bool isFacingForward(const Config& config) {
    return config.yawDeg == 0.0 && config.pitchDeg == 0.0 && config.rollDeg == 0.0;
}

}  // namespace

bool applySettingsAction(SettingsAction action, Config& config) {
    const Config before = config;
    switch (action) {
        case SettingsAction::ShowOn: config.visible = true; break;
        case SettingsAction::ShowOff: config.visible = false; break;
        case SettingsAction::PresetLeftBottom:
        case SettingsAction::PresetCenterBottom:
        case SettingsAction::PresetRightBottom:
        case SettingsAction::PresetLeftTop:
        case SettingsAction::PresetRightTop:
            for (const auto& preset : kPresets) {
                if (preset.action != action) continue;
                // 見える方向を保つため、今の距離に合わせて伸び縮みさせる
                const double scale = distanceScale(config);
                config.posX = roundMm(preset.x * scale);
                config.posY = roundMm(preset.y * scale);
                // 隅に置くと斜めから見ることになるので、置いた位置で面を頭に向ける
                faceHead(config);
            }
            break;
        case SettingsAction::MoveLeft: config.posX = roundMm(config.posX - kNudgeM); break;
        case SettingsAction::MoveRight: config.posX = roundMm(config.posX + kNudgeM); break;
        case SettingsAction::MoveUp: config.posY = roundMm(config.posY + kNudgeM); break;
        case SettingsAction::MoveDown: config.posY = roundMm(config.posY - kNudgeM); break;
        case SettingsAction::MoveNear:
        case SettingsAction::MoveFar: {
            // 距離だけ変えて、見える方向（x/z と y/z の比）は保つ
            const double oldZ = config.posZ;
            const double step = action == SettingsAction::MoveNear ? kDepthStepM : -kDepthStepM;
            const double newZ = std::clamp(roundMm(oldZ + step), -3.0, -0.2);
            if (oldZ < 0) {
                config.posX = roundMm(config.posX * newZ / oldZ);
                config.posY = roundMm(config.posY * newZ / oldZ);
            }
            config.posZ = newZ;
            break;
        }
        // 向き（位置は変えない）
        case SettingsAction::YawLeft: config.yawDeg = stepAngle(config.yawDeg, -1, 180.0); break;
        case SettingsAction::YawRight: config.yawDeg = stepAngle(config.yawDeg, +1, 180.0); break;
        case SettingsAction::PitchUp: config.pitchDeg = stepAngle(config.pitchDeg, +1, 90.0); break;
        case SettingsAction::PitchDown: config.pitchDeg = stepAngle(config.pitchDeg, -1, 90.0); break;
        case SettingsAction::FaceMe: faceHead(config); break;
        case SettingsAction::FaceForward:
            config.yawDeg = 0.0;
            config.pitchDeg = 0.0;
            config.rollDeg = 0.0;
            break;
        case SettingsAction::SizeDown: config.widthM = std::clamp(roundMm(config.widthM - 0.02), 0.06, 1.0); break;
        case SettingsAction::SizeUp: config.widthM = std::clamp(roundMm(config.widthM + 0.02), 0.06, 1.0); break;
        case SettingsAction::AlphaDown:
            config.alpha = std::clamp(std::round((config.alpha - 0.1) * 20.0) / 20.0, 0.2, 1.0);
            break;
        case SettingsAction::AlphaUp:
            config.alpha = std::clamp(std::round((config.alpha + 0.1) * 20.0) / 20.0, 0.2, 1.0);
            break;
        case SettingsAction::Reset: resetDisplaySettings(config); break;
        case SettingsAction::LanguageJa: config.language = Language::Ja; break;
        case SettingsAction::LanguageEn: config.language = Language::En; break;
        case SettingsAction::AutostartOn:   // 自動起動は設定ファイルではなく systemd なので呼び出し側で扱う
        case SettingsAction::AutostartOff:
        case SettingsAction::Quit:  // 終了は呼び出し側で扱う
        case SettingsAction::None: break;
    }
    return config.visible != before.visible || config.posX != before.posX || config.posY != before.posY ||
           config.posZ != before.posZ || config.yawDeg != before.yawDeg || config.pitchDeg != before.pitchDeg ||
           config.rollDeg != before.rollDeg || config.widthM != before.widthM || config.alpha != before.alpha ||
           config.language != before.language;
}

SettingsPanel::SettingsPanel(const FontSet& fonts) : fonts_(fonts) {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    cr_ = cairo_create(surface_);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr_, options);
    cairo_font_options_destroy(options);
    layoutButtons();
}

SettingsPanel::~SettingsPanel() {
    cairo_destroy(cr_);
    cairo_surface_destroy(surface_);
}

void SettingsPanel::layoutButtons() {
    buttons_.clear();
    /**
     * ボタンを 1 つ足す。
     */
    const auto add = [this](SettingsAction action, double x, double y, double w, double h) {
        buttons_.push_back({action, x, y, w, h});
    };
    /**
     * 2 択のセグメント切り替えの左右半分を、それぞれボタンとして足す（押せる範囲はピル全体の高さ）。
     */
    const auto segmented = [&add](double x, double y, double w, SettingsAction left, SettingsAction right) {
        add(left, x, y, w / 2, kButtonH);
        add(right, x + w / 2, y, w / 2, kButtonH);
    };

    // 左のカード「パネル」: 表示・大きさ・透明度・既定に戻す
    segmented(kRowControlX, kRowY0, kSegmentW, SettingsAction::ShowOn, SettingsAction::ShowOff);
    const double sizeY = kRowY0 + kRowStep;
    add(SettingsAction::SizeDown, kRowControlX, sizeY, kStepButtonW, kButtonH);
    add(SettingsAction::SizeUp, kRowControlX + kStepButtonW + kValueW, sizeY, kStepButtonW, kButtonH);
    const double alphaY = kRowY0 + 2 * kRowStep;
    add(SettingsAction::AlphaDown, kRowControlX, alphaY, kStepButtonW, kButtonH);
    add(SettingsAction::AlphaUp, kRowControlX + kStepButtonW + kValueW, alphaY, kStepButtonW, kButtonH);
    add(SettingsAction::Reset, kRowControlX, kRowY0 + 3 * kRowStep, kStepButtonW * 2 + kValueW, kButtonH);

    // 右のカード「位置」: 位置は見える場所どおりに 3×2（上の段の真ん中は空き）
    const double col[3] = {kPresetX, kPresetX + kPresetW + kPresetGap, kPresetX + 2 * (kPresetW + kPresetGap)};
    const double presetRow2 = kPresetY + kButtonH + kPresetGap;
    add(SettingsAction::PresetLeftTop, col[0], kPresetY, kPresetW, kButtonH);
    add(SettingsAction::PresetRightTop, col[2], kPresetY, kPresetW, kButtonH);
    add(SettingsAction::PresetLeftBottom, col[0], presetRow2, kPresetW, kButtonH);
    add(SettingsAction::PresetCenterBottom, col[1], presetRow2, kPresetW, kButtonH);
    add(SettingsAction::PresetRightBottom, col[2], presetRow2, kPresetW, kButtonH);
    // 微調整: 上下左右は十字キーの形、右に近く / 遠く
    const double arrowRow2 = kNudgeY + kButtonH + kPresetGap;
    const double arrowCol[3] = {kPresetX, kPresetX + kArrowW + kPresetGap, kPresetX + 2 * (kArrowW + kPresetGap)};
    add(SettingsAction::MoveUp, arrowCol[1], kNudgeY, kArrowW, kButtonH);
    add(SettingsAction::MoveLeft, arrowCol[0], arrowRow2, kArrowW, kButtonH);
    add(SettingsAction::MoveDown, arrowCol[1], arrowRow2, kArrowW, kButtonH);
    add(SettingsAction::MoveRight, arrowCol[2], arrowRow2, kArrowW, kButtonH);
    add(SettingsAction::MoveNear, kDepthX, kNudgeY, kDepthW, kButtonH);
    add(SettingsAction::MoveFar, kDepthX, arrowRow2, kDepthW, kButtonH);

    // 下のカード「向き」: 左右の組・上下の組・自分に向ける / 正面向きを 1 列に
    double x = kFacingCardX + kCardPad;
    add(SettingsAction::YawLeft, x, kFacingRowY, kFacingArrowW, kButtonH);
    x += kFacingArrowW + kPresetGap;
    add(SettingsAction::YawRight, x, kFacingRowY, kFacingArrowW, kButtonH);
    x += kFacingArrowW + kFacingGroupGap;
    add(SettingsAction::PitchUp, x, kFacingRowY, kFacingArrowW, kButtonH);
    x += kFacingArrowW + kPresetGap;
    add(SettingsAction::PitchDown, x, kFacingRowY, kFacingArrowW, kButtonH);
    x += kFacingArrowW + kFacingGroupGap;
    add(SettingsAction::FaceMe, x, kFacingRowY, kFaceMeW, kButtonH);
    x += kFaceMeW + kPresetGap;
    add(SettingsAction::FaceForward, x, kFacingRowY, kFacingCardX + kFacingCardW - kCardPad - x, kButtonH);
    // 下の段の言語・自動起動・終了は、見出しの幅が言語で変わるので render() のたびに置き直す
}

std::string SettingsPanel::labelOf(SettingsAction action, const UiText& text) const {
    switch (action) {
        case SettingsAction::ShowOn: return text.on;
        case SettingsAction::ShowOff: return text.off;
        // 言語の名前は、どちらの言語で表示していてもその言語自身の書き方にする
        case SettingsAction::LanguageJa: return "日本語";
        case SettingsAction::LanguageEn: return "English";
        case SettingsAction::PresetLeftBottom: return text.bottomLeft;
        case SettingsAction::PresetCenterBottom: return text.bottomCenter;
        case SettingsAction::PresetRightBottom: return text.bottomRight;
        case SettingsAction::PresetLeftTop: return text.topLeft;
        case SettingsAction::PresetRightTop: return text.topRight;
        case SettingsAction::MoveLeft: return text.moveLeft;
        case SettingsAction::MoveRight: return text.moveRight;
        case SettingsAction::MoveUp: return text.moveUp;
        case SettingsAction::MoveDown: return text.moveDown;
        case SettingsAction::MoveNear: return text.nearer;
        case SettingsAction::MoveFar: return text.farther;
        case SettingsAction::YawLeft: return text.yawLeft;
        case SettingsAction::YawRight: return text.yawRight;
        case SettingsAction::PitchUp: return text.pitchUp;
        case SettingsAction::PitchDown: return text.pitchDown;
        case SettingsAction::FaceMe: return text.faceMe;
        case SettingsAction::FaceForward: return text.faceForward;
        case SettingsAction::SizeDown:
        case SettingsAction::AlphaDown: return "−";
        case SettingsAction::SizeUp:
        case SettingsAction::AlphaUp: return "＋";
        case SettingsAction::Reset: return text.reset;
        case SettingsAction::Quit: return quitArmed_ ? text.quitConfirm : text.quit;
        case SettingsAction::AutostartOn: return text.on;
        case SettingsAction::AutostartOff: return text.off;
        case SettingsAction::None: break;
    }
    return "";
}

SettingsAction SettingsPanel::hitTest(double x, double y) const {
    for (const auto& b : buttons_) {
        const bool isAutostart = b.action == SettingsAction::AutostartOn || b.action == SettingsAction::AutostartOff;
        if (isAutostart && !autostartInstalled_) continue;  // ユニットが無いときは押せない
        if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) return b.action;
    }
    return SettingsAction::None;
}

bool SettingsPanel::pointerMove(double x, double y) {
    const SettingsAction now = hitTest(x, y);
    if (now == hover_) return false;
    hover_ = now;
    return true;
}

SettingsAction SettingsPanel::pointerDown(double x, double y, double now) {
    hover_ = hitTest(x, y);
    pressed_ = hover_;
    if (pressed_ == SettingsAction::Quit) {
        // 誤って押しても終わらないよう、1 回目は確認の表示にするだけ
        if (quitArmed_ && now <= quitArmedUntil_) return SettingsAction::Quit;
        quitArmed_ = true;
        quitArmedUntil_ = now + kQuitConfirmSec;
        return SettingsAction::None;
    }
    quitArmed_ = false;  // 別のボタンを押したら確認は取り消す
    return pressed_;
}

bool SettingsPanel::pointerUp() {
    if (pressed_ == SettingsAction::None) return false;
    pressed_ = SettingsAction::None;
    return true;
}

bool SettingsPanel::pointerLeave() {
    const bool changed = hover_ != SettingsAction::None || pressed_ != SettingsAction::None;
    hover_ = SettingsAction::None;
    pressed_ = SettingsAction::None;
    return changed;
}

bool SettingsPanel::tick(double now) {
    if (!quitArmed_ || now <= quitArmedUntil_) return false;
    quitArmed_ = false;
    return true;
}

void SettingsPanel::armQuitForPreview() {
    quitArmed_ = true;
    quitArmedUntil_ = 1e300;
}

const SettingsPanel::Button* SettingsPanel::findButton(SettingsAction action) const {
    for (const auto& b : buttons_) {
        if (b.action == action) return &b;
    }
    return nullptr;
}

void SettingsPanel::placeFooterButton(SettingsAction action, double x, double y, double w, double h) {
    for (auto& b : buttons_) {
        if (b.action == action) {
            b = {action, x, y, w, h};
            return;
        }
    }
    buttons_.push_back({action, x, y, w, h});
}

void SettingsPanel::drawSegmented(const Pen& pen, const UiText& text, SettingsAction left, SettingsAction right,
                                  int selected, bool usable) const {
    const Button* a = findButton(left);
    const Button* b = findButton(right);
    if (a == nullptr || b == nullptr) return;
    cairo_t* cr = pen.cr;
    const double x = a->x;
    const double y = a->y;
    const double w = a->w + b->w;
    const double h = a->h;
    const double r = h / 2;
    // 地のピル。押せるときは枠（3:1 以上）で部品の形を見せる。押せないときは枠なし
    pen.color(kControl);
    pen.roundedRect(x, y, w, h, r);
    cairo_fill(cr);
    if (usable) strokeRounded(pen, x, y, w, h, r, kBorder, 2);

    const double inset = 5;
    const double half = (w - inset * 2) / 2;
    const SettingsAction actions[2] = {left, right};
    const double size = 24;
    for (int i = 0; i < 2; ++i) {
        const double sx = x + inset + half * i;
        const double sy = y + inset;
        const double sh = h - inset * 2;
        const bool pressed = usable && pressed_ == actions[i];
        const bool hovered = usable && hover_ == actions[i];
        const bool isSelected = selected == i;
        if (isSelected) {
            // 選択中の側: アクセントの塗り（押している間は少し濃く）＋ ✓ ＋ 太字。押せないときは薄く残す
            pen.color(pressed ? kAccentPressed : kAccent, usable ? 1.0 : 0.35);
            pen.roundedRect(sx, sy, half, sh, sh / 2);
            cairo_fill(cr);
        } else if (pressed || hovered) {
            pen.color(kControlHover);
            pen.roundedRect(sx, sy, half, sh, sh / 2);
            cairo_fill(cr);
        }
        const std::string label = labelOf(actions[i], text);
        const Color textColor = !usable ? kTextDisabled : (isSelected ? kOnAccent : kText);
        const double checkW = isSelected ? size * 0.9 : 0;
        const double labelSize = fitSize(pen, label, size, size * 0.7, half - 20 - checkW, isSelected);
        const double textW = pen.measure(label, labelSize, isSelected) + checkW;
        const double tx = sx + (half - textW) / 2;
        if (isSelected) drawCheck(cr, tx + checkW * 0.4, sy + sh / 2, size * 0.72, usable ? kOnAccent : kTextDisabled);
        pen.text(tx + checkW, centerBaseline(sy, sh, labelSize), label, labelSize, textColor, isSelected);
    }
}

void SettingsPanel::drawButton(const Pen& pen, const UiText& text, SettingsAction action, bool selected,
                               double size) const {
    const Button* b = findButton(action);
    if (b == nullptr) return;
    const bool pressed = pressed_ == action;
    const bool hovered = hover_ == action;
    const double r = 18;
    if (selected) {
        // 選ばれている位置: アクセントの塗り ＋ ✓ ＋ 太字
        pen.color(pressed ? kAccentPressed : kAccent);
        pen.roundedRect(b->x, b->y, b->w, b->h, r);
        cairo_fill(pen.cr);
    } else {
        pen.color(pressed || hovered ? kControlHover : kControl);
        pen.roundedRect(b->x, b->y, b->w, b->h, r);
        cairo_fill(pen.cr);
        // 押している間は枠をアクセントにして、押したことが分かるようにする
        strokeRounded(pen, b->x, b->y, b->w, b->h, r, pressed ? kAccent : kBorder, 2);
    }
    const std::string label = labelOf(action, text);
    const double checkW = selected ? size * 0.9 : 0;
    const double labelSize = fitSize(pen, label, size, size * 0.65, b->w - 20 - checkW, true);
    const double textW = pen.measure(label, labelSize, true) + checkW;
    const double tx = b->x + (b->w - textW) / 2;
    if (selected) drawCheck(pen.cr, tx + checkW * 0.4, b->y + b->h / 2, size * 0.72, kOnAccent);
    pen.text(tx + checkW, centerBaseline(b->y, b->h, labelSize), label, labelSize, selected ? kOnAccent : kText, true);
}

void SettingsPanel::drawHeader(const Pen& pen, const UiText& text, const Config& config) const {
    pen.text(kPad, 60, text.settingsTitle, 34, kText, true);
    // 右上の状態のピル: 表示中は緑の塗り＋●＋文字、非表示は灰色の塗り＋○＋文字（色だけで伝えない）
    const std::string label = config.visible ? text.panelShown : text.panelHidden;
    const double size = 20;
    const double h = 42;
    const double y = 26;
    const double w = pen.measure(label, size, true) + 58;
    const double x = kWidth - kPad - w;
    pen.color(config.visible ? kSuccessTint : kControl);
    pen.roundedRect(x, y, w, h, h / 2);
    cairo_fill(pen.cr);
    const Color c = config.visible ? kSuccess : kTextMuted;
    if (config.visible) {
        drawDisc(pen.cr, x + 24, y + h / 2, 6.5, c);
    } else {
        pen.color(c);
        cairo_set_line_width(pen.cr, 2.5);
        cairo_new_sub_path(pen.cr);
        cairo_arc(pen.cr, x + 24, y + h / 2, 5.5, 0, 2 * M_PI);
        cairo_stroke(pen.cr);
    }
    pen.text(x + 40, centerBaseline(y, h, size), label, size, c, true);
}

void SettingsPanel::drawPanelCard(const Pen& pen, const UiText& text, const Config& config) const {
    drawCard(pen, kLeftCardX, kCardY, kLeftCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kLeftCardX + kCardPad, kCardY + 42, text.cardPanel, 24, kText, true);
    // 行の見出し（左）とボタン（右）
    const char* labels[3] = {text.rowShow, text.rowSize, text.rowOpacity};
    for (int i = 0; i < 3; ++i) {
        const double y = kRowY0 + i * kRowStep;
        const double size = fitSize(pen, labels[i], 24, 16, kRowControlX - kLeftCardX - kCardPad - 12, false);
        pen.text(kLeftCardX + kCardPad, centerBaseline(y, kButtonH, size), labels[i], size, kTextMuted);
    }
    drawSegmented(pen, text, SettingsAction::ShowOn, SettingsAction::ShowOff, config.visible ? 0 : 1, true);

    // 大きさ・透明度: − 値 ＋
    char value[32];
    const double valueCx = kRowControlX + kStepButtonW + kValueW / 2;
    std::snprintf(value, sizeof(value), "%.0f cm", config.widthM * 100.0);
    textCentered(pen, valueCx, centerBaseline(kRowY0 + kRowStep, kButtonH, 28), value, 28, kText, true);
    std::snprintf(value, sizeof(value), "%.0f%%", config.alpha * 100.0);
    textCentered(pen, valueCx, centerBaseline(kRowY0 + 2 * kRowStep, kButtonH, 28), value, 28, kText, true);
    for (const SettingsAction action :
         {SettingsAction::SizeDown, SettingsAction::SizeUp, SettingsAction::AlphaDown, SettingsAction::AlphaUp}) {
        drawButton(pen, text, action, false, 30);
    }
    drawButton(pen, text, SettingsAction::Reset, false, 24);
}

void SettingsPanel::drawPositionCard(const Pen& pen, const UiText& text, const Config& config) const {
    drawCard(pen, kRightCardX, kCardY, kRightCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kRightCardX + kCardPad, kCardY + 42, text.rowPosition, 24, kText, true);
    // いまの位置（HMD から見て、cm）を見出しの右に
    const std::string where = std::string(text.positionNow) + "  " + text.posX + " " + signedCm(config.posX) + "  " +
                              text.posY + " " + signedCm(config.posY) + "  " + text.posZ + " " +
                              signedCm(-config.posZ) + " cm";
    const double whereSize = fitSize(pen, where, 18, 13, kRightCardW - kCardPad * 2 - 90, false);
    pen.text(kRightCardX + kRightCardW - kCardPad, kCardY + 40, where, whereSize, kTextMuted, false, true);

    for (const auto& preset : kPresets) drawButton(pen, text, preset.action, matchesPreset(preset, config), 24);
    pen.text(kRightCardX + kCardPad, kNudgeY - 12, text.rowNudge, 18, kTextMuted);
    for (const SettingsAction action : {SettingsAction::MoveUp, SettingsAction::MoveLeft, SettingsAction::MoveDown,
                                        SettingsAction::MoveRight, SettingsAction::MoveNear, SettingsAction::MoveFar}) {
        drawButton(pen, text, action, false, 24);
    }
}

void SettingsPanel::drawFacingCard(const Pen& pen, const UiText& text, const Config& config) const {
    drawCard(pen, kFacingCardX, kFacingCardY, kFacingCardW, kFacingCardH, 20, kCard, kCard, 0);
    pen.text(kFacingCardX + kCardPad, kFacingCardY + 42, text.cardFacing, 24, kText, true);
    // いまの向き（度）を見出しの右に。roll は設定ファイルでだけ変えるので、0 でないときだけ出す
    std::string now = std::string(text.facingNow) + "  " + text.yawName + " " + signedDegrees(config.yawDeg) + "  " +
                      text.pitchName + " " + signedDegrees(config.pitchDeg);
    if (std::lround(config.rollDeg) != 0) now += std::string("  ") + text.rollName + " " + signedDegrees(config.rollDeg);
    const double nowSize = fitSize(pen, now, 18, 13, kFacingCardW - kCardPad * 2 - 160, false);
    pen.text(kFacingCardX + kFacingCardW - kCardPad, kFacingCardY + 40, now, nowSize, kTextMuted, false, true);

    for (const SettingsAction action :
         {SettingsAction::YawLeft, SettingsAction::YawRight, SettingsAction::PitchUp, SettingsAction::PitchDown}) {
        drawButton(pen, text, action, false, 24);
    }
    // 今その向きになっていれば ✓（位置のプリセットと同じ見せ方）
    drawButton(pen, text, SettingsAction::FaceMe, isFacingHead(config), 24);
    drawButton(pen, text, SettingsAction::FaceForward, isFacingForward(config), 24);
}

void SettingsPanel::drawFooter(const Pen& pen, const UiText& text, const Config& config,
                               const AutostartStatus& autostart) {
    const double y = kFooterY;
    const double labelSize = 20;
    double x = kPad;
    // 言語
    x += pen.text(x, centerBaseline(y, kButtonH, labelSize), text.rowLanguage, labelSize, kTextMuted) + 14;
    placeFooterButton(SettingsAction::LanguageJa, x, y, kFooterSegmentW / 2, kButtonH);
    placeFooterButton(SettingsAction::LanguageEn, x + kFooterSegmentW / 2, y, kFooterSegmentW / 2, kButtonH);
    drawSegmented(pen, text, SettingsAction::LanguageJa, SettingsAction::LanguageEn,
                  config.language == Language::Ja ? 0 : 1, true);
    x += kFooterSegmentW + 40;
    // 自動起動（ユニットが無いときはグレーで押せない。切り替え中も薄く）
    const bool installed = autostart.state != AutostartStatus::State::NotInstalled;
    autostartInstalled_ = installed;
    x += pen.text(x, centerBaseline(y, kButtonH, labelSize), text.rowAutostart, labelSize, kTextMuted) + 14;
    placeFooterButton(SettingsAction::AutostartOn, x, y, kFooterSegmentW / 2, kButtonH);
    placeFooterButton(SettingsAction::AutostartOff, x + kFooterSegmentW / 2, y, kFooterSegmentW / 2, kButtonH);
    const int selected = autostart.state == AutostartStatus::State::Enabled
                             ? 0
                             : (autostart.state == AutostartStatus::State::Disabled ? 1 : -1);
    drawSegmented(pen, text, SettingsAction::AutostartOn, SettingsAction::AutostartOff, selected,
                  installed && !autostart.busy);

    // 終了（赤い枠の控えめなボタン。確認中は赤い塗り）
    const double quitX = kWidth - kPad - kQuitW;
    placeFooterButton(SettingsAction::Quit, quitX, y, kQuitW, kButtonH);
    const bool quitHover = hover_ == SettingsAction::Quit || pressed_ == SettingsAction::Quit;
    pen.color(quitArmed_ ? kDanger : (quitHover ? kControlHover : kQuitFill));
    pen.roundedRect(quitX, y, kQuitW, kButtonH, kButtonH / 2);
    cairo_fill(pen.cr);
    strokeRounded(pen, quitX, y, kQuitW, kButtonH, kButtonH / 2, kDanger, 2);
    const std::string quitLabel = labelOf(SettingsAction::Quit, text);
    const double quitSize = fitSize(pen, quitLabel, 24, 14, kQuitW - 28, true);
    textCentered(pen, quitX + kQuitW / 2, centerBaseline(y, kButtonH, quitSize), quitLabel, quitSize,
                 quitArmed_ ? kOnAccent : kText, true);

    // 最下行（1 行だけ）: 失敗（赤）> 切り替え中 > ユニットが無い > ふだんの説明
    const double lineY = kHeight - 24;
    const double maxW = kWidth - kPad * 2;
    std::string note;
    Color noteColor = kTextMuted;
    bool bold = false;
    if (autostart.failed) {
        note = text.autostartFailed;
        noteColor = kDanger;
        bold = true;
    } else if (autostart.busy) {
        note = text.autostartBusy;
        noteColor = kText;
    } else if (!installed) {
        note = text.autostartNotInstalled;
    } else {
        note = std::string(text.footer) + text.sentenceBreak + text.autostartNextLaunch;
    }
    pen.text(kPad, lineY, note, fitSize(pen, note, 17, 12, maxW, bold), noteColor, bold);
}

void SettingsPanel::render(const Config& config, const AutostartStatus& autostart) {
    const Pen pen {cr_, &fonts_};
    const UiText& t = uiText(config.language);

    // 地（不透明。コントラスト比は不透明な地で計算している）
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    pen.color(kBg);
    pen.roundedRect(0, 0, kWidth, kHeight, 24);
    cairo_fill(cr_);

    // 上 = 見出しと状態のピル、左のカード = パネル、右のカード = 位置、その下の横長のカード = 向き、
    // 下 = 言語・自動起動・終了と説明
    drawHeader(pen, t, config);
    drawPanelCard(pen, t, config);
    drawPositionCard(pen, t, config);
    drawFacingCard(pen, t, config);
    drawFooter(pen, t, config, autostart);

    cairo_surface_flush(surface_);
}

const std::vector<uint8_t>& SettingsPanel::toRgba() {
    surfaceToRgba(surface_, rgba_);
    return rgba_;
}

bool SettingsPanel::writePng(const std::string& path) const {
    return cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

int SettingsPanel::width() const {
    return kWidth;
}

int SettingsPanel::height() const {
    return kHeight;
}

void renderThumbnail(const FontSet& fonts, int size, std::vector<uint8_t>& rgba, const std::string& pngPath) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts};
    const double s = size / 256.0;

    // 地と内側の縁
    pen.color(kBg);
    pen.roundedRect(8 * s, 8 * s, 240 * s, 240 * s, 48 * s);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 2 * s);
    pen.roundedRect(9 * s, 9 * s, 238 * s, 238 * s, 47 * s);
    cairo_stroke(cr);

    // グラフの台（カードの色）と、目標の点線
    pen.color(kCard);
    pen.roundedRect(34 * s, 40 * s, 188 * s, 118 * s, 18 * s);
    cairo_fill(cr);
    pen.color(kTextMuted);
    cairo_set_line_width(cr, 3 * s);
    const double dashes[] = {8 * s, 7 * s};
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_move_to(cr, 48 * s, 70 * s);
    cairo_line_to(cr, 208 * s, 70 * s);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);

    // 折れ線（イメージカラー）と、その光彩
    const double ys[] = {128, 104, 116, 82, 94, 64, 76};
    /**
     * 折れ線の道筋を作る。
     */
    const auto path = [&]() {
        for (int i = 0; i < 7; ++i) {
            const double x = (48 + i * 26.7) * s;
            if (i == 0) cairo_move_to(cr, x, ys[i] * s); else cairo_line_to(cr, x, ys[i] * s);
        }
    };
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 3; i >= 1; --i) {
        pen.color(kAccent, 0.10);
        cairo_set_line_width(cr, (10 + i * 7) * s);
        path();
        cairo_stroke(cr);
    }
    pen.color(kAccent);
    cairo_set_line_width(cr, 10 * s);
    path();
    cairo_stroke(cr);
    drawDisc(cr, (48 + 6 * 26.7) * s, 76 * s, 11 * s, kAccent);

    const double w = pen.measure("Perf", 56 * s, true);
    pen.text((size - w) / 2, 222 * s, "Perf", 56 * s, kText, true);

    cairo_surface_flush(surface);
    surfaceToRgba(surface, rgba);
    if (!pngPath.empty()) cairo_surface_write_to_png(surface, pngPath.c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

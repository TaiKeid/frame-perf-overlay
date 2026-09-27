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

// 1600×690。1px あたりの大きさは前（1200px を 2.8m）と同じにする（幅は vr_overlay.cpp で px から決める）。
// 横に長くして縦を詰める: 見出しの行に新しい版の確認の帯を入れ、パネル・位置・向きのカードを横に 3 つ並べる
constexpr int kWidth = 1600;
constexpr int kHeight = 690;
constexpr double kPad = 28;          // パネルの外側の余白
constexpr double kButtonH = 68;      // ボタンの高さ（レーザーで押しやすい大きさ。前と同じ）
constexpr double kHeaderY = 20;      // 見出しの行（見出し・新しい版の確認の帯・状態のピル）の上端
constexpr double kHeaderH = kButtonH + 16;  // 帯のボタンも 68px にして、上下に 8px ずつ
constexpr double kCardGap = 24;      // カードどうしの間
constexpr double kCardY = kHeaderY + kHeaderH + 20;  // 3 つのカードの上端
constexpr double kCardH = 420;
constexpr double kCardPad = 20;      // カードの中の余白
constexpr double kLeftCardX = kPad;  // 左のカード「パネル」
constexpr double kLeftCardW = 448;
constexpr double kRightCardX = kLeftCardX + kLeftCardW + kCardGap;  // 真ん中のカード「位置」
constexpr double kRightCardW = 560;
// 左のカードの行
constexpr double kRowControlX = kLeftCardX + 140;  // ボタンの左端（左は行の見出し）
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
constexpr double kDepthW = 170;                    // 近く / 遠く
constexpr double kDepthX = kRightCardX + kRightCardW - kCardPad - kDepthW;
// 右のカード「向き」: 上に十字（回す・上下・左右）、その下に刻みの 1° / 5°、いちばん下に自分に向ける / 正面向き
constexpr double kFacingCardX = kRightCardX + kRightCardW + kCardGap;
constexpr double kFacingCardW = kWidth - kPad - kFacingCardX;
constexpr double kFacingInnerW = kFacingCardW - kCardPad * 2;
constexpr double kFacingRowY = kCardY + 64;  // 1 段目のボタンの上端（ほかのカードと同じ）
constexpr double kFacingRow2Y = kFacingRowY + kButtonH + kPresetGap;
constexpr double kFacingArrowW = (kFacingInnerW - 2 * kPresetGap) / 3;  // 十字の 1 つ
constexpr double kFacingStepY = kNudgeY;  // 刻み（位置のカードの微調整の段とそろえる）
constexpr double kFacingFaceY = kFacingStepY + kButtonH + kPresetGap;  // 自分に向ける / 正面向き
// 下の段
constexpr double kFooterY = kCardY + kCardH + kCardGap;
constexpr double kFooterSegmentW = 240;
constexpr double kQuitW = 230;

constexpr double kNudgeM = 0.02;       // 上下左右の微調整（m）
constexpr double kDepthStepM = 0.05;   // 前後の微調整（m）
constexpr double kPresetDistance = 0.5;  // プリセットの座標はこの距離での値

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
 * 角度を刻みの次の目盛りへ進める（例: 16.7° から、1° 刻みなら増やすと 17°・減らすと 16°、
 * 5° 刻みなら増やすと 20°・減らすと 15°）。範囲の外には出さない。
 * @param degrees 今の角度（度）
 * @param direction +1 で増やす、-1 で減らす
 * @param limit 範囲（±limit）
 * @param step 刻み（度）
 * @return 新しい角度（度）
 */
double stepAngle(double degrees, int direction, double limit, double step) {
    const double steps = degrees / step;
    const double next = direction > 0 ? std::floor(steps + 1e-6) + 1 : std::ceil(steps - 1e-6) - 1;
    return std::clamp(next * step, -limit, limit);
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
 * 回す向きの絵（すき間が上にある円弧 ＋ 矢じり）を線で描く。画面の上で反時計回りなら ⟲、時計回りなら ⟳。
 * @param cr cairo
 * @param cx 中心の x
 * @param cy 中心の y
 * @param r 半径
 * @param counterclockwise 反時計回り（⟲）なら true
 * @param c 色
 */
void drawRotateArrow(cairo_t* cr, double cx, double cy, double r, bool counterclockwise, Color c) {
    const double deg = M_PI / 180.0;
    // 画面の座標は y が下向きなので、角度が増える向き（cairo_arc）が時計回り。
    // ⟲: 左上（240°）から反時計回りに左・下・右を回って右上（300° = −60°）で終わる。⟳ はその左右反転
    const double start = counterclockwise ? 240 * deg : 300 * deg;
    const double end = counterclockwise ? -60 * deg : 600 * deg;
    cairo_save(cr);
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, r * 0.3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_path(cr);
    if (counterclockwise) {
        cairo_arc_negative(cr, cx, cy, r, start, end);
    } else {
        cairo_arc(cr, cx, cy, r, start, end);
    }
    cairo_stroke(cr);
    // 矢じり: 終わりの点で、進む向き（円の接線）に向ける
    const double px = cx + r * std::cos(end);
    const double py = cy + r * std::sin(end);
    const double dx = counterclockwise ? std::sin(end) : -std::sin(end);
    const double dy = counterclockwise ? -std::cos(end) : std::cos(end);
    const double head = r * 0.9;
    cairo_move_to(cr, px + dx * head, py + dy * head);
    cairo_line_to(cr, px - dx * head * 0.35 - dy * head * 0.75, py - dy * head * 0.35 + dx * head * 0.75);
    cairo_line_to(cr, px - dx * head * 0.35 + dy * head * 0.75, py - dy * head * 0.35 - dx * head * 0.75);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
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

bool applySettingsAction(SettingsAction action, Config& config, double angleStepDeg) {
    const Config before = config;
    WristPose& pose = selectedWrist(config);
    const auto offset = [](double value) { return std::clamp(roundMm(value), -0.5, 0.5); };
    const auto rotate = [](double value) { return std::remainder(value, 360.0); };
    switch (action) {
        case SettingsAction::ChangePage: break;
        case SettingsAction::AttachHead: config.attachment = Attachment::Head; break;
        case SettingsAction::AttachLeft: config.attachment = Attachment::LeftWrist; break;
        case SettingsAction::AttachRight: config.attachment = Attachment::RightWrist; break;
        case SettingsAction::ClockOff: config.clockFormat = 0; break;
        case SettingsAction::Clock12: config.clockFormat = 12; break;
        case SettingsAction::Clock24: config.clockFormat = 24; break;
        case SettingsAction::FadeOn: config.wristFade = true; break;
        case SettingsAction::FadeOff: config.wristFade = false; break;
        case SettingsAction::FadeAngleDown: config.wristFadeEndDeg = std::max(35.0, config.wristFadeEndDeg - 5); break;
        case SettingsAction::FadeAngleUp: config.wristFadeEndDeg = std::min(90.0, config.wristFadeEndDeg + 5); break;
        case SettingsAction::OffsetXDown: if (config.attachment != Attachment::Head) pose.x = offset(pose.x - 0.01); break;
        case SettingsAction::OffsetXUp: if (config.attachment != Attachment::Head) pose.x = offset(pose.x + 0.01); break;
        case SettingsAction::OffsetYDown: if (config.attachment != Attachment::Head) pose.y = offset(pose.y - 0.01); break;
        case SettingsAction::OffsetYUp: if (config.attachment != Attachment::Head) pose.y = offset(pose.y + 0.01); break;
        case SettingsAction::OffsetZDown: if (config.attachment != Attachment::Head) pose.z = offset(pose.z - 0.01); break;
        case SettingsAction::OffsetZUp: if (config.attachment != Attachment::Head) pose.z = offset(pose.z + 0.01); break;
        case SettingsAction::WristPitchDown: if (config.attachment != Attachment::Head) pose.pitch = rotate(pose.pitch - 5); break;
        case SettingsAction::WristPitchUp: if (config.attachment != Attachment::Head) pose.pitch = rotate(pose.pitch + 5); break;
        case SettingsAction::YawDown: if (config.attachment != Attachment::Head) pose.yaw = rotate(pose.yaw - 5); break;
        case SettingsAction::YawUp: if (config.attachment != Attachment::Head) pose.yaw = rotate(pose.yaw + 5); break;
        case SettingsAction::RollDown: if (config.attachment != Attachment::Head) pose.roll = rotate(pose.roll - 5); break;
        case SettingsAction::RollUp: if (config.attachment != Attachment::Head) pose.roll = rotate(pose.roll + 5); break;
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
        case SettingsAction::YawLeft: config.yawDeg = stepAngle(config.yawDeg, -1, 180.0, angleStepDeg); break;
        case SettingsAction::YawRight: config.yawDeg = stepAngle(config.yawDeg, +1, 180.0, angleStepDeg); break;
        case SettingsAction::PitchUp: config.pitchDeg = stepAngle(config.pitchDeg, +1, 90.0, angleStepDeg); break;
        case SettingsAction::PitchDown: config.pitchDeg = stepAngle(config.pitchDeg, -1, 90.0, angleStepDeg); break;
        // roll は正で、面を見て反時計回り（⟲）
        case SettingsAction::RollLeft: config.rollDeg = stepAngle(config.rollDeg, +1, 180.0, angleStepDeg); break;
        case SettingsAction::RollRight: config.rollDeg = stepAngle(config.rollDeg, -1, 180.0, angleStepDeg); break;
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
        case SettingsAction::UpdateCheckNow:    // 更新はどれも設定ファイルを変えない。呼び出し側で扱う
        case SettingsAction::UpdateInstall:
        case SettingsAction::UpdateConfirmYes:
        case SettingsAction::UpdateConfirmNo:
        case SettingsAction::UpdateRetry:
        case SettingsAction::UpdateDismiss:
        case SettingsAction::AngleStep1:  // 刻みは設定パネルの中だけで覚える（設定ファイルは変えない）
        case SettingsAction::AngleStep5:
        case SettingsAction::None: break;
    }
    return config.attachment != before.attachment || config.clockFormat != before.clockFormat ||
           config.wristFade != before.wristFade || config.wristFadeEndDeg != before.wristFadeEndDeg ||
           config.leftWrist != before.leftWrist || config.rightWrist != before.rightWrist ||
           config.visible != before.visible || config.posX != before.posX || config.posY != before.posY ||
           config.posZ != before.posZ || config.yawDeg != before.yawDeg ||
           config.pitchDeg != before.pitchDeg || config.rollDeg != before.rollDeg || config.widthM != before.widthM || config.alpha != before.alpha ||
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

    add(SettingsAction::ChangePage, 1065, kFooterY, 245, kButtonH);
    if (wristPage_) {
        const auto addWrist = [&add](SettingsAction action, double x, double y, double w, double h) {
            add(action, x, y + 40, w, h);
        };
        addWrist(SettingsAction::AttachHead, 48, 148, 166, 68);
        addWrist(SettingsAction::AttachLeft, 225, 148, 166, 68);
        addWrist(SettingsAction::AttachRight, 402, 148, 166, 68);
        const SettingsAction down[] = {SettingsAction::OffsetXDown, SettingsAction::OffsetYDown, SettingsAction::OffsetZDown};
        const SettingsAction up[] = {SettingsAction::OffsetXUp, SettingsAction::OffsetYUp, SettingsAction::OffsetZUp};
        for (int i = 0; i < 3; ++i) {
            addWrist(down[i], 200, 252 + i * 80, 84, 68);
            addWrist(up[i], 404, 252 + i * 80, 84, 68);
        }
        addWrist(SettingsAction::ClockOff, 740, 108, 125, 60);
        addWrist(SettingsAction::Clock12, 879, 108, 125, 60);
        addWrist(SettingsAction::Clock24, 1018, 108, 125, 60);
        const SettingsAction minus[] = {SettingsAction::WristPitchDown, SettingsAction::YawDown, SettingsAction::RollDown};
        const SettingsAction plus[] = {SettingsAction::WristPitchUp, SettingsAction::YawUp, SettingsAction::RollUp};
        for (int i = 0; i < 3; ++i) {
            addWrist(minus[i], 632 + i * 177, 280, 78, 60);
            addWrist(plus[i], 720 + i * 177, 280, 78, 60);
        }
        addWrist(SettingsAction::FadeOn, 860, 360, 138, 60);
        addWrist(SettingsAction::FadeOff, 998, 360, 138, 60);
        addWrist(SettingsAction::FadeAngleDown, 820, 430, 70, 60);
        addWrist(SettingsAction::FadeAngleUp, 1080, 430, 70, 60);
        return;
    }
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

    // 右のカード「向き」: 位置の微調整と同じ十字（上の段に ⟲ ↑ ⟳、下の段に ← ↓ →）。
    // その下に刻みの 1° / 5° の切り替え、いちばん下に自分に向ける / 正面向き
    const double facingCol[3] = {kFacingCardX + kCardPad, kFacingCardX + kCardPad + kFacingArrowW + kPresetGap,
                                 kFacingCardX + kCardPad + 2 * (kFacingArrowW + kPresetGap)};
    add(SettingsAction::RollLeft, facingCol[0], kFacingRowY, kFacingArrowW, kButtonH);
    add(SettingsAction::PitchUp, facingCol[1], kFacingRowY, kFacingArrowW, kButtonH);
    add(SettingsAction::RollRight, facingCol[2], kFacingRowY, kFacingArrowW, kButtonH);
    add(SettingsAction::YawLeft, facingCol[0], kFacingRow2Y, kFacingArrowW, kButtonH);
    add(SettingsAction::PitchDown, facingCol[1], kFacingRow2Y, kFacingArrowW, kButtonH);
    add(SettingsAction::YawRight, facingCol[2], kFacingRow2Y, kFacingArrowW, kButtonH);
    segmented(facingCol[0], kFacingStepY, kFacingInnerW, SettingsAction::AngleStep1, SettingsAction::AngleStep5);
    const double faceW = (kFacingInnerW - kPresetGap) / 2;
    add(SettingsAction::FaceMe, facingCol[0], kFacingFaceY, faceW, kButtonH);
    add(SettingsAction::FaceForward, facingCol[0] + faceW + kPresetGap, kFacingFaceY, faceW, kButtonH);
    // 下の段の言語・自動起動・終了は、見出しの幅が言語で変わるので render() のたびに置き直す
}

std::string SettingsPanel::labelOf(SettingsAction action, const UiText& text) const {
    const auto& w = wristUiText(language_);
    switch (action) {
        case SettingsAction::ChangePage: return wristPage_ ? w.back : w.page;
        case SettingsAction::AttachHead: return w.head;
        case SettingsAction::AttachLeft: return w.left;
        case SettingsAction::AttachRight: return w.right;
        case SettingsAction::ClockOff: return text.off;
        case SettingsAction::Clock12: return "12h";
        case SettingsAction::Clock24: return "24h";
        case SettingsAction::FadeOn: return text.on;
        case SettingsAction::FadeOff: return text.off;
        case SettingsAction::OffsetXDown: case SettingsAction::OffsetYDown: case SettingsAction::OffsetZDown:
        case SettingsAction::WristPitchDown: case SettingsAction::YawDown: case SettingsAction::RollDown:
        case SettingsAction::FadeAngleDown: return "−";
        case SettingsAction::OffsetXUp: case SettingsAction::OffsetYUp: case SettingsAction::OffsetZUp:
        case SettingsAction::WristPitchUp: case SettingsAction::YawUp: case SettingsAction::RollUp:
        case SettingsAction::FadeAngleUp: return "+";
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
        case SettingsAction::RollLeft: return text.rollLeft;
        case SettingsAction::RollRight: return text.rollRight;
        case SettingsAction::FaceMe: return text.faceMe;
        case SettingsAction::FaceForward: return text.faceForward;
        case SettingsAction::AngleStep1: return text.angleStep1;
        case SettingsAction::AngleStep5: return text.angleStep5;
        case SettingsAction::SizeDown:
        case SettingsAction::AlphaDown: return "−";
        case SettingsAction::SizeUp:
        case SettingsAction::AlphaUp: return "＋";
        case SettingsAction::Reset: return text.reset;
        case SettingsAction::Quit: return quitArmed_ ? text.quitConfirm : text.quit;
        case SettingsAction::AutostartOn: return text.on;
        case SettingsAction::AutostartOff: return text.off;
        case SettingsAction::UpdateCheckNow: return text.updateCheckNow;
        case SettingsAction::UpdateInstall: return text.updateButton;
        case SettingsAction::UpdateConfirmYes: return text.updateConfirmYes;
        case SettingsAction::UpdateConfirmNo: return text.updateConfirmNo;
        case SettingsAction::UpdateRetry: return text.updateRetry;
        case SettingsAction::UpdateDismiss: return text.updateDismiss;
        case SettingsAction::None: break;
    }
    return "";
}

SettingsAction SettingsPanel::hitTest(double x, double y) const {
    for (const auto& b : buttons_) {
        if (!wristSelected_ && b.action >= SettingsAction::OffsetXDown && b.action <= SettingsAction::RollUp) continue;
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
    if (pressed_ == SettingsAction::ChangePage) {
        wristPage_ = !wristPage_;
        quitArmed_ = false;
        updateConfirmArmed_ = false;
        hover_ = pressed_ = SettingsAction::None;
        layoutButtons();
        return SettingsAction::None;
    }
    if (pressed_ == SettingsAction::Quit) {
        // 誤って押しても終わらないよう、1 回目は確認の表示にするだけ
        if (quitArmed_ && now <= quitArmedUntil_) return SettingsAction::Quit;
        quitArmed_ = true;
        quitArmedUntil_ = now + kQuitConfirmSec;
        return SettingsAction::None;
    }
    quitArmed_ = false;  // 別のボタンを押したら確認は取り消す
    if (pressed_ == SettingsAction::UpdateInstall) {
        // 「更新する」の 1 回目は、確認の表示（「%s に更新しますか？」＋ 更新する / やめる）に切り替えるだけ
        updateConfirmArmed_ = true;
        return SettingsAction::None;
    }
    if (pressed_ == SettingsAction::UpdateConfirmNo) {
        updateConfirmArmed_ = false;  // 確認をやめる
        return SettingsAction::None;
    }
    // ここまで来たら確認の表示は終わり（Yes で実行するときも、ほかのボタンを押して取り消すときも）
    updateConfirmArmed_ = false;
    if (pressed_ == SettingsAction::AngleStep1 || pressed_ == SettingsAction::AngleStep5) {
        // 刻みはこのパネルの中だけで覚える（設定ファイルには保存しない）
        angleStepDeg_ = pressed_ == SettingsAction::AngleStep5 ? 5.0 : 1.0;
        return SettingsAction::None;
    }
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

void SettingsPanel::showWristPage() {
    wristPage_ = true;
    layoutButtons();
}

void SettingsPanel::armQuitForPreview() {
    quitArmed_ = true;
    quitArmedUntil_ = 1e300;
}

void SettingsPanel::armUpdateConfirmForPreview() {
    updateConfirmArmed_ = true;
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
                               double size, int rotateIcon, bool check) const {
    const Button* b = findButton(action);
    if (b == nullptr) return;
    const bool disabled = !wristSelected_ && action >= SettingsAction::OffsetXDown && action <= SettingsAction::RollUp;
    const bool pressed = !disabled && pressed_ == action;
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
    const double checkW = selected && check ? size * 0.9 : 0;
    const double iconW = rotateIcon != 0 ? size * 1.15 : 0;  // 回す向きの絵と、文言との間
    const double labelSize = fitSize(pen, label, size, size * 0.65, b->w - 20 - checkW - iconW, true);
    const double labelW = pen.measure(label, labelSize, true);
    const double tx = b->x + (b->w - labelW - checkW - iconW) / 2;
    const Color fg = disabled ? kTextDisabled : selected ? kOnAccent : kText;
    if (selected && check) drawCheck(pen.cr, tx + checkW * 0.4, b->y + b->h / 2, size * 0.72, kOnAccent);
    const double labelX = tx + checkW + (rotateIcon < 0 ? iconW : 0);
    if (rotateIcon != 0) {
        const double iconCx = rotateIcon < 0 ? tx + checkW + size * 0.45 : labelX + labelW + size * 0.7;
        drawRotateArrow(pen.cr, iconCx, b->y + b->h / 2, size * 0.4, rotateIcon < 0, fg);
    }
    pen.text(labelX, centerBaseline(b->y, b->h, labelSize), label, labelSize, fg, true);
}

double SettingsPanel::drawHeader(const Pen& pen, const UiText& text, const Config& config) const {
    pen.text(kPad, centerBaseline(kHeaderY, kHeaderH, 34), text.settingsTitle, 34, kText, true);
    // 右上の状態のピル: 表示中は緑の塗り＋●＋文字、非表示は灰色の塗り＋○＋文字（色だけで伝えない）
    const std::string label = config.visible ? text.panelShown : text.panelHidden;
    const double size = 20;
    const double h = 42;
    const double y = kHeaderY + (kHeaderH - h) / 2;
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
    return x;
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
    pen.text(kRightCardX + kCardPad, kCardY + 42, wristUiText(config.language).headPosition, 24, kText, true);
    // いまの位置（HMD から見て、cm）を見出しの右に
    const std::string where = std::string(text.positionNow) + "  " + text.posX + " " + signedCm(config.posX) + "  " +
                              text.posY + " " + signedCm(config.posY) + "  " + text.posZ + " " +
                              signedCm(-config.posZ) + " cm";
    const double whereSize = fitSize(pen, where, 18, 13, kRightCardW - kCardPad * 2 - 180, false);
    pen.text(kRightCardX + kRightCardW - kCardPad, kCardY + 40, where, whereSize, kTextMuted, false, true);

    for (const auto& preset : kPresets) drawButton(pen, text, preset.action, matchesPreset(preset, config), 24);
    pen.text(kRightCardX + kCardPad, kNudgeY - 12, text.rowNudge, 18, kTextMuted);
    for (const SettingsAction action : {SettingsAction::MoveUp, SettingsAction::MoveLeft, SettingsAction::MoveDown,
                                        SettingsAction::MoveRight, SettingsAction::MoveNear, SettingsAction::MoveFar}) {
        drawButton(pen, text, action, false, 24);
    }
}

void SettingsPanel::drawWristPage(const Pen& pen, const UiText& text, const Config& config) const {
    const auto& w = wristUiText(config.language);
    const auto& p = selectedWrist(config);
    drawCard(pen, 28, 124, 560, 424, 20, kCard, kCard, 0);
    drawCard(pen, 612, 124, 560, 424, 20, kCard, kCard, 0);
    pen.text(48, 166, w.attachment, 24, kText, true);
    drawButton(pen, text, SettingsAction::AttachHead, config.attachment == Attachment::Head, 22);
    drawButton(pen, text, SettingsAction::AttachLeft, config.attachment == Attachment::LeftWrist, 22);
    drawButton(pen, text, SettingsAction::AttachRight, config.attachment == Attachment::RightWrist, 22);
    const std::string hint = wristSelected_ ? w.offsets : w.selectWrist;
    pen.text(48, 280, hint, fitSize(pen, hint, 18, 13, 520, false), kTextMuted);
    const double offsets[] = {p.x, p.y, p.z};
    const char* axes[] = {"X", "Y", "Z"};
    const SettingsAction down[] = {SettingsAction::OffsetXDown, SettingsAction::OffsetYDown, SettingsAction::OffsetZDown};
    const SettingsAction up[] = {SettingsAction::OffsetXUp, SettingsAction::OffsetYUp, SettingsAction::OffsetZUp};
    char value[64];
    for (int i = 0; i < 3; ++i) {
        const double y = 292 + i * 80;
        pen.text(85, centerBaseline(y, 68, 24), axes[i], 24, kTextMuted);
        std::snprintf(value, sizeof(value), "%+.0f", offsets[i] * 100);
        textCentered(pen, 344, centerBaseline(y, 68, 28), value, 28, wristSelected_ ? kText : kTextDisabled, true);
        drawButton(pen, text, down[i], false, 28); drawButton(pen, text, up[i], false, 28);
    }
    pen.text(632, 187, w.clock, 22, kText, true);
    drawButton(pen, text, SettingsAction::ClockOff, config.clockFormat == 0, 22);
    drawButton(pen, text, SettingsAction::Clock12, config.clockFormat == 12, 22);
    drawButton(pen, text, SettingsAction::Clock24, config.clockFormat == 24, 22);
    pen.text(632, 242, w.rotation, 20, kTextMuted);
    const double angles[] = {p.pitch, p.yaw, p.roll};
    const char* labels[] = {w.pitch, w.yaw, w.roll};
    const SettingsAction minus[] = {SettingsAction::WristPitchDown, SettingsAction::YawDown, SettingsAction::RollDown};
    const SettingsAction plus[] = {SettingsAction::WristPitchUp, SettingsAction::YawUp, SettingsAction::RollUp};
    for (int i = 0; i < 3; ++i) {
        const double x = 715 + i * 177;
        textCentered(pen, x, 270, labels[i], 18, kTextMuted, false);
        std::snprintf(value, sizeof(value), "%+.0f°", angles[i]);
        textCentered(pen, x, 302, value, 26, wristSelected_ ? kText : kTextDisabled, true);
        drawButton(pen, text, minus[i], false, 28); drawButton(pen, text, plus[i], false, 28);
    }
    pen.text(632, 438, w.fade, 22, kTextMuted);
    drawSegmented(pen, text, SettingsAction::FadeOn, SettingsAction::FadeOff, config.wristFade ? 0 : 1, true);
    pen.text(632, 508, w.fadeRange, 20, kTextMuted);
    std::snprintf(value, sizeof(value), "%.0f–%.0f°", config.wristFadeEndDeg - 30, config.wristFadeEndDeg);
    textCentered(pen, 985, 508, value, 22, kText, true);
    drawButton(pen, text, SettingsAction::FadeAngleDown, false, 26);
    drawButton(pen, text, SettingsAction::FadeAngleUp, false, 26);
}

void SettingsPanel::drawFacingCard(const Pen& pen, const UiText& text, const Config& config) const {
    drawCard(pen, kFacingCardX, kCardY, kFacingCardW, kCardH, 20, kCard, kCard, 0);
    const double titleW = pen.text(kFacingCardX + kCardPad, kCardY + 42, text.cardFacing, 24, kText, true);
    // いまの向き（度）を見出しの右に（見出しと重ならない幅で）
    const std::string now = std::string(text.facingNow) + "  " + text.yawName + " " + signedDegrees(config.yawDeg) +
                            "  " + text.pitchName + " " + signedDegrees(config.pitchDeg) + "  " + text.rollName + " " +
                            signedDegrees(config.rollDeg);
    const double nowSize = fitSize(pen, now, 18, 12, kFacingInnerW - titleW - 16, false);
    pen.text(kFacingCardX + kFacingCardW - kCardPad, kCardY + 40, now, nowSize, kTextMuted, false, true);

    for (const SettingsAction action :
         {SettingsAction::YawLeft, SettingsAction::YawRight, SettingsAction::PitchUp, SettingsAction::PitchDown}) {
        drawButton(pen, text, action, false, 24);
    }
    drawButton(pen, text, SettingsAction::RollLeft, false, 24, -1);
    drawButton(pen, text, SettingsAction::RollRight, false, 24, +1);
    drawSegmented(pen, text, SettingsAction::AngleStep1, SettingsAction::AngleStep5, angleStepDeg_ == 5.0 ? 1 : 0,
                  true);
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

void SettingsPanel::drawUpdateBar(const Pen& pen, const UiText& text, const Config& config,
                                   const frame_updater::UpdateStatus& update, double left, double right) {
    using frame_updater::UpdateState;

    // 前フレームで置いたボタンを消してから、今の状態で要るものだけ置き直す（同じ場所に別のボタンが
    // 重なって残らないように）
    for (const SettingsAction a :
         {SettingsAction::UpdateCheckNow, SettingsAction::UpdateInstall, SettingsAction::UpdateConfirmYes,
          SettingsAction::UpdateConfirmNo, SettingsAction::UpdateRetry, SettingsAction::UpdateDismiss}) {
        buttons_.erase(std::remove_if(buttons_.begin(), buttons_.end(), [a](const Button& b) { return b.action == a; }),
                      buttons_.end());
    }

    const bool confirming = updateConfirmArmed_ && update.state == UpdateState::Available;

    /** printf 書式（%s が 1 つ）に版を当てはめる。 */
    const auto format1 = [](const char* fmt, const std::string& value) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), fmt, value.c_str());
        return std::string(buf);
    };

    std::string headline;
    std::string detail;     // 見出しの後ろに続ける補足（入りきらなければ 2 行目に回す）
    std::string detailSep;  // 1 行で出すときの、見出しと補足の間
    Color color = kTextMuted;
    bool bold = false;
    std::vector<SettingsAction> buttons;  // 右に置くボタン（左から順）
    // 帯の枠（Frame のアプリ共通の見本に合わせる）: 新しい版・確認はピンク、失敗は赤、ほかは枠なし
    Color border = kCard;
    double borderWidth = 0;
    std::string hint;  // 右に小さく出す灰色の補足（更新中だけ）

    if (confirming) {
        headline = format1(text.updateConfirmFormat, update.latest);
        color = kText;
        bold = true;
        border = kAccent;
        borderWidth = 2;
        buttons = {SettingsAction::UpdateConfirmNo, SettingsAction::UpdateConfirmYes};
    } else {
        switch (update.state) {
            case UpdateState::Unknown:
                headline = update.checking ? text.updateChecking : update.current;
                if (!update.checking) buttons = {SettingsAction::UpdateCheckNow};
                break;
            case UpdateState::UpToDate:
                headline = format1(text.updateUpToDateFormat, update.current);
                if (!update.checking) buttons = {SettingsAction::UpdateCheckNow};
                break;
            case UpdateState::Available:
                headline = format1(text.updateAvailableFormat, update.latest);
                color = kAccentText;
                bold = true;
                border = kAccent;
                borderWidth = 2;
                if (!update.installable) {
                    detail = text.updateManual;
                    detailSep = "  ";
                }
                // 見本どおり［今すぐ確かめる］［更新する］の順（強調の［更新する］を右端に）
                if (!update.checking) buttons.push_back(SettingsAction::UpdateCheckNow);
                if (update.installable) buttons.push_back(SettingsAction::UpdateInstall);
                break;
            case UpdateState::Installing:
                headline = format1(text.updateInstallingFormat, updateStepText(config.language, update.step));
                color = kText;
                hint = text.updateConfirmHint;  // 途中で画面が閉じて開き直すことがある旨
                break;  // 進行中はボタンなし
            case UpdateState::Installed:
                headline = format1(text.updateInstalledFormat, update.version);
                color = kSuccess;
                bold = true;
                buttons = {SettingsAction::UpdateDismiss};
                break;
            case UpdateState::CheckFailed:
                // くわしい理由（updateLogHint）はここには出さない。ボタンと並ぶと長い文言で入りきらないため
                // （journalctl・README の「うまく動かないとき」を参照）
                headline = text.updateCheckFailed;
                detail = updateErrorText(config.language, update.error);
                detailSep = " ";
                // 枠は普通のまま、文字だけ赤（オフラインだと毎日出るので、赤い枠は目立ちすぎる）
                color = kDanger;
                bold = true;
                if (!update.checking) buttons = {SettingsAction::UpdateCheckNow};
                break;
            case UpdateState::InstallFailed:
                headline = text.updateInstallFailed;
                detail = updateErrorText(config.language, update.error);
                detailSep = " ";
                color = kDanger;
                bold = true;
                border = kDanger;
                borderWidth = 2;
                buttons = {SettingsAction::UpdateRetry, SettingsAction::UpdateDismiss};
                break;
        }
    }

    drawCard(pen, left, kHeaderY, right - left, kHeaderH, 16, kCard, border, borderWidth);

    // 右のボタンを右詰めで置く（幅は文言に合わせる。高さはほかのボタンと同じ 68px）
    const double btnY = kHeaderY + (kHeaderH - kButtonH) / 2;
    const double gap = 12;
    const double inset = 8;  // 帯の端とボタンの間（上下と同じ）
    double buttonX = right - inset;
    for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
        const double w = pen.measure(labelOf(*it, text), 22, true) + 40;
        buttonX -= w;
        placeFooterButton(*it, buttonX, btnY, w, kButtonH);
        buttonX -= gap;
    }
    const double textLeft = left + kCardPad;
    double textRight = buttons.empty() ? right - kCardPad : buttonX + gap - 16;
    if (!hint.empty()) {
        // 補足は右詰めで小さく。見出しの幅を先に取り、残りに入れる。1 行で 15px に届かなければ、
        // 文の切れ目（「。」/ ". "）で 2 行に分ける（文言は変えない）
        const double headW = pen.measure(headline, 22, bold);
        const double hintRight = right - kCardPad;
        const double hintMaxW = hintRight - (textLeft + headW + 24);
        const double hintSize = fitSize(pen, hint, 17, 12, hintMaxW, false);
        size_t cut = hint.find("。");
        size_t cutLen = 3;  // 「。」は UTF-8 で 3 バイト
        if (cut == std::string::npos) {
            cut = hint.find(". ");
            cutLen = 1;
        }
        if (hintSize >= 15 || cut == std::string::npos) {
            pen.text(hintRight, centerBaseline(kHeaderY, kHeaderH, hintSize), hint, hintSize, kTextMuted, false, true);
            textRight = hintRight - pen.measure(hint, hintSize, false) - 24;
        } else {
            const std::string first = hint.substr(0, cut + cutLen);
            std::string second = hint.substr(cut + cutLen);
            while (!second.empty() && second[0] == ' ') second.erase(0, 1);
            const double size = std::min(fitSize(pen, first, 16, 12, hintMaxW, false),
                                         fitSize(pen, second, 16, 12, hintMaxW, false));
            pen.text(hintRight, kHeaderY + 36, first, size, kTextMuted, false, true);
            pen.text(hintRight, kHeaderY + 62, second, size, kTextMuted, false, true);
            textRight = hintRight - std::max(pen.measure(first, size, false), pen.measure(second, size, false)) - 24;
        }
    }
    // 1 行で 20px 以上で入ればそのまま。入らなければ、見出しと補足を 2 行に分ける（文言は変えない）
    const double maxTextW = textRight - textLeft;
    const std::string oneLine = detail.empty() ? headline : headline + detailSep + detail;
    const double oneSize = fitSize(pen, oneLine, 22, 14, maxTextW, bold);
    if (detail.empty() || oneSize >= 20) {
        pen.text(textLeft, centerBaseline(kHeaderY, kHeaderH, oneSize), oneLine, oneSize, color, bold);
    } else {
        const double size1 = fitSize(pen, headline, 20, 14, maxTextW, bold);
        const double size2 = fitSize(pen, detail, 18, 13, maxTextW, bold);
        pen.text(textLeft, kHeaderY + 36, headline, size1, color, bold);
        pen.text(textLeft, kHeaderY + 66, detail, size2, color, bold);
    }
    // ［更新する］（確認へ・確認の実行）は強調ボタン（アクセントの塗り ＋ 濃い文字。✓ は付けない）
    for (const SettingsAction a : buttons) {
        const bool primary = a == SettingsAction::UpdateInstall || a == SettingsAction::UpdateConfirmYes;
        drawButton(pen, text, a, primary, 22, 0, false);
    }
}

void SettingsPanel::render(const Config& config, const AutostartStatus& autostart,
                            const frame_updater::UpdateStatus& update) {
    const Pen pen {cr_, &fonts_};
    const UiText& t = uiText(config.language);
    language_ = config.language;
    wristSelected_ = config.attachment != Attachment::Head;

    // 地（不透明。コントラスト比は不透明な地で計算している）
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    pen.color(kBg);
    pen.roundedRect(0, 0, kWidth, kHeight, 24);
    cairo_fill(cr_);

    const double pillX = drawHeader(pen, t, config);
    const double titleRight = kPad + pen.measure(t.settingsTitle, 34, true);
    drawUpdateBar(pen, t, config, update, titleRight + 28, pillX - 20);
    if (wristPage_) {
        drawWristPage(pen, t, config);
    } else {
        drawPanelCard(pen, t, config);
        drawPositionCard(pen, t, config);
        drawFacingCard(pen, t, config);
    }
    drawButton(pen, t, SettingsAction::ChangePage, false, 20);
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

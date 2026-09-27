// 画面に出す文言の表の中身。
#include "i18n.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

/**
 * Steam の言語設定を読む。~/.steam/registry.vdf の最初の "language" の値（"japanese" など）。
 * @return 値。読めなければ空
 */
std::string steamLanguage() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return "";
    std::ifstream file(std::string(home) + "/.steam/registry.vdf");
    std::string line;
    while (std::getline(file, line)) {
        // 形式: <タブ>"language"<タブ>"japanese"
        const std::string key = "\"language\"";
        const size_t at = line.find(key);
        if (at == std::string::npos) continue;
        const size_t open = line.find('"', at + key.size());
        const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
        if (close == std::string::npos) return "";
        return line.substr(open + 1, close - open - 1);
    }
    return "";
}

/**
 * ロケールの環境変数（LC_ALL → LC_MESSAGES → LANG の順で最初に空でないもの）が日本語か。
 * @return 日本語なら true
 */
bool localeIsJapanese() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && value[0] != '\0') return std::string(value).rfind("ja", 0) == 0;
    }
    return false;
}

/**
 * システム言語を調べる（systemLanguage の本体）。
 * @return 言語
 */
Language detectSystemLanguage() {
    const std::string steam = steamLanguage();
    if (!steam.empty()) return steam == "japanese" ? Language::Ja : Language::En;
    return localeIsJapanese() ? Language::Ja : Language::En;
}

const UiText kJapanese = {
    // 性能パネル
    "フレーム", "計測待ち", "SteamVR 未接続", "再投影 ", "落ち ",
    "電力", "全ch計 ", "ファン ",
    "温度", "℃ CPU", "℃", "電池 ", "画面 ", "排気 ", "放熱 ",
    "（最大コア ", "）",
    "直通", "未接続", "リンク ",
    "電池 ", "左手", "右手", "コントローラー なし", "メモリ",
    // 設定パネル
    "Frame Perf 設定", "パネル", "。", "パネル表示中", "パネル非表示",
    "表示", "言語", "位置", "微調整", "大きさ", "透明度",
    "自動起動", "自動起動の切り替えは次回の SteamVR 起動から効きます",
    "自動起動のユニットが入っていないので切り替えられません（README の「自動起動」を見てね）", "自動起動を切り替え中…",
    "自動起動の切り替えに失敗しました（ログを見てね）",
    "オン", "オフ",
    "左下", "中央下", "右下", "左上", "右上",
    "← 左", "右 →", "↑ 上", "↓ 下", "近く", "遠く",
    "既定に戻す", "アプリを終了", "もう一度押すと終了",
    "いまの位置", "横", "縦", "前",
    "向き", "← 左向き", "右向き →", "↑ 上向き", "↓ 下向き", "自分に向ける", "正面向き",
    "いまの向き", "左右", "上下", "回転",
    "変更はすぐ反映され、設定ファイルに保存されます",
};

const UiText kEnglish = {
    // Performance panel
    "Frame", "Measuring", "No SteamVR", "Reproj ", "Drop ",
    "Power", "All ch ", "Fan ",
    "Temp", "°C CPU", "°C", "Batt ", "Disp ", "Exh ", "Sink ",
    " (max core ", ")",
    "Link", "Not connected", "Rate ",
    "Batt ", "L", "R", "No controllers", "Mem",
    // Settings panel
    "Frame Perf Settings", "Panel", ". ", "Panel shown", "Panel hidden",
    "Show", "Language", "Position", "Nudge", "Size", "Opacity",
    "Autostart", "Autostart changes apply from the next SteamVR start",
    "The autostart unit is not installed, so it can't be switched (see \"Autostart\" in the README)",
    "Switching autostart…", "Failed to switch autostart (see the log)",
    "On", "Off",
    "Bottom L", "Bottom C", "Bottom R", "Top L", "Top R",
    "← Left", "Right →", "↑ Up", "↓ Down", "Closer", "Farther",
    "Reset", "Quit app", "Press again to quit",
    "Now", "x", "y", "z",
    "Facing", "← Left", "Right →", "↑ Up", "↓ Down", "Face me", "Face ahead",
    "Now", "yaw", "pitch", "roll",
    "Changes apply instantly and are saved",
};

/**
 * 数値を小数なしの文字列にする。
 * @param value 値
 * @return 文字列
 */
std::string whole(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", value);
    return text;
}

}  // namespace

const UiText& uiText(Language language) {
    return language == Language::En ? kEnglish : kJapanese;
}

std::string throttleBadgeText(Language language, double hz, int throttled) {
    const std::string fraction = "1/" + std::to_string(throttled + 1);
    if (language == Language::En) return "Throttled to " + fraction + " of " + whole(hz) + "Hz (reproj)";
    return whole(hz) + "Hz の" + (throttled == 1 ? std::string("半分") : fraction) + "に制限（再投影）";
}

std::string thermalBadgeText(Language language, bool cpu, bool gpu) {
    const bool en = language == Language::En;
    const std::string which = cpu && gpu ? (en ? "CPU+GPU" : "CPU・GPU") : (cpu ? "CPU" : "GPU");
    return (en ? "Thermal limit " : "熱で制限中 ") + which;
}

std::string batteryStatusText(Language language, const std::string& status, bool chargerOnline) {
    const bool en = language == Language::En;
    if (status == "Charging") return en ? "Charging" : "充電中";
    if (status == "Discharging") return en ? "On battery" : "放電中";
    if (status == "Full") return en ? "Full" : "満充電";
    if (status == "Not charging") return chargerOnline ? (en ? "Plugged" : "給電中") : (en ? "Idle" : "待機");
    return status;
}

const char* languageCode(Language language) {
    return language == Language::En ? "en" : "ja";
}

Language systemLanguage() {
    static const Language cached = detectSystemLanguage();
    return cached;
}

bool parseLanguage(const std::string& code, Language& language) {
    if (code == "ja") {
        language = Language::Ja;
        return true;
    }
    if (code == "en") {
        language = Language::En;
        return true;
    }
    return false;
}

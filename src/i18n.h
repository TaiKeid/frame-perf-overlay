// 画面に出す文言の表（日本語・英語）。描画コードの中に文言を書かず、ここから引く。
// ログや --print の出力は日本語のまま（ここには入れない）。
#pragma once

#include <string>

/** 表示の言語。 */
enum class Language { Ja, En };

/**
 * Frame のシステム言語（設定ファイルに language が無いときの既定値）。
 * Steam の言語設定（~/.steam/registry.vdf の "language"、読むだけ）が日本語なら日本語、
 * 読めなければ LC_ALL / LC_MESSAGES / LANG を見て、どれでもなければ英語。結果は最初の 1 回だけ調べて覚えておく。
 * @return 言語
 */
Language systemLanguage();

/**
 * パネルと設定パネルの文言一式。
 */
struct UiText {
    // ---- 性能パネル ----
    const char* frame;          ///< フレームの段の見出し
    const char* waiting;        ///< フレームの計測待ち
    const char* noSteamVr;      ///< SteamVR につながっていない
    const char* reproj;         ///< 再投影（後ろに % が続く）
    const char* dropped;        ///< 落ちたフレーム（後ろに数が続く）
    const char* power;          ///< 電力の段の見出し
    const char* allChannels;    ///< 全チャンネルの合計（後ろに W が続く）
    const char* fan;            ///< ファン
    const char* temp;           ///< 温度の段の見出し
    const char* cpuTempUnit;    ///< 大きい CPU 温度の単位（℃ CPU）
    const char* celsius;        ///< 温度の単位（℃ / °C）
    const char* battTemp;       ///< 電池の温度
    const char* display;        ///< 画面付近の温度
    const char* exhaust;        ///< 排気の温度
    const char* heatsink;       ///< 放熱板の温度
    const char* maxCoreOpen;    ///< 「（最大コア 」
    const char* maxCoreClose;   ///< 「）」
    const char* direct;         ///< Steam Link の直通回線
    const char* notConnected;   ///< 直通回線が未接続
    const char* linkRate;       ///< リンク速度
    const char* battery;        ///< 本体の電池
    const char* leftHand;       ///< 左手のコントローラー
    const char* rightHand;      ///< 右手のコントローラー
    const char* noControllers;  ///< コントローラーがどちらもいない
    const char* memory;         ///< メモリ
    // ---- 設定パネル ----
    const char* settingsTitle;
    const char* cardPanel;              ///< 左のカードの見出し（パネル）
    const char* sentenceBreak;          ///< 最下行で 2 つの文をつなぐ区切り（。/ . ）
    const char* panelShown;
    const char* panelHidden;
    const char* rowShow;
    const char* rowLanguage;
    const char* rowPosition;
    const char* rowNudge;
    const char* rowSize;
    const char* rowOpacity;
    const char* rowAutostart;           ///< 自動起動
    const char* autostartNextLaunch;    ///< 次回の SteamVR 起動から
    const char* autostartNotInstalled;  ///< 未インストール
    const char* autostartBusy;          ///< 切り替え中
    const char* autostartFailed;        ///< 切り替えに失敗
    const char* on;
    const char* off;
    const char* bottomLeft;
    const char* bottomCenter;
    const char* bottomRight;
    const char* topLeft;
    const char* topRight;
    const char* moveLeft;
    const char* moveRight;
    const char* moveUp;
    const char* moveDown;
    const char* nearer;
    const char* farther;
    const char* reset;
    const char* quit;           ///< アプリを終了
    const char* quitConfirm;    ///< もう一度押すと終了
    const char* positionNow;    ///< いまの位置（後ろに横・縦・前が続く）
    const char* posX;
    const char* posY;
    const char* posZ;
    const char* cardFacing;     ///< 向きのカードの見出し
    const char* yawLeft;        ///< 面を左へ向ける
    const char* yawRight;       ///< 面を右へ向ける
    const char* pitchUp;        ///< 面を上へ向ける
    const char* pitchDown;      ///< 面を下へ向ける
    const char* faceMe;         ///< 自分に向ける
    const char* faceForward;    ///< 正面向き（回転なし）
    const char* angleStep1;     ///< 向きのボタンを 1° ずつ動かす
    const char* angleStep5;     ///< 向きのボタンを 5° ずつ動かす
    const char* facingNow;      ///< いまの向き（後ろに左右・上下・回転が続く）
    const char* yawName;
    const char* pitchName;
    const char* rollName;
    const char* footer;         ///< 変更はすぐ反映される旨
    // ---- 更新（vendor/frame-updater/strings.md より） ----
    const char* rowUpdateCheck;          ///< 設定ファイルの説明用（「新しい版の確認」）
    const char* updateUpToDateFormat;    ///< `UpToDate`（%s は版）
    const char* updateChecking;          ///< `checking` で、まだ答えがない
    const char* updateAvailableFormat;   ///< `Available`（%s は版）
    const char* updateButton;            ///< 「更新する」を押す（確認へ）
    const char* updateManual;            ///< `installable` が false のときの案内
    const char* updateReleasePage;       ///< リリースページの URL の前
    const char* updateConfirmFormat;     ///< 確認の質問（%s は版）
    const char* updateConfirmHint;       ///< 確認の補足
    const char* updateConfirmYes;        ///< 確認の実行ボタン
    const char* updateConfirmNo;         ///< 確認のやめるボタン
    const char* updateInstallingFormat;  ///< `Installing`（%s は下の手順の文言）
    const char* updateInstalledFormat;   ///< `Installed`（%s は版）
    const char* updateInstallFailed;     ///< `InstallFailed` の見出し（理由が続く）
    const char* updateCheckFailed;       ///< `CheckFailed` の見出し（理由が続く）
    const char* updateCheckNow;          ///< ［確認］ボタン（strings.md のまま。幅は足りている）
    const char* updateRetry;             ///< `InstallFailed` のやり直しボタン
    const char* updateDismiss;           ///< `Installed` / `InstallFailed` を閉じる
    const char* updateLogHint;           ///< 失敗したときの補足（ログの場所）
};

/**
 * 言語の文言の表を返す。
 * @param language 言語
 * @return 文言の表（プログラムの終わりまで有効）
 */
const UiText& uiText(Language language);

/**
 * SteamVR がアプリを抑えているときのバッジの文言（例: 「72Hz の半分に制限（再投影）」）。
 * @param language 言語
 * @param hz リフレッシュレート
 * @param throttled 抑えている段数（1 以上）
 * @return 文言
 */
std::string throttleBadgeText(Language language, double hz, int throttled);

/**
 * 熱で制限がかかっているときのバッジの文言（例: 「熱で制限中 CPU・GPU」）。
 * @param language 言語
 * @param cpu CPU が制限中か
 * @param gpu GPU が制限中か
 * @return 文言
 */
std::string thermalBadgeText(Language language, bool cpu, bool gpu);

/**
 * 本体の電池の状態を短い文言にする。
 * @param language 言語
 * @param status power_supply の status（Charging など）
 * @param chargerOnline 充電器がつながっているか
 * @return 文言
 */
std::string batteryStatusText(Language language, const std::string& status, bool chargerOnline);

/**
 * 更新の手順（`UpdateStatus::step`）を文言にする。知らない手順はそのまま返す。
 * @param language 言語
 * @param step "start" / "download" / "verify" / "extract" / "install"
 * @return 文言
 */
std::string updateStepText(Language language, const std::string& step);

/**
 * 更新のエラーコード（`UpdateStatus::error`）を文言にする。知らないコードは「other」の文言にする。
 * @param language 言語
 * @param error frame-update.sh / update_check.h のエラーコード
 * @return 文言
 */
std::string updateErrorText(Language language, const std::string& error);

/**
 * 設定ファイルに書く言語の名前。
 * @param language 言語
 * @return "ja" / "en"
 */
const char* languageCode(Language language);

/**
 * 設定ファイルの言語の名前を読む。
 * @param code "ja" / "en"
 * @param language 読めたときの書き込み先
 * @return 知っている名前なら true
 */
bool parseLanguage(const std::string& code, Language& language);

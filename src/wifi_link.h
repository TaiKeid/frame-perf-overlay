// Steam Link の直通回線（Frame が AP になる wlanap）の状態。nl80211 を libnl で直接読む（iw は起動しない）。
#pragma once

#include <string>

struct nl_sock;

/**
 * 直通回線の読み取り結果。値が取れないときは NaN。
 */
struct WifiInfo {
    bool interfaceUp = false;   ///< wlanap がある
    bool connected = false;     ///< ステーション（PC）がつながっている
    double signalDbm;           ///< 電波の強さ（avg ack signal、dBm）
    double txLinkMbps;          ///< Frame → PC のリンク速度
    double rxLinkMbps;          ///< PC → Frame のリンク速度
    double rxMbps;              ///< 実際に受け取っている量（PC → Frame、映像はこちら）
    double txMbps;              ///< 実際に送っている量（Frame → PC）

    WifiInfo();
};

/**
 * nl80211 のソケットを 1 本持ち続けて、ステーションの情報を問い合わせる。書き込み（設定変更）はしない。
 */
class WifiLink {
public:
    /**
     * @param interfaceName 調べるインターフェース名（既定 wlanap）
     */
    explicit WifiLink(std::string interfaceName = "wlanap");
    ~WifiLink();
    WifiLink(const WifiLink&) = delete;
    WifiLink& operator=(const WifiLink&) = delete;

    /**
     * ステーションの情報と、流れている量（前回からの差分）を読む。
     * @param now 現在時刻（秒、単調増加）
     * @return 読み取り結果
     */
    WifiInfo read(double now);

private:
    std::string interfaceName_;
    nl_sock* socket_ = nullptr;
    int family_ = -1;
    int rxBytesFd_ = -1;
    int txBytesFd_ = -1;
    double lastTime_ = -1.0;
    unsigned long long lastRxBytes_ = 0;
    unsigned long long lastTxBytes_ = 0;

    /**
     * nl80211 のソケットを用意する（失敗したら次の read で作り直す）。
     * @return 使えるなら true
     */
    bool ensureSocket();

    /**
     * statistics の数値ファイルを先頭から読み直す。
     * @param fd 開いてあるファイル
     * @param value 書き込み先
     * @return 読めたら true
     */
    static bool readCounter(int fd, unsigned long long& value);
};

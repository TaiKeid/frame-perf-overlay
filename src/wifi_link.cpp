// 直通回線の読み取りの実装。
#include "wifi_link.h"

#include <fcntl.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <unistd.h>

#include <cstdlib>
#include <limits>

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/** ステーションのダンプを受け取る間の入れ物。 */
struct StationResult {
    bool found = false;
    double signalDbm = kNaN;
    double txLinkMbps = kNaN;
    double rxLinkMbps = kNaN;
};

/**
 * NL80211_STA_INFO_TX_BITRATE / RX_BITRATE の入れ子からリンク速度（Mbps）を取り出す。
 * @param attr ビットレートの属性
 * @return Mbps。取れなければ NaN
 */
double parseBitrate(nlattr* attr) {
    if (attr == nullptr) return kNaN;
    nlattr* rate[NL80211_RATE_INFO_MAX + 1] = {};
    if (nla_parse_nested(rate, NL80211_RATE_INFO_MAX, attr, nullptr) != 0) return kNaN;
    // どちらも 100kbps 単位
    if (rate[NL80211_RATE_INFO_BITRATE32] != nullptr) return nla_get_u32(rate[NL80211_RATE_INFO_BITRATE32]) / 10.0;
    if (rate[NL80211_RATE_INFO_BITRATE] != nullptr) return nla_get_u16(rate[NL80211_RATE_INFO_BITRATE]) / 10.0;
    return kNaN;
}

/**
 * 電波の強さを 1 つ選ぶ。AP 側では signal が 0 dBm になって使えないので、ack signal の平均を優先する。
 * @param info NL80211_STA_INFO_* の配列
 * @return dBm。取れなければ NaN
 */
double pickSignal(nlattr** info) {
    const int order[] = {NL80211_STA_INFO_ACK_SIGNAL_AVG, NL80211_STA_INFO_ACK_SIGNAL, NL80211_STA_INFO_SIGNAL_AVG,
                         NL80211_STA_INFO_SIGNAL};
    for (const int key : order) {
        if (info[key] == nullptr) continue;
        const int dbm = static_cast<int8_t>(nla_get_u8(info[key]));
        if (dbm != 0) return dbm;
    }
    return kNaN;
}

/**
 * GET_STATION のダンプで 1 ステーションごとに呼ばれる。最初のステーションだけ使う。
 * @param message 受け取ったメッセージ
 * @param arg StationResult
 * @return NL_SKIP（続きも受け取る）
 */
int onStation(nl_msg* message, void* arg) {
    auto* result = static_cast<StationResult*>(arg);
    if (result->found) return NL_SKIP;
    nlattr* attrs[NL80211_ATTR_MAX + 1] = {};
    auto* header = static_cast<genlmsghdr*>(nlmsg_data(nlmsg_hdr(message)));
    nla_parse(attrs, NL80211_ATTR_MAX, genlmsg_attrdata(header, 0), genlmsg_attrlen(header, 0), nullptr);
    if (attrs[NL80211_ATTR_STA_INFO] == nullptr) return NL_SKIP;
    nlattr* info[NL80211_STA_INFO_MAX + 1] = {};
    if (nla_parse_nested(info, NL80211_STA_INFO_MAX, attrs[NL80211_ATTR_STA_INFO], nullptr) != 0) return NL_SKIP;
    result->found = true;
    result->signalDbm = pickSignal(info);
    result->txLinkMbps = parseBitrate(info[NL80211_STA_INFO_TX_BITRATE]);
    result->rxLinkMbps = parseBitrate(info[NL80211_STA_INFO_RX_BITRATE]);
    return NL_SKIP;
}

}  // namespace

WifiInfo::WifiInfo() : signalDbm(kNaN), txLinkMbps(kNaN), rxLinkMbps(kNaN), rxMbps(kNaN), txMbps(kNaN) {}

WifiLink::WifiLink(std::string interfaceName) : interfaceName_(std::move(interfaceName)) {}

WifiLink::~WifiLink() {
    if (socket_ != nullptr) nl_socket_free(socket_);
    if (rxBytesFd_ >= 0) ::close(rxBytesFd_);
    if (txBytesFd_ >= 0) ::close(txBytesFd_);
}

bool WifiLink::ensureSocket() {
    if (socket_ != nullptr) return true;
    socket_ = nl_socket_alloc();
    if (socket_ == nullptr) return false;
    if (genl_connect(socket_) != 0 || (family_ = genl_ctrl_resolve(socket_, "nl80211")) < 0) {
        nl_socket_free(socket_);
        socket_ = nullptr;
        return false;
    }
    return true;
}

bool WifiLink::readCounter(int fd, unsigned long long& value) {
    if (fd < 0) return false;
    char buffer[32];
    const ssize_t n = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return false;
    buffer[n] = '\0';
    value = std::strtoull(buffer, nullptr, 10);
    return true;
}

WifiInfo WifiLink::read(double now) {
    WifiInfo info;
    const unsigned ifindex = if_nametoindex(interfaceName_.c_str());
    if (ifindex == 0) {
        lastTime_ = -1.0;
        return info;  // インターフェースが無い
    }
    info.interfaceUp = true;

    if (ensureSocket()) {
        StationResult station;
        nl_msg* message = nlmsg_alloc();
        if (message != nullptr) {
            genlmsg_put(message, NL_AUTO_PORT, NL_AUTO_SEQ, family_, 0, NLM_F_DUMP, NL80211_CMD_GET_STATION, 0);
            nla_put_u32(message, NL80211_ATTR_IFINDEX, ifindex);
            nl_socket_modify_cb(socket_, NL_CB_VALID, NL_CB_CUSTOM, onStation, &station);
            const bool ok = nl_send_auto(socket_, message) >= 0 && nl_recvmsgs_default(socket_) >= 0;
            nlmsg_free(message);
            if (!ok) {
                // ソケットがおかしくなったら次回作り直す
                nl_socket_free(socket_);
                socket_ = nullptr;
            }
        }
        info.connected = station.found;
        info.signalDbm = station.signalDbm;
        info.txLinkMbps = station.txLinkMbps;
        info.rxLinkMbps = station.rxLinkMbps;
    }

    // 実際に流れた量は statistics のバイト数の差分から
    if (rxBytesFd_ < 0) {
        const std::string base = "/sys/class/net/" + interfaceName_ + "/statistics/";
        rxBytesFd_ = ::open((base + "rx_bytes").c_str(), O_RDONLY | O_CLOEXEC);
        txBytesFd_ = ::open((base + "tx_bytes").c_str(), O_RDONLY | O_CLOEXEC);
    }
    unsigned long long rx = 0;
    unsigned long long tx = 0;
    if (readCounter(rxBytesFd_, rx) && readCounter(txBytesFd_, tx)) {
        const double elapsed = now - lastTime_;
        if (lastTime_ >= 0 && elapsed > 0 && rx >= lastRxBytes_ && tx >= lastTxBytes_) {
            info.rxMbps = static_cast<double>(rx - lastRxBytes_) * 8.0 / 1e6 / elapsed;
            info.txMbps = static_cast<double>(tx - lastTxBytes_) * 8.0 / 1e6 / elapsed;
        }
        lastRxBytes_ = rx;
        lastTxBytes_ = tx;
        lastTime_ = now;
    } else {
        // インターフェースが作り直されたなどで読めないときは、次回開き直す
        if (rxBytesFd_ >= 0) ::close(rxBytesFd_);
        if (txBytesFd_ >= 0) ::close(txBytesFd_);
        rxBytesFd_ = -1;
        txBytesFd_ = -1;
        lastTime_ = -1.0;
    }
    return info;
}

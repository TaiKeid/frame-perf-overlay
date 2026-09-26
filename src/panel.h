// cairo でパネル画像を描く。
#pragma once

#include "config.h"
#include "history.h"
#include "sensors.h"

#include <cstdint>
#include <string>
#include <vector>

class FontSet;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/**
 * パネルに出す内容（最新値とグラフ用の履歴）。
 */
struct PanelState {
    SensorSample sensors;
    FrameStats frame;
    bool vrConnected = false;
    double now = 0.0;  ///< 最後に更新した時刻（秒）
    History appFps;
    History powerW;
    History cpuTempC;
    History gpuTempC;
    History gpuBusyPct;
    ControllerStatus controllers;  ///< 左右のコントローラーの電池（VR から 10 秒おきに入れる）

    /**
     * 最新値を入れ替え、グラフ用の履歴に追加する。
     * @param time 現在時刻（秒）
     * @param sample センサーの読み取り結果
     * @param frameStats フレーム時間の集計
     * @param connected SteamVR につながっているか
     * @param keepSeconds 履歴に残す秒数
     */
    void update(double time, const SensorSample& sample, const FrameStats& frameStats, bool connected,
                double keepSeconds);
};

/**
 * 性能パネルの画像を描く係。
 */
class PanelRenderer {
public:
    /**
     * @param fonts 使うフォント（このオブジェクトより長く生きていること）
     */
    explicit PanelRenderer(const FontSet& fonts);
    ~PanelRenderer();
    PanelRenderer(const PanelRenderer&) = delete;
    PanelRenderer& operator=(const PanelRenderer&) = delete;

    /**
     * パネルを描く。
     * @param state 表示する内容
     * @param config 設定（しきい値とグラフの秒数を使う）
     */
    void render(const PanelState& state, const Config& config);

    /**
     * 描いた画像を OpenVR 用の非乗算済み RGBA に変換して返す。
     * @return width() * height() * 4 バイト
     */
    const std::vector<uint8_t>& toRgba();

    /**
     * 描いた画像を PNG で保存する。
     * @param path 保存先
     * @return 保存できたら true
     */
    bool writePng(const std::string& path) const;

    /** @return 画像の幅（px） */
    int width() const;
    /** @return 画像の高さ（px） */
    int height() const;

private:
    const FontSet& fonts_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    cairo_surface_t* staticLayer_ = nullptr;  ///< 動かない部分（地・段のカード・グラフの台）
    std::vector<uint8_t> rgba_;

    /** 動かない部分を 1 回だけ描いておく。 */
    void buildStaticLayer();
};

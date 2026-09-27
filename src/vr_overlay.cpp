// OpenVR との接続とオーバーレイの実装。
#include "vr_overlay.h"

#include "openvr.h"
#include "placement.h"
#include <cmath>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace {

constexpr const char* kOverlayKey = "sasaken.frame-perf-overlay";
constexpr const char* kOverlayName = "Frame Perf Overlay";
constexpr const char* kDashboardKey = "sasaken.frame-perf-overlay.settings";
constexpr const char* kDashboardName = "Frame Perf";
constexpr float kDashboardWidthM = 2.8f;  // 1200px を 2.8m（1px あたりは前の 1024px / 2.4m とほぼ同じ）
constexpr uint32_t kMaxTimings = 256;
// 終了時、オーバーレイを消してから VR_Shutdown まで待つ時間（90Hz で約 36 フレーム）
constexpr int kShutdownWaitMs = 400;

/**
 * 単調増加の時計で今の時刻を秒で返す。
 * @return 秒
 */
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/**
 * オーバーレイのエラー名を返す。
 * @param error エラー
 * @return 名前（例: VROverlayError_None）
 */
const char* overlayErrorName(vr::EVROverlayError error) {
    return vr::VROverlay()->GetOverlayErrorNameFromEnum(error);
}

/**
 * オーバーレイのエラーを、成功以外なら標準エラーに出す。
 * @param what 何をしたときか
 * @param error エラー
 * @return 成功なら true
 */
bool checkOverlay(const char* what, vr::EVROverlayError error) {
    if (error == vr::VROverlayError_None) return true;
    std::fprintf(stderr, "[VR] %s に失敗: %s\n", what, overlayErrorName(error));
    return false;
}

/**
 * PID からプロセス名（/proc/<pid>/comm）を引く（診断の表示用）。
 * @param pid プロセス ID
 * @return 名前。分からなければ "?"
 */
std::string processName(uint32_t pid) {
    if (pid == 0) return "-";
    const std::string path = "/proc/" + std::to_string(pid) + "/comm";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "?";
    char name[64] = {};
    const ssize_t n = ::read(fd, name, sizeof(name) - 1);
    ::close(fd);
    std::string text(name, n > 0 ? static_cast<size_t>(n) : 0);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

/**
 * 終了処理の 1 手順の結果を、成功でも失敗でもログに出す（あとで順番と結果を確かめるため）。
 * @param what 何をしたか
 * @param error 戻り値
 */
void logShutdownStep(const char* what, vr::EVROverlayError error) {
    std::fprintf(stderr, "[VR] 終了処理 %s -> %s\n", what, overlayErrorName(error));
}

}  // namespace

VrOverlay::VrOverlay() : timingBuffer_(sizeof(vr::Compositor_FrameTiming) * kMaxTimings) {}

VrOverlay::~VrOverlay() {
    shutdown();
}

int VrOverlay::findVrserverPid() {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return -1;
    int found = -1;
    while (const dirent* entry = ::readdir(proc)) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') continue;
        const std::string commPath = std::string("/proc/") + name + "/comm";
        const int fd = ::open(commPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char comm[64] = {};
        const ssize_t n = ::read(fd, comm, sizeof(comm) - 1);
        ::close(fd);
        if (n > 0 && std::strncmp(comm, "vrserver\n", 9) == 0) {
            found = std::atoi(name);
            break;
        }
    }
    ::closedir(proc);
    return found;
}

VrOverlay::ConnectResult VrOverlay::connect(int panelWidth, int panelHeight, int settingsWidth, int settingsHeight,
                                            std::string& message) {
    if (connected_) return ConnectResult::Ok;

    // 1) Background 型で「SteamVR が動いているか」だけ確かめる（動いていなければ起動させない）
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return error == vr::VRInitError_Init_NoServerForBackgroundApp ? ConnectResult::NotRunning
                                                                        : ConnectResult::Error;
    }
    vr::VR_Shutdown();

    // 2) オーバーレイ型でつなぎ直す
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return ConnectResult::Error;
    }
    connected_ = true;

    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError overlayError = vr::VROverlay()->CreateOverlay(kOverlayKey, kOverlayName, &handle);
    if (overlayError != vr::VROverlayError_None) {
        message = std::string("CreateOverlay: ") + overlayErrorName(overlayError);
        shutdown();
        return ConnectResult::Error;
    }
    panelHandle_ = handle;
    attachedDevice_ = vr::k_unTrackedDeviceIndexInvalid;
    transformDirty_ = true;
    wristFade_ = WristFadeState{};
    presentation_ = PanelPresentationState{};
    placementRetryPending_ = false;

    // 3) Vulkan と性能パネルのテクスチャ
    std::string vkMessage;
    if (!vulkan_.init(vkMessage) || !panelTexture_.create(vulkan_, panelWidth, panelHeight, vkMessage)) {
        message = "Vulkan の準備に失敗: " + vkMessage;
        shutdown();
        return ConnectResult::Error;
    }

    // 4) ダッシュボードの設定パネル（失敗しても性能パネルは動かす）
    createDashboard(settingsWidth, settingsHeight);

    vrserverPid_ = findVrserverPid();
    lastFrameIndex_ = 0;
    haveFrameIndex_ = false;
    hzCheckedAt_ = -1.0;
    lastPanelError_.clear();
    lastSettingsError_.clear();
    return ConnectResult::Ok;
}

bool VrOverlay::connectReadOnly(std::string& message) {
    if (connected_) return true;
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return false;
    }
    if (vr::VRCompositor() == nullptr || vr::VRSystem() == nullptr) {
        message = "Background 型では VRCompositor / VRSystem が使えません";
        vr::VR_Shutdown();
        return false;
    }
    connected_ = true;
    readOnly_ = true;
    hzCheckedAt_ = -1.0;
    return true;
}

void VrOverlay::createDashboard(int width, int height) {
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError error = overlay->CreateDashboardOverlay(kDashboardKey, kDashboardName, &main, &thumbnail);
    std::fprintf(stderr, "[VR] CreateDashboardOverlay -> %s\n", overlayErrorName(error));
    if (error != vr::VROverlayError_None) return;
    dashboardHandle_ = main;
    thumbnailHandle_ = thumbnail;
    settingsHeight_ = height;

    checkOverlay("SetOverlayWidthInMeters(設定)", overlay->SetOverlayWidthInMeters(main, kDashboardWidthM));
    checkOverlay("SetOverlayInputMethod(設定)", overlay->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse));
    // マウス座標を画像の px にそろえる
    const vr::HmdVector2_t scale = {{static_cast<float>(width), static_cast<float>(height)}};
    checkOverlay("SetOverlayMouseScale(設定)", overlay->SetOverlayMouseScale(main, &scale));
    // ダッシュボードの下のアイコンにホバーしたとき「閉じる」を出す。SteamVR の dashboard は
    // このフラグがあるオーバーレイに閉じるボタンを出し、押されると VREvent_OverlayClosed を送ってくる。
    const vr::EVROverlayError closeError =
        overlay->SetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, true);
    // 立ったかどうかを vrserver から読み返して確かめる（ログに残す）
    bool closeEnabled = false;
    const vr::EVROverlayError readError =
        overlay->GetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, &closeEnabled);
    std::fprintf(stderr, "[VR] SetOverlayFlag(EnableControlBarClose) -> %s（読み返し: %s, %s）\n",
                 overlayErrorName(closeError), overlayErrorName(readError), closeEnabled ? "true" : "false");

    std::string message;
    if (!settingsTexture_.create(vulkan_, width, height, message)) {
        std::fprintf(stderr, "[Vulkan] 設定パネルのテクスチャを作れません: %s\n", message.c_str());
    }
}

void VrOverlay::shutdown() {
    if (!connected_) return;
    if (readOnly_) {
        // オーバーレイも Vulkan も作っていないので、閉じるだけ
        vr::VR_Shutdown();
        connected_ = false;
        readOnly_ = false;
        return;
    }
    vr::IVROverlay* overlay = vr::VROverlay();
    std::fprintf(stderr, "[VR] 終了処理を始めます\n");

    // 1) 性能パネルを隠す
    if (panelHandle_ != 0) logShutdownStep("HideOverlay(パネル)", overlay->HideOverlay(panelHandle_));
    // 2) テクスチャを外す（コンポジタがこちらの画像を参照しないようにする）
    if (panelHandle_ != 0) logShutdownStep("ClearOverlayTexture(パネル)", overlay->ClearOverlayTexture(panelHandle_));
    if (dashboardHandle_ != 0) {
        logShutdownStep("ClearOverlayTexture(設定)", overlay->ClearOverlayTexture(dashboardHandle_));
        logShutdownStep("ClearOverlayTexture(サムネイル)", overlay->ClearOverlayTexture(thumbnailHandle_));
    }
    // 3) オーバーレイを消す（サムネイルは設定パネルと一緒に消える）
    if (panelHandle_ != 0) logShutdownStep("DestroyOverlay(パネル)", overlay->DestroyOverlay(panelHandle_));
    if (dashboardHandle_ != 0) logShutdownStep("DestroyOverlay(設定)", overlay->DestroyOverlay(dashboardHandle_));
    panelHandle_ = 0;
    dashboardHandle_ = 0;
    thumbnailHandle_ = 0;

    // 4) コンポジタが数フレーム回って、外したテクスチャを手放すのを待つ
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownWaitMs));
    std::fprintf(stderr, "[VR] 終了処理 %dms 待ちました\n", kShutdownWaitMs);

    // 5) OpenVR を閉じる（Vulkan の画像を壊すのはこの後、という OpenVR の決まり）
    vr::VR_Shutdown();
    connected_ = false;
    std::fprintf(stderr, "[VR] 終了処理 VR_Shutdown 済み\n");

    // 6) Vulkan の画像とデバイスを壊す
    thumbnailTexture_.destroy();
    settingsTexture_.destroy();
    panelTexture_.destroy();
    vulkan_.destroy();
    std::fprintf(stderr, "[VR] 終了処理 Vulkan を片付けました\n");
}

void VrOverlay::applyConfig(const Config& config) {
    if (!connected_ || panelHandle_ == 0) return;
    checkOverlay("SetOverlayWidthInMeters", vr::VROverlay()->SetOverlayWidthInMeters(panelHandle_, config.widthM));
    transformDirty_ = true;
    updatePlacement(config, nowSeconds());
}

void VrOverlay::updatePlacement(const Config& config, double now) {
    if (!connected_ || panelHandle_ == 0) return;
    auto* overlay = vr::VROverlay();
    const bool wrist = config.attachment != Attachment::Head;
    auto device = vr::k_unTrackedDeviceIndex_Hmd;
    bool tracked = true;
    double target = 1.0;
    vr::HmdMatrix34_t transform{};
    if (wrist) {
        device = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(
            config.attachment == Attachment::LeftWrist ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        transform = wristTransform(selectedWrist(config));
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
        const auto& head = poses[vr::k_unTrackedDeviceIndex_Hmd];
        tracked = wristTrackingValid(device, poses);
        if (tracked && config.wristFade) {
            target = wristFacingAlpha(composeTransform(poses[device].mDeviceToAbsoluteTracking, transform),
                                      head.mDeviceToAbsoluteTracking, config.wristFadeEndDeg);
        }
    } else {
        transform.m[0][0] = transform.m[1][1] = transform.m[2][2] = 1;
        transform.m[0][3] = config.posX; transform.m[1][3] = config.posY; transform.m[2][3] = config.posZ;
    }
    const bool changedDevice = attachedDevice_ != device;
    placementRetryPending_ = false;
    if (tracked && config.visible) {
        if (transformDirty_ || changedDevice) {
            const auto error = overlay->SetOverlayTransformTrackedDeviceRelative(panelHandle_, device, &transform);
            checkOverlay("SetOverlayTransformTrackedDeviceRelative", error);
            if (error != vr::VROverlayError_None) {
                tracked = false;
                placementRetryPending_ = true;
            }
            else { attachedDevice_ = device; transformDirty_ = false; }
        }
    }
    // Tracking loss hides immediately: never leave a stale wrist panel floating in space.
    const double alpha = config.alpha * wristFade_.update(wrist, tracked && config.visible, device, target, now);
    const bool retryPresentation = presentation_.apply(alpha,
        [&](double value) {
            return checkOverlay("SetOverlayAlpha", overlay->SetOverlayAlpha(panelHandle_, static_cast<float>(value)));
        },
        [&](bool show) {
            return checkOverlay(show ? "ShowOverlay" : "HideOverlay",
                                show ? overlay->ShowOverlay(panelHandle_) : overlay->HideOverlay(panelHandle_));
        });
    placementRetryPending_ |= retryPresentation;
}

VrEvents VrOverlay::pollEvents(bool includeSystem) {
    VrEvents result;
    if (!connected_) return result;
    vr::VREvent_t event {};
    if (includeSystem) {
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) result.quit = true;
        }
        while (vr::VROverlay()->PollNextOverlayEvent(panelHandle_, &event, sizeof(event))) {
            if (event.eventType == vr::VREvent_ImageFailed) std::fprintf(stderr, "[VR] 画像の読み込みに失敗しました\n");
        }
    }
    if (dashboardHandle_ != 0) {
        while (vr::VROverlay()->PollNextOverlayEvent(dashboardHandle_, &event, sizeof(event))) {
            // マウス座標は左下が原点なので、上が原点になるよう反転する
            const double x = event.data.mouse.x;
            const double y = settingsHeight_ - event.data.mouse.y;
            switch (event.eventType) {
                case vr::VREvent_MouseMove: result.pointer.push_back({PointerInput::Type::Move, x, y}); break;
                case vr::VREvent_MouseButtonDown:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        result.pointer.push_back({PointerInput::Type::Down, x, y});
                    }
                    break;
                case vr::VREvent_MouseButtonUp:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        result.pointer.push_back({PointerInput::Type::Up, x, y});
                    }
                    break;
                case vr::VREvent_FocusLeave: result.pointer.push_back({PointerInput::Type::Leave, 0, 0}); break;
                // ダッシュボードのアイコンにホバーしたときの「閉じる」（VROverlayFlags_EnableControlBarClose）。
                // フラグを立てたこのオーバーレイにだけ VREvent_OverlayClosed が届く。
                // SteamVR 自体の終了（VRSystem 側の VREvent_Quit）とは別のイベント
                case vr::VREvent_OverlayClosed:
                    std::fprintf(stderr, "[VR] ダッシュボードの「閉じる」が押されました\n");
                    result.closeRequested = true;
                    break;
                default: break;
            }
        }
    }
    if (result.quit) {
        std::fprintf(stderr, "[VR] SteamVR から終了の知らせが来ました\n");
        vr::VRSystem()->AcknowledgeQuit_Exiting();
    }
    return result;
}

bool VrOverlay::steamVrAlive() const {
    if (vrserverPid_ <= 0) return true;  // 確かめられないときは生きている扱い
    return ::kill(vrserverPid_, 0) == 0 || errno == EPERM;
}

bool VrOverlay::dashboardOpen() const {
    return connected_ && !readOnly_ && vr::VROverlay()->IsDashboardVisible();
}

SettingsVisibility VrOverlay::settingsVisibility() const {
    SettingsVisibility result;
    if (!connected_ || readOnly_ || dashboardHandle_ == 0) return result;
    vr::IVROverlay* overlay = vr::VROverlay();
    result.dashboardVisible = overlay->IsDashboardVisible();
    // ダッシュボードが閉じていれば残りは聞かない（IPC を減らす）
    if (!result.dashboardVisible) return result;
    result.activeTab = overlay->IsActiveDashboardOverlay(dashboardHandle_);
    result.overlayVisible = overlay->IsOverlayVisible(dashboardHandle_);
    return result;
}

bool VrOverlay::submitThumbnail(const uint8_t* rgba, int size) {
    if (!connected_ || thumbnailHandle_ == 0) return false;
    std::string message;
    if (!thumbnailTexture_.ready() && !thumbnailTexture_.create(vulkan_, size, size, message)) {
        std::fprintf(stderr, "[Vulkan] サムネイルのテクスチャを作れません: %s\n", message.c_str());
        return false;
    }
    if (!thumbnailTexture_.update(thumbnailHandle_, rgba, message)) {
        std::fprintf(stderr, "[VR] サムネイルを送れません: %s\n", message.c_str());
        return false;
    }
    return true;
}

double VrOverlay::displayHz(double now) {
    if (hzCheckedAt_ < 0 || now - hzCheckedAt_ > 5.0) {
        hzCheckedAt_ = now;
        vr::ETrackedPropertyError error = vr::TrackedProp_Success;
        const float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                                        vr::Prop_DisplayFrequency_Float, &error);
        displayHz_ = (error == vr::TrackedProp_Success && hz > 1.0f) ? hz : 0.0;
    }
    return displayHz_;
}

namespace {

/** 直近 1 秒ぶんのフレームタイミングから数えた値。 */
struct WindowStats {
    double fps = std::numeric_limits<double>::quiet_NaN();  ///< アプリの新しく表示されたフレーム（毎秒）
    double reprojectedPct = 0.0;  ///< 表示回数のうち、同じフレームの 2 回目以降（再投影で埋めたコマ）の %
    uint32_t dropped = 0;         ///< m_nNumDroppedFrames の合計
    int throttledFrames = 0;      ///< VRCompositor_ThrottleMask の段数（最新の確定したフレーム）
};

/**
 * 直近 1 秒ぶんのフレームタイミングを数える。1 件 = アプリの新しいフレーム 1 枚（m_nFrameIndex は重ならず、
 * 同じフレームを何回表示したかは m_nNumFramePresents に入る）なので:
 * - fps: 1 回以上表示されたフレームの数を、その時刻（m_flSystemTimeInSeconds、vsync にそろった時刻）の幅で割る
 * - 再投影: （表示回数の合計 − 新しいフレームの数）÷ 表示回数の合計。2 回ずつ表示なら 50%
 * いちばん新しい 1 件はまだ表示回数が決まっていないことがあるので数えない。
 * @param timings 古い順のフレームタイミング
 * @param count 件数
 * @return 数えた値（数えられなければ fps は NaN）
 */
WindowStats measureWindow(const vr::Compositor_FrameTiming* timings, uint32_t count) {
    WindowStats result;
    if (count < 3) return result;
    const uint32_t last = count - 2;  // 最新の 1 件は除く
    const double end = timings[last].m_flSystemTimeInSeconds;
    int shown = 0;
    uint32_t presents = 0;
    double first = end;
    for (int i = static_cast<int>(last); i >= 0; --i) {
        const vr::Compositor_FrameTiming& t = timings[i];
        if (t.m_flSystemTimeInSeconds <= end - 1.0) break;
        result.dropped += t.m_nNumDroppedFrames;
        if (t.m_nNumFramePresents == 0) continue;  // 一度も表示されなかった（目に届いていない）
        ++shown;
        presents += t.m_nNumFramePresents;
        first = t.m_flSystemTimeInSeconds;
    }
    // 1 秒に 2 枚未満（読み込み中など）は、数えた枚数をそのまま毎秒の値にする
    result.fps = (shown < 2 || end <= first) ? shown : (shown - 1) / (end - first);
    result.reprojectedPct = presents > 0 ? 100.0 * (presents - shown) / presents : 0.0;
    result.throttledFrames = static_cast<int>(VR_COMPOSITOR_NUMBER_OF_THROTTLED_FRAMES(timings[last]));
    return result;
}

}  // namespace

FrameStats VrOverlay::readFrameStats() {
    FrameStats stats;
    if (!connected_) return stats;

    stats.displayHz = displayHz(nowSeconds());
    stats.targetMs = stats.displayHz > 0 ? 1000.0 / stats.displayHz : 0.0;

    // 直近のフレームを古い順にもらい、前回より新しいものだけ集計する
    auto* timings = reinterpret_cast<vr::Compositor_FrameTiming*>(timingBuffer_.data());
    std::memset(timings, 0, timingBuffer_.size());
    timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
    const uint32_t count = vr::VRCompositor()->GetFrameTimings(timings, kMaxTimings);

    double gpuSum = 0.0;
    double cpuSum = 0.0;
    uint32_t newest = lastFrameIndex_;
    for (uint32_t i = 0; i < count; ++i) {
        const vr::Compositor_FrameTiming& t = timings[i];
        if (lastFrameIndex_ != 0 && t.m_nFrameIndex <= lastFrameIndex_) continue;
        const double gpu = t.m_flTotalRenderGpuMs;
        // アプリの CPU 時間（姿勢を受け取ってから 2 回目の Submit まで）+ コンポジタの CPU 時間
        const double appCpu = std::max(0.0, static_cast<double>(t.m_flNewFrameReadyMs - t.m_flNewPosesReadyMs));
        const double cpu = appCpu + t.m_flCompositorRenderCpuMs;
        gpuSum += gpu;
        cpuSum += cpu;
        stats.gpuMaxMs = std::max(stats.gpuMaxMs, gpu);
        stats.cpuMaxMs = std::max(stats.cpuMaxMs, cpu);
        ++stats.frames;
        newest = std::max(newest, t.m_nFrameIndex);
    }
    const bool firstRead = !haveFrameIndex_;
    lastFrameIndex_ = newest;
    haveFrameIndex_ = count > 0;
    stats.valid = count > 0;
    if (stats.frames > 0) {
        stats.gpuMs = gpuSum / stats.frames;
        stats.cpuMs = cpuSum / stats.frames;
        stats.haveTimes = true;
    }
    if (count > 0) {
        // fps・再投影・落ち・抑えている段数は、同じ直近 1 秒のフレームタイミングから出す
        // （以前の累積統計 GetCumulativeStats の差分はやめた。数える元をそろえるため）
        const WindowStats window = measureWindow(timings, count);
        // 前回から新しいフレームが 1 つも来ていなければ、アプリは止まっている（0 fps）
        stats.appFps = (!firstRead && stats.frames == 0) ? 0.0 : window.fps;
        stats.reprojectedPct = window.reprojectedPct;
        stats.dropped = window.dropped;
        stats.throttledFrames = window.throttledFrames;
    }
    return stats;
}

namespace {

/**
 * 役割（左手・右手）のコントローラーの電池を読む。
 * @param role 役割
 * @return 電池の状態
 */
ControllerBattery readControllerBattery(vr::ETrackedControllerRole role) {
    ControllerBattery battery;
    vr::IVRSystem* system = vr::VRSystem();
    const vr::TrackedDeviceIndex_t index = system->GetTrackedDeviceIndexForControllerRole(role);
    if (index == vr::k_unTrackedDeviceIndexInvalid || !system->IsTrackedDeviceConnected(index)) return battery;
    battery.present = true;
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    const float fraction = system->GetFloatTrackedDeviceProperty(index, vr::Prop_DeviceBatteryPercentage_Float, &error);
    if (error == vr::TrackedProp_Success) battery.pct = std::clamp(fraction * 100.0, 0.0, 100.0);  // 0〜1 で返る
    error = vr::TrackedProp_Success;
    const bool charging = system->GetBoolTrackedDeviceProperty(index, vr::Prop_DeviceIsCharging_Bool, &error);
    battery.charging = error == vr::TrackedProp_Success && charging;
    return battery;
}

}  // namespace

ControllerStatus VrOverlay::readControllers() {
    ControllerStatus status;
    if (!connected_) return status;
    status.left = readControllerBattery(vr::TrackedControllerRole_LeftHand);
    status.right = readControllerBattery(vr::TrackedControllerRole_RightHand);
    return status;
}

void VrOverlay::dumpFrameTimings(double seconds, const volatile int& stop) {
    if (!connected_) return;
    auto* timings = reinterpret_cast<vr::Compositor_FrameTiming*>(timingBuffer_.data());
    uint32_t last = 0;
    const double start = nowSeconds();
    std::printf("# t(s) index presents mispresented dropped reprojFlags systemTime(s) newFrameReady(ms) gpu(ms)\n");
    while (!stop && nowSeconds() - start < seconds) {
        std::memset(timings, 0, timingBuffer_.size());
        timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
        const uint32_t count = vr::VRCompositor()->GetFrameTimings(timings, kMaxTimings);
        const uint32_t focus = vr::VRCompositor()->GetCurrentSceneFocusProcess();
        const uint32_t renderer = vr::VRCompositor()->GetLastFrameRenderer();
        const WindowStats window = measureWindow(timings, count);
        std::printf("# t=%.2f 返ってきた件数=%u 表示 %.1fHz アプリ %.1ffps 再投影 %.1f%% 落ち %u 抑え %d 段 "
                    "シーンのプロセス=%u(%s) 最後に描いたプロセス=%u(%s)\n",
                    nowSeconds() - start, count, displayHz(nowSeconds()), window.fps, window.reprojectedPct,
                    window.dropped, window.throttledFrames, focus, processName(focus).c_str(), renderer,
                    processName(renderer).c_str());
        for (uint32_t i = 0; i < count; ++i) {
            const vr::Compositor_FrameTiming& t = timings[i];
            if (last != 0 && t.m_nFrameIndex <= last) continue;
            if (last == 0 && i + 12 < count) continue;  // 初回は最後の 12 件だけ
            std::printf("%.2f %u %u %u %u 0x%x %.4f %.2f %.2f\n", nowSeconds() - start, t.m_nFrameIndex,
                        t.m_nNumFramePresents, t.m_nNumMisPresented, t.m_nNumDroppedFrames, t.m_nReprojectionFlags,
                        t.m_flSystemTimeInSeconds, t.m_flNewFrameReadyMs, t.m_flTotalRenderGpuMs);
        }
        if (count > 0) last = std::max(last, timings[count - 1].m_nFrameIndex);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool VrOverlay::submitPanel(const uint8_t* rgba) {
    if (!connected_ || panelHandle_ == 0) return false;
    std::string message;
    const bool ok = panelTexture_.update(panelHandle_, rgba, message);
    // 同じエラーを毎回出さない
    if (message != lastPanelError_) {
        if (!ok) std::fprintf(stderr, "[VR] パネルを送れません: %s\n", message.c_str());
        lastPanelError_ = message;
    }
    return ok;
}

bool VrOverlay::submitSettings(const uint8_t* rgba) {
    if (!connected_ || dashboardHandle_ == 0 || !settingsTexture_.ready()) return false;
    std::string message;
    const bool ok = settingsTexture_.update(dashboardHandle_, rgba, message);
    if (message != lastSettingsError_) {
        if (!ok) std::fprintf(stderr, "[VR] 設定パネルを送れません: %s\n", message.c_str());
        lastSettingsError_ = message;
    }
    return ok;
}

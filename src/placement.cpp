#include "placement.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr double kRadians = 3.14159265358979323846 / 180.0;
}

vr::HmdMatrix34_t wristTransform(const WristPose& pose) {
    const double x = pose.pitch * kRadians, y = pose.yaw * kRadians, z = pose.roll * kRadians;
    const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y);
    const double cz = std::cos(z), sz = std::sin(z);
    // Right-handed controller-local rotations, applied X (pitch), Y (yaw), Z (roll).
    vr::HmdMatrix34_t m{};
    m.m[0][0] = cz * cy; m.m[0][1] = cz * sy * sx - sz * cx; m.m[0][2] = cz * sy * cx + sz * sx;
    m.m[1][0] = sz * cy; m.m[1][1] = sz * sy * sx + cz * cx; m.m[1][2] = sz * sy * cx - cz * sx;
    m.m[2][0] = -sy;     m.m[2][1] = cy * sx;                m.m[2][2] = cy * cx;
    m.m[0][3] = pose.x; m.m[1][3] = pose.y; m.m[2][3] = pose.z;
    return m;
}

vr::HmdMatrix34_t composeTransform(const vr::HmdMatrix34_t& parent, const vr::HmdMatrix34_t& local) {
    vr::HmdMatrix34_t out{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            for (int k = 0; k < 3; ++k) out.m[row][col] += parent.m[row][k] * local.m[k][col];
        }
        out.m[row][3] += parent.m[row][3];
    }
    return out;
}

double wristFacingAlpha(const vr::HmdMatrix34_t& panel, const vr::HmdMatrix34_t& head, double fadeEndDeg) {
    double toHead[3], length = 0;
    for (int i = 0; i < 3; ++i) {
        toHead[i] = head.m[i][3] - panel.m[i][3];
        length += toHead[i] * toHead[i];
    }
    if (!std::isfinite(length) || length < 1e-8) return 0;
    length = std::sqrt(length);
    double facing = 0;
    for (int i = 0; i < 3; ++i) facing += panel.m[i][2] * toHead[i] / length;
    if (!std::isfinite(facing)) return 0;
    const double angle = std::acos(std::clamp(facing, -1.0, 1.0)) / kRadians;
    const double end = std::clamp(fadeEndDeg, 35.0, 90.0);
    const double t = std::clamp((end - angle) / 30.0, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double smoothWristAlpha(double current, double target, double elapsed) {
    // 80 ms time constant prevents abrupt transitions across the angular fade band.
    return target + (current - target) * std::exp(-std::max(0.0, elapsed) / 0.08);
}

bool wristTrackingValid(uint32_t device, const vr::TrackedDevicePose_t (&poses)[vr::k_unMaxTrackedDeviceCount]) {
    const auto& head = poses[vr::k_unTrackedDeviceIndex_Hmd];
    return device < vr::k_unMaxTrackedDeviceCount && head.bDeviceIsConnected && head.bPoseIsValid &&
           poses[device].bDeviceIsConnected && poses[device].bPoseIsValid;
}

double WristFadeState::update(bool wrist, bool active, uint32_t device, double target, double now) {
    const bool start = !active_ || device != device_ || lastTime_ < 0;
    const double elapsed = start ? 0.0 : std::max(0.0, now - lastTime_);
    lastTime_ = now;
    device_ = device;
    active_ = active;
    if (!active || start) alpha_ = 0.0;
    if (active) alpha_ = wrist ? smoothWristAlpha(alpha_, target, elapsed) : 1.0;
    return alpha_;
}

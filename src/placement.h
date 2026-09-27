#pragma once
#include "config.h"
#include "openvr.h"
#include <cmath>

// Pure geometry helpers: controller-local pose, composed world pose, and facing fade.
vr::HmdMatrix34_t wristTransform(const WristPose& pose);
vr::HmdMatrix34_t composeTransform(const vr::HmdMatrix34_t& parent, const vr::HmdMatrix34_t& local);
double wristFacingAlpha(const vr::HmdMatrix34_t& panel, const vr::HmdMatrix34_t& head, double fadeEndDeg);
double smoothWristAlpha(double current, double target, double elapsed);

bool wristTrackingValid(uint32_t device, const vr::TrackedDevicePose_t (&poses)[vr::k_unMaxTrackedDeviceCount]);

// Animation state is separate from the last successfully applied OpenVR state.
class WristFadeState {
public:
    double update(bool wrist, bool active, uint32_t device, double target, double now);

private:
    double alpha_ = 0.0;
    double lastTime_ = -1.0;
    uint32_t device_ = vr::k_unTrackedDeviceIndexInvalid;
    bool active_ = false;
};

class PanelPresentationState {
public:
    // Callbacks return true only when OpenVR accepted the operation.
    // Return true while an operation needs retrying, including in head mode.
    template<class SetAlpha, class SetVisible>
    bool apply(double alpha, SetAlpha setAlpha, SetVisible setVisible) {
        bool retry = false;
        if (std::abs(alpha - appliedAlpha_) > 0.001 || (alpha == 0.0 && appliedAlpha_ != 0.0)) {
            if (setAlpha(alpha)) appliedAlpha_ = alpha;
            else retry = true;
        }
        const bool show = alpha > 0.001;
        // Don't reveal a newly attached panel at stale opacity if SetOverlayAlpha failed.
        // Hiding must still be attempted even if the alpha update failed.
        if (show != shown_ && (!show || !retry)) {
            if (setVisible(show)) shown_ = show;
            else retry = true;
        }
        return retry;
    }

private:
    double appliedAlpha_ = -1.0;
    bool shown_ = false;
};

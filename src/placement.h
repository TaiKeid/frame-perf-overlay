#pragma once
#include "config.h"
#include "openvr.h"

// Pure geometry helpers: controller-local pose, composed world pose, and facing fade.
vr::HmdMatrix34_t wristTransform(const WristPose& pose);
vr::HmdMatrix34_t composeTransform(const vr::HmdMatrix34_t& parent, const vr::HmdMatrix34_t& local);
double wristFacingAlpha(const vr::HmdMatrix34_t& panel, const vr::HmdMatrix34_t& head, double fadeEndDeg);
double smoothWristAlpha(double current, double target, double elapsed);

bool wristTrackingValid(uint32_t device, const vr::TrackedDevicePose_t (&poses)[vr::k_unMaxTrackedDeviceCount]);

#include "clock.h"
#include "config.h"
#include "draw.h"
#include "placement.h"
#include "settings_panel.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
void near(double actual, double expected, double epsilon = 1e-5) { CHECK(std::abs(actual - expected) < epsilon); }

void geometry() {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    CHECK(!wristTrackingValid(vr::k_unTrackedDeviceIndexInvalid, poses));
    CHECK(!wristTrackingValid(1, poses));
    poses[0].bDeviceIsConnected = poses[0].bPoseIsValid = true;
    poses[1].bDeviceIsConnected = poses[1].bPoseIsValid = true;
    CHECK(wristTrackingValid(1, poses));
    poses[1].bPoseIsValid = false; CHECK(!wristTrackingValid(1, poses));
    poses[1].bPoseIsValid = true; poses[0].bDeviceIsConnected = false; CHECK(!wristTrackingValid(1, poses));
    WristPose p; p.x = p.y = p.z = p.pitch = 0;
    auto head = wristTransform(p);
    p.z = -0.5;
    auto panel = wristTransform(p);
    near(wristFacingAlpha(panel, head, 75), 1);
    p.yaw = 60; panel = wristTransform(p);
    near(wristFacingAlpha(panel, head, 75), 0.5);
    p.yaw = 90; near(wristFacingAlpha(wristTransform(p), head, 75), 0);
    p.yaw = 180; near(wristFacingAlpha(wristTransform(p), head, 75), 0);
    p.yaw = 45; near(wristFacingAlpha(wristTransform(p), head, 75), 1);
    near(wristFacingAlpha(head, head, 75), 0); // Degenerate distance.
    WristPose world; world.x = 2; world.y = 3; world.z = 4; world.pitch = 17; world.yaw = 41; world.roll = -22;
    const auto basis = wristTransform(world);
    near(wristFacingAlpha(composeTransform(basis, panel), composeTransform(basis, head), 75), 0.5);
    // Default -90 pitch makes the panel's +Z normal point up on the controller.
    const auto wrist = wristTransform(WristPose{});
    near(wrist.m[0][2], 0); near(wrist.m[1][2], 1); near(wrist.m[2][2], 0);
    // Temporal smoothing is monotonic and independent of polling frequency.
    double alpha = 0;
    for (int i = 0; i < 60; ++i) {
        const double next = smoothWristAlpha(alpha, 1, 1.0 / 60);
        CHECK(next >= alpha && next <= 1); alpha = next;
    }
    near(alpha, smoothWristAlpha(0, 1, 1));
    CHECK(smoothWristAlpha(1, 0, 0.2) < 0.1);
}

void placementRecovery() {
    WristFadeState fade;
    near(fade.update(false, true, 0, 1, 1), 1); // Head mode.
    near(fade.update(true, true, 1, 1, 61), 0); // No 60-second jump on attachment change.
    const double firstStep = fade.update(true, true, 1, 1, 61 + 1.0 / 60);
    CHECK(firstStep > 0 && firstStep < 0.25);
    near(fade.update(true, true, 2, 1, 100), 0); // Switch wrists after a pause.
    CHECK(fade.update(true, true, 2, 1, 100.1) > 0);
    near(fade.update(true, false, 2, 1, 101), 0); // Tracking lost / Show off.
    near(fade.update(true, true, 2, 1, 150), 0); // Recovery fades from zero too.
    CHECK(fade.update(true, true, 2, 1, 150.1) > 0);
    near(fade.update(false, true, 0, 1, 151), 1); // Head stays immediate.

    struct FakeOverlay {
        double alpha = 0;
        bool shown = false;
        int failAlpha = 0, failShow = 0, failHide = 0;
        int alphaCalls = 0, showCalls = 0, hideCalls = 0;
        bool setAlpha(double value) {
            ++alphaCalls;
            if (failAlpha > 0) { --failAlpha; return false; }
            alpha = value; return true;
        }
        bool setVisible(bool show) {
            int& failures = show ? failShow : failHide;
            if (show) ++showCalls; else ++hideCalls;
            if (failures > 0) { --failures; return false; }
            shown = show; return true;
        }
    } backend;
    PanelPresentationState state;
    auto apply = [&](double alpha) {
        return state.apply(alpha, [&](double value) { return backend.setAlpha(value); },
                           [&](bool show) { return backend.setVisible(show); });
    };
    backend.failShow = 1;
    CHECK(apply(0.9)); CHECK(!backend.shown);
    CHECK(!apply(0.9)); CHECK(backend.shown && backend.showCalls == 2);
    for (int i = 0; i < 120; ++i) CHECK(!apply(0.9));
    CHECK(backend.showCalls == 2 && backend.alphaCalls == 1); // No redundant successful writes.

    backend.failHide = 1;
    CHECK(apply(0)); CHECK(backend.shown); near(backend.alpha, 0);
    CHECK(!apply(0)); CHECK(!backend.shown && backend.hideCalls == 2);

    backend.failAlpha = 1;
    CHECK(apply(0.9)); CHECK(!backend.shown); // Never show at stale alpha after a failed write.
    CHECK(!apply(0.9)); CHECK(backend.shown); near(backend.alpha, 0.9);

    backend.failAlpha = 1;
    CHECK(apply(0)); CHECK(!backend.shown); // Still attempt hiding when alpha zero failed.
    CHECK(!apply(0)); near(backend.alpha, 0);

    backend.failShow = 100;
    for (int i = 0; i < 100; ++i) CHECK(apply(0.9)); // Failure doesn't poison the cache.
    CHECK(!backend.shown);
    CHECK(!apply(0.9)); CHECK(backend.shown);
    backend.failAlpha = 1;
    CHECK(apply(0.4)); near(backend.alpha, 0.9);
    CHECK(!apply(0.9)); // A changed target can match the last successfully applied value.
    near(backend.alpha, 0.9);
}

void clockCases() {
    std::tm t{};
    CHECK(formatClock(t, 12) == "12:00 AM"); CHECK(formatClock(t, 24) == "00:00");
    t.tm_hour = 12; t.tm_min = 5;
    CHECK(formatClock(t, 12) == "12:05 PM");
    t.tm_hour = 23; t.tm_min = 59;
    CHECK(formatClock(t, 12) == "11:59 PM"); CHECK(formatClock(t, 24) == "23:59");
    CHECK(formatClock(t, 0).empty());
}

void configuration(const std::string& file) {
    Config c; std::vector<std::string> warnings; std::string error;
    { std::ofstream out(file); out << R"({"visible":false,"position":{"x":0.2,"y":-0.1,"z":-0.7}})"; }
    CHECK(loadConfig(file, c, warnings, error));
    CHECK(c.attachment == Attachment::Head && !c.visible); near(c.posX, 0.2); CHECK(c.clockFormat == 24);
    CHECK(applySettingsAction(SettingsAction::AttachLeft, c));
    CHECK(applySettingsAction(SettingsAction::OffsetXUp, c));
    CHECK(applySettingsAction(SettingsAction::WristPitchDown, c));
    const WristPose left = c.leftWrist;
    CHECK(applySettingsAction(SettingsAction::AttachRight, c));
    CHECK(applySettingsAction(SettingsAction::OffsetYDown, c));
    CHECK(applySettingsAction(SettingsAction::YawUp, c));
    CHECK(!(c.leftWrist != left)); near(c.posX, 0.2);
    // Head rotation and wrist rotation must remain independent after the merge.
    const WristPose right = c.rightWrist;
    CHECK(applySettingsAction(SettingsAction::PitchUp, c, 5));
    CHECK(applySettingsAction(SettingsAction::YawRight, c, 5));
    CHECK(applySettingsAction(SettingsAction::RollLeft, c));
    near(c.pitchDeg, 5); near(c.yawDeg, 5); near(c.rollDeg, 1);
    CHECK(!(c.rightWrist != right));
    CHECK(applySettingsAction(SettingsAction::WristPitchUp, c));
    near(c.rightWrist.pitch, right.pitch + 5); near(c.pitchDeg, 5);
    c.updateCheck = false;
    c.clockFormat = 12; c.wristFade = false; c.wristFadeEndDeg = 80;
    CHECK(saveConfig(file, c, error));
    Config roundTrip; warnings.clear(); CHECK(loadConfig(file, roundTrip, warnings, error)); CHECK(warnings.empty());
    CHECK(roundTrip.attachment == Attachment::RightWrist && roundTrip.clockFormat == 12);
    CHECK(!(roundTrip.leftWrist != c.leftWrist) && !(roundTrip.rightWrist != c.rightWrist));
    CHECK(!roundTrip.wristFade); near(roundTrip.wristFadeEndDeg, 80);
    CHECK(!roundTrip.updateCheck);
    near(roundTrip.pitchDeg, 5); near(roundTrip.yawDeg, 5); near(roundTrip.rollDeg, 1);
    near(roundTrip.posZ, -0.7);
    { std::ofstream out(file); out << R"({"attachment":"bad","clock_format":12.5,"left_wrist":{"x":9,"pitch":-300},"wrist_fade_end_deg":5})"; }
    warnings.clear(); CHECK(loadConfig(file, c, warnings, error)); CHECK(!warnings.empty());
    CHECK(c.attachment == Attachment::Head && c.clockFormat == 24);
    near(c.leftWrist.x, 0.5); near(c.leftWrist.pitch, -180); near(c.wristFadeEndDeg, 35);
    CHECK(!applySettingsAction(SettingsAction::OffsetXDown, c)); // Head mode doesn't edit a wrist.
    applySettingsAction(SettingsAction::AttachLeft, c);
    applySettingsAction(SettingsAction::OffsetXUp, c); near(c.leftWrist.x, 0.5);
    c.leftWrist.roll = 180; applySettingsAction(SettingsAction::RollUp, c); near(c.leftWrist.roll, -175);
    { std::ofstream out(file); out << "{invalid"; }
    const WristPose before = c.leftWrist;
    CHECK(!loadConfig(file, c, warnings, error)); CHECK(!(before != c.leftWrist));
    resetDisplaySettings(c); CHECK(c.attachment == Attachment::Head && c.clockFormat == 24);
    near(c.leftWrist.pitch, -90);
    near(c.pitchDeg, 0); near(c.yawDeg, 0); near(c.rollDeg, 0);
}

void controls(const std::string& directory) {
    Config c; c.language = Language::En;
    FontSet fonts; fonts.load(c.fontPath, c.boldFontPath);
    SettingsPanel panel(fonts); AutostartStatus service{AutostartStatus::State::Enabled, false, false};
    panel.render(c, service, {});
    CHECK(panel.pointerDown(1320, 220, 0) == SettingsAction::PitchUp);
    CHECK(panel.pointerDown(1320, 300, 0) == SettingsAction::PitchDown);
    CHECK(panel.pointerDown(1260, 395, 0) == SettingsAction::None);
    near(panel.angleStepDeg(), 1);
    CHECK(panel.pointerDown(1480, 395, 0) == SettingsAction::None);
    near(panel.angleStepDeg(), 5);
    CHECK(panel.pointerDown(1100, 600, 0) == SettingsAction::None); // Change page internally.
    panel.render(c, service, {});
    CHECK(panel.pointerDown(240, 325, 1) == SettingsAction::None); // Disabled in head mode.
    CHECK(panel.pointerDown(260, 220, 2) == SettingsAction::AttachLeft);
    applySettingsAction(SettingsAction::AttachLeft, c); panel.render(c, service, {});
    const struct {double x, y; SettingsAction action;} buttons[] = {
        {80,220,SettingsAction::AttachHead},{260,220,SettingsAction::AttachLeft},{430,220,SettingsAction::AttachRight},
        {240,320,SettingsAction::OffsetXDown},{440,320,SettingsAction::OffsetXUp},
        {240,400,SettingsAction::OffsetYDown},{440,400,SettingsAction::OffsetYUp},
        {240,480,SettingsAction::OffsetZDown},{440,480,SettingsAction::OffsetZUp},
        {650,345,SettingsAction::WristPitchDown},{745,345,SettingsAction::WristPitchUp},
        {825,345,SettingsAction::YawDown},{925,345,SettingsAction::YawUp},
        {1000,345,SettingsAction::RollDown},{1100,345,SettingsAction::RollUp},
        {780,175,SettingsAction::ClockOff},{920,175,SettingsAction::Clock12},{1060,175,SettingsAction::Clock24},
        {900,430,SettingsAction::FadeOn},{1040,430,SettingsAction::FadeOff},
        {850,500,SettingsAction::FadeAngleDown},{1100,500,SettingsAction::FadeAngleUp}
    };
    for (const auto& b : buttons) CHECK(panel.pointerDown(b.x,b.y,3) == b.action);
    // The shared update bar must not intercept the wrist-page button or clock controls.
    frame_updater::UpdateStatus update;
    update.state = frame_updater::UpdateState::Available;
    update.current = "0.2.0-tai.2"; update.latest = "0.3.0"; update.installable = true;
    panel.render(c, service, update);
    CHECK(panel.pointerDown(1060, 175, 3) == SettingsAction::Clock24);
    panel.armUpdateConfirmForPreview(); panel.render(c, service, update);
    CHECK(panel.pointerDown(1100, 600, 3) == SettingsAction::None);
    panel.render(c, service, update);
    CHECK(panel.pointerDown(1320, 220, 3) == SettingsAction::PitchUp);
    panel.pointerDown(1100, 600, 3); panel.render(c, service, update);
    panel.pointerLeave(); panel.render(c, service, {}); CHECK(panel.writePng(directory + "/wrist-en.png"));
    c.language = Language::Ja; panel.render(c, service, {}); CHECK(panel.writePng(directory + "/wrist-ja.png"));
    panel.pointerDown(1100,600,4); panel.render(c, service, {});
    CHECK(panel.pointerDown(240,220,5) == SettingsAction::ShowOn); // Old controls restored.
}
int main() {
    char directory[] = "/tmp/frame-overlay-test-XXXXXX";
    if (!mkdtemp(directory)) return 2;
    try {
        geometry(); placementRecovery(); clockCases(); configuration(std::string(directory) + "/config.json"); controls(directory);
        std::cout << "Geometry, placement recovery, clock, config persistence and UI controls passed.\n";
        std::filesystem::remove_all(directory);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (artifacts: " << directory << ")\n"; return 1;
    }
}

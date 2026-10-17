// Regression tests for moving local-transform work out of the wrist poll loop.
#include "vr_overlay.h"

#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

// The pre-cache head path, retained here to compare all float components exactly.
vr::HmdMatrix34_t headReference(const Config& config) {
    double rotation[3][3];
    panelRotation(config, rotation);
    const double position[3] = {config.posX, config.posY, config.posZ};
    vr::HmdMatrix34_t transform {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) transform.m[row][col] = static_cast<float>(rotation[row][col]);
        transform.m[row][3] = static_cast<float>(position[row]);
    }
    return transform;
}

void equal(const vr::HmdMatrix34_t& actual, const vr::HmdMatrix34_t& expected) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            check(actual.m[row][col] == expected.m[row][col], "attachment transform changed");
        }
    }
}

void attachmentTransforms() {
    Config config;
    config.posX = -0.41;
    config.posY = 0.23;
    config.posZ = -1.37;
    config.leftWrist.x = -0.11;
    config.leftWrist.y = 0.22;
    config.leftWrist.z = 0.33;
    config.rightWrist.x = 0.44;
    config.rightWrist.y = -0.35;
    config.rightWrist.z = -0.26;
    for (int yaw = -180; yaw <= 180; yaw += 30) {
        for (int pitch = -90; pitch <= 90; pitch += 15) {
            for (int roll = -180; roll <= 180; roll += 30) {
                config.yawDeg = yaw;
                config.pitchDeg = pitch;
                config.rollDeg = roll;
                config.leftWrist.yaw = yaw;
                config.leftWrist.pitch = pitch - 90;
                config.leftWrist.roll = roll;
                config.rightWrist.yaw = -yaw;
                config.rightWrist.pitch = -pitch - 90;
                config.rightWrist.roll = -roll;
                config.attachment = Attachment::Head;
                equal(attachmentTransform(config), headReference(config));
                config.attachment = Attachment::LeftWrist;
                equal(attachmentTransform(config), wristTransform(config.leftWrist));
                config.attachment = Attachment::RightWrist;
                equal(attachmentTransform(config), wristTransform(config.rightWrist));
            }
        }
    }
}

void eventReuse() {
    VrEvents events;
    events.quit = events.closeRequested = true;
    events.pointer.push_back({PointerInput::Type::Move, 10, 20});
    events.pointer.push_back({PointerInput::Type::Down, 30, 40});
    const auto* storage = events.pointer.data();
    const size_t capacity = events.pointer.capacity();
    events.clear();
    check(!events.quit && !events.closeRequested && events.pointer.empty(), "old events were retained");
    check(events.pointer.capacity() == capacity, "event storage was discarded");
    events.pointer.push_back({PointerInput::Type::Up, 50, 60});
    check(events.pointer.data() == storage, "event storage was not reused");
    check(events.pointer[0].type == PointerInput::Type::Up && events.pointer[0].x == 50 && events.pointer[0].y == 60,
          "new event data was lost");
}

}  // namespace

int main() {
    attachmentTransforms();
    eventReuse();
    std::puts("runtime transform/event tests passed");
}

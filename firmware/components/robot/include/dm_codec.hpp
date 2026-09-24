#pragma once
#include "robot.hpp"

namespace robot {
struct Frame {
    uint32_t id = 0;
    uint8_t size = 0;
    std::array<uint8_t, 8> data{};
    bool extended = false, remote = false;
};
// The two profiles are deliberately separate: evidence is tracked per motor model.
enum class Profile { H6215_V1_V2, H3510_V1 };
Profile profile(Model model);
Frame velocity_frame(Profile model, const Config& config, double rps);
Frame command_frame(Profile model, const Config& config, bool enable);
bool decode_feedback(Profile model, const Config& config, const Frame& frame, uint64_t now, Feedback& feedback);
}

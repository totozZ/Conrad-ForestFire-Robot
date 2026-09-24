#include "dm_codec.hpp"
#include <cmath>
#include <cstring>
#include <limits>

namespace robot {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE754 float required");
Profile profile(Model model) { return model == Model::H3510 ? Profile::H3510_V1 : Profile::H6215_V1_V2; }
static uint32_t velocity_id(Profile model, const Config& c) {
    switch (model) {
        case Profile::H6215_V1_V2: return 0x200 + c.can_id;
        case Profile::H3510_V1: return 0x200 + c.can_id;
    }
    return 0;
}
Frame velocity_frame(Profile model, const Config& c, double rps) {
    Frame f; f.id = velocity_id(model, c); f.size = 4;
    const float radians = static_cast<float>(rps * kTau * c.direction);
    uint32_t raw; std::memcpy(&raw, &radians, sizeof(raw));
    for (int i = 0; i < 4; ++i) f.data[i] = static_cast<uint8_t>(raw >> (8 * i));
    return f;
}
Frame command_frame(Profile model, const Config& c, bool enable) {
    // H6215 V2 section 7.2.1 uses base CAN_ID; H3510 V1 specifies the current mode ID.
    Frame f; f.id = model == Profile::H6215_V1_V2 ? c.can_id : velocity_id(model, c); f.size = 8;
    f.data.fill(0xff); f.data[7] = enable ? 0xfc : 0xfd; return f;
}
bool decode_feedback(Profile, const Config& c, const Frame& f, uint64_t now, Feedback& out) {
    if (f.extended || f.remote || f.size != 8 || f.id != static_cast<uint32_t>(c.feedback_id) ||
        (f.data[0] & 0x0f) != c.can_id || !std::isfinite(c.v_max_rad_s) || c.v_max_rad_s <= 0) return false;
    const int status = f.data[0] >> 4;
    // Reject register replies and unrecognised status patterns rather than refreshing freshness.
    if (status != 0 && status != 1 && (status < 8 || status > 14)) return false;
    const unsigned raw_velocity = (static_cast<unsigned>(f.data[3]) << 4) | (f.data[4] >> 4);
    out.seen = true; out.received_ms = now; out.enabled = status == 1;
    out.fault = status >= 8 ? status : 0;
    out.rps = ((raw_velocity / 4095.0) * 2 * c.v_max_rad_s - c.v_max_rad_s) / kTau * c.direction;
    out.mos_c = f.data[6]; out.rotor_c = f.data[7]; return true;
}
}

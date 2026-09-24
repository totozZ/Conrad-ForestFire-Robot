#include "robot.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace robot {
const char* const names[kMotors] = {"left_drive", "right_drive", "cutter"};
Controller::Controller(IO& transport) : io(transport) {
    motors[2].config.model = Model::H3510;
    if (io.simulation()) {
        for (int i = 0; i < kMotors; ++i) {
            auto& c = motors[i].config;
            c.verified = true; c.can_id = i + 1; c.feedback_id = 0x11 + i;
            c.max_rps = 2; c.accel_rps2 = 2; c.v_max_rad_s = 50;
            c.feedback_timeout_ms = 300; c.mode = "velocity";
        }
    }
    log("startup");
}
void Controller::log(const std::string& code) {
    if (events.size() >= 128) events.erase(events.begin());
    events.push_back({++event_seq_, now, code});
}
bool Controller::online(int i) const {
    const auto& m = motors[i];
    return m.feedback.seen && now >= m.feedback.received_ms &&
        now - m.feedback.received_ms < static_cast<uint64_t>(m.config.feedback_timeout_ms);
}
bool Controller::stopped() const {
    for (int i = 0; i < kMotors; ++i) {
        const auto& m = motors[i];
        if (m.requested || m.output != 0 ||
            (online(i) && (m.feedback.enabled || std::abs(m.feedback.rps) > 0.05))) return false;
    }
    return true;
}
Result Controller::control(uint64_t client) const {
    if (!client || owner != client) return no("not_owner");
    if (now - heartbeat_ms >= kLeaseMs) return no("lease_expired");
    return ok();
}
Result Controller::claim(uint64_t client) {
    if (owner && owner != client) return no("owner_busy");
    if (!owner) {
        stop_all("control_acquired");
        owner = client; heartbeat_ms = now; log("owner_acquired");
    }
    return ok();
}
Result Controller::heartbeat(uint64_t client) {
    auto r = control(client); if (!r.ok) return r;
    heartbeat_ms = now; return ok();
}
bool Controller::stop_one(int index) {
    auto& m = motors[index];
    m.target = m.output = 0; m.requested = false;
    return !m.config.verified || io.stop(index, m.config);
}
void Controller::stop_all(const std::string& why, bool release) {
    bool delivered = true;
    for (int i = 0; i < kMotors; ++i) delivered = stop_one(i) && delivered;
    locked = true; cutter_unlocked = false;
    if (reason != why || !delivered) log(delivered ? why : why + ":stop_delivery_failed");
    reason = why;
    if (release) owner = 0;
}
void Controller::disconnect(uint64_t client) {
    if (owner == client) stop_all("owner_disconnected", true);
}
Result Controller::reset(uint64_t client) {
    auto r = control(client); if (!r.ok) return r;
    if (!io.ready()) return no("board_unverified");
    if (!io.healthy()) return no("bus_fault");
    if (!stopped()) return no("not_stopped");
    for (int i = 0; i < kMotors; ++i)
        if (online(i) && motors[i].feedback.fault) return no("motor_fault");
    locked = false; reason = "ready"; cutter_unlocked = false; log("reset"); return ok();
}
Result Controller::select(uint64_t client, const std::string& next, int index) {
    auto r = control(client); if (!r.ok) return r;
    if (!stopped()) return no("not_stopped");
    if ((next != "drive" && next != "test") || index < 0 || index >= kMotors) return no("invalid_mode");
    mode = next; selected = index; cutter_unlocked = false; log("mode:" + next); return ok();
}
Result Controller::unlock(uint64_t client) {
    auto r = control(client); if (!r.ok) return r;
    if (locked) return no("locked");
    cutter_unlocked = true; log("cutter_unlocked"); return ok();
}
Result Controller::enable(uint64_t client, int index) {
    auto r = control(client); if (!r.ok) return r;
    if (index < 0 || index >= kMotors) return no("invalid_motor");
    if (locked) return no("locked");
    if (mode == "test" && index != selected) return no("wrong_mode");
    if (index == 2 && !cutter_unlocked) return no("cutter_locked");
    auto& m = motors[index];
    if (!io.ready() || !m.config.verified) return no("unverified_config");
    if (!online(index)) return no("motor_offline");
    if (m.feedback.fault) return no("motor_fault");
    if (m.requested) return no("already_enabled");
    m.target = m.output = 0;
    if (!io.velocity(index, m.config, 0) || !io.enable(index, m.config)) {
        stop_all("can_tx_fault"); return no("can_tx_fault");
    }
    m.requested = true; m.enable_ms = now; log(std::string("enable:") + names[index]); return ok();
}
Result Controller::speed(uint64_t client, int index, double value) {
    auto r = control(client); if (!r.ok) return r;
    if (index < 0 || index >= kMotors || !std::isfinite(value)) return no("invalid_value");
    if (locked) return no("locked");
    if (mode == "test" ? index != selected : index != 2) return no("wrong_mode");
    auto& m = motors[index];
    if (!m.requested || !online(index) || !m.feedback.enabled || m.feedback.received_ms < m.enable_ms)
        return no("enable_unconfirmed");
    if (index == 2 && (!cutter_unlocked || value < 0)) return no("cutter_locked_or_reverse");
    m.target = std::clamp(value, -m.config.max_rps, m.config.max_rps);
    log(std::string("speed:") + names[index]);
    return ok(m.target == value ? "accepted" : "clamped");
}
Result Controller::drive(uint64_t client, double forward, double turn, double limit) {
    auto r = control(client); if (!r.ok) return r;
    if (locked) return no("locked");
    if (mode != "drive") return no("wrong_mode");
    if (!std::isfinite(forward) || !std::isfinite(turn) || !std::isfinite(limit) ||
        std::abs(forward) > 1 || std::abs(turn) > 1 || limit < 0) return no("invalid_value");
    for (int i = 0; i < 2; ++i) {
        const auto& m = motors[i];
        if (!m.requested || !online(i) || !m.feedback.enabled || m.feedback.received_ms < m.enable_ms)
            return no("enable_unconfirmed");
    }
    double bounded = std::min({limit, motors[0].config.max_rps, motors[1].config.max_rps});
    const double scale = std::max({1.0, std::abs(forward - turn), std::abs(forward + turn)});
    motors[0].target = (forward - turn) / scale * bounded;
    motors[1].target = (forward + turn) / scale * bounded;
    return ok(bounded == limit ? "accepted" : "clamped");
}
Result Controller::stop_motor(uint64_t client, int index) {
    auto r = control(client); if (!r.ok) return r;
    if (index < 0 || index >= kMotors) return no("invalid_motor");
    if (!stop_one(index)) { stop_all("can_tx_fault"); return no("can_tx_fault"); }
    if (index == 2) cutter_unlocked = false;
    log(std::string("stop:") + names[index]); return ok();
}
Result Controller::validate_config(int index, const Config& c) const {
    if (index < 0 || index >= kMotors || c.model != (index == 2 ? Model::H3510 : Model::H6215)) return no("invalid_model");
    if (c.can_id < 1 || c.can_id > 15 || c.feedback_id < 0 || c.feedback_id >= 0x7ff ||
        (c.direction != -1 && c.direction != 1) || c.mode != "velocity" ||
        !std::isfinite(c.max_rps) || !std::isfinite(c.accel_rps2) || !std::isfinite(c.v_max_rad_s) ||
        c.max_rps <= 0 || c.accel_rps2 <= 0 || c.v_max_rad_s <= 0 || c.v_max_rad_s > std::numeric_limits<float>::max() ||
        c.max_rps * kTau > c.v_max_rad_s || c.feedback_timeout_ms < 100 || c.feedback_timeout_ms > 500 ||
        c.feedback_id == 0x200 + c.can_id || c.feedback_id == c.can_id) return no("invalid_config");
    for (int i = 0; i < kMotors; ++i) {
        if (i == index || !motors[i].config.verified || !c.verified) continue;
        const auto& b = motors[i].config;
        if (b.can_id == c.can_id || b.feedback_id == c.feedback_id || b.feedback_id == c.can_id || c.feedback_id == b.can_id ||
            b.feedback_id == 0x200 + c.can_id || c.feedback_id == 0x200 + b.can_id) return no("id_collision");
    }
    return ok();
}
Result Controller::configure(uint64_t client, int index, const Config& config) {
    auto r = control(client); if (!r.ok) return r;
    if (!locked || !stopped()) return no("must_lock_and_stop");
    r = validate_config(index, config); if (!r.ok) return r;
    motors[index] = Motor{}; motors[index].config = config;
    log(std::string("config:") + names[index]); return ok();
}
void Controller::tick(uint64_t time) {
    if (time < now) return;
    now = time;
    const double dt = last_tick_ ? std::min(0.05, (now - last_tick_) / 1000.0) : 0.01;
    last_tick_ = now;
    if (owner && now - heartbeat_ms >= kLeaseMs) stop_all("heartbeat_timeout", true);
    io.poll(now, motors);
    if (!io.healthy()) { if (reason != "bus_fault") stop_all("bus_fault"); return; }
    for (int i = 0; i < kMotors; ++i) {
        auto& m = motors[i];
        if (!m.requested) continue;
        if (!online(i)) { stop_all(std::string("feedback_timeout:") + names[i]); return; }
        if (m.feedback.fault) { stop_all(std::string("motor_fault:") + names[i]); return; }
        if (!m.feedback.enabled) {
            if (now - m.enable_ms >= static_cast<uint64_t>(m.config.feedback_timeout_ms)) {
                stop_all(std::string("enable_lost:") + names[i]); return;
            }
            continue;
        }
        const double delta = m.config.accel_rps2 * dt;
        m.output += std::clamp(m.target - m.output, -delta, delta);
        if (!io.velocity(i, m.config, m.output)) { stop_all("can_tx_fault"); return; }
    }
}

void SimIO::poll(uint64_t now, std::array<Motor, kMotors>& motors) {
    const double dt = previous_ ? std::min(0.05, (now - previous_) / 1000.0) : 0.01;
    previous_ = now;
    for (int i = 0; i < kMotors; ++i) {
        auto& d = devices_[i];
        d.speed += std::clamp((d.enabled && !d.fault ? d.target : 0) - d.speed, -dt * 8, dt * 8);
        if (d.offline || bus_fault_ || !motors[i].config.verified) continue;
        auto& f = motors[i].feedback;
        f = {true, d.enabled && !d.fault, now, d.speed, d.fault ? 10 : 0, 27, 28};
    }
}
bool SimIO::enable(int i, const Config&) {
    if (bus_fault_ || devices_[i].offline) return false;
    devices_[i].enabled = true; return true;
}
bool SimIO::velocity(int i, const Config&, double rps) {
    if (bus_fault_) return false;
    devices_[i].target = rps; return true;
}
bool SimIO::stop(int i, const Config&) {
    if (bus_fault_ || devices_[i].offline) return false;
    devices_[i].enabled = false; devices_[i].target = 0; return true;
}
bool SimIO::inject(int i, const std::string& kind) {
    if (i < 0 || i >= kMotors) return false;
    if (kind == "can") bus_fault_ = true;
    else if (kind == "offline") devices_[i].offline = true;
    else if (kind == "driver") devices_[i].fault = true;
    else if (kind == "clear") { bus_fault_ = false; devices_[i].offline = devices_[i].fault = false; devices_[i].enabled = false; devices_[i].target = 0; }
    else return false;
    return true;
}
}

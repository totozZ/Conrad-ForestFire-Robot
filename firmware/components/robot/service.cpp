#include "service.hpp"
#include <cmath>
#include <memory>

namespace robot {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const cJSON* field(const cJSON* o, const char* key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
std::string string(const cJSON* o, const char* key) { const auto* p = field(o, key); return cJSON_IsString(p) ? p->valuestring : ""; }
double number(const cJSON* o, const char* key) { const auto* p = field(o, key); return cJSON_IsNumber(p) ? p->valuedouble : NAN; }
bool integer(double v, double low, double high) { return std::isfinite(v) && std::floor(v) == v && v >= low && v <= high; }
bool bounded_json(const std::string& input) {
    if (input.size() > 4096 || input.find('\0') != std::string::npos) return false;
    int depth = 0; bool quoted = false, escaped = false;
    for (char c : input) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if ((c == '{' || c == '[') && ++depth > 12) return false;
        else if ((c == '}' || c == ']') && --depth < 0) return false;
    }
    return depth == 0 && !quoted;
}
bool unique_keys(const cJSON* o) {
    if (!o) return true;
    if (cJSON_IsObject(o)) for (auto* a = o->child; a; a = a->next)
        for (auto* b = a->next; b; b = b->next)
            if (a->string && b->string && std::string(a->string) == b->string) return false;
    for (auto* c = o->child; c; c = c->next) if (!unique_keys(c)) return false;
    return true;
}
std::string dump(cJSON* value) {
    char* raw = cJSON_PrintUnformatted(value);
    std::string result = raw ? raw : "{\"v\":1,\"type\":\"ack\",\"accepted\":false,\"code\":\"out_of_memory\"}";
    cJSON_free(raw); cJSON_Delete(value); return result;
}
int motor_index(const cJSON* args) {
    const auto name = string(args, "motor");
    for (int i = 0; i < kMotors; ++i) if (name == names[i]) return i;
    return -1;
}
}
bool Service::connect(uint64_t id) {
    if (!id) return false;
    for (auto& c : clients_) if (c.id == id) return false;
    for (auto& c : clients_) if (!c.id) { c = {id, 0}; return true; }
    return false;
}
void Service::disconnect(uint64_t id) {
    controller.disconnect(id);
    for (auto& c : clients_) if (c.id == id) c = {};
}
cJSON* Service::config_json(const Config& c) {
    auto* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "verified", c.verified);
    cJSON_AddStringToObject(o, "model", c.model == Model::H3510 ? "H3510" : "H6215");
    cJSON_AddStringToObject(o, "mode", c.mode.c_str());
    cJSON_AddNumberToObject(o, "can_id", c.can_id); cJSON_AddNumberToObject(o, "feedback_id", c.feedback_id);
    cJSON_AddNumberToObject(o, "direction", c.direction); cJSON_AddNumberToObject(o, "max_rps", c.max_rps);
    cJSON_AddNumberToObject(o, "accel_rps2", c.accel_rps2); cJSON_AddNumberToObject(o, "v_max_rad_s", c.v_max_rad_s);
    cJSON_AddNumberToObject(o, "feedback_timeout_ms", c.feedback_timeout_ms); return o;
}
bool Service::parse_config(const cJSON* o, Config& c) {
    if (!cJSON_IsObject(o) || !unique_keys(o) || !cJSON_IsBool(field(o, "verified"))) return false;
    const auto model = string(o, "model");
    if (model != "H6215" && model != "H3510") return false;
    c.model = model == "H3510" ? Model::H3510 : Model::H6215;
    c.verified = cJSON_IsTrue(field(o, "verified")); c.mode = string(o, "mode");
    const double id = number(o, "can_id"), feedback = number(o, "feedback_id"), direction = number(o, "direction"), timeout = number(o, "feedback_timeout_ms");
    if (!integer(id, 0, 15) || !integer(feedback, -1, 2046) || !integer(direction, -1, 1) || !integer(timeout, 0, 500)) return false;
    c.can_id = static_cast<int>(id); c.feedback_id = static_cast<int>(feedback);
    c.direction = static_cast<int>(direction); c.feedback_timeout_ms = static_cast<int>(timeout);
    c.max_rps = number(o, "max_rps"); c.accel_rps2 = number(o, "accel_rps2"); c.v_max_rad_s = number(o, "v_max_rad_s");
    return std::isfinite(c.max_rps) && std::isfinite(c.accel_rps2) && std::isfinite(c.v_max_rad_s);
}
std::string Service::configuration() const {
    auto* root = cJSON_CreateObject(); cJSON_AddNumberToObject(root, "v", 1);
    auto* values = cJSON_AddArrayToObject(root, "motors");
    for (const auto& motor : controller.motors) cJSON_AddItemToArray(values, config_json(motor.config));
    return dump(root);
}
bool Service::load_configuration(const std::string& json) {
    Json root(bounded_json(json) ? cJSON_ParseWithOpts(json.c_str(), nullptr, true) : nullptr, cJSON_Delete);
    if (!root || !unique_keys(root.get()) || number(root.get(), "v") != 1) return false;
    auto* list = field(root.get(), "motors");
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) != kMotors) return false;
    auto previous = controller.motors;
    for (auto& m : controller.motors) m.config.verified = false;
    bool valid = true;
    for (int i = 0; i < kMotors && valid; ++i) {
        Config c;
        valid = parse_config(cJSON_GetArrayItem(list, i), c) && c.model == (i == 2 ? Model::H3510 : Model::H6215);
        if (valid && c.verified) valid = controller.validate_config(i, c).ok;
        if (valid) { controller.motors[i] = {}; controller.motors[i].config = c; }
    }
    if (!valid) controller.motors = previous;
    return valid;
}
cJSON* Service::state(uint64_t client) const {
    const auto& c = controller;
    auto* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "now_ms", static_cast<double>(c.now));
    cJSON_AddNumberToObject(o, "session", static_cast<double>(client));
    cJSON_AddNumberToObject(o, "owner", static_cast<double>(c.owner));
    cJSON_AddBoolToObject(o, "simulation", c.io.simulation());
    cJSON_AddBoolToObject(o, "board_ready", c.io.ready()); cJSON_AddBoolToObject(o, "bus_healthy", c.io.healthy());
    cJSON_AddBoolToObject(o, "locked", c.locked); cJSON_AddBoolToObject(o, "cutter_unlocked", c.cutter_unlocked);
    cJSON_AddStringToObject(o, "reason", c.reason.c_str()); cJSON_AddStringToObject(o, "mode", c.mode.c_str());
    cJSON_AddStringToObject(o, "selected", names[c.selected]);
    auto* values = cJSON_AddArrayToObject(o, "motors");
    for (int i = 0; i < kMotors; ++i) {
        const auto& m = c.motors[i]; const auto& f = m.feedback;
        auto* v = cJSON_CreateObject(); cJSON_AddItemToArray(values, v);
        cJSON_AddStringToObject(v, "name", names[i]); cJSON_AddItemToObject(v, "config", config_json(m.config));
        cJSON_AddBoolToObject(v, "online", c.online(i)); cJSON_AddBoolToObject(v, "requested", m.requested);
        if (c.online(i)) cJSON_AddBoolToObject(v, "enabled", f.enabled); else cJSON_AddNullToObject(v, "enabled");
        cJSON_AddNumberToObject(v, "target_rps", m.target); cJSON_AddNumberToObject(v, "output_rps", m.output);
        if (c.online(i)) cJSON_AddNumberToObject(v, "actual_rps", f.rps); else cJSON_AddNullToObject(v, "actual_rps");
        if (f.seen) cJSON_AddNumberToObject(v, "age_ms", static_cast<double>(c.now - f.received_ms)); else cJSON_AddNullToObject(v, "age_ms");
        if (c.online(i)) cJSON_AddNumberToObject(v, "fault", f.fault); else cJSON_AddNullToObject(v, "fault");
        if (c.online(i) && f.mos_c >= 0) cJSON_AddNumberToObject(v, "mos_c", f.mos_c); else cJSON_AddNullToObject(v, "mos_c");
        if (c.online(i) && f.rotor_c >= 0) cJSON_AddNumberToObject(v, "rotor_c", f.rotor_c); else cJSON_AddNullToObject(v, "rotor_c");
        cJSON_AddNullToObject(v, "current_a");
    }
    auto* logs = cJSON_AddArrayToObject(o, "events");
    // Full bounded ring lets a reconnecting operator export faults missed while disconnected.
    for (const auto& event : c.events) {
        auto* e = cJSON_CreateObject(); cJSON_AddItemToArray(logs, e);
        cJSON_AddNumberToObject(e, "seq", static_cast<double>(event.seq));
        cJSON_AddNumberToObject(e, "ms", static_cast<double>(event.ms));
        cJSON_AddStringToObject(e, "code", event.code.c_str());
    }
    return o;
}
std::string Service::request(uint64_t id, const std::string& input) {
    Client* client = nullptr; for (auto& c : clients_) if (c.id == id) client = &c;
    Json root(bounded_json(input) ? cJSON_ParseWithOpts(input.c_str(), nullptr, true) : nullptr, cJSON_Delete);
    const auto* o = root.get();
    double seq = o ? number(o, "id") : NAN;
    auto response = [&](Result r, bool snapshot = false) {
        auto* out = cJSON_CreateObject(); cJSON_AddNumberToObject(out, "v", 1);
        cJSON_AddStringToObject(out, "type", snapshot ? "state" : "ack");
        if (integer(seq, 1, 9007199254740991.0)) cJSON_AddNumberToObject(out, "id", seq); else cJSON_AddNullToObject(out, "id");
        cJSON_AddBoolToObject(out, "accepted", r.ok); cJSON_AddStringToObject(out, "code", r.code.c_str());
        cJSON_AddNumberToObject(out, "now_ms", static_cast<double>(controller.now));
        if (snapshot) cJSON_AddItemToObject(out, "state", state(id));
        return dump(out);
    };
    if (!client) return response(no("unknown_session"));
    if (!cJSON_IsObject(o) || !unique_keys(o) || number(o, "v") != 1 || !integer(seq, 1, 9007199254740991.0))
        return response(no("invalid_envelope"));
    if (seq <= static_cast<double>(client->sequence)) return response(no("duplicate_or_reordered"));
    client->sequence = static_cast<uint64_t>(seq);
    const auto op = string(o, "op"); const auto* args = field(o, "args");
    if (op == "sync") return response(ok(), true);
    // A valid all-stop is always honoured, even with an old clock or from an observer.
    if (op == "stop_all") { controller.stop_all("software_stop"); return response(ok()); }
    const double issued = number(o, "issued_ms");
    if (!std::isfinite(issued) || issued > static_cast<double>(controller.now) + 100 ||
        static_cast<double>(controller.now) - issued > kCommandAgeMs) return response(no("expired_command"));
    Result r = no("unknown_operation");
    const int index = motor_index(args);
    if (op == "claim") r = controller.claim(id);
    else if (op == "heartbeat") r = controller.heartbeat(id);
    else if (op == "release") {
        if (controller.owner != id) r = no("not_owner");
        else { controller.stop_all("released", true); r = ok(); }
    }
    else if (op == "reset") r = controller.reset(id);
    else if (op == "select") r = controller.select(id, string(args, "mode"), index);
    else if (op == "unlock_cutter") r = controller.unlock(id);
    else if (op == "enable") r = controller.enable(id, index);
    else if (op == "speed") r = controller.speed(id, index, number(args, "rps"));
    else if (op == "drive") r = controller.drive(id, number(args, "forward"), number(args, "turn"), number(args, "limit_rps"));
    else if (op == "stop_motor") r = controller.stop_motor(id, index);
    else if (op == "configure") {
        Config c;
        if (!parse_config(field(args, "config"), c)) r = no("invalid_config");
        else {
            const auto previous = controller.motors;
            r = controller.configure(id, index, c);
            if (r.ok && persist && !persist(configuration())) {
                controller.motors = previous; controller.log("config_persist_failed"); r = no("config_persist_failed");
            }
        }
    }
    else if (op == "inject") {
        if (!controller.io.simulation()) r = no("simulation_only");
        else if (controller.owner != id) r = no("not_owner");
        else if (controller.io.inject(index, string(args, "kind"))) { controller.log("inject:" + string(args, "kind")); r = ok(); }
        else r = no("invalid_fault");
    }
    if (!r.ok && op != "heartbeat") controller.log("rejected:" + op + ":" + r.code);
    return response(r);
}
}

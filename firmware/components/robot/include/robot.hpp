#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace robot {
constexpr double kTau = 6.2831853071795864769;
constexpr int kMotors = 3;
constexpr int kLeaseMs = 500;
constexpr int kCommandAgeMs = 250;
extern const char* const names[kMotors];
enum class Model { H6215, H3510 };
struct Config {
    bool verified = false;
    Model model = Model::H6215;
    int can_id = 0, feedback_id = -1, direction = 1;
    double max_rps = 0, accel_rps2 = 0, v_max_rad_s = 0;
    int feedback_timeout_ms = 0;
    std::string mode = "unconfirmed";
};
struct Feedback {
    bool seen = false, enabled = false;
    uint64_t received_ms = 0;
    double rps = 0;
    int fault = 0, mos_c = -1, rotor_c = -1;
};
struct Motor {
    Config config;
    Feedback feedback;
    bool requested = false;
    uint64_t enable_ms = 0;
    double target = 0, output = 0;
};
struct Event { uint64_t seq, ms; std::string code; };
struct Result { bool ok; std::string code; };
inline Result ok(const char* code = "accepted") { return {true, code}; }
inline Result no(const char* code) { return {false, code}; }

class IO {
public:
    virtual ~IO() = default;
    virtual bool simulation() const = 0;
    virtual bool ready() const = 0;
    virtual bool healthy() const = 0;
    virtual void poll(uint64_t now, std::array<Motor, kMotors>& motors) = 0;
    virtual bool enable(int index, const Config& config) = 0;
    virtual bool velocity(int index, const Config& config, double rps) = 0;
    virtual bool stop(int index, const Config& config) = 0;
    virtual bool inject(int, const std::string&) { return false; }
};

class Controller {
public:
    explicit Controller(IO& io);
    IO& io;
    std::array<Motor, kMotors> motors;
    uint64_t now = 0, owner = 0, heartbeat_ms = 0;
    bool locked = true, cutter_unlocked = false;
    std::string reason = "startup", mode = "drive";
    int selected = 0;
    std::vector<Event> events;
    void tick(uint64_t time);
    void disconnect(uint64_t client);
    Result claim(uint64_t client);
    Result heartbeat(uint64_t client);
    Result reset(uint64_t client);
    Result select(uint64_t client, const std::string& mode, int index);
    Result unlock(uint64_t client);
    Result enable(uint64_t client, int index);
    Result speed(uint64_t client, int index, double rps);
    Result drive(uint64_t client, double forward, double turn, double limit);
    Result stop_motor(uint64_t client, int index);
    void stop_all(const std::string& why, bool release = false);
    Result configure(uint64_t client, int index, const Config& config);
    Result validate_config(int index, const Config& config) const;
    bool online(int index) const;
    bool stopped() const;
    void log(const std::string& code);
private:
    uint64_t last_tick_ = 0, event_seq_ = 0;
    Result control(uint64_t client) const;
    bool stop_one(int index);
};

class SimIO final : public IO {
public:
    bool simulation() const override { return true; }
    bool ready() const override { return true; }
    bool healthy() const override { return !bus_fault_; }
    void poll(uint64_t now, std::array<Motor, kMotors>& motors) override;
    bool enable(int index, const Config&) override;
    bool velocity(int index, const Config&, double rps) override;
    bool stop(int index, const Config&) override;
    bool inject(int index, const std::string& kind) override;
private:
    struct Device { bool enabled = false, offline = false, fault = false; double speed = 0, target = 0; };
    std::array<Device, kMotors> devices_{};
    bool bus_fault_ = false;
    uint64_t previous_ = 0;
};
}

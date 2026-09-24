#pragma once
#include "robot.hpp"
#include "dm_codec.hpp"

class TwaiIO final : public robot::IO {
public:
    void begin();
    bool simulation() const override { return false; }
    bool ready() const override { return started_; }
    bool healthy() const override { return !fault_; }
    void poll(uint64_t now, std::array<robot::Motor, robot::kMotors>& motors) override;
    bool enable(int, const robot::Config& c) override;
    bool velocity(int, const robot::Config& c, double rps) override;
    bool stop(int, const robot::Config& c) override;
private:
    bool started_ = false, fault_ = false;
    uint64_t last_probe_ = 0;
    bool send(const robot::Frame& frame);
};

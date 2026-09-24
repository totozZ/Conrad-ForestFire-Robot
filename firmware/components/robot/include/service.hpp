#pragma once
#include "robot.hpp"
#include "cJSON.h"
#include <functional>

namespace robot {
class Service {
public:
    explicit Service(Controller& controller) : controller(controller) {}
    Controller& controller;
    // Save only stopped motor configuration. Never persist owner, enable, target or unlock.
    std::function<bool(const std::string&)> persist;
    bool connect(uint64_t client);
    void disconnect(uint64_t client);
    std::string request(uint64_t client, const std::string& json);
    std::string configuration() const;
    bool load_configuration(const std::string& json);
    static bool parse_config(const cJSON* value, Config& out);
private:
    struct Client { uint64_t id = 0, sequence = 0; };
    std::array<Client, 4> clients_{};
    cJSON* state(uint64_t client) const;
    static cJSON* config_json(const Config& c);
};
}

#include "service.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    robot::SimIO io;
    robot::Controller controller(io);
    robot::Service service(controller);
    if (argc == 2) {
        const std::string path = argv[1];
        std::ifstream input(path);
        if (input) {
            const std::string json((std::istreambuf_iterator<char>(input)), {});
            if (!service.load_configuration(json)) std::cerr << "Invalid simulator config; using simulation defaults.\n";
        }
        service.persist = [path](const std::string& json) {
            std::ofstream out(path, std::ios::trunc); out << json; out.flush(); return out.good();
        };
    }
    std::cout << "{\"ready\":true}" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        cJSON* root = cJSON_Parse(line.c_str());
        if (!root) continue;
        auto* time = cJSON_GetObjectItemCaseSensitive(root, "now_ms");
        if (cJSON_IsNumber(time) && time->valuedouble >= 0) controller.tick(static_cast<uint64_t>(time->valuedouble));
        auto* kind_value = cJSON_GetObjectItemCaseSensitive(root, "kind");
        auto* client_value = cJSON_GetObjectItemCaseSensitive(root, "client");
        std::string kind = cJSON_IsString(kind_value) ? kind_value->valuestring : "";
        uint64_t client = cJSON_IsNumber(client_value) ? static_cast<uint64_t>(client_value->valuedouble) : 0;
        if (kind == "connect") service.connect(client);
        else if (kind == "disconnect") service.disconnect(client);
        else if (kind == "request") {
            auto* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
            std::string response = service.request(client, cJSON_IsString(payload) ? payload->valuestring : "");
            std::cout << "{\"to\":" << client << ",\"message\":" << response << "}" << std::endl;
        }
        cJSON_Delete(root);
    }
    controller.stop_all("host_exit", true);
}

#include "robot.hpp"
#include "dm_codec.hpp"
#include "service.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ << " failed: " #x "\n"; std::exit(1); } } while (0)
using namespace robot;
struct UnverifiedIO : IO {
    int writes = 0;
    bool simulation() const override { return false; }
    bool ready() const override { return false; }
    bool healthy() const override { return true; }
    void poll(uint64_t, std::array<Motor,kMotors>&) override {}
    bool enable(int,const Config&) override { ++writes; return false; }
    bool velocity(int,const Config&,double) override { ++writes; return false; }
    bool stop(int,const Config&) override { ++writes; return false; }
};
struct Fixture {
    SimIO io; Controller c{io}; Service s{c};
    Fixture() { c.tick(1000); }
    void ready() { CHECK(c.claim(1).ok); CHECK(c.reset(1).ok); }
    void drive() { ready(); CHECK(c.enable(1,0).ok); CHECK(c.enable(1,1).ok); c.tick(1010); CHECK(c.drive(1,1,0,1).ok); }
};
static std::string code(const std::string& json) {
    auto* o = cJSON_Parse(json.c_str()); CHECK(o); auto* c = cJSON_GetObjectItem(o, "code");
    std::string result = c && c->valuestring ? c->valuestring : ""; cJSON_Delete(o); return result;
}
static void state_machine_tests() {
    { UnverifiedIO io; Controller c(io); Service s(c); c.tick(1000); CHECK(c.claim(1).ok);
      CHECK(c.reset(1).code == "board_unverified"); CHECK(!c.enable(1,0).ok);
      c.stop_all("stop"); CHECK(io.writes == 0);
      for (const auto& m : c.motors) CHECK(!m.config.verified && !m.requested);
      CHECK(s.connect(1)); CHECK(code(s.request(1,R"({"v":1,"id":1,"issued_ms":1000,"op":"inject","args":{"motor":"left_drive","kind":"clear"}})")) == "simulation_only"); }
    { Fixture f; CHECK(f.c.locked); CHECK(f.c.owner == 0); CHECK(!f.c.enable(1,0).ok);
      f.ready(); CHECK(!f.c.claim(2).ok); CHECK(!f.c.enable(2,0).ok); CHECK(!f.c.enable(1,2).ok);
      CHECK(f.c.unlock(1).ok); CHECK(f.c.enable(1,2).ok);
      CHECK(!f.c.speed(1,2,1).ok); f.c.tick(1010); CHECK(f.c.speed(1,2,1).ok);
      CHECK(!f.c.speed(1,2,-1).ok); f.c.stop_all("stop"); CHECK(!f.c.cutter_unlocked); }
    { Fixture f; f.drive(); f.c.tick(1020); CHECK(f.c.motors[0].output > 0 && f.c.motors[0].output <= 0.021);
      CHECK(f.c.drive(1,0,1,0.2).ok); CHECK(f.c.motors[0].target == -0.2 && f.c.motors[1].target == 0.2);
      CHECK(f.c.drive(1,1,1,500).code == "clamped"); CHECK(f.c.motors[0].target == 0); CHECK(f.c.motors[1].target == 2);
      CHECK(!f.c.drive(1,NAN,0,1).ok); CHECK(!f.c.select(1,"test",0).ok);
      f.c.tick(1499); CHECK(!f.c.locked); f.c.tick(1500); CHECK(f.c.locked); CHECK(f.c.owner == 0);
      CHECK(f.c.motors[0].target == 0 && !f.c.motors[0].requested);
      f.c.tick(1600); CHECK(f.c.locked); CHECK(!f.c.drive(1,1,0,1).ok); }
    { Fixture f; f.drive(); CHECK(f.io.inject(0,"offline")); f.c.tick(1200); CHECK(!f.c.locked);
      CHECK(f.c.heartbeat(1).ok); f.c.tick(1310); CHECK(f.c.locked); CHECK(f.c.reason == "feedback_timeout:left_drive");
      CHECK(f.io.inject(0,"clear")); f.c.tick(1320); CHECK(f.c.locked && !f.c.motors[0].requested); }
    { Fixture f; f.drive(); CHECK(f.io.inject(0,"driver")); f.c.tick(1020); CHECK(f.c.locked); CHECK(f.c.reason == "motor_fault:left_drive");
      CHECK(!f.c.reset(1).ok); CHECK(f.io.inject(0,"clear")); f.c.tick(1030); CHECK(f.c.reset(1).ok); CHECK(!f.c.motors[0].requested); }
    { Fixture f; f.drive(); CHECK(f.io.inject(0,"can")); f.c.tick(1020); CHECK(f.c.locked && f.c.reason == "bus_fault");
      CHECK(!f.c.reset(1).ok); CHECK(f.c.motors[0].output == 0); }
    { Fixture f; CHECK(f.io.inject(2,"offline")); f.drive(); f.c.tick(1320); CHECK(!f.c.locked); CHECK(f.c.online(0)); CHECK(!f.c.online(2));
      f.c.disconnect(2); CHECK(!f.c.locked); f.c.disconnect(1); CHECK(f.c.locked && f.c.owner == 0); }
    { Fixture f; f.ready(); CHECK(f.c.select(1,"test",0).ok); CHECK(!f.c.enable(1,1).ok);
      CHECK(f.c.enable(1,0).ok); f.c.tick(1010); CHECK(f.c.speed(1,0,100).code == "clamped"); CHECK(f.c.motors[0].target == 2);
      CHECK(!f.c.drive(1,1,0,1).ok); CHECK(f.c.stop_motor(1,0).ok); f.c.tick(1020); CHECK(f.c.select(1,"drive",0).ok); }
    { Fixture f; f.ready(); auto c = f.c.motors[0].config; CHECK(!f.c.configure(1,0,c).ok);
      f.c.stop_all("stop"); CHECK(f.c.configure(1,0,c).ok);
      c.can_id = 2; CHECK(f.c.configure(1,0,c).code == "id_collision"); c.can_id = 1;
      c.feedback_id = 0x202; CHECK(f.c.configure(1,0,c).code == "id_collision"); c.feedback_id = 0x11;
      c.max_rps = 28; CHECK(f.c.configure(1,0,c).code == "invalid_config");
      c.max_rps = NAN; CHECK(!f.c.configure(1,0,c).ok); c.max_rps = 1;
      c.direction = 0; CHECK(!f.c.configure(1,0,c).ok); c.direction = 1;
      c.feedback_timeout_ms = 501; CHECK(!f.c.configure(1,0,c).ok); }
}
static void protocol_tests() {
    Fixture f; CHECK(f.s.connect(1)); CHECK(f.s.connect(2));
    CHECK(code(f.s.request(1,R"({"v":1,"id":1,"op":"sync"})")) == "accepted");
    CHECK(code(f.s.request(1,R"({"v":1,"id":2,"issued_ms":1000,"op":"claim"})")) == "accepted");
    CHECK(code(f.s.request(1,R"({"v":1,"id":2,"issued_ms":1000,"op":"reset"})")) == "duplicate_or_reordered");
    CHECK(f.c.locked);
    CHECK(code(f.s.request(1,R"({"v":1,"id":3,"issued_ms":0,"op":"reset"})")) == "expired_command");
    CHECK(code(f.s.request(1,R"({"v":1,"id":4,"issued_ms":2000,"op":"reset"})")) == "expired_command");
    CHECK(code(f.s.request(1,R"({"v":1,"id":5,"issued_ms":1000,"op":"reset"})")) == "accepted");
    CHECK(!f.c.locked);
    CHECK(code(f.s.request(2,R"({"v":1,"id":1,"issued_ms":1000,"op":"enable","args":{"motor":"left_drive"}})")) == "not_owner");
    CHECK(code(f.s.request(2,R"({"v":1,"id":2,"issued_ms":0,"op":"stop_all"})")) == "accepted");
    CHECK(f.c.locked);
    CHECK(code(f.s.request(1,R"({"v":1,"id":6,"id":7,"op":"sync"})")) == "invalid_envelope");
    CHECK(code(f.s.request(1,R"({"v":1,"id":6,"op":"sync"} garbage)")) == "invalid_envelope");
    CHECK(code(f.s.request(1,std::string(20,'[') + std::string(20,']'))) == "invalid_envelope");
    CHECK(code(f.s.request(1,std::string("{}\0{}",5))) == "invalid_envelope");
    CHECK(code(f.s.request(1,R"({"v":1,"id":6.5,"op":"sync"})")) == "invalid_envelope");
    CHECK(code(f.s.request(1,R"({"v":1,"id":6,"issued_ms":1000,"op":"drive","args":{"forward":"1","turn":0,"limit_rps":1}})")) == "locked");
    CHECK(f.s.connect(3)); CHECK(f.s.connect(4)); CHECK(!f.s.connect(5));
    f.s.disconnect(1); CHECK(f.c.owner == 0); CHECK(f.s.connect(5));
    const auto saved = f.s.configuration();
    Fixture next; CHECK(next.s.load_configuration(saved)); CHECK(next.c.locked && !next.c.motors[0].requested);
    CHECK(!next.s.load_configuration("{}")); CHECK(next.s.configuration() == saved);
    f.c.tick(1010); CHECK(f.c.claim(2).ok);
    f.s.persist = [](const std::string&) { return false; };
    auto* root = cJSON_Parse(saved.c_str()); auto* cfg = cJSON_GetArrayItem(cJSON_GetObjectItem(root,"motors"),0);
    cJSON_SetNumberValue(cJSON_GetObjectItem(cfg,"max_rps"),1);
    char* raw = cJSON_PrintUnformatted(cfg);
    const std::string command = std::string(R"({"v":1,"id":3,"issued_ms":1010,"op":"configure","args":{"motor":"left_drive","config":)") + raw + "}}";
    cJSON_free(raw); cJSON_Delete(root);
    CHECK(code(f.s.request(2,command)) == "config_persist_failed"); CHECK(f.c.motors[0].config.max_rps == 2);
}
static void codec_tests() {
    Config c; c.can_id = 1; c.feedback_id = 0x11; c.v_max_rad_s = 45;
    for (auto p : {Profile::H6215_V1_V2,Profile::H3510_V1}) {
        // H6215 V2 sec. 5.8/7.2: 5 rad/s example. H3510 shares the documented float format;
        // applying this numeric vector to H3510 is a derived structural test, not an on-wire capture.
        const auto speed = velocity_frame(p,c,5/kTau);
        CHECK(speed.id == 0x201 && speed.size == 4);
        CHECK(speed.data[0] == 0 && speed.data[1] == 0 && speed.data[2] == 0xa0 && speed.data[3] == 0x40);
        auto enable = command_frame(p,c,true); CHECK(enable.size == 8 && enable.data[7] == 0xfc);
        CHECK(enable.id == (p == Profile::H6215_V1_V2 ? 1u : 0x201u));
        CHECK(command_frame(p,c,false).data[7] == 0xfd);
        Frame feedback; feedback.id = 0x11; feedback.size = 8; feedback.data = {0x11,0,0,0xff,0xf0,0,30,31};
        Feedback f; CHECK(decode_feedback(p,c,feedback,123,f)); CHECK(f.enabled && !f.fault && f.received_ms == 123);
        CHECK(std::abs(f.rps - 45/kTau) < 1e-7); CHECK(f.mos_c == 30 && f.rotor_c == 31);
        feedback.data[3] = feedback.data[4] = 0; CHECK(decode_feedback(p,c,feedback,124,f)); CHECK(f.rps < 0);
        c.direction = -1; CHECK(decode_feedback(p,c,feedback,125,f)); CHECK(f.rps > 0); c.direction = 1;
        feedback.data[0] = 0xa1; CHECK(decode_feedback(p,c,feedback,126,f)); CHECK(f.fault == 10 && !f.enabled);
        feedback.data[0] = 0xa2; CHECK(!decode_feedback(p,c,feedback,127,f)); CHECK(f.received_ms == 126);
        feedback.data[0] = 0x11; feedback.extended = true; CHECK(!decode_feedback(p,c,feedback,128,f)); feedback.extended = false;
        feedback.size = 4; CHECK(!decode_feedback(p,c,feedback,129,f)); feedback.size = 8;
        feedback.remote = true; CHECK(!decode_feedback(p,c,feedback,130,f)); feedback.remote = false;
        feedback.data[0] = 0x31; CHECK(!decode_feedback(p,c,feedback,131,f));
    }
}
int main() {
    state_machine_tests(); protocol_tests(); codec_tests();
    std::cout << checks << " C++ checks passed (controller, protocol, persistence, codec).\n";
}

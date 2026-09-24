#include "twai_io.hpp"
#include "sdkconfig.h"
#include "driver/twai.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <cstring>

void TwaiIO::begin() {
#if CONFIG_ROBOT_BOARD_VERIFIED
    const bool board_verified = true;
#else
    const bool board_verified = false;
#endif
    if (!board_verified) {
        ESP_LOGW("CAN", "Board unverified: TWAI is not initialized; no CAN output");
        return;
    }
    const int tx = CONFIG_ROBOT_CAN_TX_GPIO, rx = CONFIG_ROBOT_CAN_RX_GPIO;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(tx) || !GPIO_IS_VALID_GPIO(rx) || tx == rx ||
        std::strcmp(CONFIG_ROBOT_BOARD_NAME, "UNCONFIRMED") == 0) {
        ESP_LOGE("CAN", "Invalid board/pins; no CAN output"); return;
    }
    twai_timing_config_t timing{};
    switch (CONFIG_ROBOT_CAN_BITRATE) {
        case 125000: timing = TWAI_TIMING_CONFIG_125KBITS(); break;
        case 250000: timing = TWAI_TIMING_CONFIG_250KBITS(); break;
        case 500000: timing = TWAI_TIMING_CONFIG_500KBITS(); break;
        case 1000000: timing = TWAI_TIMING_CONFIG_1MBITS(); break;
        default: ESP_LOGE("CAN", "Bitrate unconfirmed/unsupported; no CAN output"); return;
    }
    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(static_cast<gpio_num_t>(tx), static_cast<gpio_num_t>(rx), TWAI_MODE_NORMAL);
    general.tx_queue_len = 0; // No application backlog of stale velocity frames.
    general.rx_queue_len = 32;
    general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_TX_FAILED | TWAI_ALERT_RX_QUEUE_FULL;
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    esp_err_t result = twai_driver_install(&general, &timing, &filter);
    if (result == ESP_OK) result = twai_start();
    started_ = result == ESP_OK;
    if (!started_) ESP_LOGE("CAN", "TWAI initialization failed: %s", esp_err_to_name(result));
}
bool TwaiIO::send(const robot::Frame& f) {
    if (!started_) return false;
    twai_message_t m{}; m.identifier = f.id; m.data_length_code = f.size;
    m.ss = 1; // Single shot: a missing ACK cannot keep an old command retrying indefinitely.
    std::memcpy(m.data, f.data.data(), f.size);
    const bool sent = twai_transmit(&m, pdMS_TO_TICKS(2)) == ESP_OK;
    if (!sent) fault_ = true;
    return sent;
}
bool TwaiIO::enable(int, const robot::Config& c) { return send(robot::command_frame(robot::profile(c.model), c, true)); }
bool TwaiIO::velocity(int, const robot::Config& c, double rps) { return send(robot::velocity_frame(robot::profile(c.model), c, rps)); }
bool TwaiIO::stop(int, const robot::Config& c) {
    const bool zero = send(robot::velocity_frame(robot::profile(c.model), c, 0));
    const bool disable = send(robot::command_frame(robot::profile(c.model), c, false));
    return zero && disable;
}
void TwaiIO::poll(uint64_t now, std::array<robot::Motor, robot::kMotors>& motors) {
    if (!started_) return;
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) == ESP_OK && alerts) fault_ = true;
    twai_status_info_t status{};
    if (twai_get_status_info(&status) != ESP_OK || status.state != TWAI_STATE_RUNNING) fault_ = true;
    twai_message_t m{};
    for (int count = 0; count < 32 && twai_receive(&m, 0) == ESP_OK; ++count) {
        robot::Frame f; f.id = m.identifier; f.size = m.data_length_code; f.extended = m.extd; f.remote = m.rtr;
        std::memcpy(f.data.data(), m.data, 8);
        for (auto& motor : motors) if (motor.config.verified)
            robot::decode_feedback(robot::profile(motor.config.model), motor.config, f, now, motor.feedback);
    }
    // Disabled verified devices are polled using their disable command, never an enable/auto-calibration.
    if (!fault_ && now - last_probe_ >= 100) {
        last_probe_ = now;
        for (int i = 0; i < robot::kMotors; ++i)
            if (motors[i].config.verified && !motors[i].requested)
                send(robot::command_frame(robot::profile(motors[i].config.model), motors[i].config, false));
    }
    // A transport fault stays latched until an explicit controller reboot after inspection.
}

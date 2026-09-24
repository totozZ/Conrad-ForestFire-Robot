#include "service.hpp"
#include "twai_io.hpp"
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
#if CONFIG_ROBOT_SIMULATION
robot::SimIO transport;
#else
TwaiIO transport;
#endif
robot::Controller controller(transport);
robot::Service service(controller);
SemaphoreHandle_t mutex;
nvs_handle_t settings;
bool storage_ready = false;
uint64_t next_client = 1;
constexpr const char* tag = "conrad";
uint64_t clock_ms() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
struct Lock { Lock() { xSemaphoreTake(mutex, portMAX_DELAY); } ~Lock() { xSemaphoreGive(mutex); } };
struct Session { uint64_t id; };
void free_session(void* ptr) {
    auto* s = static_cast<Session*>(ptr);
    { Lock lock; service.disconnect(s->id); }
    delete s;
}
void control_task(void*) {
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        { Lock lock; controller.tick(clock_ms()); }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
    }
}
esp_err_t websocket(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        // Browser clients must be same-origin; non-browser local test clients may omit Origin.
        char origin[160]{}, host[96]{};
        if (httpd_req_get_hdr_value_len(req, "Origin")) {
            if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
                httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
                std::string(origin) != std::string("http://") + host) return ESP_FAIL;
        }
        Lock lock;
        auto* session = new Session{next_client++};
        if (!service.connect(session->id)) { delete session; return ESP_FAIL; }
        req->sess_ctx = session; req->free_ctx = free_session;
        return ESP_OK;
    }
    auto* session = static_cast<Session*>(req->sess_ctx);
    if (!session) return ESP_FAIL;
    httpd_ws_frame_t frame{};
    esp_err_t result = httpd_ws_recv_frame(req, &frame, 0);
    if (result != ESP_OK || frame.type != HTTPD_WS_TYPE_TEXT || frame.len > 4096) return ESP_FAIL;
    std::string input(frame.len, '\0'); frame.payload = reinterpret_cast<uint8_t*>(input.data());
    result = httpd_ws_recv_frame(req, &frame, input.size());
    if (result != ESP_OK) return result;
    std::string response;
    { Lock lock;
      controller.tick(clock_ms());
      response = service.request(session->id, input);
    }
    httpd_ws_frame_t out{}; out.type = HTTPD_WS_TYPE_TEXT;
    out.payload = reinterpret_cast<uint8_t*>(response.data()); out.len = response.size();
    return httpd_ws_send_frame(req, &out);
}
extern const uint8_t html_start[] asm("_binary_index_html_start");
extern const uint8_t html_end[] asm("_binary_index_html_end");
extern const uint8_t js_start[] asm("_binary_app_js_start");
extern const uint8_t js_end[] asm("_binary_app_js_end");
extern const uint8_t css_start[] asm("_binary_style_css_start");
extern const uint8_t css_end[] asm("_binary_style_css_end");
struct Asset { const uint8_t* start; const uint8_t* end; const char* type; };
Asset html{html_start, html_end, "text/html; charset=utf-8"};
Asset js{js_start, js_end, "text/javascript; charset=utf-8"};
Asset css{css_start, css_end, "text/css; charset=utf-8"};
esp_err_t asset_handler(httpd_req_t* req) {
    auto* asset = static_cast<Asset*>(req->user_ctx);
    httpd_resp_set_type(req, asset->type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, reinterpret_cast<const char*>(asset->start), asset->end - asset->start - 1);
}
void start_wifi() {
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    wifi_config_t config{};
    std::snprintf(reinterpret_cast<char*>(config.ap.ssid), sizeof(config.ap.ssid), "Conrad-%02X%02X", mac[4], mac[5]);
    std::string password = CONFIG_ROBOT_AP_PASSWORD;
    if (password.size() < 8 || password.size() > 63) {
        char generated[17]; std::snprintf(generated, sizeof(generated), "%08lx%08lx", static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
        password = generated;
    }
    std::memcpy(config.ap.password, password.data(), password.size());
    config.ap.channel = 6; config.ap.max_connection = 4; config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(tag, "SSID: %s | password: %s | http://192.168.4.1", config.ap.ssid, password.c_str());
}
void start_web() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.stack_size = 12288;
    config.max_open_sockets = 7; config.lru_purge_enable = false; config.recv_wait_timeout = 1; config.send_wait_timeout = 1;
    httpd_handle_t server = nullptr; ESP_ERROR_CHECK(httpd_start(&server, &config));
    auto add = [&](const char* uri, Asset* value) {
        httpd_uri_t route{}; route.uri = uri; route.method = HTTP_GET; route.handler = asset_handler; route.user_ctx = value;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    };
    add("/", &html); add("/app.js", &js); add("/style.css", &css);
    httpd_uri_t ws{}; ws.uri = "/ws"; ws.method = HTTP_GET; ws.handler = websocket; ws.is_websocket = true;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws));
}
}
extern "C" void app_main() {
    mutex = xSemaphoreCreateMutex(); configASSERT(mutex);
    // Never erase an existing NVS partition automatically when initialization fails.
    const esp_err_t nvs_result = nvs_flash_init();
    storage_ready = nvs_result == ESP_OK && nvs_open(transport.simulation() ? "robot_sim" : "robot_real", NVS_READWRITE, &settings) == ESP_OK;
    if (storage_ready) {
        size_t length = 0;
        if (nvs_get_str(settings, "motors", nullptr, &length) == ESP_OK && length < 4096) {
            std::string config(length, '\0');
            if (nvs_get_str(settings, "motors", config.data(), &length) == ESP_OK) {
                config.resize(length - 1);
                if (!service.load_configuration(config)) ESP_LOGE(tag, "Stored config invalid; defaults remain disabled");
            }
        }
    }
    service.persist = [](const std::string& value) {
        return storage_ready && nvs_set_str(settings, "motors", value.c_str()) == ESP_OK && nvs_commit(settings) == ESP_OK;
    };
#if !CONFIG_ROBOT_SIMULATION
    transport.begin();
#endif
    controller.tick(clock_ms());
    controller.stop_all("startup");
    const BaseType_t created = xTaskCreate(control_task, "robot_control", 6144, nullptr, 8, nullptr);
    configASSERT(created == pdPASS);
    start_wifi(); start_web();
    ESP_LOGI(tag, "%s; control starts stopped", transport.simulation() ? "SIMULATION: no TWAI access" : "REAL MODE: per-motor verification required");
}

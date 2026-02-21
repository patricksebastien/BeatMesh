#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/gptimer.h"
#include "lwip/sockets.h"
#include "abl_link.h"
#include "esp_private/esp_clk.h"
#include "esp_heap_caps.h" 

// --- LovyanGFX Configuration (CYD) ---
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_CYD : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel_instance;
    lgfx::Bus_SPI       _bus_instance;
    lgfx::Light_PWM     _light_instance;
    lgfx::Touch_XPT2046 _touch_instance; 
public:
    LGFX_CYD() {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = VSPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.pin_sclk = 14; cfg.pin_mosi = 13; cfg.pin_miso = 12; cfg.pin_dc = 2;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = 15; cfg.pin_rst = -1;
            cfg.panel_width = 240; cfg.panel_height = 320;
            _panel_instance.config(cfg);
        }
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = 21; cfg.freq = 44100; cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance); 
        }
        { 
            auto cfg = _touch_instance.config();
            cfg.x_min = 200;  cfg.x_max = 3800; 
            cfg.y_min = 3700; cfg.y_max = 200;
            cfg.pin_int = 36; cfg.bus_shared = false; cfg.offset_rotation = 0;
            cfg.spi_host = HSPI_HOST; 
            cfg.pin_sclk = 25; cfg.pin_mosi = 32; cfg.pin_miso = 39; cfg.pin_cs = 33;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
        setPanel(&_panel_instance);
    }
};

static LGFX_CYD *tft = NULL;
static const char *TAG = "BeatMesh_HUB";

// --- Global State ---
#define LED_RED            GPIO_NUM_4
#define BTN_BOOT           GPIO_NUM_0 
#define LED_ON             0           
#define LED_OFF            1
#define MAX_WIFI_RETRIES   10

static EventGroupHandle_t wifi_event_group;
static bool s_is_connected = false;
static bool s_portal_active = false;
static int s_retry_num = 0;
static char s_ssid_name[33] = {0}; 

// --- Color Macros ---
#define C_VINTAGE_BG      tft->color565(36, 72, 85)
#define C_VINTAGE_RUST    tft->color565(230, 72, 51)
#define C_VINTAGE_SAGE    tft->color565(255, 255, 255)
#define C_VINTAGE_BROWN   tft->color565(135, 79, 65)
#define C_VINTAGE_CREAM   tft->color565(251, 233, 208)
#define C_VINTAGE_FERN    tft->color565(109, 151, 115)
#define C_VINTAGE_CREAM_DIM tft->color565(122, 128, 121)
#define C_VINTAGE_AMBER   tft->color565(195, 110, 45)

typedef struct { 
    abl_link link; 
    abl_link_session_state session_state; 
} link_state_t;

static link_state_t g_link_state;
enum midi_mode_t { MODE_LINK = 0, MODE_MIDI_CLK = 1, MODE_MIDI_THRU = 2 };
static volatile int midi_mode = MODE_LINK;

// --- MIDI Clock (UART2 on CYD CN1 connector) ---
#define MIDI_UART       UART_NUM_2
#define MIDI_TX_PIN     27
#define MIDI_RX_PIN     22
#define MIDI_BAUD       31250
#define MIDI_TICK_PERIOD 100  // Timer period in microseconds (10kHz polling)

static uint8_t midi_out_byte;

static void midi_init(void) {
    uart_config_t uart_cfg = {
        .baud_rate = MIDI_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
    };
    uart_param_config(MIDI_UART, &uart_cfg);
    uart_set_pin(MIDI_UART, MIDI_TX_PIN, MIDI_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(MIDI_UART, 512, 256, 0, NULL, 0);
}

static void midi_send_tick(void) {
    midi_out_byte = 0xF8;
    uart_write_bytes(MIDI_UART, (const char *)&midi_out_byte, 1);
}

static void midi_send_start(void) {
    midi_out_byte = 0xFA;
    uart_write_bytes(MIDI_UART, (const char *)&midi_out_byte, 1);
}

static void midi_send_stop(void) {
    midi_out_byte = 0xFC;
    uart_write_bytes(MIDI_UART, (const char *)&midi_out_byte, 1);
}

// Timer ISR: notify MIDI task every 100us
static bool IRAM_ATTR midi_timer_alarm_cb(gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata, void *user_ctx) {
    BaseType_t high_task_wakeup = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)user_ctx, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static void midi_clock_task(void *param) {
    link_state_t *s = (link_state_t *)param;

    // Own session state to avoid races with visual_task
    abl_link_session_state midi_ss = abl_link_create_session_state();

    // Initialize tick counter to current position (avoid burst on start)
    abl_link_capture_audio_session_state(s->link, midi_ss);
    uint64_t now = abl_link_clock_micros(s->link);
    int last_ticks = (int)floor(abl_link_beat_at_time(midi_ss, now, 1.0) * 24.0);
    bool last_playing = abl_link_is_playing(midi_ss);

    // Setup gptimer: 1MHz resolution, 100us alarm period
    gptimer_handle_t timer = NULL;
    gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    gptimer_new_timer(&timer_cfg, &timer);

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = MIDI_TICK_PERIOD,
        .reload_count = 0,
        .flags = { .auto_reload_on_alarm = true },
    };
    gptimer_set_alarm_action(timer, &alarm_cfg);

    gptimer_event_callbacks_t cbs = {
        .on_alarm = midi_timer_alarm_cb,
    };
    gptimer_register_event_callbacks(timer, &cbs, xTaskGetCurrentTaskHandle());
    gptimer_enable(timer);
    gptimer_start(timer);

    bool was_active = true; // Start as if active to avoid re-sync on first loop

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (midi_mode != MODE_LINK) {
            was_active = false;
            continue;
        }

        abl_link_capture_audio_session_state(s->link, midi_ss);
        now = abl_link_clock_micros(s->link);
        bool playing = abl_link_is_playing(midi_ss);

        // Re-sync after returning from MIDI mode to avoid spurious ticks/start/stop
        if (!was_active) {
            last_playing = playing;
            last_ticks = (int)floor(abl_link_beat_at_time(midi_ss, now, 1.0) * 24.0);
            was_active = true;
            continue;
        }

        // Start/stop transitions
        if (playing && !last_playing) {
            midi_send_start();
        } else if (!playing && last_playing) {
            midi_send_stop();
        }
        last_playing = playing;

        if (playing) {
            double beat = abl_link_beat_at_time(midi_ss, now, 1.0);
            int ticks = (int)floor(beat * 24.0);
            if (ticks > last_ticks) {
                midi_send_tick();
            }
            last_ticks = ticks;
        }
    }
}

// --- MIDI IN: forward + drive Link (mode-dependent filtering) ---
static void midi_in_task(void *param) {
    link_state_t *s = (link_state_t *)param;
    abl_link_session_state midi_in_ss = abl_link_create_session_state();

    uint8_t byte;
    int tick_count = 0;
    int64_t first_tick_us = 0;

    while (true) {
        if (midi_mode == MODE_LINK) {
            vTaskDelay(pdMS_TO_TICKS(100)); // Idle when Link is master
            tick_count = 0;
            continue;
        }
        int len = uart_read_bytes(MIDI_UART, &byte, 1, pdMS_TO_TICKS(20));
        if (len <= 0) continue;

        switch (byte) {
        case 0xF8: // Clock tick - always forward + calculate BPM
            uart_write_bytes(MIDI_UART, (const char *)&byte, 1);
            tick_count++;
            if (tick_count == 1) {
                first_tick_us = (int64_t)abl_link_clock_micros(s->link);
            } else if (tick_count >= 25) {
                int64_t now_us = (int64_t)abl_link_clock_micros(s->link);
                int64_t elapsed = now_us - first_tick_us;
                if (elapsed > 0) {
                    double bpm = 60000000.0 / (double)elapsed;
                    if (bpm > 20.0 && bpm < 999.0) {
                        abl_link_capture_audio_session_state(s->link, midi_in_ss);
                        abl_link_set_tempo(midi_in_ss, bpm, (uint64_t)now_us);
                        abl_link_commit_audio_session_state(s->link, midi_in_ss);
                    }
                }
                tick_count = 1;
                first_tick_us = (int64_t)abl_link_clock_micros(s->link);
            }
            break;

        case 0xFA: // Start - always forward + set Link playing
            uart_write_bytes(MIDI_UART, (const char *)&byte, 1);
            abl_link_capture_audio_session_state(s->link, midi_in_ss);
            abl_link_set_is_playing(midi_in_ss, true, abl_link_clock_micros(s->link));
            abl_link_commit_audio_session_state(s->link, midi_in_ss);
            tick_count = 0;
            break;

        case 0xFC: // Stop - always forward + set Link stopped
            uart_write_bytes(MIDI_UART, (const char *)&byte, 1);
            abl_link_capture_audio_session_state(s->link, midi_in_ss);
            abl_link_set_is_playing(midi_in_ss, false, abl_link_clock_micros(s->link));
            abl_link_commit_audio_session_state(s->link, midi_in_ss);
            tick_count = 0;
            break;

        default:
            // MIDI THRU: forward everything. MIDI CLK: drop non-clock bytes.
            if (midi_mode == MODE_MIDI_THRU)
                uart_write_bytes(MIDI_UART, (const char *)&byte, 1);
            break;
        }
    }
}

// --- DNS Hijacker (Optimized for low latency) ---
static void dns_hijacker_task(void *pvParameters) {
    char rx_buffer[128];
    struct sockaddr_in dest_addr = {};
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    // Set socket options for better performance
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Use non-blocking with timeout for efficient polling
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 100000 }; // 100ms timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        struct sockaddr_in src;
        socklen_t len = sizeof(src);
        int r = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&src, &len);

        if (r > 12) {
            // Build minimal DNS response pointing to AP IP (192.168.4.1)
            rx_buffer[2] |= 0x80;  // Response flag
            rx_buffer[3] = 0x80;   // Recursion available
            rx_buffer[7] = 1;      // Answer count

            int p = r;
            rx_buffer[p++] = 0xc0; rx_buffer[p++] = 0x0c;  // Name pointer
            rx_buffer[p++] = 0x00; rx_buffer[p++] = 0x01;  // Type A
            rx_buffer[p++] = 0x00; rx_buffer[p++] = 0x01;  // Class IN
            rx_buffer[p++] = 0x00; rx_buffer[p++] = 0x00;
            rx_buffer[p++] = 0x00; rx_buffer[p++] = 0x3c;  // TTL 60s
            rx_buffer[p++] = 0x00; rx_buffer[p++] = 0x04;  // Data length
            rx_buffer[p++] = 192;  rx_buffer[p++] = 168;
            rx_buffer[p++] = 4;    rx_buffer[p++] = 1;     // 192.168.4.1

            sendto(sock, rx_buffer, p, 0, (struct sockaddr *)&src, sizeof(src));
        }
        // No explicit delay needed - socket timeout provides natural pacing
        taskYIELD();
    }
}

// --- Web Server Handlers ---
static abl_link_session_state g_web_ss; // Dedicated session state for HTTP handlers (init in app_main)

// Custom error handler - silently close instead of sending error response
static esp_err_t custom_error_handler(httpd_req_t *req, httpd_err_code_t err) {
    // Just close the connection without sending error - prevents crash on malformed requests
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t aggressive_close_handler(httpd_req_t *req) {
    const char *resp = "Microsoft Connect Test"; 
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close"); 
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t captive_portal_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t set_transport_handler(httpd_req_t *req) {
    if (midi_mode != MODE_LINK) return captive_portal_redirect(req);
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char state_val[16];
        if (httpd_query_key_value(buf, "state", state_val, sizeof(state_val)) == ESP_OK) {
            bool is_playing = (strcmp(state_val, "play") == 0);
            abl_link_capture_app_session_state(g_link_state.link, g_web_ss);
            uint64_t now = abl_link_clock_micros(g_link_state.link);
            abl_link_set_is_playing(g_web_ss, is_playing, now);
            abl_link_commit_app_session_state(g_link_state.link, g_web_ss);
        }
    }
    return captive_portal_redirect(req);
}

static esp_err_t set_bpm_handler(httpd_req_t *req) {
    if (midi_mode != MODE_LINK) return captive_portal_redirect(req);
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char bpm_val[16];
        if (httpd_query_key_value(buf, "bpm", bpm_val, sizeof(bpm_val)) == ESP_OK) {
            double nb = atof(bpm_val);
            if (nb > 20.0 && nb < 999.0) {
                abl_link_capture_app_session_state(g_link_state.link, g_web_ss);
                abl_link_set_tempo(g_web_ss, nb, abl_link_clock_micros(g_link_state.link));
                abl_link_commit_app_session_state(g_link_state.link, g_web_ss);
            }
        }
    }
    return captive_portal_redirect(req);
}

static esp_err_t save_handler(httpd_req_t *req) {
    char buf[512];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char s[33]={0}, p[65]={0};
        httpd_query_key_value(buf, "ssid", s, 32);
        httpd_query_key_value(buf, "pass", p, 64);
        
        wifi_config_t wc = {};
        strncpy((char*)wc.sta.ssid, s, 32);
        strncpy((char*)wc.sta.password, p, 64);
        
        esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        esp_wifi_set_mode(WIFI_MODE_STA); 
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_restart(); 
    }
    return ESP_OK;
}

static esp_err_t get_handler(httpd_req_t *req) {
    abl_link_capture_app_session_state(g_link_state.link, g_web_ss);
    double current_bpm = abl_link_tempo(g_web_ss);
    bool is_playing = abl_link_is_playing(g_web_ss);

    char html_buf[2560];
    const char* play_style = is_playing ? "border:2px solid #FFF;" : "";
    const char* stop_style = !is_playing ? "border:2px solid #FFF;" : "";

    static const char *mode_names[] = { "LINK", "MIDI CLK", "MIDI THRU" };
    const char *mode_label = mode_names[midi_mode];
    bool midi_is_master = (midi_mode != MODE_LINK);
    const char *disabled = midi_is_master ? " disabled" : "";
    const char *midi_banner = midi_is_master
        ? "<p style='background:#874F41;padding:10px;border-radius:8px;font-size:14px;margin-bottom:15px'>"
          "External MIDI clock &mdash; controls disabled</p>"
        : "";

    snprintf(html_buf, sizeof(html_buf),
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=0'>"
        "<title>BeatMesh</title>"
        "<style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#244855;color:#FBE9D0;margin:0;padding:20px;text-align:center}"
        ".container{max-width:400px;margin:0 auto}"
        ".card{background:#2c5563;border-radius:12px;padding:20px;margin-bottom:20px;box-shadow:0 4px 10px rgba(0,0,0,0.5);border:1px solid #874F41}"
        "h1{color:#E64833;margin:0 0 15px 0;font-size:26px;letter-spacing:1px}"
        "h2{font-size:16px;color:#90AEAD;border-bottom:1px solid #874F41;padding-bottom:10px;margin-bottom:15px}"
        "input{width:100%%;box-sizing:border-box;padding:15px;margin:8px 0 20px 0;background:#1e3b46;border:1px solid #90AEAD;color:#FBE9D0;border-radius:8px;font-size:18px}"
        "input:focus{border-color:#E64833;outline:none}"
        ".btn{width:100%%;background:#E64833;color:#FBE9D0;font-weight:bold;border:none;padding:15px;border-radius:8px;font-size:16px;cursor:pointer;transition:0.2s}"
        ".btn:active{background:#874F41;transform:scale(0.98)}"
        ".btn:disabled{opacity:0.4;cursor:not-allowed}"
        ".row{display:flex;gap:10px;margin-top:15px;}"
        ".mode{font-size:12px;color:#90AEAD;margin-bottom:10px}"
        "</style></head>"
        "<body><div class='container'>"
            "<div class='card'>"
                "<h1>BeatMesh</h1>"
                "<div class='mode'>Clock: %s</div>"
                "%s"
                "<form action='/set_bpm' method='get'>"
                    "<h2>TEMPO: %.1f BPM</h2>"
                    "<input type='number' name='bpm' value='%.1f' step='0.1' inputmode='decimal'%s>"
                    "<input type='submit' class='btn' value='UPDATE BPM'%s>"
                "</form>"
                "<form action='/transport' method='get' class='row'>"
                    "<button name='state' value='play' class='btn' style='background:#6D9773;color:#FFFFFF;font-size:24px;%s'%s>&#9658;</button>"
                    "<button name='state' value='stop' class='btn' style='background:#874F41;color:#FBE9D0;font-size:24px;%s'%s>&#9632;</button>"
                "</form>"
            "</div>"
            "<div class='card'>"
                "<form action='/save' method='get'>"
                    "<h2>WIFI SETUP</h2>"
                    "<input name='ssid' placeholder='SSID Name'>"
                    "<input type='password' name='pass' placeholder='Password'>"
                    "<input type='submit' class='btn' value='CONNECT'>"
                "</form>"
            "</div>"
        "</div></body></html>",
        mode_label, midi_banner,
        current_bpm, current_bpm, disabled, disabled,
        play_style, disabled, stop_style, disabled
    );

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, html_buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- Start Access Point + Captive Portal ---
void start_setup_portal() {
    if (s_portal_active) return;
    s_portal_active = true;

    wifi_config_t ap_cfg = {};
    strcpy((char*)ap_cfg.ap.ssid, "BeatMesh");
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 10;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.channel = 6;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Start DNS hijacker for captive portal redirect (Core 0 with WiFi)
    xTaskCreatePinnedToCore(dns_hijacker_task, "dns", 4096, NULL, 5, NULL, 0);

    // Start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_handler };
        httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler };
        httpd_uri_t save = { .uri = "/save", .method = HTTP_GET, .handler = save_handler };
        httpd_uri_t set_bpm = { .uri = "/set_bpm", .method = HTTP_GET, .handler = set_bpm_handler };
        httpd_uri_t transport = { .uri = "/transport", .method = HTTP_GET, .handler = set_transport_handler };
        httpd_uri_t connecttest = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = aggressive_close_handler };
        httpd_uri_t redirect = { .uri = "/redirect", .method = HTTP_GET, .handler = captive_portal_redirect };
        httpd_uri_t hotspot = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_portal_redirect };
        httpd_uri_t gen204 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_redirect };

        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &favicon);
        httpd_register_uri_handler(server, &save);
        httpd_register_uri_handler(server, &set_bpm);
        httpd_register_uri_handler(server, &transport);
        httpd_register_uri_handler(server, &connecttest);
        httpd_register_uri_handler(server, &redirect);
        httpd_register_uri_handler(server, &hotspot);
        httpd_register_uri_handler(server, &gen204);

        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, custom_error_handler);
        httpd_register_err_handler(server, HTTPD_400_BAD_REQUEST, custom_error_handler);
        httpd_register_err_handler(server, HTTPD_500_INTERNAL_SERVER_ERROR, custom_error_handler);

        ESP_LOGW(TAG, "AP Started: BeatMesh - Portal at 192.168.4.1");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

// --- Visual & Link Task ---
static void visual_task(void *param) {
    link_state_t *s = (link_state_t *)param;
    int last_beat = -1;
    double last_bpm = 0;
    size_t last_peers = 999;
    uint32_t last_touch = 0;
    bool last_connection_state = false; 
    bool last_portal_state = false;
    int last_btn_state = 1;
    uint32_t last_btn_time = 0;
    int last_midi_mode = MODE_LINK;

    tft = new LGFX_CYD();
    tft->init(); 
    tft->setRotation(1); 
    tft->setBrightness(200); 
    tft->fillScreen(C_VINTAGE_BG);

    gpio_reset_pin(LED_RED); gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);
    gpio_set_direction(BTN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_BOOT, GPIO_PULLUP_ONLY);

    tft->setTextSize(2);
    tft->setTextColor(C_VINTAGE_CREAM);
    tft->setTextDatum(middle_center);
    tft->fillRoundRect(10, 10, 70, 50, 5, C_VINTAGE_RUST);
    tft->fillRect(35, 25, 20, 20, C_VINTAGE_CREAM); 
    tft->fillRoundRect(90, 10, 70, 50, 5, C_VINTAGE_FERN); 
    tft->fillTriangle(115, 25, 115, 45, 135, 35, TFT_WHITE); 
    tft->fillRoundRect(170, 10, 65, 50, 5, C_VINTAGE_AMBER);
    tft->drawString("-", 202, 35);
    tft->fillRoundRect(245, 10, 65, 50, 5, C_VINTAGE_AMBER);
    tft->drawString("+", 277, 35);

    // Mode toggle button
    tft->fillRoundRect(10, 100, 300, 40, 5, C_VINTAGE_FERN);
    tft->setTextColor(C_VINTAGE_CREAM);
    tft->drawString("Clock: LINK", 160, 120);
    tft->setTextDatum(top_left);

    while (true) {
        int btn_state = gpio_get_level(BTN_BOOT);
        if (btn_state == 0 && last_btn_state == 1 && (esp_log_timestamp() - last_btn_time > 300)) {
            abl_link_capture_app_session_state(s->link, s->session_state);
            bool current_playing = abl_link_is_playing(s->session_state);
            uint64_t now = abl_link_clock_micros(s->link);
            abl_link_set_is_playing(s->session_state, !current_playing, now);
            abl_link_commit_app_session_state(s->link, s->session_state);
            last_btn_time = esp_log_timestamp();
        }
        last_btn_state = btn_state;

        uint16_t tx, ty;
        if (tft->getTouch(&tx, &ty)) {
            if (esp_log_timestamp() - last_touch > 300) {
                abl_link_capture_app_session_state(s->link, s->session_state);
                uint64_t now = abl_link_clock_micros(s->link);

                if (ty >= 100 && ty < 140) {
                    // Mode cycle: LINK → MIDI CLK → MIDI THRU → LINK
                    midi_mode = (midi_mode + 1) % 3;
                    last_touch = esp_log_timestamp();
                }
                else if (ty < 70) {
                    if (tx < 85) {
                        abl_link_set_is_playing(s->session_state, false, now);
                        tft->drawRoundRect(10, 10, 70, 50, 5, C_VINTAGE_CREAM); 
                    }
                    else if (tx < 165) {
                        abl_link_set_is_playing(s->session_state, true, now);
                        tft->drawRoundRect(90, 10, 70, 50, 5, C_VINTAGE_CREAM);
                    }
                    else if (tx < 240) {
                        double new_bpm = abl_link_tempo(s->session_state) - 1.0;
                        abl_link_set_tempo(s->session_state, new_bpm, now);
                        tft->drawRoundRect(170, 10, 65, 50, 5, C_VINTAGE_CREAM);
                    }
                    else {
                        double new_bpm = abl_link_tempo(s->session_state) + 1.0;
                        abl_link_set_tempo(s->session_state, new_bpm, now);
                        tft->drawRoundRect(245, 10, 65, 50, 5, C_VINTAGE_CREAM);
                    }
                    abl_link_commit_app_session_state(s->link, s->session_state);
                    last_touch = esp_log_timestamp();
                    vTaskDelay(pdMS_TO_TICKS(50));
                    tft->drawRoundRect(10, 10, 70, 50, 5, C_VINTAGE_BG); 
                    tft->drawRoundRect(90, 10, 70, 50, 5, C_VINTAGE_BG); 
                    tft->drawRoundRect(170, 10, 65, 50, 5, C_VINTAGE_BG); 
                    tft->drawRoundRect(245, 10, 65, 50, 5, C_VINTAGE_BG); 
                }
            }
        }

        abl_link_capture_app_session_state(s->link, s->session_state);
        uint64_t time = abl_link_clock_micros(s->link);
        bool is_playing = abl_link_is_playing(s->session_state);
        double phase = abl_link_phase_at_time(s->session_state, time, 4.0);
        int cur_beat = (int)floor(phase);
        double cur_bpm = abl_link_tempo(s->session_state);
        size_t peers = abl_link_num_peers(s->link);

        int rect_x[] = {10, 90, 170, 245};
        int rect_w[] = {70, 70, 65, 65};

        if (is_playing) {
            if (cur_beat != last_beat) {
                for (int i=0; i<4; i++) {
                    uint16_t color = (i == cur_beat) ? C_VINTAGE_CREAM : C_VINTAGE_CREAM_DIM;
                    tft->fillRoundRect(rect_x[i], 70, rect_w[i], 12, 3, color);
                }
                gpio_set_level(LED_RED, (cur_beat == 0) ? LED_ON : LED_OFF);
                last_beat = cur_beat;
            }
        } else {
            if (last_beat != -2) {
                 for (int i=0; i<4; i++) tft->fillRoundRect(rect_x[i], 70, rect_w[i], 12, 3, C_VINTAGE_CREAM_DIM);
                 gpio_set_level(LED_RED, LED_OFF);
                 last_beat = -2; 
            }
        }

        if (fabs(cur_bpm - last_bpm) > 0.1) {
            tft->fillRect(10, 160, 160, 50, C_VINTAGE_BG); 
            tft->setTextColor(C_VINTAGE_CREAM); tft->setTextSize(4);
            
            // --- FIX: NO DECIMAL ---
            char bstr[16]; sprintf(bstr, "%.0f", cur_bpm);
            tft->drawString(bstr, 10, 172); 
            tft->setTextSize(2); 
            tft->drawString("BPM", 92, 185); 
            last_bpm = cur_bpm;
        }

        if (peers != last_peers || last_peers == 999) {
            tft->fillRect(180, 180, 140, 30, C_VINTAGE_BG);
            tft->setTextColor(C_VINTAGE_SAGE); tft->setTextSize(2);
            char pstr[20]; sprintf(pstr, "PEERS: %zu", peers);
            tft->drawRightString(pstr, 310, 185);
            last_peers = peers;
        }

        // Mode toggle button redraw
        if (midi_mode != last_midi_mode) {
            static const uint16_t mode_colors[] = { C_VINTAGE_FERN, C_VINTAGE_AMBER, C_VINTAGE_RUST };
            static const char *mode_labels[] = { "Clock: LINK", "Clock: MIDI", "Clock: THRU" };
            tft->fillRoundRect(10, 100, 300, 40, 5, mode_colors[midi_mode]);
            tft->setTextColor(C_VINTAGE_CREAM); tft->setTextSize(2);
            tft->setTextDatum(middle_center);
            tft->drawString(mode_labels[midi_mode], 160, 120);
            tft->setTextDatum(top_left);
            last_midi_mode = midi_mode;
        }

        // Status bar: alternate with router warning when peers > 2 in AP mode
        static bool show_warning = false;
        static uint32_t last_status_toggle = 0;
        bool needs_redraw = (s_is_connected != last_connection_state || s_portal_active != last_portal_state);

        if (peers > 2 && s_portal_active && !s_is_connected) {
            if (esp_log_timestamp() - last_status_toggle > 2000) {
                show_warning = !show_warning;
                last_status_toggle = esp_log_timestamp();
                needs_redraw = true;
            }
        } else {
            if (show_warning) { show_warning = false; needs_redraw = true; }
        }

        if (needs_redraw) {
            tft->fillRect(0, 215, 320, 25, C_VINTAGE_BG);
            if (show_warning) {
                tft->fillRect(0, 215, 320, 25, C_VINTAGE_RUST);
                tft->setTextColor(C_VINTAGE_CREAM); tft->setTextSize(2);
                tft->drawCenterString("Use a router for >2 peers", 160, 220);
            } else if (s_is_connected) {
                tft->fillRect(0, 215, 320, 25, C_VINTAGE_CREAM);
                tft->setTextColor(C_VINTAGE_BG); tft->setTextSize(2);
                char status_msg[64];
                snprintf(status_msg, sizeof(status_msg), "LINK: %s", s_ssid_name);
                tft->drawCenterString(status_msg, 160, 220);
            } else if (s_portal_active) {
                tft->fillRect(0, 215, 320, 25, C_VINTAGE_CREAM);
                tft->setTextColor(C_VINTAGE_BG); tft->setTextSize(2);
                tft->drawCenterString("AP: BeatMesh (192.168.4.1)", 160, 220);
            }
            last_connection_state = s_is_connected;
            last_portal_state = s_portal_active;
        }


        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        if (s_retry_num < MAX_WIFI_RETRIES) { 
            esp_wifi_connect(); 
            s_retry_num++; 
        } else {
            start_setup_portal();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_is_connected = true;
        s_retry_num = 0;
        wifi_config_t conf;
        esp_wifi_get_config(WIFI_IF_STA, &conf);
        strncpy(s_ssid_name, (char*)conf.sta.ssid, 32);
    }
}

extern "C" void app_main(void) {
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init();
    }

    esp_netif_init(); 
    esp_event_loop_create_default();
    wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta(); 
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); 
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    g_link_state.link = abl_link_create(120.0);
    g_link_state.session_state = abl_link_create_session_state();
    g_web_ss = abl_link_create_session_state();
    abl_link_enable(g_link_state.link, true);
    abl_link_enable_start_stop_sync(g_link_state.link, true);

    uint64_t now = abl_link_clock_micros(g_link_state.link);
    abl_link_capture_app_session_state(g_link_state.link, g_link_state.session_state);
    abl_link_set_is_playing(g_link_state.session_state, true, now);
    abl_link_commit_app_session_state(g_link_state.link, g_link_state.session_state);

    // Visual task: Core 0 (display + WiFi), Link/MIDI on Core 1
    // Priority 5: Below MIDI tasks, above idle
    xTaskCreatePinnedToCore(visual_task, "visual", 8192, &g_link_state, 5, NULL, 0);

    // MIDI tasks: Core 1 (alongside Link), high priority for timing precision
    midi_init();
    xTaskCreatePinnedToCore(midi_clock_task, "midi_out", 4096, &g_link_state, 10, NULL, 1);
    xTaskCreatePinnedToCore(midi_in_task, "midi_in", 4096, &g_link_state, 10, NULL, 1);

    wifi_config_t sta_cfg;
    esp_wifi_get_config(WIFI_IF_STA, &sta_cfg);
    if (strlen((char*)sta_cfg.sta.ssid) == 0) start_setup_portal();
    else { esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start(); esp_wifi_set_ps(WIFI_PS_NONE); }
}
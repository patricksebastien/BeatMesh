#include <stdio.h>
#include <stdint.h>
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
#include "esp_timer.h"
#include "driver/ledc.h"
#include "lwip/sockets.h"
#include "abl_link.h"
#include "esp_private/esp_clk.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "mdns.h"

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
static char s_ip_str[16] = {0};            // STA IP, shown on TFT + web UI
static httpd_handle_t s_http_server = NULL; // web server runs in AP portal and STA mode

// Firmware version string; GIT_VERSION is injected at build time (git_version.py)
#ifndef GIT_VERSION
#define GIT_VERSION "unknown"
#endif

// --- Color Macros ---
#define C_VINTAGE_BG      tft->color565(36, 72, 85)
#define C_VINTAGE_RUST    tft->color565(230, 72, 51)
#define C_VINTAGE_SAGE    tft->color565(255, 255, 255)
#define C_VINTAGE_BROWN   tft->color565(135, 79, 65)
#define C_VINTAGE_CREAM   tft->color565(251, 233, 208)
#define C_VINTAGE_FERN    tft->color565(109, 151, 115)
#define C_VINTAGE_CREAM_DIM tft->color565(122, 128, 121)
#define C_VINTAGE_AMBER   tft->color565(195, 110, 45)
#define C_VINTAGE_BG_LT   tft->color565(55, 95, 108)

// --- Mode/PPQN button layout (y=100, h=40) ---
static const int mode_btn_x[] = { 10, 86, 162, 238 };
static const int mode_btn_w[] = { 71, 71, 71, 72 };
static const int ppqn_btn_x = 162;   // single cycling button (tap to advance), right after METRONOME
static const int ppqn_btn_w = 71;
static const char *mode_labels[] = { "LINK", "MIDI", "THR", "CV" };
static const char *ppqn_labels[] = { "1", "2", "4", "8", "24", "48" };

typedef struct { 
    abl_link link; 
    abl_link_session_state session_state; 
} link_state_t;

static link_state_t g_link_state;
enum midi_mode_t { MODE_LINK = 0, MODE_MIDI_CLK = 1, MODE_MIDI_THRU = 2, MODE_CV = 3, MODE_COUNT = 4 };
static volatile int midi_mode = MODE_LINK;

// Shared PPQN setting for CV in/out — single button cycles through these
static volatile int cv_ppqn = 4;
static const int cv_ppqn_options[] = { 1, 2, 4, 8, 24, 48 };
static const int cv_ppqn_count = 6;
static volatile int cv_ppqn_idx = 2; // index into cv_ppqn_options (default: 4 PPQN)

// Metronome (LEDC PWM on GPIO 26 → onboard amp/speaker)
static volatile bool metronome_enabled = false;
#define METRO_PIN GPIO_NUM_26

// --- MIDI Monitor (small UI box, left of metronome button) ---
static portMUX_TYPE g_mon_mux = portMUX_INITIALIZER_UNLOCKED;
static char g_midi_mon_l1[10] = "MONITOR:";
static char g_midi_mon_l2[12] = "No midi in";
static volatile uint32_t g_midi_mon_seq = 0;
static volatile int64_t g_midi_mon_last_us = 0;
// Parser running-status state (only touched by midi_in_task)
static uint8_t g_mon_status = 0;
static uint8_t g_mon_data[2];
static uint8_t g_mon_data_idx = 0;
static uint8_t g_mon_data_needed = 0;

// --- MIDI Clock (UART2 routed to CYD's UART0 header pins via GPIO matrix) ---
// Board net ESP32_TX = GPIO 1 (U0TXD), ESP32_RX = GPIO 3 (U0RXD).
// Using UART_NUM_2 on these pins avoids touching the boot-ROM UART0 driver,
// but the GPIO mux will steal GPIO1/3 from UART0 — USB serial console stops
// working once midi_init() runs. Disconnect/close the USB serial monitor when
// feeding MIDI in, otherwise the host CDC chip will drive GPIO 3 against the opto.
#define MIDI_UART       UART_NUM_2
#define MIDI_TX_PIN     1
#define MIDI_RX_PIN     3
#define MIDI_BAUD       31250
#define MIDI_TICK_PERIOD 100  // Timer period in microseconds (10kHz polling)

// --- CV Clock input (GPIO35, input-only pin) ---
#define CV_CLK_PIN      GPIO_NUM_35

static volatile int32_t cv_interval_us = 0;  // atomic on 32-bit MCU
static TaskHandle_t cv_task_handle = NULL;

static void IRAM_ATTR cv_clock_isr(void *arg) {
    static int64_t last_edge = 0;  // ISR-local, no torn-read risk
    int64_t now = esp_timer_get_time();
    int64_t delta = now - last_edge;
    last_edge = now;
    if (delta > 0 && delta < INT32_MAX)
        cv_interval_us = (int32_t)delta;  // atomic 32-bit write
    BaseType_t wake = pdFALSE;
    if (cv_task_handle)
        vTaskNotifyGiveFromISR(cv_task_handle, &wake);
    if (wake) portYIELD_FROM_ISR();
}

static void cv_clock_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CV_CLK_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(CV_CLK_PIN, cv_clock_isr, NULL);
}

// --- CV Clock output (Link → CV gates/triggers, always active) ---
// Dedicated pins from SD card connector (no conflicts)
#define CV_OUT_CLK_PIN      GPIO_NUM_22  // CV Clock (CYD IO22, not a strapping pin)
#define CV_OUT_RESET_PIN    GPIO_NUM_27  // CYD IO27 (expansion header) → CV Reset
// #define CV_OUT_RUN_PIN      GPIO_NUM_18  // SD_SCK → CV Play/Stop (disabled: modular gear uses Clock+Reset only)
#define CV_PULSE_WIDTH_US   5000         // 5ms trigger pulse

static void cv_out_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CV_OUT_CLK_PIN) | (1ULL << CV_OUT_RESET_PIN), // | (1ULL << CV_OUT_RUN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(CV_OUT_CLK_PIN, 0);
    gpio_set_level(CV_OUT_RESET_PIN, 0);
    // gpio_set_level(CV_OUT_RUN_PIN, 0);
}

static void metronome_init(void) {
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1200,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg = {
        .gpio_num = METRO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_cfg);
}

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
    // Default RXFIFO threshold is 120 bytes = ~38ms of silence before ISR fires at MIDI baud.
    // Set to 1 so the ISR fires on the very first byte (~us wakeup).
    uart_set_rx_full_threshold(MIDI_UART, 1);
}

static void midi_send_tick(void) {
    const uint8_t b = 0xF8;
    uart_write_bytes(MIDI_UART, (const char *)&b, 1);
}

static void midi_send_start(void) {
    const uint8_t b = 0xFA;
    uart_write_bytes(MIDI_UART, (const char *)&b, 1);
}

static void midi_send_stop(void) {
    const uint8_t b = 0xFC;
    uart_write_bytes(MIDI_UART, (const char *)&b, 1);
}

static void midi_send_continue(void) {
    const uint8_t b = 0xFB;
    uart_write_bytes(MIDI_UART, (const char *)&b, 1);
}

static void midi_mon_publish(const char *l1, const char *l2) {
    portENTER_CRITICAL(&g_mon_mux);
    strncpy(g_midi_mon_l1, l1, sizeof(g_midi_mon_l1));
    g_midi_mon_l1[sizeof(g_midi_mon_l1) - 1] = 0;
    strncpy(g_midi_mon_l2, l2, sizeof(g_midi_mon_l2));
    g_midi_mon_l2[sizeof(g_midi_mon_l2) - 1] = 0;
    g_midi_mon_last_us = esp_timer_get_time();
    g_midi_mon_seq++;
    portEXIT_CRITICAL(&g_mon_mux);
}

static void midi_mon_byte(uint8_t b) {
    // System realtime: single-byte, doesn't disturb running status
    if (b >= 0xF8) {
        const char *name = NULL;
        switch (b) {
            case 0xF8: name = "CLK";   break;
            case 0xFA: name = "START"; break;
            case 0xFB: name = "CONT";  break;
            case 0xFC: name = "STOP";  break;
            case 0xFF: name = "RSET";  break;
        }
        if (name) midi_mon_publish(name, "");
        return;
    }
    // Status byte
    if (b >= 0x80) {
        // if (b == 0xF6) { midi_mon_publish("TUNE", ""); g_mon_status = 0; return; }
        g_mon_status = b;
        g_mon_data_idx = 0;
        switch (b & 0xF0) {
            case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: g_mon_data_needed = 2; break;
            case 0xC0: case 0xD0: g_mon_data_needed = 1; break;
            case 0xF0:
                if (b == 0xF1 || b == 0xF3)      g_mon_data_needed = 1;
                else if (b == 0xF2)              g_mon_data_needed = 2;
                else                             g_mon_data_needed = 0;
                break;
            default: g_mon_data_needed = 0; break;
        }
        return;
    }
    // Data byte
    if (g_mon_status == 0 || g_mon_data_needed == 0) return;
    g_mon_data[g_mon_data_idx++] = b;
    if (g_mon_data_idx < g_mon_data_needed) return;
    g_mon_data_idx = 0;

    char l1[10], l2[12];
    if (g_mon_status >= 0xF0) {
        switch (g_mon_status) {
            // case 0xF1: snprintf(l1, sizeof(l1), "MTC");  snprintf(l2, sizeof(l2), "%d", g_mon_data[0]); break;
            // case 0xF2: { int p = (g_mon_data[1] << 7) | g_mon_data[0];
            //              snprintf(l1, sizeof(l1), "SPP");  snprintf(l2, sizeof(l2), "%d", p); break; }
            // case 0xF3: snprintf(l1, sizeof(l1), "SONG"); snprintf(l2, sizeof(l2), "%d", g_mon_data[0]); break;
            default: g_mon_status = 0; return;
        }
        g_mon_status = 0;
    } else {
        uint8_t st = g_mon_status & 0xF0;
        uint8_t ch = (g_mon_status & 0x0F) + 1;
        switch (st) {
            case 0x80:
                snprintf(l1, sizeof(l1), "OFF c%d", ch);
                snprintf(l2, sizeof(l2), "%d v%d", g_mon_data[0], g_mon_data[1]);
                break;
            case 0x90:
                snprintf(l1, sizeof(l1), g_mon_data[1] ? "NOTE c%d" : "OFF c%d", ch);
                snprintf(l2, sizeof(l2), "%d v%d", g_mon_data[0], g_mon_data[1]);
                break;
            // case 0xA0:
            //     snprintf(l1, sizeof(l1), "AT c%d", ch);
            //     snprintf(l2, sizeof(l2), "%d:%d", g_mon_data[0], g_mon_data[1]);
            //     break;
            case 0xB0:
                snprintf(l1, sizeof(l1), "CC c%d", ch);
                snprintf(l2, sizeof(l2), "%d:%d", g_mon_data[0], g_mon_data[1]);
                break;
            case 0xC0:
                snprintf(l1, sizeof(l1), "PRG c%d", ch);
                snprintf(l2, sizeof(l2), "%d", g_mon_data[0]);
                break;
            // case 0xD0:
            //     snprintf(l1, sizeof(l1), "CHP c%d", ch);
            //     snprintf(l2, sizeof(l2), "%d", g_mon_data[0]);
            //     break;
            // case 0xE0: {
            //     int pb = ((g_mon_data[1] << 7) | g_mon_data[0]) - 8192;
            //     snprintf(l1, sizeof(l1), "PB c%d", ch);
            //     snprintf(l2, sizeof(l2), "%d", pb);
            //     break;
            // }
            default: return;
        }
    }
    midi_mon_publish(l1, l2);
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

    // Initialize tick counters to current position (avoid burst on start)
    abl_link_capture_audio_session_state(s->link, midi_ss);
    uint64_t now = abl_link_clock_micros(s->link);
    double beat0 = abl_link_beat_at_time(midi_ss, now, 1.0);
    int last_midi_ticks = (int)floor(beat0 * 24.0);
    int last_cv_ticks = (int)floor(beat0 * (double)cv_ppqn);
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

    bool midi_was_active = true;

    // CV out pulse end timestamps (0 = no active pulse)
    int64_t cv_clk_pulse_end = 0;
    int64_t cv_reset_pulse_end = 0;
    bool cv_last_playing = last_playing;
    int cached_cv_ppqn = cv_ppqn;  // track ppqn changes to re-snap ticks

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        abl_link_capture_audio_session_state(s->link, midi_ss);
        now = abl_link_clock_micros(s->link);
        bool playing = abl_link_is_playing(midi_ss);
        double beat = abl_link_beat_at_time(midi_ss, now, 1.0);
        int64_t now_us = (int64_t)esp_timer_get_time();

        // ---- CV OUT: always active, independent of mode ----

        // Re-snap CV tick counter if PPQN changed (prevents freeze/burst)
        int cur_ppqn = cv_ppqn;
        if (cur_ppqn != cached_cv_ppqn) {
            last_cv_ticks = (int)floor(beat * (double)cur_ppqn);
            cached_cv_ppqn = cur_ppqn;
        }

        // End active pulses that exceeded their width
        if (cv_clk_pulse_end && now_us >= cv_clk_pulse_end) {
            gpio_set_level(CV_OUT_CLK_PIN, 0);
            cv_clk_pulse_end = 0;
        }
        if (cv_reset_pulse_end && now_us >= cv_reset_pulse_end) {
            gpio_set_level(CV_OUT_RESET_PIN, 0);
            cv_reset_pulse_end = 0;
        }

        // Start/stop: reset trigger on start (run gate disabled)
        if (playing && !cv_last_playing) {
            // gpio_set_level(CV_OUT_RUN_PIN, 1);
            gpio_set_level(CV_OUT_RESET_PIN, 1);
            cv_reset_pulse_end = now_us + CV_PULSE_WIDTH_US;
            last_cv_ticks = (int)floor(beat * (double)cur_ppqn);
        } else if (!playing && cv_last_playing) {
            // gpio_set_level(CV_OUT_RUN_PIN, 0);
            gpio_set_level(CV_OUT_CLK_PIN, 0);
            cv_clk_pulse_end = 0;
        }
        cv_last_playing = playing;

        // Clock tick pulses (uses shared cv_ppqn setting)
        if (playing) {
            int ticks = (int)floor(beat * (double)cur_ppqn);
            if (ticks > last_cv_ticks) {
                gpio_set_level(CV_OUT_CLK_PIN, 1);
                cv_clk_pulse_end = now_us + CV_PULSE_WIDTH_US;
            }
            last_cv_ticks = ticks;
        }

        // ---- MIDI OUT: active in LINK and CV modes ----
        bool midi_active = (midi_mode == MODE_LINK || midi_mode == MODE_CV);

        if (!midi_active) {
            midi_was_active = false;
            continue;
        }

        // Re-sync after returning from MIDI-in mode
        if (!midi_was_active) {
            last_playing = playing;
            last_midi_ticks = (int)floor(beat * 24.0);
            midi_was_active = true;
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
            int ticks = (int)floor(beat * 24.0);
            if (ticks > last_midi_ticks) {
                midi_send_tick();
            }
            last_midi_ticks = ticks;
        }
    }
}

// Tempo update from a MIDI-clock-derived measurement. Three regimes:
//   * First call after boot or post-silence (*first == true): snap. Avoids
//     spending tens of seconds slewing from a stale Link tempo to whatever
//     the source actually is.
//   * |measured - smoothed| >= SNAP_THRESHOLD: snap. Anything that big after
//     4-beat averaging is much more likely to be a real tempo change than
//     measurement noise (residual σ is ~1 BPM, so ≥5 BPM is rare enough that
//     treating it as intentional gives much better UX than slowly slewing).
//   * Otherwise: EMA at ALPHA. The 1/ALPHA exponential time constant is what
//     attenuates the residual jitter into a stable rounded display; with
//     ALPHA=0.15 and a 2.4 s measurement window that's ~16 s to lock in on a
//     small tempo nudge.
//
// *bpm_smoothed holds the high-precision running estimate. We round only when
// writing into Link, so what gear/peers/UI all see is an integer BPM matching
// what the source is set to. Smoothing itself runs in continuous space —
// quantizing the state would create a systematic bias locking Link onto
// whichever side of 0.5 the first snap landed on.
#define MIDI_TEMPO_EMA_ALPHA       0.15
#define MIDI_TEMPO_SNAP_THRESHOLD  5.0
static void midi_apply_tempo_slewed(link_state_t *s, abl_link_session_state ss,
                                    double measured_bpm, uint64_t now_us,
                                    bool *first, double *bpm_smoothed) {
    if (measured_bpm <= 20.0 || measured_bpm >= 999.0) return;
    if (*first) {
        *bpm_smoothed = measured_bpm;
        *first = false;
    } else {
        double diff = measured_bpm - *bpm_smoothed;
        if (fabs(diff) >= MIDI_TEMPO_SNAP_THRESHOLD) {
            *bpm_smoothed = measured_bpm;
        } else {
            *bpm_smoothed += MIDI_TEMPO_EMA_ALPHA * diff;
        }
    }
    abl_link_capture_app_session_state(s->link, ss);
    abl_link_set_tempo(ss, round(*bpm_smoothed), now_us);
    abl_link_commit_app_session_state(s->link, ss);
}

// --- MIDI IN: forward + drive Link (mode-dependent filtering) ---
static void midi_in_task(void *param) {
    link_state_t *s = (link_state_t *)param;
    abl_link_session_state midi_in_ss = abl_link_create_session_state();

    uint8_t buf[128];
    int tick_count = 0;
    int64_t first_tick_us = 0;
    // Time of the most recent 0xF8 byte. Used to detect a long silence in the
    // clock stream (gear stopped, song change), at which point we discard the
    // partial measurement window and snap the next BPM measurement instead of
    // slewing — which would otherwise spend ~30s converging to the new tempo.
    int64_t last_tick_us = 0;
    // True until the first BPM measurement is made (boot or post-silence). Snap
    // that one, then slew afterwards. Toggled by the silence-detection logic
    // below and consumed by midi_apply_tempo_slewed().
    bool first_tempo_measurement = true;
    // High-precision smoothed BPM estimate. Lives in continuous space so the
    // EMA can converge cleanly; midi_apply_tempo_slewed() rounds when writing
    // into Link. Initialised on the first snap.
    double bpm_smoothed = 0.0;

    // CV clock state
    cv_task_handle = xTaskGetCurrentTaskHandle();
    bool cv_playing = false;
    int64_t cv_last_pulse_us = 0;

    while (true) {
        if (midi_mode == MODE_LINK) {
            // Passive MIDI sniff for monitor display: blocks in the UART driver
            // until bytes arrive (ISR-driven, ~zero idle CPU) or 100ms passes.
            int len = uart_read_bytes(MIDI_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
            for (int i = 0; i < len; i++) midi_mon_byte(buf[i]);
            tick_count = 0;
            cv_playing = false;
            continue;
        }

        // --- CV Clock input: edge-triggered BPM → Link ---
        if (midi_mode == MODE_CV) {
            int ppqn = cv_ppqn;

            // Real timeout for clock-stopped detection (independent of poll rate)
            int64_t cv_timeout_us = (cv_interval_us > 0)
                ? ((int64_t)cv_interval_us * 2)
                : 2000000LL;

            // Cap notify wait at 100ms so we periodically peek the UART for the
            // monitor. ulTaskNotifyTake sleeps; ~10 wakes/sec when idle is negligible.
            bool got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            int64_t now = esp_timer_get_time();

            // Non-blocking peek (timeout=0): consumes any queued bytes if present,
            // returns immediately when empty. Cost when idle ≈ 0.
            int len = uart_read_bytes(MIDI_UART, buf, sizeof(buf), 0);
            for (int i = 0; i < len; i++) midi_mon_byte(buf[i]);

            if (got) {
                cv_last_pulse_us = now;
                int64_t interval = cv_interval_us;
                if (interval > 0) {
                    double bpm = 60000000.0 / ((double)interval * ppqn);
                    if (bpm > 20.0 && bpm < 999.0) {
                        abl_link_capture_app_session_state(s->link, midi_in_ss);
                        abl_link_set_tempo(midi_in_ss, bpm, abl_link_clock_micros(s->link));
                        if (!cv_playing) {
                            abl_link_set_is_playing(midi_in_ss, true, abl_link_clock_micros(s->link));
                            cv_playing = true;
                        }
                        abl_link_commit_app_session_state(s->link, midi_in_ss);
                    }
                }
            } else if (cv_playing && cv_last_pulse_us > 0 &&
                       (now - cv_last_pulse_us) > cv_timeout_us) {
                // Clock stopped
                abl_link_capture_app_session_state(s->link, midi_in_ss);
                abl_link_set_is_playing(midi_in_ss, false, abl_link_clock_micros(s->link));
                abl_link_commit_app_session_state(s->link, midi_in_ss);
                cv_playing = false;
            }
            continue;
        }

        // --- MIDI THRU: bulk forward first, parse after ---
        if (midi_mode == MODE_MIDI_THRU) {
            // With RXFIFO threshold=1, task wakes within µs of first byte arriving.
            // Returns immediately with all available bytes (1-128). 10ms is only
            // the idle timeout for mode-switch responsiveness when no MIDI flows.
            int len = uart_read_bytes(MIDI_UART, buf, sizeof(buf), pdMS_TO_TICKS(10));
            if (len <= 0) continue;

            // Forward everything immediately - data hits TX before any processing
            uart_write_bytes(MIDI_UART, (const char *)buf, len);

            // Scan forwarded data for transport messages to keep Link in sync
            for (int i = 0; i < len; i++) {
                midi_mon_byte(buf[i]);
                switch (buf[i]) {
                case 0xF8: {
                    int64_t now = (int64_t)abl_link_clock_micros(s->link);
                    if (last_tick_us > 0 && (now - last_tick_us) > 5000000LL) {
                        // >5s silence: assume a session boundary (transport
                        // stopped, song changed). Discard the partial window
                        // and snap the next measurement so we don't spend ~30s
                        // slewing to a possibly very different new tempo.
                        tick_count = 0;
                        first_tempo_measurement = true;
                    }
                    last_tick_us = now;
                    tick_count++;
                    if (tick_count == 1) {
                        first_tick_us = now;
                    } else if (tick_count >= 97) {
                        // 96 intervals = 4 beats. Wider window dilutes per-byte
                        // task-scheduling jitter that otherwise shows up as ±10% BPM noise.
                        int64_t elapsed = now - first_tick_us;
                        if (elapsed > 0) {
                            double bpm = 240000000.0 / (double)elapsed;
                            midi_apply_tempo_slewed(s, midi_in_ss, bpm,
                                                    (uint64_t)now,
                                                    &first_tempo_measurement,
                                                    &bpm_smoothed);
                        }
                        tick_count = 1;
                        first_tick_us = now;
                    }
                    break;
                }
                case 0xFA: {
                    // START: align Link beat 0 to this instant. Use request
                    // (not force) so that when other Link peers are present
                    // they get a phase-aligned transition at the next beat
                    // boundary instead of an instant yank. Solo session: this
                    // behaves identically to an immediate force.
                    uint64_t now = abl_link_clock_micros(s->link);
                    abl_link_capture_app_session_state(s->link, midi_in_ss);
                    abl_link_set_is_playing(midi_in_ss, true, now);
                    abl_link_request_beat_at_time(midi_in_ss, 0.0, (int64_t)now, 1.0);
                    abl_link_commit_app_session_state(s->link, midi_in_ss);
                    tick_count = 0;
                    break;
                }
                case 0xFB:
                    abl_link_capture_app_session_state(s->link, midi_in_ss);
                    abl_link_set_is_playing(midi_in_ss, true, abl_link_clock_micros(s->link));
                    abl_link_commit_app_session_state(s->link, midi_in_ss);
                    break;
                case 0xFC:
                    abl_link_capture_app_session_state(s->link, midi_in_ss);
                    abl_link_set_is_playing(midi_in_ss, false, abl_link_clock_micros(s->link));
                    abl_link_commit_app_session_state(s->link, midi_in_ss);
                    tick_count = 0;
                    break;
                }
            }
            continue;
        }

        // --- MIDI CLK: byte-by-byte, forward only clock/transport ---
        uint8_t byte;
        int len = uart_read_bytes(MIDI_UART, &byte, 1, pdMS_TO_TICKS(20));
        if (len <= 0) continue;

        midi_mon_byte(byte);

        switch (byte) {
        case 0xF8: { // Clock tick
            uart_write_bytes(MIDI_UART, (const char *)&byte, 1);
            int64_t now = (int64_t)abl_link_clock_micros(s->link);
            if (last_tick_us > 0 && (now - last_tick_us) > 5000000LL) {
                // >5s silence: see MODE_MIDI_THRU note. Discard partial window,
                // snap next measurement.
                tick_count = 0;
                first_tempo_measurement = true;
            }
            last_tick_us = now;
            tick_count++;
            if (tick_count == 1) {
                first_tick_us = now;
            } else if (tick_count >= 97) {
                // 96 intervals = 4 beats; see MODE_MIDI_THRU note above.
                int64_t elapsed = now - first_tick_us;
                if (elapsed > 0) {
                    double bpm = 240000000.0 / (double)elapsed;
                    midi_apply_tempo_slewed(s, midi_in_ss, bpm,
                                            (uint64_t)now,
                                            &first_tempo_measurement,
                                            &bpm_smoothed);
                }
                tick_count = 1;
                first_tick_us = now;
            }
            break;
        }

        case 0xFA: { // Start
            // Align Link beat 0 to this instant. request (not force) so peers
            // get a phase-aligned transition at the next beat boundary instead
            // of an instant yank; solo session is identical to immediate force.
            uart_write_bytes(MIDI_UART, (const char *)&byte, 1);
            uint64_t now = abl_link_clock_micros(s->link);
            abl_link_capture_app_session_state(s->link, midi_in_ss);
            abl_link_set_is_playing(midi_in_ss, true, now);
            abl_link_request_beat_at_time(midi_in_ss, 0.0, (int64_t)now, 1.0);
            abl_link_commit_app_session_state(s->link, midi_in_ss);
            tick_count = 0;
            break;
        }

        case 0xFB: // Continue
            uart_write_bytes(MIDI_UART, (const char *)&byte, 1);
            abl_link_capture_app_session_state(s->link, midi_in_ss);
            abl_link_set_is_playing(midi_in_ss, true, abl_link_clock_micros(s->link));
            abl_link_commit_app_session_state(s->link, midi_in_ss);
            break;

        case 0xFC: // Stop
            uart_write_bytes(MIDI_UART, (const char *)&byte, 1);
            abl_link_capture_app_session_state(s->link, midi_in_ss);
            abl_link_set_is_playing(midi_in_ss, false, abl_link_clock_micros(s->link));
            abl_link_commit_app_session_state(s->link, midi_in_ss);
            tick_count = 0;
            break;

        case 0xF2: { // Song Position Pointer (3 bytes)
            uint8_t spp[3] = { byte, 0, 0 };
            int r1 = uart_read_bytes(MIDI_UART, &spp[1], 1, pdMS_TO_TICKS(20));
            int r2 = uart_read_bytes(MIDI_UART, &spp[2], 1, pdMS_TO_TICKS(20));
            if (r1 > 0) midi_mon_byte(spp[1]);
            if (r2 > 0) midi_mon_byte(spp[2]);
            if (r1 > 0 && r2 > 0) {
                uart_write_bytes(MIDI_UART, (const char *)spp, 3);
            }
            break;
        }

        default: // MIDI CLK: drop non-transport bytes (monitor still sees them via the call above)
            break;
        }
    }
}

// --- DNS Hijacker (Optimized for low latency) ---
static void dns_hijacker_task(void *pvParameters) {
    char rx_buffer[128 + 16];  // extra 16 bytes for appended DNS answer
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
        int r = recvfrom(sock, rx_buffer, 128, 0, (struct sockaddr *)&src, &len);

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

// --- OTA firmware update: raw .bin POST body streamed into the inactive app slot ---
static esp_err_t update_post_handler(httpd_req_t *req) {
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");

    if (update_part == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "ERROR: no OTA partition available");
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > update_part->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "ERROR: image is empty or larger than the OTA slot");
        return ESP_OK;
    }

    char *buf = (char *)malloc(4096);
    if (buf == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "ERROR: out of memory");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "OTA: receiving %u bytes into '%s'", (unsigned)req->content_len, update_part->label);

    esp_ota_handle_t ota = 0;
    esp_err_t err = ESP_OK;
    size_t remaining = req->content_len;
    bool started = false;
    int timeouts = 0;

    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) { err = ESP_FAIL; break; }
        timeouts = 0;
        if (!started) {
            // Every ESP32 app image starts with magic byte 0xE9 — reject anything else
            if ((uint8_t)buf[0] != 0xE9) { err = ESP_ERR_INVALID_ARG; break; }
            err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
            if (err != ESP_OK) break;
            started = true;
        }
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) break;
        remaining -= r;
    }
    free(buf);

    if (err == ESP_OK && started) err = esp_ota_end(ota); // validates the written image
    else if (started) esp_ota_abort(ota);

    if (err == ESP_OK) err = esp_ota_set_boot_partition(update_part);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA: update OK, rebooting into '%s'", update_part->label);
        httpd_resp_sendstr(req, "OK: firmware flashed, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // let the response reach the browser
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA: update failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "ERROR: update failed, current firmware kept");
    }
    return ESP_OK;
}

static esp_err_t set_mode_handler(httpd_req_t *req) {
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(buf, "mode", val, sizeof(val)) == ESP_OK) {
            int m = atoi(val);
            if (m >= 0 && m <= 3) midi_mode = m;
        }
        if (httpd_query_key_value(buf, "ppqn", val, sizeof(val)) == ESP_OK) {
            int p = atoi(val);
            if (p >= 0 && p < cv_ppqn_count) {
                cv_ppqn_idx = p;
                cv_ppqn = cv_ppqn_options[p];
            }
        }
        if (httpd_query_key_value(buf, "metro", val, sizeof(val)) == ESP_OK) {
            metronome_enabled = (atoi(val) != 0);
            if (!metronome_enabled) {
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            }
        }
    }
    return captive_portal_redirect(req);
}

static esp_err_t get_handler(httpd_req_t *req) {
    abl_link_capture_app_session_state(g_link_state.link, g_web_ss);
    double current_bpm = abl_link_tempo(g_web_ss);
    bool is_playing = abl_link_is_playing(g_web_ss);
    size_t peers = abl_link_num_peers(g_link_state.link);

    char html_buf[7168];
    const char* play_style = is_playing ? "border:2px solid #FFF;" : "";
    const char* stop_style = !is_playing ? "border:2px solid #FFF;" : "";

    static const char *mode_names[] = { "LINK", "MIDI CLK", "MIDI THRU", "CV" };
    int cur_mode = midi_mode;
    bool midi_is_master = (cur_mode == MODE_MIDI_CLK || cur_mode == MODE_MIDI_THRU || cur_mode == MODE_CV);
    const char *disabled = midi_is_master ? " disabled" : "";
    const char *midi_banner = midi_is_master
        ? "<p style='background:#874F41;padding:10px;border-radius:8px;font-size:14px;margin-bottom:15px'>"
          "External clock &mdash; controls disabled</p>"
        : "";

    // Pre-build mode buttons HTML
    char mode_btns[512];
    int mpos = 0;
    for (int i = 0; i < 4; i++) {
        const char *sel = (i == cur_mode) ? "background:#E64833;color:#FBE9D0;" : "background:#1e3b46;color:#90AEAD;";
        mpos += snprintf(mode_btns + mpos, sizeof(mode_btns) - mpos,
            "<a href='/set_mode?mode=%d' class='mbtn' style='%s'>%s</a>", i, sel, mode_names[i]);
    }

    // Pre-build PPQN cycle button HTML — single button advances to the next option
    char ppqn_btns[256];
    int cur_ppqn_idx = cv_ppqn_idx;
    int next_ppqn_idx = (cur_ppqn_idx + 1) % cv_ppqn_count;
    snprintf(ppqn_btns, sizeof(ppqn_btns),
        "<a href='/set_mode?ppqn=%d' class='mbtn' style='background:#E64833;color:#FBE9D0;'>%s</a>",
        next_ppqn_idx, ppqn_labels[cur_ppqn_idx]);

    // Metronome toggle button
    char metro_btn[128];
    bool cur_metro = metronome_enabled;
    const char *metro_style = cur_metro ? "background:#6D9773;color:#FBE9D0;" : "background:#1e3b46;color:#90AEAD;";
    snprintf(metro_btn, sizeof(metro_btn),
        "<a href='/set_mode?metro=%d' class='mbtn' style='%s'>%s</a>", cur_metro ? 0 : 1, metro_style, cur_metro ? "ON" : "OFF");

    // Connection status
    char conn_str[80];
    if (s_is_connected) {
        snprintf(conn_str, sizeof(conn_str), "LINK: %s (%s)", s_ssid_name, s_ip_str);
    } else {
        snprintf(conn_str, sizeof(conn_str), "AP Mode");
    }

    // Running OTA slot label, shown on the firmware card
    const esp_partition_t *running_part = esp_ota_get_running_partition();

    double bpm_minus = current_bpm - 1.0;
    double bpm_plus = current_bpm + 1.0;
    if (bpm_minus < 20.0) bpm_minus = 20.0;
    if (bpm_plus > 999.0) bpm_plus = 999.0;

    snprintf(html_buf, sizeof(html_buf),
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=0'>"
        "<title>BeatMesh</title>"
        "<style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#2448555c;color:#FBE9D0;margin:0;padding:20px;text-align:center}"
        ".container{max-width:400px;margin:0 auto}"
        ".card{background:#2c5563;border-radius:12px;padding:20px;margin-bottom:20px;box-shadow:0 4px 10px rgba(0,0,0,0.5);border:1px solid #874F41}"
        "h1{color:#6D9773;margin:0 0 5px 0;font-size:32px;letter-spacing:1px}"
        "h2{font-size:16px;color:#90AEAD;border-bottom:1px solid #874F41;padding-bottom:10px;margin-bottom:15px}"
        "input{width:100%%;box-sizing:border-box;padding:12px;margin:4px 0 8px 0;background:#1e3b46;border:1px solid #90AEAD;color:#FBE9D0;border-radius:8px;font-size:18px;text-align:center}"
        "input:focus{border-color:#E64833;outline:none}"
        ".btn{width:100%%;background:#E64833;color:#FBE9D0;font-weight:bold;border:none;padding:15px;border-radius:8px;font-size:16px;cursor:pointer;transition:0.2s}"
        ".btn:active{background:#874F41;transform:scale(0.98)}"
        ".btn:disabled{opacity:0.4;cursor:not-allowed}"
        ".row{display:flex;gap:10px;margin-top:8px}"
        ".mbtn{display:inline-block;padding:8px 12px;border-radius:6px;font-size:13px;font-weight:bold;text-decoration:none;margin:0 3px}"
        ".sel-row{margin:10px 0}"
        ".pm{display:inline-block;padding:10px 18px;border-radius:8px;font-size:20px;font-weight:bold;text-decoration:none;background:#E64833;color:#FBE9D0}"
        ".pm.off{opacity:0.4;pointer-events:none}"
        ".status{font-size:20px;color:#FBE9D0;margin-top:15px;font-weight:bold}"
        "</style></head>"
        "<body><div class='container'>"
            "<div class='card'>"
                "<h1 style='font-size:40px;font-weight:900;letter-spacing:-2px;-webkit-text-stroke:1px #6D9773'>\xF0\x9D\x99\xB1\xF0\x9D\x9A\x8E\xF0\x9D\x9A\x8A\xF0\x9D\x9A\x9D\xF0\x9D\x99\xBC\xF0\x9D\x9A\x8E\xF0\x9D\x9A\x9C\xF0\x9D\x9A\x91</h1>"
                "<div class='status'>PEERS: %zu &bull; %s</div>"
                "<hr style='border:0;border-top:1px solid #874F41;margin:12px 0'>"
                "<div class='sel-row'>%s</div>"
                "<div class='sel-row'><span style='font-size:12px;color:#90AEAD'>PPQN: </span>%s</div>"
                "<div class='sel-row'><span style='font-size:12px;color:#90AEAD'>METRONOME: </span>%s</div>"
                "%s"
                "<form action='/set_bpm' method='get'>"
                    "<div style='display:flex;gap:8px;align-items:center;margin:10px 0'>"
                        "<a href='/set_bpm?bpm=%.0f' class='pm%s'>-</a>"
                        "<input type='number' name='bpm' value='%.0f' step='1' inputmode='numeric' style='flex:1;margin:0;font-size:32px;font-weight:bold'%s>"
                        "<a href='/set_bpm?bpm=%.0f' class='pm%s'>+</a>"
                    "</div>"
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
            "<div class='card'>"
                "<h2>FIRMWARE UPDATE</h2>"
                "<p style='font-size:12px;color:#90AEAD;margin:0 0 10px 0'>version: %s &bull; running slot: %s</p>"
                "<input type='file' id='fw' accept='.bin'>"
                "<button class='btn' id='fwbtn' onclick='fwUp()'>UPLOAD &amp; FLASH</button>"
                "<p id='fwst' style='font-size:14px;font-weight:bold'></p>"
            "</div>"
        "</div>"
        "<script>"
        "function fwUp(){var f=document.getElementById('fw').files[0];var st=document.getElementById('fwst');"
        "if(!f){st.innerText='Choose a .bin file first';return;}"
        "if(!confirm('Flash '+f.name+' ('+Math.round(f.size/1024)+' KB)? The device reboots when done.'))return;"
        "document.getElementById('fwbtn').disabled=true;"
        "var x=new XMLHttpRequest();x.open('POST','/update');"
        "x.upload.onprogress=function(e){st.innerText='Uploading: '+Math.round(e.loaded*100/e.total)+'%%';};"
        "x.onload=function(){st.innerText=x.responseText;if(x.status!=200){document.getElementById('fwbtn').disabled=false;}};"
        "x.onerror=function(){st.innerText='Connection closed (device may be rebooting)';};"
        "x.send(f);}"
        "</script>"
        "</body></html>",
        peers, conn_str,
        mode_btns, ppqn_btns, metro_btn,
        midi_banner,
        bpm_minus, midi_is_master ? " off" : "", current_bpm, disabled, bpm_plus, midi_is_master ? " off" : "",
        disabled,
        play_style, disabled, stop_style, disabled,
        GIT_VERSION, running_part ? running_part->label : "?"
    );

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, html_buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- Web server: shared by the AP captive portal and STA mode (remote control + OTA) ---
static void start_web_server() {
    if (s_http_server != NULL) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.stack_size = 12288; // get_handler builds the page in a ~7KB stack buffer

    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_handler };
        httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler };
        httpd_uri_t save = { .uri = "/save", .method = HTTP_GET, .handler = save_handler };
        httpd_uri_t set_bpm = { .uri = "/set_bpm", .method = HTTP_GET, .handler = set_bpm_handler };
        httpd_uri_t transport = { .uri = "/transport", .method = HTTP_GET, .handler = set_transport_handler };
        httpd_uri_t set_mode = { .uri = "/set_mode", .method = HTTP_GET, .handler = set_mode_handler };
        httpd_uri_t update = { .uri = "/update", .method = HTTP_POST, .handler = update_post_handler };
        httpd_uri_t connecttest = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = aggressive_close_handler };
        httpd_uri_t redirect = { .uri = "/redirect", .method = HTTP_GET, .handler = captive_portal_redirect };
        httpd_uri_t hotspot = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_portal_redirect };
        httpd_uri_t gen204 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_redirect };

        httpd_register_uri_handler(s_http_server, &root);
        httpd_register_uri_handler(s_http_server, &favicon);
        httpd_register_uri_handler(s_http_server, &save);
        httpd_register_uri_handler(s_http_server, &set_bpm);
        httpd_register_uri_handler(s_http_server, &transport);
        httpd_register_uri_handler(s_http_server, &set_mode);
        httpd_register_uri_handler(s_http_server, &update);
        httpd_register_uri_handler(s_http_server, &connecttest);
        httpd_register_uri_handler(s_http_server, &redirect);
        httpd_register_uri_handler(s_http_server, &hotspot);
        httpd_register_uri_handler(s_http_server, &gen204);

        httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND, custom_error_handler);
        httpd_register_err_handler(s_http_server, HTTPD_400_BAD_REQUEST, custom_error_handler);
        httpd_register_err_handler(s_http_server, HTTPD_500_INTERNAL_SERVER_ERROR, custom_error_handler);

        ESP_LOGW(TAG, "Web server started");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
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

    start_web_server();
    ESP_LOGW(TAG, "AP Started: BeatMesh - Portal at 192.168.4.1");
}

// --- Help screens ---
#define HELP_PAGE_COUNT 4
enum ui_screen_t { SCREEN_MAIN = 0, SCREEN_HELP = 1 };

// GIT_VERSION define moved to the top globals (also used by the web UI firmware card)
//#ifndef GIT_VERSION
//#define GIT_VERSION "unknown"
//#endif

// Draw the static main-screen chrome: transport buttons (row 0) + HELP button (row 2).
static void draw_main_chrome(void) {
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
    // HELP button (row 2, right after the PPQN button)
    tft->fillRoundRect(240, 150, 71, 28, 4, C_VINTAGE_RUST);
    tft->setTextSize(1);
    tft->drawString("HELP", 240 + 71 / 2, 164);
    tft->setTextDatum(top_left);
}

// Draw one full-screen help page. NEXT is hidden on the last page.
static void draw_help_page(int page) {
    tft->fillScreen(C_VINTAGE_BG);
    tft->setTextDatum(middle_center);
    tft->setTextColor(C_VINTAGE_CREAM);
    tft->setTextSize(1.5);
    // BACK button (bottom-left) -> returns to main UI
    tft->fillRoundRect(10, 208, 52, 24, 4, C_VINTAGE_RUST);
    tft->drawString("BACK", 10 + 52 / 2, 220);
    // NEXT button (bottom-right) -> advance page; absent on the last page
    if (page < HELP_PAGE_COUNT - 1) {
        tft->fillRoundRect(258, 208, 52, 24, 4, C_VINTAGE_FERN);
        tft->drawString("NEXT", 258 + 52 / 2, 220);
    }
    tft->setTextDatum(top_left);
    tft->setTextSize(1.7);
    tft->setTextColor(C_VINTAGE_CREAM);
    if (page == 0) {
        // Up / down arrowhead triangles (top-center / bottom-center)
        tft->fillTriangle(160, 12, 148, 32, 172, 32, C_VINTAGE_CREAM);    // up
        tft->fillTriangle(160, 228, 148, 208, 172, 208, C_VINTAGE_CREAM); // down
        // Left / right arrowheads, lowered to leave room for the top info
        tft->fillTriangle(12, 150, 32, 138, 32, 162, C_VINTAGE_CREAM);    // left
        tft->fillTriangle(308, 150, 288, 138, 288, 162, C_VINTAGE_CREAM); // right
        // Top arrow label (single lines, just below the up arrow)
        tft->setTextDatum(middle_center);
        tft->drawString("Switch: midi bypass BeatMesh", 160, 50);
        tft->drawString("CV clock in", 160, 70);
        tft->drawString("midi in 1/8 | midi in din", 160, 90);
        // Left arrow label (right of the arrow)
        tft->setTextDatum(middle_left);
        tft->drawString("5 midi out", 40, 150);
        // Right arrow label (left of the arrow)
        tft->setTextDatum(middle_right);
        tft->drawString("5 midi out", 280, 150);
        // Bottom arrow label (above the down arrow)
        tft->setTextDatum(middle_center);
        tft->drawString("CV out, CV reset", 160, 190);
        tft->setTextDatum(top_left);
    } else if (page == 1) {
        // Mode descriptions (wrapped to fit) with subtle separators between modes
        const int px = 16; // left padding
        tft->setTextDatum(top_left);
        tft->setTextColor(C_VINTAGE_CREAM);
        tft->setTextSize(1.7);
        tft->drawString("Clock", px, 10);
        tft->setTextSize(1.2);
        tft->drawString("Link: clock controlled by ableton link", px, 38);
        tft->drawFastHLine(px, 55, 288, C_VINTAGE_BG_LT);
        tft->drawString("Midi: clock using midi in and filtering", px, 61);
        tft->drawString("out any other midi signal", px, 75);
        tft->drawFastHLine(px, 91, 288, C_VINTAGE_BG_LT);
        tft->drawString("THR: clock using midi in but send also", px, 97);
        tft->drawString("all midi signal", px, 111);
        tft->drawFastHLine(px, 127, 288, C_VINTAGE_BG_LT);
        tft->drawString("CV: clock using CV in.", px, 133);
        tft->setTextSize(1.5);
        tft->drawString("Clock sent to 10 midi out and", px, 161);
        tft->drawString("cv clock out.", px, 181);
    } else if (page == 2) {
        // Web interface / access point info
        const int px = 16; // left padding
        tft->setTextDatum(top_left);
        tft->setTextColor(C_VINTAGE_CREAM);
        tft->setTextSize(1.7);
        tft->drawString("Web interface", px, 14);
        tft->setTextSize(1.2);
        tft->drawString("To configure wifi and settings,", px, 42);
        tft->drawString("join the 'BeatMesh' wifi access", px, 56);
        tft->drawString("point, then open in a browser:", px, 70);
        tft->setTextSize(2.5);
        tft->drawString("192.168.4.1", px, 88);
        tft->setTextSize(1.2);
        tft->drawString("Access point mode works for up to", px, 126);
        tft->drawString("2-4 peers; for more, connect all", px, 138);
        tft->drawString("devices to a router.", px, 152);
    } else {
        // Credits / version
        const int px = 16; // left padding
        tft->setTextDatum(top_left);
        tft->setTextColor(C_VINTAGE_CREAM);
        tft->setTextSize(1.7);
        tft->drawString("CREDITS", px, 8);
        tft->setTextSize(1.2);
        tft->drawString("THOMAS O. FREDERICKS", px, 32);
        tft->drawString("(this circuit is inspired by!)", px, 45);
        tft->drawString("visit: t-o-f.info", px, 58);
        tft->drawString("Alexandre Castonguay", px, 76);
        tft->drawString("(Technologie de la fete) and", px, 89);
        tft->drawString("Andre Girard (for all the", px, 102);
        tft->drawString("brainstorming!)", px, 115);
        tft->setTextSize(1.5);
        tft->drawString("Made with love by:", px, 137); // bigger + 4px margin-top
        tft->setTextSize(1.2);
        tft->drawString("Patrick Sebastien Coulombe", px, 155);
        tft->drawString("workinprogress.ca", px, 168);
        tft->setTextColor(TFT_WHITE);
        tft->setTextDatum(bottom_right);
        tft->drawString("version: " GIT_VERSION, 320 - px, 240 - 6);
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
    int last_cv_ppqn = 0;
    uint32_t metro_off_time = 0;
    bool last_metro_enabled = true; // force initial draw (metronome starts disabled)
    uint32_t last_mon_seq = 0xFFFFFFFFu; // MIDI monitor redraw tracker (force first draw)
    bool last_mon_led = false;
    int ui_screen = SCREEN_MAIN;        // active screen: main UI vs help
    int help_page = 0;                  // active help page index

    tft = new LGFX_CYD();
    tft->init(); 
    tft->setRotation(1); 
    tft->setBrightness(200); 
    tft->fillScreen(C_VINTAGE_BG);

    gpio_reset_pin(LED_RED); gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);
    gpio_set_direction(BTN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_BOOT, GPIO_PULLUP_ONLY);

    draw_main_chrome();

    // Restore the main UI after leaving a help screen: redraw static chrome and
    // force every change-tracked block below to repaint on the next iteration.
    auto restore_main = [&]() {
        tft->fillScreen(C_VINTAGE_BG);
        draw_main_chrome();
        last_beat = -1; last_bpm = -1; last_peers = 999;
        last_midi_mode = -1; last_cv_ppqn = -1;
        last_metro_enabled = !metronome_enabled;
        last_connection_state = !s_is_connected;
        last_portal_state = !s_portal_active;
        last_mon_seq = 0xFFFFFFFFu;
    };

    while (true) {
        // Metronome silence check
        if (metro_off_time && esp_log_timestamp() >= metro_off_time) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            metro_off_time = 0;
        }

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
              if (ui_screen == SCREEN_HELP) {
                // BACK button (bottom-left) returns to the main UI
                if (tx < 70 && ty >= 200) {
                    ui_screen = SCREEN_MAIN;
                    restore_main();
                }
                // NEXT button (bottom-right); absent on the last page
                else if (help_page < HELP_PAGE_COUNT - 1 && tx >= 250 && ty >= 200) {
                    help_page++;
                    draw_help_page(help_page);
                }
                last_touch = esp_log_timestamp();
              } else {
                abl_link_capture_app_session_state(s->link, s->session_state);
                uint64_t now = abl_link_clock_micros(s->link);

                if (ty >= 100 && ty < 135) {
                    // Mode buttons (row 1)
                    for (int i = 0; i < 4; i++) {
                        if (tx >= mode_btn_x[i] && tx < mode_btn_x[i] + mode_btn_w[i]) {
                            midi_mode = i;
                            break;
                        }
                    }
                    last_touch = esp_log_timestamp();
                }
                else if (ty >= 150 && ty < 178) {
                    if (tx >= 84 && tx < 155) {
                        // MET toggle button
                        metronome_enabled = !metronome_enabled;
                        if (!metronome_enabled) {
                            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                            metro_off_time = 0;
                        }
                    } else if (tx >= ppqn_btn_x && tx < ppqn_btn_x + ppqn_btn_w) {
                        // PPQN cycle button (row 2): advance to next option
                        cv_ppqn_idx = (cv_ppqn_idx + 1) % cv_ppqn_count;
                        cv_ppqn = cv_ppqn_options[cv_ppqn_idx];
                    } else if (tx >= 240 && tx < 311) {
                        // HELP button (row 2, right of PPQN): open help screens
                        ui_screen = SCREEN_HELP;
                        help_page = 0;
                        draw_help_page(help_page);
                    }
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
        }

        // Help screen is modal: skip all main-UI rendering while it is shown.
        if (ui_screen != SCREEN_MAIN) {
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
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
                // Metronome click: bip (1200Hz) on beat 1, bop (800Hz) on 2-4
                if (metronome_enabled) {
                    uint32_t freq = (cur_beat == 0) ? 1200 : 800;
                    uint32_t dur = (cur_beat == 0) ? 40 : 30;
                    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq);
                    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512);
                    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                    metro_off_time = esp_log_timestamp() + dur;
                }
                last_beat = cur_beat;
            }
        } else {
            if (last_beat != -2) {
                 for (int i=0; i<4; i++) tft->fillRoundRect(rect_x[i], 70, rect_w[i], 12, 3, C_VINTAGE_CREAM_DIM);
                 gpio_set_level(LED_RED, LED_OFF);
                 // Silence metronome on stop
                 ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                 ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                 metro_off_time = 0;
                 last_beat = -2;
            }
        }

        if (fabs(cur_bpm - last_bpm) > 0.1) {
            tft->fillRect(10, 176, 160, 35, C_VINTAGE_BG);
            tft->setTextColor(C_VINTAGE_CREAM); tft->setTextSize(4);
            char bstr[16]; sprintf(bstr, "%.0f", cur_bpm);
            tft->drawString(bstr, 9, 180);
            tft->setTextSize(2);
            tft->drawString("BPM", 92, 194);
            last_bpm = cur_bpm;
        }

        if (peers != last_peers || last_peers == 999) {
            tft->fillRect(180, 192, 140, 22, C_VINTAGE_BG);
            tft->setTextColor(C_VINTAGE_CREAM); tft->setTextSize(2);
            char pstr[20]; sprintf(pstr, "PEERS: %zu", peers);
            tft->drawRightString(pstr, 310, 194);
            last_peers = peers;
        }

        // Mode + PPQN button redraw
        if (midi_mode != last_midi_mode || last_cv_ppqn != cv_ppqn) {
            static const uint16_t mode_colors[] = { C_VINTAGE_FERN, C_VINTAGE_FERN, C_VINTAGE_FERN, C_VINTAGE_FERN };
            // Row 1: Mode buttons (y=100, h=35)
            tft->setTextSize(2);
            tft->setTextDatum(middle_center);
            for (int i = 0; i < 4; i++) {
                uint16_t bg = (i == midi_mode) ? mode_colors[i] : C_VINTAGE_BG_LT;
                uint16_t fg = C_VINTAGE_CREAM; // always white text, even when inactive
                tft->fillRoundRect(mode_btn_x[i], 100, mode_btn_w[i], 35, 5, bg);
                tft->setTextColor(fg);
                tft->drawString(mode_labels[i], mode_btn_x[i] + mode_btn_w[i] / 2, 118);
            }
            // Row 2: PPQN button (right after METRONOME) - small "PPQN" label + value inside
            tft->fillRoundRect(ppqn_btn_x, 150, ppqn_btn_w, 28, 4, C_VINTAGE_AMBER);
            tft->setTextColor(C_VINTAGE_CREAM);
            tft->setTextSize(1);
            tft->setTextDatum(middle_left);
            tft->drawString("PPQN", ppqn_btn_x + 6, 164);
            tft->setTextSize(2);
            tft->setTextDatum(middle_right);
            tft->drawString(ppqn_labels[cv_ppqn_idx], ppqn_btn_x + ppqn_btn_w - 6, 164);
            tft->setTextDatum(top_left);
            last_midi_mode = midi_mode;
            last_cv_ppqn = cv_ppqn;
        }

        // METRONOME button redraw (left side of row 2)
        if (metronome_enabled != last_metro_enabled) {
            uint16_t bg = metronome_enabled ? C_VINTAGE_FERN : C_VINTAGE_BG_LT;
            uint16_t fg = C_VINTAGE_CREAM; // always white text, even when inactive
            tft->fillRoundRect(84, 150, 71, 28, 4, bg);
            tft->setTextSize(1);
            tft->setTextColor(fg);
            tft->setTextDatum(middle_center);
            tft->drawString("METRONOME", 119, 164);
            tft->setTextDatum(top_left);
            last_metro_enabled = metronome_enabled;
        }

        // MIDI monitor (left of metronome): activity LED + last message
        {
            char l1[10], l2[12];
            uint32_t cur_seq;
            int64_t last_us;
            portENTER_CRITICAL(&g_mon_mux);
            cur_seq = g_midi_mon_seq;
            last_us = g_midi_mon_last_us;
            strncpy(l1, g_midi_mon_l1, sizeof(l1));
            strncpy(l2, g_midi_mon_l2, sizeof(l2));
            portEXIT_CRITICAL(&g_mon_mux);
            l1[sizeof(l1) - 1] = 0;
            l2[sizeof(l2) - 1] = 0;

            bool led_on = (last_us != 0) && ((esp_timer_get_time() - last_us) < 80000);

            if (cur_seq != last_mon_seq) {
                // Two-line monitor label same color as button labels (LINK, MIDI...)
                uint16_t mon_fg = C_VINTAGE_CREAM;
                tft->fillRect(10, 146, 70, 28, C_VINTAGE_BG);
                tft->fillRect(10, 145, 8, 8, led_on ? C_VINTAGE_AMBER : C_VINTAGE_BG_LT);
                tft->setTextSize(1);
                tft->setTextDatum(top_left);
                tft->setTextColor(mon_fg);
                tft->drawString(l1, 22, 146);
                tft->drawString(l2, 11, 160); // bottom line aligns with the LED's left edge
                last_mon_seq = cur_seq;
                last_mon_led = led_on;
            } else if (led_on != last_mon_led) {
                tft->fillRect(10, 145, 8, 8, led_on ? C_VINTAGE_AMBER : C_VINTAGE_BG_LT);
                last_mon_led = led_on;
            }
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
                char status_msg[80];
                snprintf(status_msg, sizeof(status_msg), "LINK: %s (%s)", s_ssid_name, s_ip_str);
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
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
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

    // mDNS: device reachable at http://beatmesh.local on the LAN
    mdns_init();
    mdns_hostname_set("beatmesh");
    mdns_instance_name_set("BeatMesh");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

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
    cv_clock_init();
    cv_out_init();
    metronome_init();
    xTaskCreatePinnedToCore(midi_clock_task, "midi_out", 4096, &g_link_state, 10, NULL, 1);
    xTaskCreatePinnedToCore(midi_in_task, "midi_in", 4096, &g_link_state, 10, NULL, 1);

    wifi_config_t sta_cfg;
    esp_wifi_get_config(WIFI_IF_STA, &sta_cfg);
    if (strlen((char*)sta_cfg.sta.ssid) == 0) start_setup_portal();
    else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        esp_wifi_set_ps(WIFI_PS_NONE);
        start_web_server(); // web UI + OTA also available on the home network
    }

    // Init reached the end without crashing: confirm this image as good so the
    // bootloader won't roll back to the previous OTA slot on next reboot.
    esp_ota_mark_app_valid_cancel_rollback();
}
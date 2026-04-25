#include <hidapi/hidapi.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>

using Clock = std::chrono::steady_clock;
constexpr auto MIC_READ_TIMEOUT = std::chrono::milliseconds(100);
constexpr auto LOOP_SLEEP = std::chrono::milliseconds(50);
constexpr auto LED_KEEPALIVE_INTERVAL = std::chrono::seconds(2);
constexpr auto LED_MIN_UPDATE_INTERVAL = std::chrono::milliseconds(250);
constexpr auto HANDSHAKE_INTERVAL = std::chrono::seconds(10);

void send_cmd(hid_device* handle, const std::vector<unsigned char>& data) {
    unsigned char buf[65] = {0};
    buf[0] = 0x02; buf[1] = 0x09;
    for (size_t i = 0; i < data.size() && (i + 2) < 65; ++i) {
        buf[i + 2] = data[i];
    }
    hid_write(handle, buf, 65);
}

void send_led_state(hid_device* handle, bool mic_muted) {
    unsigned char led_pkt[65] = {0};
    led_pkt[0] = 0x02;
    led_pkt[1] = 0x09;
    led_pkt[2] = 0x06;
    led_pkt[3] = 0x00;
    led_pkt[4] = 0x09;

    led_pkt[10] = mic_muted ? 255 : 0;  // Mic (I2)
    led_pkt[12] = 255;                  // Power verde (I1)

    hid_write(handle, led_pkt, 65);
}

int main() {
    if (hid_init()) return 1;

    struct hid_device_info *devs, *cur_dev;
    devs = hid_enumerate(0x1B1C, 0x0A6B);
    cur_dev = devs;
    hid_device* h = nullptr;
    while (cur_dev) {
        if (cur_dev->interface_number == 3) {
            h = hid_open_path(cur_dev->path);
            if (h) break;
        }
        cur_dev = cur_dev->next;
    }
    hid_free_enumeration(devs);

    if (!h) return 1;

    std::cout << "--- STEP 2.8: SYNC TOTALE (STABILE + DINAMICO) ---" << std::endl;

    unsigned char read_buf[65];
    bool mic_muted = false;
    bool led_dirty = true;
    auto last_led_time = Clock::now() - LED_KEEPALIVE_INTERVAL;
    auto last_handshake_time = Clock::now() - HANDSHAKE_INTERVAL;

    while (true) {
        auto now = Clock::now();
        bool mic_changed = false;

        // 1. Handshake ogni 10 secondi
        if (now - last_handshake_time >= HANDSHAKE_INTERVAL) {
            send_cmd(h, {0x01, 0x03, 0x00, 0x02});
            send_cmd(h, {0x0D, 0x00, 0x01});
            last_handshake_time = now;
        }

        // 2. Leggi stato microfono (Reattivo 100ms)
        int res = hid_read_timeout(h, read_buf, 65, MIC_READ_TIMEOUT.count());
        if (res > 0 && read_buf[0] == 0x03 && read_buf[3] == 0xA6) {
            bool new_mic_muted = (read_buf[5] == 1);
            mic_changed = (new_mic_muted != mic_muted);
            mic_muted = new_mic_muted;
            led_dirty = led_dirty || mic_changed;
            std::cout << "\rSTATO RILEVATO: " << (mic_muted ? "MUTO" : "ATTIVO") << "    " << std::flush;
        }

        // 3. Aggiorna subito sui cambi mic, ma mantieni un keepalive LED stabile.
        bool led_keepalive_due = (now - last_led_time >= LED_KEEPALIVE_INTERVAL);
        bool led_change_due = led_dirty && (now - last_led_time >= LED_MIN_UPDATE_INTERVAL);
        if (led_keepalive_due || led_change_due) {
            send_led_state(h, mic_muted);
            led_dirty = false;
            last_led_time = now;
        }

        std::this_thread::sleep_for(LOOP_SLEEP);
    }

    hid_exit();
    return 0;
}

# BeatMesh

A module to synchronize via Ableton Link, MIDI, or CV clock. 

Built with PlatformIO and the ESP-IDF framework (espressif32), BeatMesh acts as a hub to keep your hardware and software locked in time.

## 🚀 Features

- **10x MIDI Out ports**
- **Flexible Routing:** Send raw MIDI to all 10 outputs OR filter to keep only the MIDI clock.
- **CV Integration:** CV clock, reset, and play-stop outputs.
- **Ableton Link:** Synchronize seamlessly over WiFi.

---

## 🎛️ Custom PCB (KiCad)

The repository includes a production-ready printed circuit board designed in **KiCad 9**. You can find the source files (`.kicad_sch` and `.kicad_pcb`) in the `board/` directory.

### Hardware Design Highlights:
- **Form Factor:** 100mm x 100mm, 2-layer board (optimized for low-cost fabrication).
- **MIDI Input:** Fully opto-isolated using a **6N138** optocoupler. The circuit is properly level-shifted to send safe 3.3V logic to the ESP32, while powering the optocoupler with 5V for fast, reliable data transmission.
- **10x MIDI Outputs:** Buffered (using 2N3904 NPN transistors) and split to drive up to 10 distinct devices simultaneously without signal degradation. Designed with space-saving 3.5mm TRS jacks (SJ1-3515N).
- **CV / Sync Out:** Capable of sending analog clock, reset, and play/stop signals to Eurorack/modular gear. Includes **BAT41** Schottky diodes for signal protection.
- **CYD Integration:** Designed to interface cleanly with the Cheap Yellow Display (ESP32-2432S028R) ecosystem via standard 2.54mm pin headers.
- **Power Delivery:** Robust decoupling with 100nF ceramics and 47µF (25V) electrolytics to keep clock and CV paths stable and noise-free.

---

## 💻 Installation & Dependencies

This project relies on several external components. Clone them into a `components` directory at the root of your project:

```bash
mkdir components
cd components
git clone [https://github.com/chriskohlhoff/asio.git](https://github.com/chriskohlhoff/asio.git)
git clone [https://github.com/Ableton/link.git](https://github.com/Ableton/link.git)
git clone [https://github.com/lovyan03/LovyanGFX.git](https://github.com/lovyan03/LovyanGFX.git)
```

### ASIO Patch (Required)
To prevent the ESP32 from rebooting during transient network errors (e.g., `send_to ENOMEM`), you must modify the ASIO library. Link recovers from packet loss gracefully, so we do not want `std::terminate` to abort the program.

Find the exception handling block in ASIO and modify it to set the pointer to `nullptr`:

```cpp
#if !defined(ASIO_NO_EXCEPTIONS)
    if (has_pending_exception_ > 0)
    {
      // ESP32 FIX: Don't rethrow - transient network errors 
      // would call std::terminate -> abort -> reboot. Link recovers from packet loss.
      has_pending_exception_ = 0;
      pending_exception_ = nullptr;
    }
#endif // !defined(ASIO_NO_EXCEPTIONS)
```

---

## ⚙️ Configuration Files

### 1. `platformio.ini`
Add the following to your environment configuration to ensure correct serial speeds, flash sizing, and ASIO compatibility:

```ini
monitor_speed = 115200
board_upload.flash_size = 4MB
board_build.partitions = huge_app.csv

build_flags = 
    -fexceptions
    -D ASIO_STANDALONE
    -D ASIO_NO_TYPEID
```

### 2. `huge_app.csv` (Partition Table)
Create this file in the root directory. Link and LovyanGFX make the binary quite large, so the default partition table is too small.

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x4000,
otadata,  data, ota,     ,        0x2000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        0x300000,
```

### 3. `sdkconfig.defaults`
Create this file in the root directory. These settings configure flash size/speed, enable C++ exceptions (required for ASIO/Link), and set the partition table layout.

```ini
# Flash Size & Speed (CYD has a 4MB Flash chip)
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y

# C++ Exceptions (Required for ASIO/Link)
CONFIG_COMPILER_CXX_EXCEPTIONS=y
CONFIG_COMPILER_CXX_RTTI=y

# Partition Table Layout
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y

# CPU Frequency (Ensure Link timing on Core 1 and Display/WiFi on Core 0 run smoothly)
CONFIG_ESP_DEFAULT_CPU_FREQ_240=y

# LWIP (Networking for Link - Multicast support is crucial)
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_LWIP_IGMP=y

# Common NVS/WiFi Settings
CONFIG_ESP_WIFI_AUTH_PHASE2_ENABLED=y
CONFIG_BT_ENABLED=n
```

### 4. `src/CMakeLists.txt`
Include the necessary libraries via `REQUIRES` so ESP-IDF links them properly:

```cmake
idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES LovyanGFX esp_http_server nvs_flash esp_wifi esp_event esp_abl_link)
```

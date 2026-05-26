| Supported Targets | ESP32S3 |
| ----------------- | -------- |

# ESP32 Home Hub

This is a implementation of a custom server on ESP32S3, which is the central hub for checking the status of my own homelab setup as well as for waking them up on LAN via Tailscale.

# Features
-  Tailscale connection via a customized [Microlink](https://github.com/sumagnadas/microlink) implementation to my needs.
- Wake-on-Lan to not run my home lab servers 24/7.
- Two new endpoints for status Wake-on-LAN
    - `/status` -> (GET) Checks the current status of the machines set by a simple ping check every 10s.
    - `/wol` -> (GET/POST) Endpoint for waking up the required machine via a dropdown to send the machine index (in effect, its ID) to wake.

# Future tasks
- Remove the auto-refresh to not increase the load on the computer.
- Beautify the root page and add a section for adding new machines from the web page instead of via code.
- Migrate the code to current ESP-IDF version from `v5.3.5`



# Build 
### Prerequisites
- ESP-IDF v5.3 (Doesn't work with higher versions for now)
- ESP32S3 (or any ESP32 board with PSRAM)
### Steps
- Add the SSID, password for the initial Wi-Fi connection and the Tailscale auth key to `sdkconfig.defaults.example`.
- Compile and flash
```bash
mv sdkconfig.defaults.example sdkconfig.defaults # Move the file for config
idf.py build flash
```

## Directory structure
Below is a short explanation of remaining files in the project folder.

```
├── CMakeLists.txt
├── sdkconfig.defaults.example // Example sdkconfig.defaults for ease of setup
├── main
│   ├── CMakeLists.txt
│   ├── http_ep.h              // Sets up the new endpoints and their functions
│   ├── http_ep.c              
│   ├── wifi_sta.h             // Sets up the Wi-Fi connection for microlink
│   ├── wifi_sta.c
│   └── main.c
└── README.md                  // This is the file you are currently reading
```


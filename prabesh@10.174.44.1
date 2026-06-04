BCG Research — ESP32 Ballistocardiography Rig
=============================================

Firmware + PC tooling for an ESP32-based ballistocardiography (BCG) data-collection
rig. The device samples 4 load-cell channels (FL, FR, BL, BR) at 256 Hz, records to
an SD card, and serves the recorded files to a PC over WiFi.

How it works
------------

```
                          ┌─→ SD card: data.csv (continuous) + trial_N.csv (per minute)
ADC sampling @ 256 Hz ────┤
   (timer ISR → queue)    └─→ WiFi HTTP server  ──→  PC downloads completed trial files
```

- **Sampling** — a periodic timer toggles CONVST and shift-reads both ADCs at 256 Hz,
  pushing each sample onto a FreeRTOS queue (`onTimer` / `readADCs` in `main/main.c`).
- **SD recording** — `sdTask` drains the queue in batches, appending to `data.csv`
  (one continuous file) and rolling a fresh `trial_N.csv` every 60 seconds.
- **WiFi transfer** — the ESP32 joins your PC's hotspot (STA mode) and runs a small
  HTTP server exposing the SD files. 
  
> WiFi path captures >99.9% (only a brief startup transient in `trial_1.csv`).

Firmware
--------

Built with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html).

1. **Set your hotspot credentials** in `main/main.c` (WiFi section):

2. **Build, flash, and watch the boot log:**
   ```
   idf.py flash monitor
   ```
   Note the printed IP, e.g. `WIFI: connected, IP: ..........

   Check: open `http://<that-ip>/list` in a browser — you should see the file list.

HTTP endpoints (served from the SD card):
- `GET /list` — one `name,size` line per file on the card.
- `GET /get?file=trial_3.csv` — streams that file.

PC tooling (`server_upload/`)
-----------------------------
Run these from the **repo root** so output lands in `data/`

- **`download_trials.py`** — polls the ESP32 every 5 s and downloads each completed
  trial file (skips the one still being written):
  ```
  python server_upload/download_trials.py "IP"
  ```
  Files are saved into `data/`.

- **`check_gaps.py`** — verifies capture quality by checking timestamp gaps (256 Hz →
  ~3906 µs expected between samples) and reporting estimated dropped samples per file:
  ```
  python server_upload/check_gaps.py data
  ```

Project layout
--------------

```
main/main.c            ESP32 firmware (sampling, SD recording, WiFi HTTP server)
server_upload/         
  download_trials.py     pull completed trial files from the ESP32 over WiFi
  check_gaps.py          check captured CSVs for dropped samples
data/                  downloaded trial_N.csv files
```

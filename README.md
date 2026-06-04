BCG Research — ESP32 Ballistocardiography Rig
=============================================

Firmware + PC tooling for an ESP32-based ballistocardiography (BCG) data-collection. The device samples 4 load-cell channels (FL, FR, BL, BR) at 256 Hz, records to an SD card, and serves the recorded files to a PC over WiFi. The PC then forwards the collected trials on to a separate machine on a static IP for storage.

How it works
------------

```
                          ┌─→ SD card: data.csv (continuous) + trial_N.csv (per minute)
ADC sampling @ 256 Hz ────┤
   (timer ISR → queue)    └─→ WiFi HTTP server  ──→  PC downloads completed trial files
```

End-to-end, a completed trial flows:

```
ESP32 (SD + WiFi HTTP) ──→ download_trials.py ──→ data/ ──→ sync_to_server.py ──→ server:~/bcg_data
   records @ 256 Hz          (PC, WiFi pull)       on PC      (rsync/SSH, every 60s)   (static IP)
```

- **Sampling** — a periodic timer toggles CONVST and shift-reads both ADCs at 256 Hz,
  pushing each sample onto a FreeRTOS queue (`onTimer` / `readADCs` in `main/main.c`).
- **SD recording** — `sdTask` drains the queue in batches, appending to `data.csv`
  (one continuous file) and rolling a fresh `trial_N.csv` every 60 seconds.
- **WiFi transfer** — the ESP32 joins your PC's hotspot (STA mode) and runs a small
  HTTP server exposing the SD files. 
- **Server sync** — `sync_to_server.py` rsyncs the `data/` folder to a separate
  machine on a static IP every 60 s. Sync is incremental (only new/changed files),
  and runs on the PC alongside `download_trials.py`.
  
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

- **`sync_to_server.py`** — rsyncs `data/` to a machine on a static IP every 60 s, over
  SSH. Run it in a second terminal alongside `download_trials.py`:
  ```
  python server_upload/sync_to_server.py user@<static-ip> bcg_data data 60
  #                                       └ remote login   └remote └local └secs
  ```
  Setup:
  - **rsync** must be installed. On Windows: `winget install MSYS2.MSYS2`, then
    `C:\msys64\usr\bin\pacman.exe -S rsync openssh`.
  - **Key-based SSH** to the receiver so the loop never prompts for a password:
    `ssh-keygen` (if needed), then install your public key in the receiver's
    `~/.ssh/authorized_keys`.
  - Config defaults live at the top of the file. Two settings matter when the
    **sender is Windows**: `SSH_COMMAND` forces MSYS2's ssh (the native Windows ssh
    breaks the rsync stream); and when the **receiver is Windows**, set `REMOTE_RSYNC`
    to its `rsync.exe` path. For a Linux sender/receiver, leave both empty.

How to run
--------------------

All three devices (ESP32, this PC, and the receiver) share one same internet. The ESP32 and the receiver each get a fixed local IP on it.

**0. One-time setup**
- Flash the firmware (see *Firmware* above) and confirm `http://<esp32-ip>/list` works.
- Install rsync on this PC and set up key-based SSH to the receiver (see
  `sync_to_server.py` *Setup* above).

**1. Power the ESP32** and let it join the hotspot. It records to its SD card and
   serves the files automatically. Grab its IP from the
   boot log (`idf.py monitor`, then `Ctrl+]` to exit) or your hotspot's device list.

**2. Terminal 1 — pull trials onto this PC** (into `data/`):
   ```
   python server_upload/download_trials.py <esp32-ip>
   ```

**3. Terminal 2 — sync to the receiver** every 60 s:
   ```
   python server_upload/sync_to_server.py user@<static-ip> bcg_data data 60
   ```

**4. Verify** — on the receiver, `ls ~/bcg_data` should show trials appearing a minute
   or two after each completes
   Optionally check capture quality: `python server_upload/check_gaps.py data`.

The ESP32 keeps recording as long as it has power and stays on the hotspot.

Project layout
--------------

```
main/main.c            ESP32 firmware (sampling, SD recording, WiFi HTTP server)
server_upload/         
  download_trials.py     pull completed trial files from the ESP32 over WiFi
  sync_to_server.py      rsync data/ to a static-IP server every 60 s (over SSH)
  check_gaps.py          check captured CSVs for dropped samples
data/                  downloaded trial_N.csv files (git-ignored)
```

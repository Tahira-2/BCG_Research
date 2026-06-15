"""
Downloads BCG trial files from the ESP32 over WiFi (PC hotspot) as they complete.

The ESP32 records to its SD card and rolls a new trial_N.csv every minute, while
serving the files via a tiny HTTP server. This script polls that server and pulls
each completed trial file to the PC.

Usage (run from the repo root):
    python server_upload/download_trials.py "IP"        
    python server_upload/download_trials.py "IP" data   # IP + output folder

Find the ESP32's IP from `idf.py monitor` at boot
"""

import os
import sys
import time
import urllib.request

ESP_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.137.1"
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else "data"
POLL_SECONDS = 5
BASE = f"http://{ESP_IP}"


def list_files():
    """Return {name: size} from the ESP32's /list endpoint."""
    with urllib.request.urlopen(f"{BASE}/list", timeout=5) as r:
        text = r.read().decode(errors="ignore")
    files = {}
    for line in text.splitlines():
        if "," in line:
            name, _, size = line.rpartition(",")
            try:
                files[name] = int(size)
            except ValueError:
                pass
    return files


def trial_index(name):
    """trial_7.csv -> 7, or None if the name isn't a trial file."""
    if name.startswith("trial_") and name.endswith(".csv"):
        try:
            return int(name[len("trial_"):-len(".csv")])
        except ValueError:
            return None
    return None


def download(name, outdir):
    dest = os.path.join(outdir, name)
    urllib.request.urlretrieve(f"{BASE}/get?file={name}", dest)
    return dest


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    done = set()
    print(f"Polling {BASE}/list every {POLL_SECONDS}s -> {OUTDIR}/  (Ctrl+C to stop)")

    while True:
        try:
            files = list_files()
        except Exception as e:
            print(f"list failed ({e}); is the ESP32 connected to the hotspot?")
            time.sleep(POLL_SECONDS)
            continue

        trials = [(trial_index(n), n) for n in files if trial_index(n) is not None]
        latest = max((i for i, _ in trials), default=None)  # still being written

        for idx, name in sorted(trials):
            if name in done or idx == latest:
                continue
            try:
                dest = download(name, OUTDIR)
                done.add(name)
                print(f"downloaded {name} ({files[name]} bytes) -> {dest}")
            except Exception as e:
                print(f"download {name} failed: {e}")

        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()

"""
Downloads BCG trial files from the ESP32 over WiFi (PC hotspot) as they complete.

Usage (run from the repo root):
    python server_upload/download_trials.py "IP"        
    python server_upload/download_trials.py "IP" data   # IP + output folder
"""

import os
import shutil
import sys
import time
import urllib.request

DEFAULT_SEC = 2 * 36000
ESP_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.43.145"
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else "data"
POLL_SECONDS = sys.argv[3] if len(sys.argv) > 3 else (DEFAULT_SEC)

# Generous timeout: the ESP serves files with the same single web server that is
# busy sampling and writing the SD card. /list and /get touch the SD card, so
# they block behind the writer's SD lock while a trial is being recorded and can
# take several seconds. A short 5 s timeout fired spuriously during collection;
# these values retry instead of giving up.
HTTP_TIMEOUT = 20
RETRIES = 3
BASE = f"http://{ESP_IP}"


def list_files():
    """Return {name: size} from the ESP32's /list endpoint (retried)."""
    text = None
    last = None
    for attempt in range(RETRIES):
        try:
            with urllib.request.urlopen(f"{BASE}/list", timeout=HTTP_TIMEOUT) as r:
                text = r.read().decode(errors="ignore")
            break
        except Exception as e:
            last = e
            time.sleep(1 + attempt)
    if text is None:
        raise last
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
    """Download to a temp file, then atomically rename into place. If the
    transfer times out or drops mid-stream it raises (after retries) and never
    leaves a truncated file at the destination."""
    dest = os.path.join(outdir, name)
    tmp = dest + ".part"
    last = None
    for attempt in range(RETRIES):
        try:
            with urllib.request.urlopen(f"{BASE}/get?file={name}", timeout=HTTP_TIMEOUT) as r, \
                    open(tmp, "wb") as out:
                shutil.copyfileobj(r, out)
            os.replace(tmp, dest)   # atomic on the same filesystem
            return dest
        except Exception as e:
            last = e
            time.sleep(1 + attempt)
    try:
        os.remove(tmp)
    except OSError:
        pass
    raise last


def local_size(name, outdir):
    """Byte size of an already-downloaded file, or None if we don't have it."""
    try:
        return os.path.getsize(os.path.join(outdir, name))
    except OSError:
        return None


def main():
    os.makedirs(OUTDIR, exist_ok=True)
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
            if idx == latest:
                continue
            # Skip files we already have at the same size; re-download if the
            # ESP32's copy grew/changed (these trial CSVs only ever get appended).
            have = local_size(name, OUTDIR)
            if have == files[name]:
                continue
            try:
                dest = download(name, OUTDIR)
                if have is None:
                    print(f"downloaded {name} ({files[name]} bytes) -> {dest}")
                else:
                    print(f"updated {name} ({have} -> {files[name]} bytes) -> {dest}")
            except Exception as e:
                print(f"download {name} failed: {e}")

        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()

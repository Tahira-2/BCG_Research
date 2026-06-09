"""
Checks BCG trial CSV files for dropped samples by looking at timestamp gaps.

The firmware samples at 256 Hz, so consecutive ts(us) values should be ~3906 us
apart. A bigger gap means samples were dropped there. This reports, per file:
the sample count, how many gaps were too large, the estimated dropped samples,
and the worst gap.

Usage (run from the repo root):
    python server_upload/check_gaps.py data            # check every *.csv in the folder
    python server_upload/check_gaps.py data/trial_3.csv
    python server_upload/check_gaps.py data 256        # folder + sample rate (Hz)
"""

import glob
import os
import sys

NOMINAL_HZ = 256


def check_file(path, period_us):
    """Return (n_samples, n_gaps, est_dropped, max_gap_us, n_corrupt) for one file.
    """
    timestamps = []
    n_corrupt = 0
    with open(path, "rb") as f:
        for raw in f:
            try:
                line = raw.decode("ascii").strip()
            except UnicodeDecodeError:
                n_corrupt += 1  # non-text bytes -> corrupted line
                continue
            if not line:
                continue
            if not line[0].isdigit():
                if line.startswith("ts(us)"):
                    continue  # header line is expected, not corruption
                n_corrupt += 1  # any other non-numeric junk is suspect
                continue
            ts_str = line.split(",", 1)[0]
            try:
                timestamps.append(int(ts_str))
            except ValueError:
                n_corrupt += 1
                continue

    if len(timestamps) < 2:
        return len(timestamps), 0, 0, 0, n_corrupt

    n_gaps = 0
    est_dropped = 0
    max_gap = 0
    # A gap is "too large" if it exceeds 1.5 periods (allows normal jitter).
    threshold = 1.5 * period_us
    for prev, cur in zip(timestamps, timestamps[1:]):
        delta = cur - prev
        if delta > max_gap:
            max_gap = delta
        if delta > threshold:
            n_gaps += 1
            # round(delta / period) - 1 samples are missing in this gap
            est_dropped += max(0, round(delta / period_us) - 1)

    return len(timestamps), n_gaps, est_dropped, max_gap, n_corrupt


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "data"
    hz = int(sys.argv[2]) if len(sys.argv) > 2 else NOMINAL_HZ
    period_us = 1_000_000 / hz

    if os.path.isdir(target):
        paths = sorted(glob.glob(os.path.join(target, "*.csv")))
    else:
        paths = [target]

    if not paths:
        print(f"No CSV files found at {target}")
        return

    print(f"Sample rate {hz} Hz  ->  nominal gap {period_us:.1f} us\n")
    print(f"{'file':<24}{'samples':>9}{'big gaps':>10}{'dropped':>9}{'corrupt':>9}{'max gap us':>12}")
    print("-" * 73)

    tot_samples = tot_dropped = tot_corrupt = 0
    n_corrupt_files = 0
    for path in paths:
        try:
            n, gaps, dropped, max_gap, corrupt = check_file(path, period_us)
        except Exception as e:
            # Never let one unreadable file abort the whole report.
            print(f"{os.path.basename(path):<24}  <-- could not read ({e})")
            n_corrupt_files += 1
            continue
        tot_corrupt += corrupt
        if corrupt:
            # Corrupt files' gap stats are meaningless; keep them out of the total.
            n_corrupt_files += 1
            flag = f"  <-- CORRUPT ({corrupt} bad lines)"
        else:
            tot_samples += n
            tot_dropped += dropped
            flag = "  <-- loss" if dropped else ""
        print(f"{os.path.basename(path):<24}{n:>9}{gaps:>10}{dropped:>9}{corrupt:>9}{max_gap:>12}{flag}")

    print("-" * 73)
    if tot_samples:
        pct = 100.0 * tot_dropped / (tot_samples + tot_dropped)
        print(f"TOTAL  samples={tot_samples}  est dropped={tot_dropped}  ({pct:.2f}% loss)")
    if n_corrupt_files:
        print(f"WARNING: {n_corrupt_files} file(s) corrupted ({tot_corrupt} bad lines) -- "
              f"their gap stats are unreliable around the damage.")


if __name__ == "__main__":
    main()

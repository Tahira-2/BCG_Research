"""
Plots a BCG data file

Usage:
    python main/plot_trial.py data/trial_5.csv                 # default 0-1 s
    python main/plot_trial.py data/trial_5.csv --start 2 --end 5    #from 2s to 5s window
    python main/plot_trial.py data/trial_5.csv -s 10 -e 10.25   # 250 ms window
    python main/plot_trial.py data/trial_5.csv --save out.png   # write PNG instead of showing
"""

import argparse
import sys

import matplotlib.pyplot as plt

CHANNELS = ["FL", "FR", "BL", "BR"]


def read_file(path):
    """Return (t_s, {channel: [values]}) with t_s in seconds relative to sample 0.

    Tolerates the header line and skips anything that doesn't parse as a data row,
    so a few corrupt lines won't abort the plot.
    """
    ts_us = []
    cols = {ch: [] for ch in CHANNELS}
    with open(path, "rb") as f:
        for raw in f:
            try:
                line = raw.decode("ascii").strip()
            except UnicodeDecodeError:
                continue  # corrupted line, skip
            if not line or not line[0].isdigit():
                continue  # blank line or header
            parts = line.split(",")
            if len(parts) < 5:
                continue
            try:
                ts_us.append(int(parts[0]))
                for ch, v in zip(CHANNELS, parts[1:5]):
                    cols[ch].append(float(v))
            except ValueError:
                # corrupt row: drop the timestamp we may have appended
                if len(ts_us) > len(cols["FL"]):
                    ts_us.pop()
                continue

    if not ts_us:
        return [], cols

    t0 = ts_us[0]
    t_s = [(t - t0) / 1_000_000.0 for t in ts_us]
    return t_s, cols


def main():
    p = argparse.ArgumentParser(description="Plot a BCG data file, all four sensors together.")
    p.add_argument("file", help="path to the trial/data CSV")
    p.add_argument("-s", "--start", type=float, default=0.0, help="window start in seconds (default 0)")
    p.add_argument("-e", "--end", type=float, default=1.0, help="window end in seconds (default 1)")
    p.add_argument("--save", metavar="PNG", help="save to this PNG instead of showing a window")
    args = p.parse_args()

    t_s, cols = read_file(args.file)
    if not t_s:
        print(f"No data rows found in {args.file}")
        sys.exit(1)

    # Keep only samples inside the requested [start, end] window.
    idx = [i for i, t in enumerate(t_s) if args.start <= t <= args.end]
    if not idx:
        print(f"No samples in window {args.start}-{args.end} s "
              f"(file spans 0-{t_s[-1]:.3f} s).")
        sys.exit(1)

    tw = [t_s[i] for i in idx]

    plt.figure(figsize=(12, 6))
    for ch in CHANNELS:
        plt.plot(tw, [cols[ch][i] for i in idx], label=ch, linewidth=0.8)

    plt.xlabel("time (s, relative to first sample)")
    plt.ylabel("ADC value")
    plt.title(f"{args.file}   [{args.start}-{args.end} s, {len(idx)} samples]")
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()

    if args.save:
        plt.savefig(args.save, dpi=120)
        print(f"saved {args.save}")
    else:
        plt.show()


if __name__ == "__main__":
    main()

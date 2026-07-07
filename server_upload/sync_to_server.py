"""
Periodically syncs the collected BCG data files from THIS PC to another
computer that has a static IP, using rsync over SSH.

rsync's delta algorithm only transfers files that are new or have changed
"""

import os
import shutil
import subprocess
import sys
import time

RSYNC_FALLBACKS = [
    r"C:\msys64\usr\bin\rsync.exe",
    r"C:\Program Files\cwRsync\bin\rsync.exe",
]

def find_rsync():
    """Return a usable rsync path: PATH first, then known install locations."""
    found = shutil.which("rsync")
    if found:
        return found
    for path in RSYNC_FALLBACKS:
        if os.path.exists(path):
            return path
    return None


# ---------------- CONFIG (override via command-line args) ----------------
REMOTE = "user@IP"   # SSH login of the static-IP computer
REMOTE_DIR = "bcg_data"         # destination folder on receiver computer
LOCAL_DIR = "data"              # folder of trial_N.csv files on sender PC
SYNC_SECONDS = 60               # run rsync this every "SYNC_SECONDS "

#path to the receiver's rsync, passed via --rsync-path when set.
# blank for Linux user
REMOTE_RSYNC = ""

#On a Linux/Mac sender, set SSH_COMMAND = "" to use the system ssh.
#SSH_COMMAND = "/usr/bin/ssh -i /c/Users/tahir/.ssh/id_ed25519 -o StrictHostKeyChecking=accept-new"
SHH_COMMAND = ""

def build_command(rsync, remote, remote_dir, local_dir, remote_rsync="",
                  ssh_command=""):
    src = local_dir.rstrip("/\\") + "/"
    dest = f"{remote}:{remote_dir.rstrip('/')}/"
    cmd = [rsync, "-avz", "--partial"]
    if ssh_command:
        cmd += ["-e", ssh_command]
    if remote_rsync:
        cmd.append(f"--rsync-path={remote_rsync}")
    cmd += [src, dest]
    return cmd
#from linux
#rsync -avz --partial source_folder/ user@windows_ip:/path/on/windows/

def sync_once(cmd):
    """Run one rsync pass. Returns True on success (exit code 0)."""
    result = subprocess.run(cmd)
    return result.returncode == 0


def main():
    remote = sys.argv[1] if len(sys.argv) > 1 else REMOTE
    remote_dir = sys.argv[2] if len(sys.argv) > 2 else REMOTE_DIR
    local_dir = sys.argv[3] if len(sys.argv) > 3 else LOCAL_DIR
    interval = int(sys.argv[4]) if len(sys.argv) > 4 else SYNC_SECONDS

    rsync = find_rsync()
    if rsync is None:
        print("ERROR: rsync not found on PATH or in known install locations.")
        print("Install it (e.g. MSYS2: pacman -S rsync, or cwRsync), or add its")
        print("folder to RSYNC_FALLBACKS. See the header of this file for notes.")
        sys.exit(1)

    cmd = build_command(rsync, remote, remote_dir, local_dir, REMOTE_RSYNC,
                        SSH_COMMAND)
    print(f"Syncing {local_dir}/ -> {remote}:{remote_dir}/ every {interval}s")
    print(f"Command: {' '.join(cmd)}")
    print("Press Ctrl+C to stop.\n")

    try:
        while True:
            start = time.monotonic()
            ok = sync_once(cmd)
            if not ok:
                print("rsync failed this cycle; will retry next interval. "
                      "Is the server up and SSH reachable?")
            # Sleep the remaining of the interval
            elapsed = time.monotonic() - start
            time.sleep(max(0, interval - elapsed))
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()

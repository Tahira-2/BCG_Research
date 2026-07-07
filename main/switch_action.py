import RPi.GPIO as GPIO
import time
import urllib.request
import sys

# ----------- HTTP ----------------
ESP_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.137.1"

def send_state(state):
    url = f"http://{ESP_IP}/switch?state={state}"

    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            print("ESP:", r.read().decode().strip())
    except Exception as e:
        print("sent failed:", e)

# ----------- GPIO ----------------
GPIO.setmode(GPIO.BCM)
GPIO.setup(17, GPIO.IN, pull_up_down=GPIO.PUD_UP)

print("Watching switch")
last = None

try:
    while True:
        state = GPIO.input(17)

        if state != last:
            last = state
            send_state(state)   # 1-active, 0 - pause
            if state == 0:  # switch pressed
                print("Switch pressed — shutting down until next cycle")
                time.sleep(30)
                #print("Resuming...")
                last = None
            else:
                print("Data collection active")  # only prints when state changes TO active

        time.sleep(0.05)

except KeyboardInterrupt:
    GPIO.cleanup()
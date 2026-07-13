"""
PlatformIO pre-script for device testing.
Adds delay after upload to allow USB CDC to re-enumerate before opening serial.
"""
import sys
import time


def load_platformio_env():
    """Return PlatformIO's injected env, or pass standalone script discovery."""
    try:
        Import("env")
        return env
    except NameError as exc:
        if __name__ == "__main__":
            print("[test-delay] PlatformIO pre-script self-check OK")
            sys.exit(0)
        raise RuntimeError("scripts/test_delay.py must be loaded by PlatformIO") from exc


env = load_platformio_env()

def post_upload_delay(source, target, env):
    """Wait for USB CDC device to re-enumerate after upload."""
    print("Waiting 5 seconds for USB CDC to re-enumerate...")
    time.sleep(5)

# Only apply to device environment during testing
if env.get("PIOENV") == "device":
    env.AddPostAction("upload", post_upload_delay)

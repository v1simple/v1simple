"""
PlatformIO pre-script for device testing.
Adds delay after upload to allow USB CDC to re-enumerate before opening serial.
"""
import time


Import("env")

def post_upload_delay(source, target, env):
    print("Waiting 5 seconds for USB CDC to re-enumerate...")
    time.sleep(5)

if env.get("PIOENV") == "device":
    env.AddPostAction("upload", post_upload_delay)

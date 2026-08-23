"""Fixed hardware/profile bounds for raw bench camera evidence."""

EXPECTED_CAMERA_NAME = "Global Shutter Camera"
EXPECTED_CAMERA_PROFILE = {
    "auto_exposure_mode": 8,
    "auto_exposure_priority": 0,
    "focus_abs": 306,
    "video_exposure_time_abs": 50,
    "gain": 0,
    "framerate": 200,
    "input_pixel_format": "nv12",
    "video_size": "1280x720",
    "capture_backend": "avfoundation_native",
}

MIN_DISPLAY_SCALE = 0.55
MAX_DISPLAY_SCALE = 1.60

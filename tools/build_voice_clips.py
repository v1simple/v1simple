#!/usr/bin/env python3
"""Generate V1 Simple voice clips with Piper (redistributable neural TTS).

This replaces the previous macOS `say` / "Samantha" pipeline, whose rendered
audio could not be redistributed. Default voice: en_US-libritts_r-medium
(LibriTTS-R, CC BY 4.0, https://openslr.org/141/) — free to redistribute with
attribution. See THIRD_PARTY_NOTICES.md.

One-time setup:
    pip install piper-tts
    python3 -m piper.download_voices en_US-libritts_r-medium --download-dir voices

Generate (run from the repo root; overwrites the audio artifacts in place):
    python3 tools/build_voice_clips.py \
        --model voices/en_US-libritts_r-medium.onnx --out .

Then redeploy the clips to the LittleFS image before flashing:
    cd interface && npm run deploy        # (or ./build.sh --upload-fs)

Outputs (formats are byte-compatible with the originals, so the firmware and
config/audio_asset_manifest.json are unchanged):
    tools/freq_audio/mulaw/*.mul   118 raw mu-law @22050Hz clips
    include/alert_audio.h          12 alert phrases (int16 PCM C arrays)
    include/warning_audio.h        volume-zero warning (int16 PCM C array)

To change wording or pronunciation, edit the text maps below.
"""
import argparse, audioop, os
import numpy as np
from piper import PiperVoice

SR = 22050
ONES  = ["oh","one","two","three","four","five","six","seven","eight","nine"]
TEENS = ["ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen"]
TENW  = {2:"twenty",3:"thirty",4:"forty",5:"fifty",6:"sixty",7:"seventy",8:"eighty",9:"ninety"}

def num2word(n):
    if n < 10: return "oh " + ONES[n]          # tens_00..09 -> "oh oh".."oh nine"
    if 10 <= n <= 19: return TEENS[n-10]
    t, o = divmod(n, 10)
    return TENW[t] if o == 0 else f"{TENW[t]} {ONES[o]}"

def freq_clips():
    # name -> spoken text (matches the original generate_freq_audio.sh wording)
    c = {"band_ka":"K A","band_k":"K","band_x":"X","band_laser":"Laser",
         "dir_ahead":"ahead","dir_behind":"behind","dir_side":"side","bogeys":"bogeys"}
    for d in range(10): c[f"digit_{d}"] = ONES[d]
    for n in range(100): c[f"tens_{n:02d}"] = num2word(n)
    return c

# alert_audio.h — order must match include/alert_audio.h consumers
ALERT_ORDER = [
    ("laser_ahead","Laser ahead"),("laser_behind","Laser behind"),("laser_side","Laser side"),
    ("ka_ahead","K A ahead"),("ka_behind","K A behind"),("ka_side","K A side"),
    ("k_ahead","K ahead"),("k_behind","K behind"),("k_side","K side"),
    ("x_ahead","X ahead"),("x_behind","X behind"),("x_side","X side"),
]
WARNING_TEXT = "Warning. Volume zero."

def synth(voice, text):
    """Synthesize -> trim silence -> peak-normalize -> int16 @22050Hz mono."""
    pcm = b"".join(ch.audio_int16_bytes for ch in voice.synthesize(text))
    a = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    thr = 0.015 * 32767.0
    idx = np.where(np.abs(a) > thr)[0]
    if len(idx):
        pad = int(0.006 * SR)
        a = a[max(0, idx[0]-pad): min(len(a), idx[-1]+pad+1)]
    peak = float(np.abs(a).max()) if len(a) else 0.0
    if peak > 0: a *= (0.89 * 32767.0 / peak)
    return np.clip(np.round(a), -32768, 32767).astype(np.int16)

def _rows(f, arr):
    n = len(arr)
    for i in range(0, n, 12):
        line = ", ".join(f"{int(s):6d}" for s in arr[i:i+12])
        f.write(f"    {line},\n" if i+12 < n else f"    {line}\n")

def write_alert_header(path, items):
    with open(path, "w") as f:
        f.write("// Auto-generated alert voice samples\n")
        f.write("// 12 phrases: [Laser|Ka|K|X] [ahead|behind|side]\n")
        f.write("// 22050Hz mono 16-bit PCM\n#pragma once\n\n#include <stdint.h>\n\n")
        for name, arr in items:
            n = len(arr); dur = (n*1000)//SR
            f.write(f"// {name}: {n} samples, {dur}ms\n")
            f.write(f"#define ALERT_{name.upper()}_SAMPLES {n}\n")
            f.write(f"#define ALERT_{name.upper()}_DURATION_MS {dur}\n")
            f.write(f"static const int16_t alert_{name}[{n}] PROGMEM = {{\n")
            _rows(f, arr); f.write("};\n\n")

def write_warning_header(path, arr):
    n = len(arr); dur = (n*1000)//SR
    with open(path, "w") as f:
        f.write("// Auto-generated from warning_volume_zero.raw\n")
        f.write(f"// {n} samples @ 22050Hz = {dur}ms\n// Size: {n*2} bytes\n")
        f.write("#pragma once\n\n#include <stdint.h>\n\n")
        f.write(f"#define WARNING_VOLUME_ZERO_PCM_SAMPLES {n}\n")
        f.write(f"#define WARNING_VOLUME_ZERO_PCM_DURATION_MS {dur}\n\n")
        f.write(f"static const int16_t warning_volume_zero_pcm[{n}] PROGMEM = {{\n")
        _rows(f, arr); f.write("};\n")

def main():
    ap = argparse.ArgumentParser(description="Generate V1 Simple voice clips with Piper.")
    ap.add_argument("--model", required=True, help="path to the Piper .onnx voice model")
    ap.add_argument("--out", required=True, help="repo root (writes tools/freq_audio/mulaw + include/)")
    args = ap.parse_args()

    voice = PiperVoice.load(args.model)
    muldir = os.path.join(args.out, "tools/freq_audio/mulaw"); os.makedirs(muldir, exist_ok=True)
    incdir = os.path.join(args.out, "include"); os.makedirs(incdir, exist_ok=True)

    fc = freq_clips()
    for name, text in fc.items():
        arr = synth(voice, text)
        with open(os.path.join(muldir, name + ".mul"), "wb") as f:
            f.write(audioop.lin2ulaw(arr.tobytes(), 2))
    print(f"[freq]    wrote {len(fc)} .mul clips -> {muldir}")

    items = [(name, synth(voice, text)) for name, text in ALERT_ORDER]
    write_alert_header(os.path.join(incdir, "alert_audio.h"), items)
    print(f"[alerts]  wrote alert_audio.h ({len(items)} phrases)")

    warr = synth(voice, WARNING_TEXT)
    write_warning_header(os.path.join(incdir, "warning_audio.h"), warr)
    print(f"[warning] wrote warning_audio.h ({len(warr)} samples, {(len(warr)*1000)//SR}ms)")

if __name__ == "__main__":
    main()

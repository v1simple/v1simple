#!/bin/bash
# DEPRECATED. The macOS `say` (Samantha) pipeline is no longer used: Apple's
# system voices cannot be redistributed. Voice clips are now generated with
# Piper (redistributable, CC BY 4.0 voice). See tools/build_voice_clips.py.
echo "Deprecated. Use: python3 tools/build_voice_clips.py --model <voice>.onnx --out ."
exit 1

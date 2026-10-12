# Analyzes audio_dump.raw (16-bit stereo 48 kHz): level and zero-crossing rate per second.
# Music/effects: typical ZCR 0.01..0.15; white noise ~0.5; silence: RMS ~0. Also writes audio_dump.wav.
# Usage: python -I tools/audio_check.py audio_dump.raw
import array
import math
import sys
import wave

data = array.array("h")
with open(sys.argv[1], "rb") as f:
    data.frombytes(f.read())
rate = 48000
frames = len(data) // 2
print("duration: %.1f s" % (frames / rate))
for sec in range(int(frames / rate)):
    seg = data[sec * rate * 2:(sec + 1) * rate * 2:2]   # left channel
    if not seg:
        break
    rms = math.sqrt(sum(x * x for x in seg) / len(seg)) / 32768.0
    zc = sum(1 for i in range(1, len(seg)) if (seg[i - 1] < 0) != (seg[i] < 0)) / len(seg)
    clip = sum(1 for x in seg if abs(x) >= 32767)
    print("t=%3ds rms %.3f zcr %.3f clipped %d" % (sec, rms, zc, clip))
with wave.open(sys.argv[1].rsplit(".", 1)[0] + ".wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(data.tobytes())

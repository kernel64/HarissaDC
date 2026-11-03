# SoundTouch in HarissaDC

- Version: 2.3.3, from https://codeberg.org/soundtouch/soundtouch (tag 2.3.3).
- License: LGPL-2.1 (COPYING.TXT), Olli Parviainen.
- Files kept: the time-stretch path only (SoundTouch, TDStretch, RateTransposer, the
  interpolators, AAFilter, FIRFilter, FIFOSampleBuffer, cpu_detect) and their headers,
  flattened into this folder. BPM detection, the MMX/SSE paths and the build files are left out.
- Local change: `override` removed from destructor declarations (the Xbox 360 XDK's VS2010
  compiler rejects it there). Nothing else is modified.
- Built by harissadc.vcxproj with SOUNDTOUCH_INTEGER_SAMPLES=1 and ST_NO_EXCEPTION_HANDLING=1.
- Used by src/platform/xbox360/xb_audio.cpp (audiostretch=1).

Drop brew-stage alert `.mp3` clips here, then run `python scripts/embed_audio.py`
and rebuild. Files in this folder are not read by the firmware directly -
they get baked into `src/main.cpp` as PROGMEM byte arrays (same reason as
the logos - no filesystem-backed static assets in this project, see
DECISIONS.md) and served by the single generic `/audio?file=<name>` route.

Currently expected by the Water Prep stage (`web/ys-boot.html`, `STAGES`):
- `water_prep_fill.mp3` - "Fill vessel with water."
- `water_prep_heating.mp3` - "Starting heating to strike temperature."
- `water_prep_ready.mp3` - "Water is ready."

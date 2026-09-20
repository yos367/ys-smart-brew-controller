Stage-announcement voice clips (`.mp3`). **Currently NOT used or embedded** -
the voice-audio system was removed from the firmware and the page (flash
space + simplicity) and comes back once a microSD card module is wired in.
Keep these files: they cost nothing here and will be needed again. Do not
run `scripts/embed_audio.py` until that hardware step is underway (before
buying the module, confirm a free GPIO for its CS line).

Which clip belongs to which step: the `audio:` event ids on the stage steps
in `web/ys-boot.html` (e.g. `water_prep_fill`) match these filenames
(`water_prep_fill.mp3`, ...).

# Minimum audible MIDI reference

This fixed project is an audible acceptance fixture, not a generated test input.

Builder request: `sample_rate=48000`, `maximum_block_frames=256`,
`output_channels=2`, `output_channel_mask=3`, `playback_start_sample=0`,
`loop_start_tick=0`, and `loop_end_tick=3840`.

The project is 120 BPM, 4/4, and contains one centred unity-gain instrument
track. C4, E4, G4, and C5 start at ticks 0, 960, 1920, and 2880. Each note is
480 ticks long with velocity 96 on channel 1. At 48 kHz their Note On samples
are 0, 24000, 48000, and 72000; Note Off samples are 12000, 36000, 60000, and
84000. The loop is 96000 samples long. Event sample tolerances are one sample.

`expected-events.tsv` describes the non-empty `events` table. The
`boundary`, `initial-chase`, and `loop-start-chase` tables are intentionally
empty. `expected-audio.properties` defines the offline render and measurement
acceptance limits. Frequency is measured only from same-direction upward
zero-crossing intervals while C4 is in sustain and before its Note Off.

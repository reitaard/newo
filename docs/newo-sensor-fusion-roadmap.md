# Newo sensor-fusion roadmap

This note preserves promising directions for extending the Newo/Newo2 CSI experiment beyond simple motion-triggered camera capture. It is intentionally exploratory: these are candidate sensor-fusion paths to test, not claims of validated capability.

## Core principle

Wi-Fi CSI is an ambient RF disturbance signal. It is useful for detecting changes in the propagation environment, but it is weak at answering semantic questions such as who moved, exactly where they are, what object is involved, or why the scene changed. Pair CSI with sensors that add a missing physical dimension rather than asking CSI to infer everything alone.

## Promising combinations

### CSI + mmWave radar

- CSI: broad room-scale RF disturbance and multi-path changes.
- mmWave: stronger range, velocity and micro-Doppler information.
- Goal: improve localization, activity classification and confidence compared with either sensor alone.

### CSI + microphone / passive audio

- CSI: silent body/environment movement.
- Audio: acoustic events and scene cues.
- Examples: distinguish quiet walking from impact events, door/object sounds, typing/desk activity, or noisy appliance activity.
- Newo already has a microphone, so this is a low-hardware-cost fusion path.

### CSI + BLE identity/proximity

- CSI: someone/something moved through a region.
- BLE phone/watch/beacon: likely device/person proximity.
- Goal: associate anonymous RF movement with a probable known device without asking CSI to perform fragile gait identity.

### CSI + UWB or Wi-Fi FTM

- CSI: device-free movement/disturbance.
- UWB/FTM: absolute or relative range to tagged devices.
- Goal: honest spatial tracking and room coordinates rather than pretending a CSI heatmap provides precise position.

### CSI + simple physical-state sensors

Useful partners include door/contact switches, smart-plug/power telemetry, chair/bed pressure sensors, light, CO2, temperature and air-quality sensors.

Examples:
- door opens + CSI disturbance + desk power rises -> likely room entry followed by desk activity;
- bed/chair pressure active + CSI settles -> occupied/resting state;
- contact event + CSI direction change -> stronger entry/exit evidence.

### CSI + camera

Do more than wake the camera. CSI can guide when/where to look, while the camera provides semantic ground truth. Camera/VPS labels can also become training labels for CSI feasibility studies.

### CSI + pan/tilt or mobile camera

Use RF-path changes to guide another sensor toward the region of interest, then let vision confirm what is there. This is more valuable than a fixed binary camera trigger if direction/zone estimates become reliable.

### CSI + active acoustic sensing

Use a speaker as a transmitter and a microphone as a receiver, turning Newo into a crude sonar-like sensor in addition to its Wi-Fi RF sensing.

Two main modes:

1. **Doppler tone sensing**: emit a steady high-frequency/inaudible tone; moving hands or bodies reflect it with a small frequency shift. The microphone measures that shift to estimate motion/direction/gesture properties.
2. **Chirp / echo sensing**: emit a known short chirp or coded waveform; correlate the recorded echo against the transmitted signal. Echo delay and changes over time can reveal near-field distance/motion or support gesture tracking.

This gives Newo complementary physics:

```text
Wi-Fi CSI       -> RF multipath / room-scale disturbance
Active acoustics -> short-range acoustic motion / echo geometry
Camera          -> visual semantics / ground truth
```

Important hardware caveat: the existing Newo speaker + amplifier + microphone have not been characterized above the audible band. Do not assume true ultrasonic operation. First measure usable frequency response. If the current hardware rolls off before ~18-20 kHz, experiments can still use audible or near-ultrasonic chirps, but they will not be silent and performance/range will differ.

Historical references proving the basic idea on commodity speaker/microphone hardware:
- Microsoft Research SoundWave: https://www.microsoft.com/en-us/research/project/soundwave-using-the-doppler-effect-to-sense-gestures/
- University of Washington FingerIO: https://fingerio.cs.washington.edu/

### CSI + VPS/AI context

Do not send raw CSI directly to the conversational model. Convert it into compact validated state such as:

```text
room_presence=true
activity_level=medium
motion_zone=desk_side
motion_trend=approaching
last_large_motion_ms=2100
```

Then combine that state with microphone, camera and conversation context. This can influence when Newo waits, looks, asks for another frame, or interprets a command without pretending the CSI itself understands semantics.

## Priority order for Newo

Low extra hardware / high value:
1. CSI + microphone/passive audio + camera + VPS fusion.
2. CSI + active acoustic experiments using existing speaker/mic, after frequency-response testing.
3. CSI + contact/power/pressure/environment state for practical room context.

One new module:
4. CSI + mmWave radar for stronger spatial/motion sensing.

For real tagged positioning:
5. CSI + UWB / Wi-Fi FTM.

Longer-term experimental direction:
6. CSI-guided pan/tilt camera or mobile node.

## Validation rule

Treat every added modality as an independent measured signal. Prefer fusion of weak but complementary evidence over unsupported CSI-only claims. Preserve raw data and labels so negative results remain useful.
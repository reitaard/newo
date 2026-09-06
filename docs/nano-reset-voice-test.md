# Nano RESET voice-trigger test

## Scope

The sketch at `arduino/nano-reset-voice-test/nano-reset-voice-test.ino` turns a classic Nano's onboard RESET button into a one-shot physical voice trigger. The Nano carries no microphone audio. Newo continues to capture INMP441 PCM, send it to the existing `/voice` stream, stop on the existing Sherpa `final` event or 30-second firmware timeout, and enter the existing assistant/speaker path exactly once.

The Nano reads and clears `MCUSR` during AVR early initialization. Power-on, watchdog and brownout resets do not trigger voice. An external reset arms one event, emitted only after a successful NEOWIRE/1 handshake. `READY` covers Nano resets where the USB-serial bridge never disconnects.

Classic Nano hardware cannot distinguish the onboard RESET switch from another source that pulls RESET low, including some USB-serial DTR transitions. Record whether initial VCP open produces an external-reset trigger on the exact board/bridge used.

## Build

With Arduino AVR Boards installed:

```powershell
arduino-cli compile --fqbn "arduino:avr:nano:cpu=atmega328" arduino/nano-reset-voice-test
```

Some clone Nanos require `cpu=atmega328old` for upload, but that bootloader selection does not change this sketch's ATmega328P logic. This task does not authorize uploading either firmware.

Validated on Arduino AVR Boards 1.8.8: 5,130 bytes program storage and 521 bytes globals. The complete Newo build on Arduino-ESP32 3.3.10 uses 2,888,807 bytes program storage and 81,984 bytes globals.

## Expected serial markers

Newo USB serial:

```text
[usb-vcp] VCP_READY ...
[arduino] RESET_CAUSE external
[arduino] HANDSHAKE_READY version=1 capabilities=reset_trigger,led
[arduino] EVENT voice_trigger reset
[voice] PHYSICAL_TRIGGER_ACCEPTED
[arduino] LED_STATE listening
[arduino] LED_STATE thinking
[arduino] LED_STATE speaking
[arduino] LED_STATE idle
```

A busy or duplicate trigger prints `PHYSICAL_TRIGGER_REJECTED reason=voice_active`, `speaker_busy`, `duplicate`, or `unavailable`, and sends `LED_STATE error`.

## Physical sequence

1. Flash the Nano sketch and Newo firmware only after explicit authorization.
2. Power Newo with its serial diagnostics visible. Connect the Nano directly to Newo USB host and wait for `VCP_READY` and `HANDSHAKE_READY`.
3. Confirm initial power-on reports `RESET_CAUSE power_on` and does not print `EVENT voice_trigger reset`.
4. Press Nano RESET once. Confirm `RESET_CAUSE external`, one event, one accepted voice session, and solid L LED.
5. Speak once, then stop. Confirm Sherpa produces one final transcript, the LED changes to fast blink while thinking, slow blink while speaking, then OFF.
6. Press RESET during listening and during speaker playback. Confirm clean rejection, three short LED flashes, and no overlapping session or duplicate assistant turn.
7. Repeat at least 25 resets, including rapid resets and a Nano unplug/replug. Confirm one event per external-reset boot, renewed handshake, bounded request timeouts, and no lost Newo voice session if the Nano disappears after acceptance.
8. Repeat through the intended powered hub, then with MSC + UAC + VCP attached. Confirm storage and audio remain responsive and channel exhaustion fails cleanly as described in `docs/usb-vcp.md`.

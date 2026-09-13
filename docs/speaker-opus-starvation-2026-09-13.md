# Speaker Opus starvation findings — 2026-09-13

## Status

Voice ingress and Opus codec negotiation are fixed in commit `7284e42`.

Physical validation confirms the remaining audible chopping is a playback
starvation/buffering problem, not an Opus decoder performance problem.

## Physical Opus validation

Test playback:

- codec: Opus
- packets received: 362
- packets decoded: 362
- compressed wire bytes: 42,188
- decoded PCM bytes: 695,040
- Opus queue high-water: 1 packet
- Opus queue overflows: 0
- decoder errors: 0
- decode average: 10,839 us
- decode worst: 23,938 us
- Opus frame duration: 40 ms

The ESP32-S3 decoder therefore has substantial real-time headroom:
even the worst measured decode remained below one 40 ms frame period.

## Starvation evidence

ESP playback diagnostics:

- underruns: 96
- minimum decoded PCM buffer: 0 bytes
- maximum decoded PCM buffer: 13,440 bytes
- decoded PCM buffer capacity: 24,576 bytes

The receiver repeatedly drained completely during playback.

Server diagnostics from the same physical run:

- codec: opus
- max schedule lateness: 1,314 ms
- catch-up frames: 188
- max flow wait: 405 ms
- total flow wait: 12,075 ms
- max send gap: 886 ms
- minimum receiver buffer: 0

Pocket/TTS source wait remained only a few milliseconds.

## Conclusion

Do not optimize or replace the ESP32-S3 Opus decoder based on this issue.

Do not treat Opus itself as the cause of the chopping.

The compressed Opus queue barely accumulated (`q_high_packets=1`) while the
decoded PCM playback buffer repeatedly reached zero. The current sender/flow
control does not maintain enough future audio at the receiver.

The remaining issue is receiver starvation / jitter buffering.

## Next experiment

Keep Opus.

Add a receiver-side compressed Opus jitter reservoir before playback:

- increase Opus queue depth from 16 to 32 packets
- 32 x 40 ms provides about 1.28 seconds of compressed audio capacity
- initial reservoir target: about 1.0 second
- low-water point: about 0.6 second
- desired refill level: about 1.2 seconds
- hard target/cap: about 1.5 seconds if needed

Playback timing should be owned by I2S after playback begins.

The network sender should maintain the receiver reservoir rather than trying
to repay wall-clock lateness with burst/catch-up transmission.

Do not simply raise the old PCM outstanding-byte window to a very large value;
that previously reproduced receiver overflow.

## Validation criteria for next change

A successful physical run should show:

- codec=opus
- q_high_packets meaningfully above 1
- q_overflows=0
- decoder_errors=0
- underruns near 0, ideally 0
- min_buffer staying above 0 during active playback
- no long burst/silence cadence
- audible continuous speech

Latency may intentionally increase by roughly 1 second in exchange for
continuous playback.

## Implementation status

The next build implements this experiment without changing the validated Opus
codec or speaker connection-readiness path. It uses a 32-packet PSRAM queue,
waits for 25 packets (about 1,000 ms) before decoding normal-length playback,
and makes server flow credit target about 1,200 ms with a 1,500 ms hard
ceiling. Compressed admission, decoded PCM, and consumed PCM are reported
separately. The Opus sender no longer performs wall-clock catch-up bursts after
playback starts.

This implementation is host-tested and must still be flashed and physically
validated before the chopping can be considered solved.

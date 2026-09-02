# Opus physical validation checklist

Do not run this until Sol approves `opus-final-integration`. The first firmware baseline is fixed: 40 ms Opus frames, 24 kbps, 12,288-byte (256 ms) decoded prebuffer, 16 x 4,000-byte compressed queue.

## Flash and deploy

Compile the application into the upload directory (preserves ESP_SR model partition):

```sh
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,UploadSpeed=921600' --build-path /tmp/newo-esp-sr-build Newo
```

Then application-only upload (preserves ESP_SR model partition):

```sh
arduino-cli upload --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,UploadSpeed=921600' --input-dir /tmp/newo-esp-sr-build --port <PORT>
```

`./Newo/flash_esp_sr.sh <PORT>` also writes `srmodels.bin` at `0xC10000`; do not use it for this change because the model contents are unchanged.

Native server deployment (do not run before approval):

```sh
cd /opt/newo/server
SPEAKER_CODEC=opus NODE_OPTIONS="" pm2 restart newo-cloud --update-env
```

PCM rollback:

```sh
cd /opt/newo/server
SPEAKER_CODEC=pcm NODE_OPTIONS="" pm2 restart newo-cloud --update-env
```

The old preload is prototype/rollback only: `NODE_OPTIONS=--import=/opt/newo/server/src/opus-bootstrap.js`.

## Five-run baseline

Boot the device and confirm firmware `0.5.0-dev`, autonomy revision `2`, PSRAM, persistent speaker connection, and `codecs=["pcm","opus"]`. Then run five times:

> Hello, I'm Newo. This is a realtime voice latency test.

For each run record: Kokoro request-to-first-raw and first-conditioned PCM; server stream ms, raw PCM, wire bytes, packet count, compression, sent/received/consumed; ESP underruns, PCM/Opus queue overflows, decoder errors, callback count/average/worst, decode average/worst, decoder/playback stack low-water, queue high-water packets/bytes, PCM buffer min/max, heap/minimum heap/PSRAM, display frame average/worst, I2S drain, and `received == consumed == speaker_end bytes` with `SPEAKER_COMPLETE`.

Record subjective smooth/choppy audio, word cuts, gaps, and pronunciation. Success requires zero decoder, queue, and PCM overflows with exact completion; prefer zero underruns for all five. An isolated single underrun is evidence for Sol review, not automatic architectural failure.

Only after five zero-underrun baseline runs: test 9,600-byte/200-ms prebuffer five times; only then 7,680-byte/160-ms five times. Keep 40-ms frames and 24 kbps during those tests.

## Task/concurrency review notes

The WebSocket callback only validates/copies Opus packets into PSRAM queues. The decoder task is core 1, priority 1; playback is core 1, priority 2. Opus decode no longer executes synchronously inside the WebSocket callback; decoder work runs independently while playback has precedence once PCM is ready. Decoder and Arduino-loop/Wi-Fi work can still contend or time-slice on a core, so physical callback timing must confirm scheduling behavior and must not be assumed.

Static review must be rechecked on hardware: result publication waits for `taskFinished_` and decoder-task exit before resource release; late frames are rejected after completion; queue slots are returned after decode; disconnect is converted to failure. Shared telemetry is observational and can be non-atomic; consumers must not infer `buffered == received-consumed` from separately sampled counters.

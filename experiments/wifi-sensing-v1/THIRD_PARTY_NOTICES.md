# Third-party notices

## RuView

Project: [RuView](https://github.com/ruvnet/RuView)

Reviewed revision: `d613a576ea848f96a9b15bac4e7f60b6be7c08e7`

RuView measurement-plane concepts informed this experiment design, specifically ESP32-S3 CSI configuration and callbacks, invalid-first-word sanitation, source-MAC filtering, connected-AP channel detection, gateway self-ping for controlled OFDM traffic, callback rate limiting, sequence and RF metadata capture, fixed ring buffers and drop diagnostics, independent raw/DSP cadences, phase extraction/unwrapping, running statistics, and top-K subcarrier selection.

Phase 6 also reviewed RuView's documented one-way leader beacons, offset EMA,
freshness state, and separate synchronization telemetry. Newo implements an
independent `NSYN`/NCSI contract and fixed-leader session model; no RuView wire
format or synchronization source code was copied.

No RuView wire format was copied. Phase 2's standalone receiver and Phase 3's
shared Newo2 measurement plane adapt the narrow patterns listed above in
`newo-rx/main/newo_rx_main.c`; that source carries an attribution header. The
Newo serializers, ESP-NOW probe format, bounded ring, diagnostics,
configuration, and protocol tests are original implementations. RuView's
pose/health/person-count/fall claims and implementations, WASM support,
Matter/Home Assistant integrations, channel hopping, 5 GHz and ESP32-C6
features, NDP injection, Rust platform, and mesh architecture are outside this
experiment's scope.

RuView is distributed under the MIT License:

```text
MIT License

Copyright (c) 2024 rUv

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Espressif documentation

The raw sample ordering and invalid-prefix handling in the protocol follow Espressif's ESP32-S3 Wi-Fi CSI documentation: complex items are signed bytes in imaginary-then-real order, and the first four CSI bytes are invalid when `first_word_invalid` is true.

- [ESP-IDF ESP32-S3 Wi-Fi CSI programming guide](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32s3/api-guides/wifi.html#wi-fi-channel-state-information)

Documentation references describe interoperability facts and are not incorporated source code.

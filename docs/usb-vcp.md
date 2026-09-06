# Arduino USB/VCP production foundation

## Architecture

- `NewoUsbHost` remains the only installer/owner of the ESP32-S3 USB Host Library. It installs the Espressif MSC and CDC class clients before starting enumeration.
- `NewoUsbVcp` is protocol-agnostic transport. It uses Espressif `usb_host_cdc_acm` 2.4.1 plus the upstream CH34x, CP210x and FTDI C drivers vendored from `esp-usb` commit `9067a5e6abee710b7df0c4dde973b54cb2699ea7`.
- `NewoArduinoNode` owns the Newo/Arduino wire protocol, handshake, capability/version state, request IDs, ACK correlation and unsolicited events.
- No servo, relay, buzzer or flashing command is implemented. The request protocol and DTR/RTS controls leave room for later reset/bootloader work without coupling it to USB transport.

## Bounded behavior

VCP RX is a 2048-byte stream buffer. TX is eight 256-byte chunks. Discovery holds four candidates. The Arduino layer accepts frames shorter than 256 bytes, tracks eight pending requests, and queues eight unsolicited events and eight acknowledgements. Callbacks never block; overflow is dropped and counted. The VCP worker uses a 40 ms transfer timeout and priority 1. The Arduino parser task uses priority 1 and polls RX for at most 5 ms. Neither task runs application features.

The default serial configuration is 115200 8N1 with DTR and RTS asserted. `NewoUsbVcp::configure()` supports 5-8 data bits, CDC parity/stop-bit values, baud, DTR and RTS through the selected Espressif driver.

Payloads are percent-encoded ASCII tokens. CRLF and fragmented input are accepted. Non-printable bytes, overlong frames, invalid escapes, unknown ACK IDs and incomplete frames across disconnect are rejected without growing memory.

## ESP32-S3 host-channel audit

The S3 has eight host channels. One vacant channel is needed for enumeration; claimed interfaces then consume one channel per endpoint. Typical worst-case hub coexistence is:

| Consumer | Typical channels |
|---|---:|
| External hub interrupt endpoint | 1 |
| MSC bulk IN + OUT | 2 |
| UAC playback isochronous OUT | 1 |
| VCP bulk IN + OUT | 2 |
| CDC notification interrupt IN | 1 |
| Enumeration reserve | 1 |
| Total | 8 |

This supports the target hub combination only at the hardware limit. UAC duplex or extra interfaces/devices can exceed it. The drivers treat open/claim/enumeration errors as recoverable: VCP logs `OPEN_REJECTED ... channels=clean-fail`, remains disconnected, and retries only after a new-device event. Existing MSC/UAC teardown paths remain unchanged. Do not claim three-way hub coexistence until it is physically proven with the exact devices, because descriptors determine the actual endpoint count.

## Wire protocol

One printable-ASCII frame per LF, maximum 255 bytes including content but excluding LF:

```text
NEOWIRE/1 HELLO id=1 min=1 max=1
NEOWIRE/1 HELLO_ACK id=1 version=1 capabilities=gpio,sensors
NEOWIRE/1 REQ id=2 command=ping payload=
NEOWIRE/1 ACK id=2 status=ok payload=pong
NEOWIRE/1 EVENT name=button payload=pressed
```

The Arduino must answer `HELLO` within 1500 ms. Requests time out after 2000 ms. Capability names and application commands are intentionally not defined in this phase.

## Host validation

Run:

```powershell
./tools/run-arduino-wire-host-test.ps1
./tools/run-usb-audio-host-test.ps1
```

Then compile the complete sketch with the repository's documented Arduino-ESP32 3.3.10 FQBN. Compilation does not validate physical endpoint/channel behavior.

Latest production build validation:

- Arduino-ESP32 3.3.10 full sketch compile: passed.
- Program storage: 2,886,815 of 3,145,728 bytes (91%).
- Global variables: 81,960 of 327,680 bytes (25%).
- Arduino wire framing and USB audio descriptor/policy host tests: passed.
- COM5 production upload: bootloader, partitions, application and `srmodels.bin` written with hash verification; hard reset completed.
- Physical VCP and three-class hub validation remains pending until the steps below are executed.

## Physical validation (pending)

Do not flash until explicitly authorized.

1. Direct CDC-ACM: connect a native-USB Arduino, confirm `VCP_READY driver=cdc-acm`, handshake, bidirectional fragmented frames, ACK/event delivery, DTR/RTS and non-default line coding.
2. Direct bridges: repeat with CH340/CH341, CP210x and FT232/FT231 hardware. Confirm the reported driver and line settings with a logic analyzer or bridge-side test firmware.
3. Disconnect stress: unplug during RX, queued TX and an outstanding request. Confirm bounded timeout, one disconnect sequence, cancelled request state, and clean reconnect/handshake for at least 100 cycles.
4. Malformed input: send oversized, binary, unterminated, invalid-percent and unknown-ACK frames while voice, display and Wi-Fi remain responsive.
5. Powered hub: attach MSC + UAC playback + VCP, confirm mount/read, sustained UAC playback and VCP request/event traffic together. Repeat plug orders and hot removal of each device.
6. Channel exhaustion: add an endpoint-heavy device or enable UAC duplex. Confirm the extra interface/device fails to open cleanly without panic, watchdog, stalled audio, stale mount, or lost cloud/Wi-Fi service.
7. Record VID/PID, full descriptors, observed channel outcome, transfer/drop counters, hub model/power supply and soak duration for review.

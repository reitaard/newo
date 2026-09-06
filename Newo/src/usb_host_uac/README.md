# Espressif USB Host UAC 1.5.0, Arduino integration

Upstream: https://github.com/espressif/esp-usb/tree/6d24137e14a4f6c8138662a7a074be16a899c2c6/host/class/uac/usb_host_uac

The component registry's 1.5.0 release points at this exact revision. `LICENSE`
and source SPDX notices are retained. `idf_component.yml` records upstream
requirements; Arduino does not run it. `newo_uac_config.h` supplies bounded
Kconfig values. No second USB Host library is vendored. Header forwarding from
`Newo/usb/uac_host.h` supports Arduino sketch includes.

The following are local changes, not claims that the upstream release already
includes these fixes:

- Use recorded `alt_idx` for claims/SET_INTERFACE instead of array index + 1.
- Reject non-full-speed/non-1ms endpoints; remove descriptor interval rewriting.
- Retain the configuration descriptor's device reference through discovery;
  check absent alternate/endpoint/next class descriptor pointers.
- Stop alternate parsing at the next interface, clamp stored rate count to its
  fixed array, preserve allocations on realloc failure, check feature-unit size.
- Correct open/add failure cleanup (null/freed interface access and removal of an
  unlinked device); clear freed transfer-list pointers for stop/restart.
- Mark RX active before submitting so immediate completions are not canceled.
  Drain partial starts before release; retain ownership for retry on failed halt.
- Defer disconnect close to the application worker. This avoids synchronous
  close/control requests from the event task that must dispatch their completion.
  The application's disconnect callback is atomic flags only and is called while
  the driver list lock is held, preventing concurrent close from invalidating the
  traversal. Other callbacks retain upstream behavior.
- Track control-transfer ownership across timeouts and refuse reuse/free while
  completion is pending. Do not decrement the last open count before close succeeds.
- Add bounded cumulative packet/error/drop/underrun/submit/high-water counters
  and `newo_uac_get_stats()`. RX/TX data paths remain the official driver paths.

Driver rings are 8KiB per opened stream, selected by the Newo wrapper. DMA URB
allocation remains through IDF's `usb_host_transfer_alloc()`. The driver is
experimental on Newo pending physical hotplug and simultaneous MSC/UAC validation.

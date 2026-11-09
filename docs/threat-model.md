<!-- SPDX-License-Identifier: GPL-3.0-or-later
     Copyright (C) 2026 Pau Aliagas -->

# Threat model

## What it is and where it runs

- `libsurface` (`src/surface.c`, `src/motor.c`, `src/bank.c`, `src/meter.c`): the device behaviour of a motorised control surface. It speaks no wire and owns no socket, thread or clock; every timed call takes the caller's `now_us`. It allocates once, in `surface_create()`, and nothing after (`include/surface/surface.h`). It takes no lock and makes no claim of thread safety: one surface is driven from one thread. No real-time guarantee is documented; the code after creation does no allocation and no I/O of its own, only bounded loops over fixed tables and the backend callbacks the host supplies.
- `libsurface-mackie` (`src/mackie/surface_mackie.c`): the Mackie Control module (X-Touch). The host hands it each MIDI message it receives (`surface_mackie_input()`) and gets back each message to send through its write callback. It opens no port.
- `libsurface-diginet` (`src/diginet/surface_diginet.c`): the Control|24 module over libdiginet's desk mux. It reads raw Ethernet frames from a packet socket the host passes in (`surface_diginet_open()`), or frames the host hands to `surface_diginet_rx()`.
- `surface-c24` (`tools/surface-c24.c`, `docs/surface-c24.1`): a CLI that takes its packet socket from `diginet-packet-helper` (which holds `cap_net_raw`; the tool holds no privilege), attaches the first Control|24 that announces on any interface, and optionally reads write commands from a FIFO.

There is no config file, no file format and no IP network code in this repository.

## Inputs

| Input | Read by | Who can write it | Trust |
|---|---|---|---|
| DigiNet frames on the LAN segment (announce, fader, touch, keys, V-Pot, jog) | `surface_diginet_service()` / `_rx()` via libdiginet, then `data_in()` | any device on the same Ethernet segment | untrusted |
| MIDI messages from a Mackie surface (pitch bend, notes, CC, SysEx handshake and version reply) | `surface_mackie_input()` via libmackie | the surface, or anything that can write the host's MIDI port | untrusted |
| Decoded device events: fader raw value, touch, key code, V-Pot and jog delta, warning | `surface_in_fader`, `_in_touch`, `_in_key`, `_in_vpot`, `_in_jog`, `_in_warning`, `_in_online`, `_in_offline` | the backend, carrying device data | untrusted values, trusted caller |
| Host writes: served fader values, bank list, LED states, ring values and codes, scribble text, counter text, colours, meter fills | `surface_fader_served*`, `surface_bank_*`, `surface_led`, `surface_ring*`, `surface_scribble`, `surface_counter`, `surface_colour`, `surface_meter` | the linking program; text and values may come from its own clients | trusted caller, values checked |
| `struct surface_config` (geometry, ops, `max_targets`, `max_leds`) and `struct surface_mackie_config` | `surface_create()`, `surface_mackie_open()` | the linking program | trusted |
| Caller clock `now_us` | every timed call | the linking program | trusted (assumed monotonic) |
| `surface-c24` arguments `--helper PATH`, `--fifo PATH`, and `PATH` lookup of the helper | `main()` in `tools/surface-c24.c` | local user running it | trusted |
| `surface-c24` FIFO commands (`scrib`, `led`, `fader`, `ring`, `quit`) | `fifo_read()`, `command()` | any local user with write access to the FIFO | untrusted |

## Trust boundary

The boundary is the wire. Anything that arrives from a surface, or from a device pretending to be one, is untrusted: DigiNet has no authentication, so any host on the segment can announce as a desk or send it input, and `surface-c24` attaches the first Control|24 that announces. MIDI is likewise unauthenticated. The linking program is trusted: it fills the config, supplies the backend callbacks and the clock, and owns the surface. Values it passes are still range-checked, because they often come from its own clients. The `surface-c24` FIFO is a local boundary guarded only by file permissions.

## What the code assumes and enforces

- Geometry: `geometry_ok()` in `src/surface.c` refuses 0 or more than `SURFACE_MAX_STRIPS` (32) strips, `fader_max` 0, more than 2 meter sides, 31 ring LEDs, 2 scribble rows or 16 scribble characters, with `SURFACE_E_ARG`. `meter_segments`, `counter_digits` and `motor_epsilon_q` are not bounded there.
- Allocation: `max_targets` and `max_leds` are `uint16_t` (0 means 256), so the single `calloc` in `surface_create()` cannot overflow; failure is `SURFACE_E_NOMEM`.
- Strips: every strip-indexed call checks `strip >= strips` and refuses (`SURFACE_E_ARG`) or ignores the input (`surface_in_*`, `src/motor.c` `surface_fader_paint_strip`, `surface_strip_fader`).
- Fader raw: `surface_in_fader()` clamps `raw` to `fader_max`. Served positions are not clamped when stored; `motor_raw()` maps NaN and values at or below 0 to 0 and values at or above 1 to `fader_max`.
- Meters and rings: `surface_meter_step()` treats NaN as empty and clamps to 0..1 (`src/meter.c`); `surface_ring_mask()` returns 0 for NaN, zero LEDs, more than 31 LEDs or an unknown mode. `surface_ring_code()` stores the code as given.
- Text: `scribble_text()` copies at most `scribble_chars`, maps non-ASCII to `?` and control characters to space. `surface_counter()` truncates to 31 bytes. `surface_colour()` truncates to the strip count. NULL text is `SURFACE_E_ARG`.
- Tables: `surface_bank_set()` truncates the list to `max_targets`; a full LED table is `SURFACE_E_FULL`; bank steps are clamped to the list (`bank_clamp()` in `src/bank.c`).
- V-Pot and jog deltas are truncated to `int16_t` in the event; key codes pass through unchecked as opaque ids.
- Mackie: wire channels past the surface's strips are dropped (`core_strip()`); `surface_mackie_open()` refuses more than `MACKIE_STRIPS`; the assignment text is cut to 2 characters. Message length checks are libmackie's decoders.
- DigiNet: decoder errors are skipped in `data_in()`; only a `DIGINET_DESK_CONTROL24` is attached; desks are kept in a fixed table of `DIGINET_MUX_MAX_DESKS`; desk name, version, interface and warning text are copied with `snprintf` into fixed buffers. Frame parsing is libdiginet's.
- `surface-c24` FIFO: strips must parse as 1..24, key codes as 0..0xffff or a known `NAME:S`, fader 0..1, ring mask 0..0x7fff; anything else is printed as refused. A line longer than 1023 bytes is dropped. The FIFO is created mode 0600; an existing path is opened as it is, without checking that it is a FIFO or who owns it.

## Fuzz coverage

Harnesses are in `fuzz/`, run under ASan and UBSan in `.github/workflows/fuzz.yml`; see `fuzz/README.md`.

| Untrusted input | Fuzz target |
|---|---|
| Decoded device events (fader, touch, key, V-Pot, jog, warning, online/offline) and the clock | `fuzz_motor` |
| Host values that may come from clients (served faders, LED, ring, scribble, counter, colour) | `fuzz_motor`, `fuzz_board` |
| Bank lists and moves | `fuzz_bank` |
| Geometry and config sizes | `fuzz_board` (create), `fuzz_pure` (presets) |
| Meter and ring values | `fuzz_pure` |
| Decoded Control|24 key codes | `fuzz_keys` |

### Gaps

- `src/mackie/surface_mackie.c` and `src/diginet/surface_diginet.c` have no harness; the raw MIDI and DigiNet bytes are fuzzed only in libmackie and libdiginet, not through these modules.
- `tools/surface-c24.c` (arguments and FIFO parser) has no harness.
- No authentication of a DigiNet desk or a MIDI source; `surface-c24` trusts the first desk that announces.
- The FIFO path is not checked for type or owner when it already exists.
- No thread safety and no documented real-time guarantee.

## Reporting

Report a vulnerability privately through GitHub: the repository's Security tab, "Report a vulnerability".
The policy is the organisation's `SECURITY.md`.

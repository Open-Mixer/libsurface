# libsurface: overview

The design is [FreeMixer/libdiginet#29](https://github.com/FreeMixer/libdiginet/issues/29). This repository holds the core and its optional modules, one per device family, each its own library and package so a host installs only what it uses:

| Module | Library | Packages | Needs |
|---|---|---|---|
| core | `libsurface` | `libsurface`, `libsurface-devel` (deb: `libsurface0`, `libsurface-dev`) | libc, libm |
| Mackie Control: the X-Touch | `libsurface-mackie` | `libsurface-mackie`, `libsurface-mackie-devel` (deb: `libsurface-mackie0`, `libsurface-mackie-dev`) | [libmackie](https://github.com/Open-Mixer/libmackie) 0.1.0 or later |
| DigiNet: the Control\|24 | `libsurface-diginet` | `libsurface-diginet`, `libsurface-diginet-devel`, `surface-c24` (deb: `libsurface-diginet0`, `libsurface-diginet-dev`, `surface-c24`) | [libdiginet](https://github.com/Open-Mixer/libdiginet) 0.1.10 or later |

The protocol libraries stay independent and never depend on libsurface. The Python binding comes in a later phase.

## What it does

- **Motor faders that never fight the hand.** A fader has a touch flag and a target; the motor tracks the target only while untouched, and on release the cap is written once, to where the hand left it. An implicit-touch window holds the motor off while reports say a hand moves the cap even when the touch sensor drops out. The surface's own moves are awaited until the host echoes them, so a lagging burst of echoes never walks the cap back through the gesture, and a burst of served frames costs one leading and one trailing write.
- **Touch** with a self-heal for a release lost on the wire (a surface that repeats its touch message while held), and a release debounce for a sensor that chatters (the Control|24's drops out for 30-240 ms during a move).
- **The Control|24's set-point.** Its motor holds a set-point of its own and drives back to it when the cap is let go, and the desk never reports its own moves; so while a hand is on the cap each reported position is written back as the set-point, and once more at the release.
- **Bank window** over the host's ordered list of targets: bank and channel steps, show, jump.
- **Board shadow and replay.** The latest value of every element (LEDs, rings, scribbles, counter, colours, fader targets) is kept; a device that comes back blank gets the whole board again without asking the host.
- **Meter steps** with hysteresis, sent only on a change, with a keep-alive for a surface that decays a lit meter by itself.

What it is not: it speaks no wire, owns no socket, thread or clock, and knows no channel or verb. Each call that needs time takes the caller's `now_us`; `surface_deadline()` and `surface_tick()` run the timers. It allocates once, in `surface_create()`.

## The Mackie module

`libsurface-mackie` ([`include/surface/surface_mackie.h`](../include/surface/surface_mackie.h), `src/mackie/`) drives a Mackie Control surface such as the Behringer X-Touch, with libmackie as its codec:

- **The transport is the host's.** Every message the module writes goes to the host's write callback as one complete MIDI message, and the host hands over each message it receives. The module owns no port, thread or clock.
- **The map is the host's.** The control map is a `struct mackie_map` filled from the host's own surface profile, checked whole at open.
- **The device's own rings.** An MCU device draws a ring from mode, position and centre: `surface_ring_code()` keeps that code in the shadow and replays it as it is; a ring the core rendered comes back to the nearest code.
- **The handshake.** The device's connection query is answered and brings the surface online, which replays the board and the assignment display.

## The DigiNet module

`libsurface-diginet` ([`include/surface/surface_diginet.h`](../include/surface/surface_diginet.h), `src/diginet/`) drives a Digidesign Control|24 over libdiginet's desk mux:

- **One listener for every desk.** One packet socket hears every interface (libdiginet's `diginet_packet_open_any`, or the descriptor `diginet-packet-helper` hands over); each desk that announces is reported to the host with its MAC, interface, name and version, and the host attaches the ones it wants.
- **One surface per desk,** with the Control|24 geometry: 24 strips, 10-bit faders, 14-segment meters on two sides, 15-LED rings, 4-character scribbles, an 8-digit counter, the touch-release debounce and the set-point echo.
- **The board on every session.** A new session blanks the desk; every ONLINE replays everything the host wrote, faders included. A desk that drops off and comes back, or a host that restarts, is repainted without the host asking.
- **The wire both ways.** Writes go through libdiginet's encoders into the session queue, where a newer write to an address replaces a queued one; faders, touches, keys, V-Pots and the jog come up as events.
- **Warnings.** A desk that announces but answers no wake (booted on a noisy link) and a desk in session with another host are raised as warnings.

```c
int fd = diginet_packet_open_by_helper(NULL);              /* the socket, from the helper */
struct surface_diginet *d;
surface_diginet_open(&d, fd, on_desk, host);               /* on_desk(host, info, present) */
/* in on_desk, for a desk the host wants: */
struct surface *s;
surface_diginet_attach(d, info->mac, on_event, host, &s);  /* then the API below on s */
/* the loop: poll surface_diginet_fd(d) until surface_diginet_deadline(d, now), then */
surface_diginet_service(d, now_us);
```

`surface-c24 [--helper PATH] [--fifo PATH]` drives a desk through the whole stack with no privilege of its own: its socket comes from `diginet-packet-helper`. It attaches the first Control|24 that announces, labels its strips F1..F24, plays the console (each move served back), mirrors each fader on its own meter, and prints every event. The FIFO takes `scrib S TEXT`, `led NAME:S|KEY on|off`, `fader S 0..1`, `ring S MASK` and `quit`, strips counted from 1; `quit` leaves `OFF ` on the scribbles, which the desk keeps under "Offline".

## The API

Everything is in [`include/surface/surface.h`](../include/surface/surface.h).

```c
#include <surface/surface.h>

struct surface_config c = { .ops = &my_backend_ops, .backend = dev, .event = on_event, .event_ctx = host };
surface_geometry_preset(SURFACE_MODEL_CONTROL24, &c.geometry);
struct surface *s;
surface_create(&s, &c);

surface_bank_set(s, targets, n);                 /* what the strips show: opaque ids of the host's */
surface_in_online(s, now_us);                    /* the backend: the device is ready; the board replays */

surface_fader_served(s, target, 0.75, now_us);   /* the host's truth -> the motor, by the policy */
surface_fader_served_at(s, target, 0.75, sent_us, now_us);   /* the same, with when the host sent it */
surface_scribble(s, 0, 0, "KICK");
surface_meter(s, 0, 0, fill, now_us);

surface_in_fader(s, strip, raw, now_us);         /* the backend's decoded input ... */
/* ... comes up as SURFACE_EV_FADER_MOVED / _TOUCHED / _RELEASED, _KEY, _VPOT, _JOG, _WARNING */

uint64_t when = surface_deadline(s, now_us);     /* sleep until then, or until input */
surface_tick(s, when);
```

Up the seam go events with the strip's target; down go fader targets, LED states by key id, ring values, scribble text, meter fills, counter text and colours. MIDI bytes, DigiNet frames, channel ids and verb names never cross it.

## Building

`make`, `make test`, `make install`. A C11 compiler, `make` and libm for the core; the DigiNet module is built when libdiginet with the desk mux is found (pkg-config, or `make DIGINET_SRC=../libdiginet`). See [BUILDING.md](../BUILDING.md).


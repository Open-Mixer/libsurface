# libsurface

The device behaviour of a motorised control surface, written once in C for every surface and every host: the motor-fader policy, touch, the bank window, a shadow of the whole board and its replay, meter steps and LED ring rendering. A Behringer X-Touch, a Digidesign Control|24 and, later, a ProControl run on the same code; a mixer or DAW links it and keeps only its mixing semantics (what a strip shows, names, colours, its verbs).

It speaks no wire, owns no socket, thread or clock, and knows no channel or verb. The core is one library; each device family is an optional module with its own library and package: `libsurface-mackie` (the X-Touch, over [libmackie](https://github.com/Open-Mixer/libmackie)) and `libsurface-diginet` (the Control|24, over [libdiginet](https://github.com/Open-Mixer/libdiginet)), with `surface-c24`, a tool that drives a desk through the whole stack.

## Install

Fedora 44:

```
sudo dnf install https://www.openmixer.org/rpm/openmixer-release.noarch.rpm
sudo dnf install libsurface-devel libsurface-mackie-devel libsurface-diginet-devel
```

Debian bookworm and trixie (also Raspberry Pi OS):

```
curl -fsSLO https://www.openmixer.org/deb/openmixer-release.deb && sudo apt install ./openmixer-release.deb
sudo apt install libsurface-dev libsurface-mackie-dev libsurface-diginet-dev
```

Install only the modules for the surfaces you drive.

## Start

```c
#include <surface/surface.h>

struct surface *s;
surface_create(&s, &config);                    /* the backend ops, the event callback, the geometry */
surface_fader_served(s, target, 0.75, now_us);  /* the host's value -> the motor, by the policy */
surface_tick(s, surface_deadline(s, now_us));   /* the timers, on the caller's clock */
```

```
cc app.c $(pkg-config --cflags --libs libsurface-mackie)
```

`surface-c24` drives the first Control|24 that announces (`man surface-c24`).

## Documentation

<https://www.openmixer.org/projects/libsurface>, and in this repository: the modules and their packages, what the behaviour layer does, the Mackie and DigiNet modules and the API ([docs/overview.md](docs/overview.md)); building and testing ([BUILDING.md](BUILDING.md)).

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).

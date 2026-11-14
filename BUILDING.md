# Building libsurface

A hand-kept Makefile, no build system to configure. You need a C11 compiler and `make`.
Nothing else: no library outside libc and libm.

```
make                          # libsurface.a
make test                     # the unit suite, the same command CI runs
make shared                   # libsurface.so, as the packages build it
make install PREFIX=/usr/local [DESTDIR=...]
make clean
```

The code is plain C11 built with `-Wall -Wextra -Werror`, so a warning is a build failure.
`CC` and `CFLAGS` can be overridden on the command line; the warning flags are kept even then.

`make install` copies the header to `$(PREFIX)/include/surface/`, the archive to
`$(PREFIX)/lib` (`LIBDIR=` to change it) and a pkg-config file, so a consumer builds with:

```
cc app.c $(pkg-config --cflags --libs libsurface)
```

(`--static` adds libm for a link against the archive.)

## Tests

Every test runs through `tests/run-test.sh`, which gives it a timeout: a hang is a failure,
never a stalled build. A test that exits 77 proved this machine cannot host it and is reported
as SKIP, never as a pass.

The suites need no device. `tests/harness.h` is a fake backend that records every write, a
fake host that answers the surface's moves the way a console does (echoing at once, lagging,
or refusing), and a virtual clock that runs the surface's timers in order:

- `test_motor`: the motor policy, case by case as the X-Touch driver's suite proved it;
- `test_audit`: the motor-law audit of 2026-09-25 and the fast-throw round trip;
- `test_bank`: the bank window;
- `test_meter`: meter ballistics, ring rendering, and the step law against
  `tests/golden/meter-steps.txt`, vectors generated once from the TypeScript law and fixed;
- `test_c24`: the Control|24's rules: release debounce, set-point echo, the replay on every
  session.

The script checks: `constants-check.sh` (each timing constant declared once, in the header),
`conformance-packaging.sh` (the version and soname agree in every packaging copy),
`test_install.sh` (a consumer builds against an installed tree through pkg-config alone),
and `test_workflows.sh` (the workflows parse).

What the repository publishes is checked by Open-Mixer/.github's shared checks, which CI runs
through its `public-sources-check` workflow: a pull request's commit messages carry no
attribution line, and no tracked file points into a private repository. `make public-sources`
runs the second from a checkout of Open-Mixer/.github (`OPENMIXER_DOTGITHUB`, else
`../freemixer-dotgithub`). The commit-msg hook in `.githooks/` runs the first; enable it once
per clone with `git config core.hooksPath .githooks`.

## The DigiNet module

`libsurface-diginet` and `tools/surface-c24` need libdiginet with the desk mux
(`diginet/transport/diginet_mux.h`, libdiginet 0.1.10 and later). `DIGINET` says what to do:

```
make                                   # DIGINET=auto: the module when libdiginet is found, else a SKIP line
make DIGINET=yes                       # the module, or a failed build (the packages)
make DIGINET=no                        # the core alone
make DIGINET_SRC=../libdiginet         # libdiginet from a built source tree instead of pkg-config
make DIGINET_SRC=../libdiginet test    # + the module's tests
```

A source tree is linked from its archives, into the shared library too: that is for working on
the two together, never for a package. `make shared` adds `libsurface-diginet.so` (its own soname,
`SURFACE_DIGINET_ABI`) and `surface-c24` linked to it; `make install` adds the header,
`libsurface-diginet.pc` and the tool.

`surface-c24`'s default helper is `DIGINET_SRC`'s `tools/diginet-packet-helper`, else
`diginet-packet-helper` in PATH (`SURFACE_C24_HELPER=` builds it to look in PATH always, as the
packages do; `--helper PATH` overrides either at run time). The helper needs
`setcap cap_net_raw+ep` (libdiginet's `docs/diginet-packet-helper.1`).

The module's tests, in `tests/diginet/`, run after the core's:

- `test_diginet`: the module against libdiginet's desk emulator, in process, on a virtual clock:
  the board replayed on every ONLINE; each report of a touched cap echoed as its set-point, and
  once more at the release with no snap; a host move on an untouched cap and never on a touched
  one; the inputs; a deaf desk and a foreign session as warnings; the repaint after a desk
  restart and at the first announce after a host restart; a desk that goes and comes back;
- `test_c24_tool`: `surface-c24` itself, unprivileged, its socket from `diginet-packet-helper`,
  against the emulator on a veth pair in a user and network namespace of the test's own. SKIP
  with no helper or no such namespace, which is every container job; CI's `DigiNet module on the
  runner` job has both, and a skip there fails it;
- `test_install.sh`: the module installed, a consumer built through pkg-config alone;
- the tests run on libdiginet's desk emulator and child harness, `libdiginet-emu` (libdiginet
  0.1.8 and later install them as test support), from `DIGINET_SRC` or through pkg-config; without
  `diginet/testsupport/emu_child.h` they fail;
- `no-emu-copy.sh` (in every `make test`): no C file of this tree defines the emulator again,
  and `no-emu-copy-sabotage.sh` puts a copy of libdiginet's `desk_emu.h` back in a scratch copy
  of the tree and requires that check to go red on it.

## The Mackie module

`libsurface-mackie` is built when libmackie is found: through pkg-config, or from a built tree with
`MACKIE_SRC=../libmackie`. `MACKIE=auto` (the default) builds it when found, `MACKIE=yes` stops the
build when not, `MACKIE=no` never builds it. `make test` runs `tests/mackie/test_mackie` when it is
built, and says SKIP when it is not.

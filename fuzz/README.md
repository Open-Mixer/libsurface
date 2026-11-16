# Fuzzing libsurface

libsurface takes its input from three places: the device (a fader report, a touch, a key, a
V-Pot, a jog, the clock), the host (a served fader value, a bank list, an LED, a ring, a text, a
colour) and the key table of the Control|24. Every function that reads those has a libFuzzer
harness here, built with clang under AddressSanitizer and UndefinedBehaviorSanitizer and run in CI
on every push and pull request (`.github/workflows/fuzz.yml`). The harnesses are written to the
build contract of OSS Verify (its `docs/build-contract.md`, "The checks of a fuzz target"), which
proves never-crash and the declared properties over them.

Each harness is one plain C file defining `LLVMFuzzerTestOneInput`. It reads its input as a
small program: a few bytes that choose the surface (a preset, or a geometry of the input's own),
then operations, one after another, until the input ends. Every input means something, and the
end of the input ends it. No harness reads a clock, the environment, a file or the network,
starts a thread or installs a signal handler; a time the surface needs is a time the input chooses,
and it never goes back. Every input of 64 MiB or more is refused, and every other outcome an input
can choose returns 0. A text handed to the library is copied into an allocation of exactly its
length, so a read one byte past it is a sanitizer report.

## The expected values come from the contract, not from the library

A check that takes its expected value from the code it judges holds for a library that is wrong
the same way. So no check here compares libsurface with itself. `fuzz/spec.h` is the library as
the harnesses hold it, written from the comments of `include/surface/surface.h` and
`include/surface/surface_diginet_keys.h`: the sizes, the status codes, the two presets, the meter
law, the ring renderer bit by bit, the text of a scribble strip, each with a `contract:` line
naming the words it stands on. Each harness also keeps a model of its own, run beside the library:
the bank window (`fuzz_bank`), the hand on each cap and the position last written to it
(`fuzz_motor`), the board the host asked for and the board the device shows (`fuzz_board`).

## The checks and their records

Every property is an `if (...) abort();`, never `assert()`, inside a `noinline` function
`check_<property>` (or `check_<property>_<part>`). The comment of each check says what it checks,
names its `contract:`, and carries one or more records, as OSS Verify's `tools/check` reads them:

```
/* The previous count holds while the unrounded value stays within 0.5 + hysteresis of it.
 * contract: surface.h SURFACE_METER_STEP_HYSTERESIS; spec.h spec_meter_step
 * mutant: src/meter.c:35 `<= 0.5 + hysteresis` -> `< 0.5 + hysteresis`
 * input: hold-edge */
```

A record is a one-line change of `src/` and the seed it shows on: with the change, that seed makes
THAT check abort; without it, the seed passes. The one stop that is not a property, the harness's
own failed allocation in `fuzz_alloc()` (`fuzz/fuzz.h`), carries the line `not a property: the
harness's own allocation failed` in place of a record.

`make fuzz-records` (`fuzz/records.sh`) proves every record at this commit. First the ratchet:
`fuzz/checks.awk` reads every `abort()` of `fuzz/` and its comment, and a check with no record
and no `not a property:` line, or a record not of the form, is red before anything is built.
Then, for each record alone, it makes the change (the old text must occur once on that line),
builds the plain native harness against the library compiled the same way (no sanitizer,
`fuzz/standalone.c` as the driver, every `abort()` of the harness naming its own line), runs the
named seed, requires exit 134 from the record's own check, and puts the file back. Every named
seed must exit 0 on the unchanged tree. The count of records met is the proof:

```
make fuzz-records: all 90 records and 46 patches met, each by its own check on its own seed (71 checks)
```

| Harness | Checks | Sabotage patches |
| --- | ---: | ---: |
| pure | 4 | 4 |
| board | 17 | 17 |
| bank | 5 | 5 |
| motor | 14 | 14 |
| keys | 3 | 3 |

### What each harness checks

The header comment of each harness states its properties in full; in short:

| Harness | Functions it feeds | Properties |
| --- | --- | --- |
| `fuzz_pure` | `surface_meter_step`, `surface_ring_mask`, `surface_geometry_preset`, `surface_strerror` | meter: plain rounding half up, NaN as empty, a count held within 0.5 + hysteresis, never past the steps; ring: each mode bit by bit, nothing for no LEDs, more than 31, NaN or an unknown mode, no bit past the width; preset: every field of both models, a NULL geometry and an unknown model refused; strerror: each code's text |
| `fuzz_board` | `surface_create`, `surface_led`, `_ring`, `_ring_code`, `_scribble`, `_counter`, `_colour`, `_blank_strip`, `surface_in_online`, `_offline` | create: the geometries the sizes allow and refuse; refusal: every argument off the geometry is `SURFACE_E_ARG`, an LED table of `max_leds` is `SURFACE_E_FULL`; offline: no call reaches a device that is offline; live: each write reaches the backend once as written (the ASCII text of a scribble, the counter cut to 31 bytes, the colours cut to the strips); args: every call the backend gets is within the geometry; replay: after the device comes back blank it shows the host's whole intent, no more and no less; blank: a blanked strip shows spaces, an empty ring and, on an online device, a meter written 0 on every side |
| `fuzz_bank` | `surface_bank_set`, `_step`, `_show`, `_jump`, `_offset`, `surface_strip_target`, `surface_target_strip`, `surface_master_set` | set: the count kept and the NULL list refused; window: the offset after any delta, and "1 moved" exactly when it moved; targets: what each strip and the master show; target_strip: the first strip that shows a target; meters: every move and every new list clears the meter steps, a call that moved nothing (a refused list, a master) clears nothing |
| `fuzz_motor` | `surface_in_fader`, `_in_touch`, `_in_key`, `_in_vpot`, `_in_jog`, `_in_warning`, `_in_online`, `_in_offline`, `surface_fader_served`, `_served_at`, `_refused`, `_paint`, `_paint_strip`, `_suspend`, `surface_strip_fader`, `surface_note_commanded`, `surface_fader_is_report`, `surface_deadline`, `surface_tick` | hand: no write to a cap a hand holds (the Control|24 is written only the position the hand reports); offline: no write to a device that is offline; range; touch: the host hears each touch begin and each release once, at the time the geometry says; move: a fader report is one move unless it is the motor's own report, and `surface_fader_is_report` says the same; a release after a report is at the last reported position; passthrough, the V-Pot's target from the harness's own bank window; clock: the deadline is never in the past, nothing waits when nothing is in flight (on the new surface and after every operation that leaves no hand, no pending release and no flush that could be armed), a tick before it does nothing and a tick at it leaves nothing due |
| `fuzz_keys` | `surface_c24_key_find`, `surface_c24_keys[]` | find: the entry of a decoded key, or NULL exactly when the table names none; table: no two entries of one button and zone, the zones, the names, the order the header gives; measured: the strip buttons measured on the desk |

## The sabotage patches

`fuzz/sabotage/<harness>-<property>.patch` is a bug in the code a harness drives, at least one
for each property, as `git diff` writes it. Above its `diff --git` line it names the check it
breaks and the seed that shows it, the two lines OSS Verify reads (its `docs/project-yaml.md`,
"mutants"):

```
Breaks the check of fuzz/fuzz_bank.c:122 (return SURFACE_E_ARG; -> return SURFACE_OK;).
Caught by the seed input "set-refusal" of fuzz_bank.
```

`make fuzz-sabotage` applies each, rebuilds the sanitizer harness and runs every seed of its
corpus once, without fuzzing: the named seed must abort inside that property's `check_`
function, and every seed that fails must fail by an abort in some check, not a sanitizer report.
`make fuzz-records` proves the line the header names.

## The seed corpus and the coverage witness

`fuzz/corpus/<harness>/` is written by `fuzz/gen-corpus.c` (`make fuzz-corpus`) from the
library's own key table and the input forms above: every geometry the sizes allow and refuse,
every ring mode at its edges, the extremes of a delta, a touch that lapses and a release that a
touch cancels, a table of motor states too small for its targets. Nothing is fetched and nothing
is random; the corpus is committed, and `make fuzz-corpus-check` is red when it is not what the
generator writes. Together the seeds run every check of every harness.

`make fuzz-coverage` builds each harness for `llvm-cov` (no sanitizers), runs it once over its
corpus and prints the line coverage of the functions it drives (`fuzz/coverage.sh`), then every
line of them no seed reached. `fuzz/coverage-floor.txt` holds the covered lines each harness
reaches today, and the target is red when one falls below it. Every check of a harness must be
reached by a seed, and must carry a record or a `not a property:` line; a check that misses
either is named by file and line and turns the target red.

`make fuzz-static` builds each harness with `fuzz/standalone.c` for its driver and the library
sources, fully static, checks that the program has no interpreter and no dynamic section, and
runs it over its corpus: the build a verifier that brings its own driver takes.

## What has no harness, and why

- **The DigiNet module** (`src/diginet/surface_diginet.c`) and **the Mackie module**
  (`src/mackie/surface_mackie.c`). They decode what libdiginet and libmackie hand them, and
  need those libraries to build. The bytes they read are the readers of libdiginet and libmackie,
  which those libraries fuzz; what they hand libsurface is the core above, driven here through
  the same calls. Their key table (`src/diginet/c24_keys.c`) is data and has `fuzz_keys`.
- **`tools/surface-c24.c`** reads its arguments and drives the DigiNet module over a packet
  socket.
- **A file or config reader.** There is none. `struct surface_config` is a struct the caller
  fills, not bytes from outside; `fuzz_board` fills it from its input, every field in the range
  `surface_create` takes and some outside it.

## Running

```
make fuzz                    # build every harness into build/fuzz/
make fuzz-run FUZZ_TIME=60   # fuzz each for 60 s; red on any crash
make fuzz-sabotage           # every patch found, each in its own property
make fuzz-records            # every record met, each at its own check
make fuzz-coverage           # the coverage witness and its floors
make fuzz-static             # each harness linked fully static, over its corpus
make fuzz-corpus             # write fuzz/corpus/ again
```

They need clang with libFuzzer and the sanitizer runtimes, `llvm-cov` and `llvm-profdata`, awk,
and a static libc for `fuzz-static`. None of them is part of `make test`.

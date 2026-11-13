# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# libsurface: the device behaviour of motorised control surfaces, as libsurface.a, and its
# optional DigiNet module (a Control|24 over libdiginet) as libsurface-diginet.a. See BUILDING.md.
#
# `make shared` and `make install-shared` are what the packages build: the same objects as shared
# libraries.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -g
LDFLAGS ?=
# -fPIC always: the same objects go into the archive and the shared library.
WARN    := -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fPIC
INC     := -Iinclude
LIBS    := -lm
PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
INCDIR  ?= $(PREFIX)/include
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man
PCDIR   ?= $(LIBDIR)/pkgconfig
DESTDIR ?=

# The version is the VERSION file; the soname's major is in surface.h. The header's version
# string and the packaging copies are held to them by tests/conformance-packaging.sh.
VERSION := $(shell cat VERSION)
ABI     := $(shell sed -n 's/^\#define SURFACE_ABI  *\([0-9]*\)$$/\1/p' include/surface/surface.h)

OBJS := src/surface.o src/motor.o src/bank.o src/meter.o

TESTS := tests/test_motor tests/test_audit tests/test_bank tests/test_meter tests/test_c24
KEYS_TEST := tests/test_c24_keys
# The library compiled into the test with UBSan trapping: a NULL handed to memcpy stops it.
UB_TEST := tests/test_colour_null

GOLDEN := tests/golden/meter-steps.txt
RUN_TEST := tests/run-test.sh

all: libsurface.a

# -MMD -MP: a header edit rebuilds every object that includes it.
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(WARN) -MMD -MP $(INC) -c $< -o $@

-include $(OBJS:.o=.d)

libsurface.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

# ---- the DigiNet module (libsurface-diginet) ------------------------------------------------
# Built when libdiginet with the desk mux (diginet/transport/diginet_mux.h, libdiginet 0.1.4 and
# later) is found: through pkg-config when it is installed, or from a built source tree with
#   make DIGINET_SRC=../libdiginet
# which is linked from its archives (for working on the two together, never for a package).
# Without it the core builds and tests alone, and `make` and `make test` say so on a SKIP line.
DIGINET_SRC ?=
ifneq ($(DIGINET_SRC),)
DIGINET_CFLAGS := -I$(DIGINET_SRC)/include
DIGINET_LIBS   := $(DIGINET_SRC)/libdiginet-transport.a $(DIGINET_SRC)/libdiginet.a
EMU_LIBS       := $(DIGINET_SRC)/libdiginet-emu.a
EMU_INC        := $(DIGINET_SRC)/include
DIGINET_HELPER ?= $(abspath $(DIGINET_SRC))/tools/diginet-packet-helper
else
DIGINET_CFLAGS := $(shell pkg-config --cflags libdiginet-transport 2>/dev/null)
DIGINET_LIBS   := $(shell pkg-config --libs libdiginet-transport 2>/dev/null)
EMU_LIBS       := $(shell pkg-config --libs libdiginet-emu 2>/dev/null)
EMU_INC        := $(shell pkg-config --variable=includedir libdiginet-emu 2>/dev/null)
DIGINET_HELPER ?= $(shell command -v diginet-packet-helper 2>/dev/null)
endif
# DIGINET=auto (the default) builds the module when libdiginet is found; DIGINET=yes stops the
# build when it is not (the packages); DIGINET=no never builds it.
DIGINET ?= auto
ifneq ($(DIGINET),no)
HAVE_DIGINET := $(shell printf '\043include <diginet/transport/diginet_mux.h>\n' | \
	$(CC) $(DIGINET_CFLAGS) -E -x c - >/dev/null 2>&1 && echo yes)
endif
DIGINET_NONE := libdiginet with the desk mux (diginet/transport/diginet_mux.h) was not found: \
	install libdiginet 0.1.4 or later, or set DIGINET_SRC to a built libdiginet tree
ifeq ($(DIGINET),no)
DIGINET_NONE := DIGINET=no
endif
ifeq ($(DIGINET),yes)
ifneq ($(HAVE_DIGINET),yes)
$(error $(DIGINET_NONE))
endif
endif

DOBJS := src/diginet/surface_diginet.o src/diginet/c24_keys.o
DABI  := $(shell sed -n 's/^\#define SURFACE_DIGINET_ABI  *\([0-9]*\)$$/\1/p' include/surface/surface_diginet.h)

src/diginet/%.o: src/diginet/%.c
	$(CC) $(CFLAGS) $(WARN) -MMD -MP $(INC) $(DIGINET_CFLAGS) -c $< -o $@

-include $(DOBJS:.o=.d)

libsurface-diginet.a: $(DOBJS)
	$(AR) rcs $@ $(DOBJS)

# surface-c24. Its default helper is DIGINET_SRC's, else diginet-packet-helper in PATH; the
# packages build it with SURFACE_C24_HELPER= (empty: PATH at run time).
C24 := tools/surface-c24
SURFACE_C24_HELPER ?= $(DIGINET_HELPER)
C24_DEF = $(if $(SURFACE_C24_HELPER),-DSURFACE_C24_HELPER='"$(SURFACE_C24_HELPER)"')

$(C24): tools/surface-c24.c libsurface-diginet.a libsurface.a
	$(CC) $(CFLAGS) $(WARN) $(INC) $(DIGINET_CFLAGS) $(C24_DEF) $< libsurface-diginet.a \
	    libsurface.a $(DIGINET_LIBS) $(LIBS) $(LDFLAGS) -o $@

ifeq ($(HAVE_DIGINET),yes)
diginet: libsurface-diginet.a $(C24)
else
diginet:
	@echo "SKIP: the DigiNet module (libsurface-diginet) not built: $(DIGINET_NONE)"
endif

all: diginet

# The module's tests run on libdiginet's desk emulator and its child harness, libdiginet-emu
# (emu_child.h, which pulls in the emulator: libdiginet 0.1.8 and later install them as test
# support), from DIGINET_SRC or through pkg-config.
HAVE_DIGINET_EMU := $(shell printf '\043include <diginet/testsupport/emu_child.h>\n' | \
	$(CC) $(DIGINET_CFLAGS) -E -x c - >/dev/null 2>&1 && echo yes)
DTESTS := tests/diginet/test_diginet tests/diginet/test_c24_tool

tests/diginet/test_diginet: tests/diginet/test_diginet.c libsurface-diginet.a libsurface.a
	$(CC) $(CFLAGS) $(WARN) $(INC) $(DIGINET_CFLAGS) $< \
	    libsurface-diginet.a libsurface.a $(EMU_LIBS) $(DIGINET_LIBS) $(LIBS) $(LDFLAGS) -o $@

tests/diginet/test_c24_tool: tests/diginet/test_c24_tool.c
	$(CC) $(CFLAGS) $(WARN) $(INC) $(DIGINET_CFLAGS) $< $(EMU_LIBS) $(DIGINET_LIBS) \
	    $(LDFLAGS) -o $@

# test_c24_tool takes the helper to run the tool with: "" (none found) is its SKIP.
ifeq ($(HAVE_DIGINET),yes)
test-diginet: diginet-emu-found $(DTESTS) $(C24)
	$(RUN_TEST) tests/diginet/test_diginet
	$(RUN_TEST) tests/diginet/test_c24_tool "$(DIGINET_HELPER)" $(C24)
	$(RUN_TEST) tests/diginet/test_install.sh "$(DIGINET_SRC)"
	$(RUN_TEST) tests/diginet/no-emu-copy-sabotage.sh "$(EMU_INC)"

# the module's tests need the harness: a libdiginet without it is a failure, not a skip
.PHONY: diginet-emu-found
diginet-emu-found:
	@[ "$(HAVE_DIGINET_EMU)" = yes ] || { echo "FAIL: libdiginet's child harness (diginet/testsupport/emu_child.h, libdiginet-emu) was not found: install libdiginet 0.1.8 or later, or set DIGINET_SRC to a built libdiginet tree"; exit 1; }
else
test-diginet:
	@echo "SKIP: the DigiNet module's tests: $(DIGINET_NONE)"
endif

# ---- the Mackie module (libsurface-mackie): MCU surfaces such as the X-Touch, over libmackie ----
# Built when libmackie is found: through pkg-config, or from a built tree with MACKIE_SRC=../libmackie.
# MACKIE=auto (the default) builds it when found, MACKIE=yes stops the build when not, MACKIE=no never.
MACKIE_SRC ?=
ifneq ($(MACKIE_SRC),)
MACKIE_CFLAGS := -I$(MACKIE_SRC)/include
MACKIE_LIBS   := $(MACKIE_SRC)/libmackie.a
else
MACKIE_CFLAGS := $(shell pkg-config --cflags libmackie 2>/dev/null)
MACKIE_LIBS   := $(shell pkg-config --libs libmackie 2>/dev/null)
endif
MACKIE ?= auto
ifneq ($(MACKIE),no)
HAVE_MACKIE := $(shell printf '\043include <mackie/mackie_codec.h>\n' | \
	$(CC) $(MACKIE_CFLAGS) -E -x c - >/dev/null 2>&1 && echo yes)
endif
MACKIE_NONE := libmackie (mackie/mackie_codec.h) was not found: install libmackie 0.1.0 or later, or set \
	MACKIE_SRC to a built libmackie tree
ifeq ($(MACKIE),no)
MACKIE_NONE := MACKIE=no
endif
ifeq ($(MACKIE),yes)
ifneq ($(HAVE_MACKIE),yes)
$(error $(MACKIE_NONE))
endif
endif

MOBJS := src/mackie/surface_mackie.o
MABI  := $(shell sed -n 's/^\#define SURFACE_MACKIE_ABI  *\([0-9]*\)$$/\1/p' include/surface/surface_mackie.h)

src/mackie/%.o: src/mackie/%.c
	$(CC) $(CFLAGS) $(WARN) -MMD -MP $(INC) $(MACKIE_CFLAGS) -c $< -o $@

-include $(MOBJS:.o=.d)

libsurface-mackie.a: $(MOBJS)
	$(AR) rcs $@ $(MOBJS)

ifeq ($(HAVE_MACKIE),yes)
mackie: libsurface-mackie.a
else
mackie:
	@echo "SKIP: the Mackie module (libsurface-mackie) not built: $(MACKIE_NONE)"
endif

all: mackie

tests/mackie/test_mackie: tests/mackie/test_mackie.c tests/harness.h libsurface-mackie.a libsurface.a
	$(CC) $(CFLAGS) $(WARN) $(INC) $(MACKIE_CFLAGS) -Itests $< libsurface-mackie.a libsurface.a \
	    $(MACKIE_LIBS) $(LIBS) $(LDFLAGS) -o $@

ifeq ($(HAVE_MACKIE),yes)
test-mackie: tests/mackie/test_mackie
	$(RUN_TEST) tests/mackie/test_mackie
else
test-mackie:
	@echo "SKIP: the Mackie module's tests: $(MACKIE_NONE)"
endif

# the Control|24 key table is data alone: its test needs no libdiginet
$(KEYS_TEST): tests/test_c24_keys.c src/diginet/c24_keys.c include/surface/surface_diginet_keys.h
	$(CC) $(CFLAGS) $(WARN) $(INC) tests/test_c24_keys.c src/diginet/c24_keys.c -o $@

$(TESTS): tests/%: tests/%.c tests/harness.h libsurface.a
	$(CC) $(CFLAGS) $(WARN) $(INC) $< libsurface.a $(LIBS) -o $@

$(UB_TEST): tests/%: tests/%.c tests/harness.h $(OBJS:.o=.c) src/surface_impl.h
	$(CC) $(CFLAGS) $(WARN) -fsanitize=undefined -fsanitize-undefined-trap-on-error $(INC) $< \
	    $(OBJS:.o=.c) $(LIBS) -o $@

# ---- shared library (the packages) ---------------------------------------------------------
# --no-undefined: a symbol nothing defines fails here, not at a consumer's link.
SO := libsurface.so.$(VERSION)

$(SO): $(OBJS)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libsurface.so.$(ABI) -Wl,--no-undefined -o $@ $(OBJS) $(LIBS)
	ln -sf $@ libsurface.so.$(ABI)
	ln -sf libsurface.so.$(ABI) libsurface.so

DSO := libsurface-diginet.so.$(VERSION)

MSO := libsurface-mackie.so.$(VERSION)

$(MSO): $(MOBJS) $(SO)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libsurface-mackie.so.$(MABI) -Wl,--no-undefined -o $@ \
	    $(MOBJS) -L. -lsurface $(MACKIE_LIBS) $(LIBS)
	ln -sf $@ libsurface-mackie.so.$(MABI)
	ln -sf libsurface-mackie.so.$(MABI) libsurface-mackie.so

ifeq ($(HAVE_MACKIE),yes)
shared-mackie: $(MSO)
else
shared-mackie: mackie
endif

$(DSO): $(DOBJS) $(SO)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libsurface-diginet.so.$(DABI) -Wl,--no-undefined -o $@ \
	    $(DOBJS) -L. -lsurface $(DIGINET_LIBS) $(LIBS)
	ln -sf $@ libsurface-diginet.so.$(DABI)
	ln -sf libsurface-diginet.so.$(DABI) libsurface-diginet.so

# surface-c24 linked against the shared libraries, as the packages carry it
build/surface-c24: tools/surface-c24.c $(DSO)
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(INC) $(DIGINET_CFLAGS) $(C24_DEF) $< $(LDFLAGS) \
	    -L. -lsurface-diginet -lsurface $(DIGINET_LIBS) $(LIBS) -o $@

ifeq ($(HAVE_DIGINET),yes)
shared-diginet: $(DSO) build/surface-c24
else
shared-diginet: diginet
endif

shared: $(SO) shared-diginet shared-mackie

test: $(TESTS) $(KEYS_TEST) $(UB_TEST)
	$(RUN_TEST) tests/test_motor
	$(RUN_TEST) tests/test_audit
	$(RUN_TEST) tests/test_bank
	$(RUN_TEST) tests/test_meter $(GOLDEN)
	$(RUN_TEST) tests/test_c24
	$(RUN_TEST) tests/test_c24_keys
	$(RUN_TEST) tests/test_colour_null
	$(RUN_TEST) tests/constants-check.sh
	$(RUN_TEST) tests/diginet/no-emu-copy.sh
	$(RUN_TEST) tests/diginet/test_no_emu_copy_sabotage.sh
	$(RUN_TEST) tests/test_install.sh
	$(RUN_TEST) tests/conformance-packaging.sh
	$(RUN_TEST) tests/test_workflows.sh
	@$(MAKE) --no-print-directory test-diginet
	@$(MAKE) --no-print-directory test-mackie

# No tracked file points into a private repository: Open-Mixer/.github's private-refs check, from a
# checkout of it (FREEMIXER_DOTGITHUB, else ../freemixer-dotgithub). CI runs it, with the commit
# message check, through the public-sources-check workflow; `make test` does not need it.
FREEMIXER_DOTGITHUB ?= ../freemixer-dotgithub
public-sources:
	$(FREEMIXER_DOTGITHUB)/.github/actions/private-refs-check/check.sh .

# The pkg-config file is written at install time, from the install's own PREFIX and LIBDIR: a
# file kept from an earlier build would name that build's directories.
PCSED = sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCDIR@|$(INCDIR)|' \
	    -e 's|@VERSION@|$(VERSION)|'

# What both installs carry: the headers and the pkg-config files, the module's when it is built.
install-common:
	install -d $(DESTDIR)$(INCDIR)/surface $(DESTDIR)$(PCDIR)
	install -m 644 include/surface/surface.h $(DESTDIR)$(INCDIR)/surface/
	$(PCSED) packaging/libsurface.pc.in > $(DESTDIR)$(PCDIR)/libsurface.pc
ifeq ($(HAVE_DIGINET),yes)
	install -m 644 include/surface/surface_diginet.h include/surface/surface_diginet_keys.h $(DESTDIR)$(INCDIR)/surface/
	$(PCSED) packaging/libsurface-diginet.pc.in > $(DESTDIR)$(PCDIR)/libsurface-diginet.pc
endif
ifeq ($(HAVE_MACKIE),yes)
	install -m 644 include/surface/surface_mackie.h $(DESTDIR)$(INCDIR)/surface/
	$(PCSED) packaging/libsurface-mackie.pc.in > $(DESTDIR)$(PCDIR)/libsurface-mackie.pc
endif

install: libsurface.a diginet mackie install-common
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 libsurface.a $(DESTDIR)$(LIBDIR)/
ifeq ($(HAVE_MACKIE),yes)
	install -m 644 libsurface-mackie.a $(DESTDIR)$(LIBDIR)/
endif
ifeq ($(HAVE_DIGINET),yes)
	install -d $(DESTDIR)$(BINDIR)
	install -m 644 libsurface-diginet.a $(DESTDIR)$(LIBDIR)/
	install -m 755 $(C24) $(DESTDIR)$(BINDIR)/
endif

# The packages: the shared libraries with their soname links.
install-shared: shared install-common
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 $(SO) $(DESTDIR)$(LIBDIR)/
	ln -sf $(SO) $(DESTDIR)$(LIBDIR)/libsurface.so.$(ABI)
	ln -sf libsurface.so.$(ABI) $(DESTDIR)$(LIBDIR)/libsurface.so
ifeq ($(HAVE_MACKIE),yes)
	install -m 755 $(MSO) $(DESTDIR)$(LIBDIR)/
	ln -sf $(MSO) $(DESTDIR)$(LIBDIR)/libsurface-mackie.so.$(MABI)
	ln -sf libsurface-mackie.so.$(MABI) $(DESTDIR)$(LIBDIR)/libsurface-mackie.so
endif
ifeq ($(HAVE_DIGINET),yes)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 build/surface-c24 $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 644 docs/surface-c24.1 $(DESTDIR)$(MANDIR)/man1/
	install -m 755 $(DSO) $(DESTDIR)$(LIBDIR)/
	ln -sf $(DSO) $(DESTDIR)$(LIBDIR)/libsurface-diginet.so.$(DABI)
	ln -sf libsurface-diginet.so.$(DABI) $(DESTDIR)$(LIBDIR)/libsurface-diginet.so
endif

# ---- fuzzing (fuzz/README.md) ----------------------------------------------------------------
# One libFuzzer harness per input surface of the library in fuzz/, each linked against a library
# of its own: the same sources compiled with clang, AddressSanitizer, UndefinedBehaviorSanitizer
# and libFuzzer's coverage hooks into build/fuzz/obj, so the ordinary build above is never
# touched. None of these targets is part of `make test`.
#   make fuzz                   build every harness
#   make fuzz-run FUZZ_TIME=60  fuzz each for the budget; red on any crash, the input left in
#                               fuzz/findings/
#   make fuzz-sabotage          each patch of fuzz/sabotage/ must make its harness abort on a
#                               seed, in the property its header names
#   make fuzz-records           each check's mutant: and input: record, alone, must make that
#                               check abort on that seed; red on a check with no record and no
#                               not-a-property line
#   make fuzz-coverage          line coverage of each reader over its seed corpus, red below
#                               fuzz/coverage-floor.txt or on a property check no seed reaches
#   make fuzz-static            each harness linked fully static with a plain driver, over its
#                               corpus
#   make fuzz-corpus            write fuzz/corpus/ again from the library and the builders
#   make fuzz-corpus-check      red when fuzz/corpus/ is not what fuzz-corpus writes
FUZZ_CC        ?= clang
FUZZ_TIME      ?= 60
FUZZ_BUILD     := build/fuzz
FUZZ_HARNESSES := pure board bank motor keys
FUZZ_SAN       := -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ_CFLAGS    := -O1 -g -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fno-omit-frame-pointer
# What the harnesses drive: the core and the Control|24 key table (not the modules that need a
# transport library).
FUZZ_SRCS      := $(OBJS:.o=.c) src/diginet/c24_keys.c
FUZZ_LIB       := $(FUZZ_BUILD)/libsurface-fuzz.a
FUZZ_BINS      := $(FUZZ_HARNESSES:%=$(FUZZ_BUILD)/fuzz_%)

$(FUZZ_BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SAN) -fsanitize=fuzzer-no-link -MMD -MP $(INC) -c $< -o $@

$(FUZZ_LIB): $(FUZZ_SRCS:%.c=$(FUZZ_BUILD)/obj/%.o)
	$(AR) rcs $@ $^

$(FUZZ_BUILD)/fuzz_%: fuzz/fuzz_%.c fuzz/fuzz.h fuzz/spec.h $(FUZZ_LIB)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SAN) -fsanitize=fuzzer -MMD -MP -MF $@.d $(INC) $< $(FUZZ_LIB) $(LIBS) -o $@

fuzz: $(FUZZ_BINS)

fuzz-run: fuzz
	FUZZ_BUILD=$(FUZZ_BUILD) FUZZ_TIME=$(FUZZ_TIME) fuzz/run.sh $(FUZZ_HARNESSES)

fuzz-sabotage:
	FUZZ_BUILD=$(FUZZ_BUILD) MAKE="$(MAKE)" fuzz/sabotage.sh

# Each record built plain and native with fuzz/standalone.c for its driver, no sanitizer, as the
# OSS Verify coverage build is.
fuzz-records:
	FUZZ_BUILD=$(FUZZ_BUILD) FUZZ_CC=$(FUZZ_CC) fuzz/records.sh $(FUZZ_HARNESSES)

# The same harnesses driven by fuzz/standalone.c instead of libFuzzer and linked fully static,
# each run over its corpus: the build a verifier that brings its own driver takes. Needs the
# static libc (glibc-static on Fedora).
FUZZ_STATIC_BUILD := $(FUZZ_BUILD)/static
FUZZ_STATIC_FLAGS := -O2 -g -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE

$(FUZZ_STATIC_BUILD)/fuzz_%: fuzz/fuzz_%.c fuzz/fuzz.h fuzz/spec.h fuzz/standalone.c $(FUZZ_SRCS)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_STATIC_FLAGS) -static $(INC) -Ifuzz $< fuzz/standalone.c $(FUZZ_SRCS) $(LIBS) -o $@

fuzz-static: $(FUZZ_HARNESSES:%=$(FUZZ_STATIC_BUILD)/fuzz_%)
	@for h in $(FUZZ_HARNESSES); do \
	  if readelf -lW $(FUZZ_STATIC_BUILD)/fuzz_$$h | grep -qE 'INTERP|DYNAMIC'; then \
	    echo "fuzz-static: fuzz_$$h is not static"; exit 1; fi; \
	  out=$$($(FUZZ_STATIC_BUILD)/fuzz_$$h fuzz/corpus/$$h/* 2>&1) || \
	    { echo "$$out"; echo "fuzz-static: fuzz_$$h failed on its own corpus"; exit 1; }; \
	  echo "fuzz_$$h, static: $$out"; done

# The coverage witness: the same harnesses without sanitizers, instrumented for llvm-cov, each
# run once over its seed corpus (-runs=0 executes the corpus and nothing else); the llvm-cov
# and llvm-profdata of FUZZ_CC's own LLVM.
FUZZ_COV_BUILD := $(FUZZ_BUILD)/cov
FUZZ_COV_FLAGS := -O0 -g -std=c11 -D_DEFAULT_SOURCE -fprofile-instr-generate -fcoverage-mapping
FUZZ_COV_LIB   := $(FUZZ_COV_BUILD)/libsurface-cov.a
LLVM_COV       ?= llvm-cov
LLVM_PROFDATA  ?= llvm-profdata

$(FUZZ_COV_BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_COV_FLAGS) -MMD -MP $(INC) -c $< -o $@

$(FUZZ_COV_LIB): $(FUZZ_SRCS:%.c=$(FUZZ_COV_BUILD)/obj/%.o)
	$(AR) rcs $@ $^

$(FUZZ_COV_BUILD)/fuzz_%: fuzz/fuzz_%.c fuzz/fuzz.h fuzz/spec.h $(FUZZ_COV_LIB)
	$(FUZZ_CC) $(FUZZ_COV_FLAGS) -fsanitize=fuzzer $(INC) $< $(FUZZ_COV_LIB) $(LIBS) -o $@

fuzz-coverage: $(FUZZ_HARNESSES:%=$(FUZZ_COV_BUILD)/fuzz_%)
	@set -e; for h in $(FUZZ_HARNESSES); do \
	  rm -f $(FUZZ_COV_BUILD)/$$h.profraw; \
	  LLVM_PROFILE_FILE=$(FUZZ_COV_BUILD)/$$h.profraw $(FUZZ_COV_BUILD)/fuzz_$$h -runs=0 \
	      fuzz/corpus/$$h/ >$(FUZZ_COV_BUILD)/$$h.log 2>&1 || \
	    { cat $(FUZZ_COV_BUILD)/$$h.log; echo "fuzz-coverage: fuzz_$$h failed on its own corpus"; exit 1; }; \
	  $(LLVM_PROFDATA) merge -sparse $(FUZZ_COV_BUILD)/$$h.profraw -o $(FUZZ_COV_BUILD)/$$h.profdata; \
	done
	@LLVM_COV=$(LLVM_COV) fuzz/coverage.sh $(FUZZ_COV_BUILD) $(FUZZ_HARNESSES)

# The seed corpus is written by fuzz/gen-corpus.c from the library's own presets and the
# harnesses' input forms, never from a network or a capture, and committed.
FUZZ_GEN := build/fuzz-gen-corpus
$(FUZZ_GEN): fuzz/gen-corpus.c fuzz/fuzz.h fuzz/spec.h libsurface.a src/diginet/c24_keys.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(INC) -Ifuzz fuzz/gen-corpus.c src/diginet/c24_keys.c libsurface.a $(LIBS) -o $@

fuzz-corpus: $(FUZZ_GEN)
	rm -rf fuzz/corpus
	$(FUZZ_GEN) fuzz/corpus

fuzz-corpus-check: $(FUZZ_GEN)
	@rm -rf build/fuzz-corpus-check
	@$(FUZZ_GEN) build/fuzz-corpus-check
	@diff -r build/fuzz-corpus-check fuzz/corpus && echo "fuzz/corpus is what fuzz-corpus writes"

-include $(FUZZ_SRCS:%.c=$(FUZZ_BUILD)/obj/%.d) $(FUZZ_BINS:=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) libsurface.a $(TESTS) $(KEYS_TEST) $(UB_TEST) libsurface.so*
	rm -f $(DOBJS) $(DOBJS:.o=.d) libsurface-diginet.a libsurface-diginet.so* $(DTESTS) $(C24)
	rm -f $(MOBJS) $(MOBJS:.o=.d) libsurface-mackie.a libsurface-mackie.so* tests/mackie/test_mackie
	rm -rf build

.PHONY: fuzz fuzz-run fuzz-sabotage fuzz-records fuzz-coverage fuzz-static fuzz-corpus fuzz-corpus-check all diginet mackie test public-sources test-diginet test-mackie shared shared-diginet shared-mackie install install-common install-shared clean

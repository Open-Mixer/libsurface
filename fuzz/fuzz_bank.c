// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: THE BANK WINDOW (src/bank.c): a movable window of `strips` targets over the
 * host's ordered list, paged by a bank or by a channel, brought into view or put on strip 0, with
 * a master fader beside it.
 *
 * Input: a surface (a preset, or a window of the input's own width, with a list that holds
 * between 1 and 8 targets or the default 256), then a program, one operation after another until
 * the input ends: set the list (targets include 0, repeats and 0xffffffff; the list may be
 * longer than the table, or NULL with a length), step by a delta (any int, the extremes
 * included), show a target, jump to one, set the master.
 *
 * The harness keeps the window on its own, from surface.h's words (a window of `strips` targets
 * over the list; a step moves by its delta clamped to the list; show brings the bank-aligned page
 * of a target into view; jump puts the target on strip 0 and lets the window overhang the end of
 * the list). Properties, each an abort() in a check_ function:
 *   - set: the count kept is the list's length up to the table's size, a NULL list with a length
 *     is refused, and the window follows the clamp to the shorter list.
 *   - window: after every operation the offset is the model's, whatever the delta, and each call
 *     says it moved exactly when the offset changed.
 *   - targets: a strip shows the list at offset + strip (0 past its end), the master fader the
 *     master target on a geometry that has one, and nothing a strip past those; target_strip is the
 *     first strip that shows a target, and -1 for 0 and for a target off the window.
 *   - meters: every move of the window, and every new list, clears the meter steps (the next
 *     level of every strip paints at once); a call that moved nothing clears nothing: a step,
 *     show or jump that stayed, a refused list, a new master.
 */
#include <surface/surface.h>

#include "fuzz.h"

#define MAXLIST 256

static struct {
	struct surface_geometry g;
	unsigned cap;               /* the table's size */
	surface_target list[MAXLIST];
	size_t n;
	size_t offset;
	surface_target master;
	unsigned meter_calls;
} W;

static const surface_target targets[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 0xffffffffu };

static int op_fader(void *b, unsigned strip, uint16_t raw) { (void)b; (void)strip; (void)raw; return 1; }
static int op_led(void *b, uint16_t k, enum surface_led st) { (void)b; (void)k; (void)st; return 1; }
static int op_ring(void *b, unsigned s, uint32_t m) { (void)b; (void)s; (void)m; return 1; }
static int op_meter(void *b, unsigned s, unsigned side, unsigned seg)
{
	(void)b; (void)s; (void)side; (void)seg;
	W.meter_calls++;
	return 1;
}
static int op_scribble(void *b, unsigned s, unsigned r, const char *t, size_t n)
{
	(void)b; (void)s; (void)r; (void)t; (void)n;
	return 1;
}
static int op_counter(void *b, const char *t) { (void)b; (void)t; return 1; }

static const struct surface_backend_ops ops = {
	.fader = op_fader, .led = op_led, .ring = op_ring, .meter = op_meter,
	.scribble = op_scribble, .counter = op_counter,
};

/* ---- the window as surface.h describes it ------------------------------------------------- */

static size_t m_max(void)
{
	return W.n > W.g.strips ? W.n - W.g.strips : 0;
}

static long m_index(surface_target t)
{
	if (!t)
		return -1;
	for (size_t i = 0; i < W.n; i++)
		if (W.list[i] == t)
			return (long)i;
	return -1;
}

static surface_target m_target(unsigned strip)
{
	if (strip < W.g.strips)
		return W.offset + strip < W.n ? W.list[W.offset + strip] : 0;
	if (strip == W.g.strips && W.g.has_master)
		return W.master;
	return 0;
}

static int m_strip(surface_target t)
{
	if (!t)
		return -1;
	for (unsigned k = 0; k < W.g.strips; k++)
		if (m_target(k) == t)
			return (int)k;
	return -1;
}

/* the window moves to `to` (clamped to the list): 1 when it moved */
static int m_move(long to)
{
	size_t before = W.offset;
	W.offset = to < 0 ? 0 : (size_t)to;
	if (W.offset > m_max())
		W.offset = m_max();
	return W.offset != before;
}

/* ---- the checks --------------------------------------------------------------------------- */

FUZZ_CHECK check_set(int rc, size_t n, int null_list)
{
	/* A NULL list with a length is refused.
	 * contract: surface.h surface_bank_set; SURFACE_E_ARG ("a NULL pointer")
	 * mutant: src/bank.c:54 `return SURFACE_E_ARG;` -> `return SURFACE_OK;`
	 * input: set-refusal */
	if (null_list && n > 0 ? rc != SPEC_E_ARG : 0)
		abort();
	if (null_list && n > 0)
		return;
	size_t want = n < W.cap ? n : W.cap;
	/* The count kept is the list's length, up to the table's size.
	 * contract: surface.h surface_bank_set ("kept count"), struct surface_config max_targets
	 * ("motor states and bank list length; 0 = 256")
	 * mutant: src/bank.c:55 `n > s->cap` -> `n > s->cap + 1`
	 * input: set-long */
	if (rc != (int)want)
		abort();
}

FUZZ_CHECK check_window(const struct surface *s, int moved, int expect_moved)
{
	/* The offset is the model's: a step moves by its delta, clamped to the list, whatever the
	 * delta.
	 * contract: surface.h surface_bank_step ("+-strips a bank, +-1 a channel")
	 * mutant: src/bank.c:33 `at < 0 ? 0 : (size_t)at` -> `at < 0 ? 1 : (size_t)at`
	 * input: steps
	 * mutant: src/bank.c:23 `s->nlist > w ? s->nlist - w : 0` -> `s->nlist > w ? s->nlist - w + 1 : 0`
	 * input: steps */
	if (surface_bank_offset(s) != (int)W.offset)
		abort();
	/* A call says it moved exactly when the offset changed.
	 * contract: surface.h "1 moved"
	 * mutant: src/bank.c:35 `if (s->offset == before)` -> `if (s->offset <= before)`
	 * input: steps */
	if (moved != expect_moved)
		abort();
}

FUZZ_CHECK check_targets(const struct surface *s)
{
	int bad = 0;
	for (unsigned k = 0; k <= W.g.strips + 2; k++)
		bad |= surface_strip_target(s, k) != m_target(k);
	/* Each strip shows the list at offset + strip, 0 past its end; the master fader the master;
	 * nothing a strip past those.
	 * contract: surface.h surface_strip_target ("master included"), surface_master_set,
	 * struct surface_geometry has_master ("a dedicated master fader: strip index `strips`")
	 * mutant: src/bank.c:113 `i < s->nlist ? s->list[i] : 0` -> `i <= s->nlist ? s->list[i] : 0`
	 * input: steps
	 * mutant: src/bank.c:116 `return s->master;` -> `return 0;`
	 * input: master */
	if (bad)
		abort();
}

FUZZ_CHECK check_target_strip(const struct surface *s, surface_target t)
{
	/* A target's strip is the first that shows it, -1 for 0 and a target off the window; the
	 * master fader is no bank strip.
	 * contract: surface.h surface_target_strip ("bank strip, -1 off-window")
	 * mutant: src/bank.c:122 `if (!s || !t)` -> `if (!s)`
	 * input: repeats
	 * mutant: src/bank.c:124 `for (unsigned k = 0; k < s->g.strips; k++)` -> `for (unsigned k = 0; k <= s->g.strips; k++)`
	 * input: master */
	if (surface_target_strip(s, t) != m_strip(t))
		abort();
}

FUZZ_CHECK check_meters(unsigned calls, int cleared)
{
	unsigned want = cleared ? W.g.strips * W.g.meter_sides : 0;
	/* A move of the window, and a new list, clear the meter steps: the next level of every
	 * strip and side is sent, however little it changed; a call that moved nothing clears
	 * nothing, and the level that did not change is not sent again.
	 * contract: surface.h "Every move clears the meter steps"; bank.c's header
	 * mutant: src/bank.c:37 `meters_reset(s);` -> ``
	 * input: meters
	 * mutant: src/bank.c:61 `meters_reset(s);` -> ``
	 * input: meters
	 * mutant: src/bank.c:98 `meters_reset(s);` -> ``
	 * input: meters
	 * mutant: src/bank.c:54 `return SURFACE_E_ARG;` -> `return meters_reset(s), SURFACE_E_ARG;`
	 * input: set-refusal
	 * mutant: src/bank.c:133 `s->master = t;` -> `s->master = t, meters_reset(s);`
	 * input: master */
	if (calls != want)
		abort();
}

/* ---- the program -------------------------------------------------------------------------- */

static struct surface *S;
static uint64_t T = 1000000;

/* every level, once: the steps are known */
static void meters_prime(void)
{
	for (unsigned k = 0; k < W.g.strips; k++)
		for (unsigned side = 0; side < W.g.meter_sides; side++)
			surface_meter(S, k, side, 0.5, T);
}

static unsigned meters_probe(void)
{
	W.meter_calls = 0;
	meters_prime();
	return W.meter_calls;
}

static void check_all(surface_target t)
{
	check_targets(S);
	check_target_strip(S, t);
}

static void do_set(struct fuzz_in *in)
{
	size_t n = fuzz_u8(in) % 44;
	int null_list = (fuzz_u8(in) & 7) == 0;
	surface_target *l = (surface_target *)fuzz_alloc(n * sizeof *l);
	for (size_t i = 0; i < n; i++)
		l[i] = targets[fuzz_u8(in) % 9];
	meters_prime();
	int rc = surface_bank_set(S, null_list ? NULL : l, n);
	check_set(rc, n, null_list);
	if (!(null_list && n > 0)) {
		W.n = n < W.cap ? n : W.cap;
		memcpy(W.list, l, W.n * sizeof *l);
		m_move((long)W.offset);
		check_meters(meters_probe(), 1);
	} else {
		check_meters(meters_probe(), 0);
	}
	check_window(S, 0, 0);
	free(l);
}

static void do_step(struct fuzz_in *in)
{
	static const int deltas[] = { 0, 1, -1, 8, -8, 24, -24, 3, 100, -100, 2147483647, -2147483647 - 1 };
	uint8_t b = fuzz_u8(in);
	int delta = b < 12 ? deltas[b] : (int)(int8_t)b;
	meters_prime();
	int rc = surface_bank_step(S, delta);
	int moved = m_move((long)W.offset + delta);
	check_window(S, rc, moved);
	check_meters(meters_probe(), moved);
	check_all(0);
}

static void do_show(struct fuzz_in *in)
{
	surface_target t = targets[fuzz_u8(in) % 9];
	meters_prime();
	int rc = surface_bank_show(S, t), moved = 0;
	if (m_strip(t) < 0 && m_index(t) >= 0)
		moved = m_move(m_index(t) / (long)W.g.strips * (long)W.g.strips);
	check_window(S, rc, moved);
	check_meters(meters_probe(), moved);
	check_all(t);
}

static void do_jump(struct fuzz_in *in)
{
	surface_target t = targets[fuzz_u8(in) % 9];
	meters_prime();
	int rc = surface_bank_jump(S, t), moved = 0;
	long at = m_index(t);
	if (at >= 0 && (size_t)at != W.offset) {
		W.offset = (size_t)at;
		moved = 1;
	}
	check_window(S, rc, moved);
	check_meters(meters_probe(), moved);
	check_all(t);
}

static void do_master(struct fuzz_in *in)
{
	surface_target t = targets[fuzz_u8(in) % 9];
	meters_prime();
	surface_master_set(S, t);
	if (W.g.has_master)
		W.master = t;
	check_meters(meters_probe(), 0);
	check_all(t);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	memset(&W, 0, sizeof W);

	struct surface_config c = { .ops = &ops };
	uint8_t model = fuzz_u8(&in) % 3;
	if (model < 2) {
		surface_geometry_preset(model ? SURFACE_MODEL_CONTROL24 : SURFACE_MODEL_XTOUCH, &c.geometry);
	} else {
		struct surface_geometry *g = &c.geometry;
		g->strips = 1 + fuzz_u8(&in) % 32;
		g->has_master = fuzz_u8(&in) & 1;
		g->fader_max = 1023;
		g->meter_sides = fuzz_u8(&in) % 3;
		g->meter_segments = 12;
		g->ring_leds = 11;
		g->scribble_rows = 1;
		g->scribble_chars = 4;
		g->meter_keepalive_ms = 0;
	}
	uint8_t cap = fuzz_u8(&in) % 9;
	c.max_targets = cap;
	W.cap = cap ? cap : 256;
	W.g = c.geometry;
	if (surface_create(&S, &c) != SPEC_OK)
		return 0;
	surface_in_online(S, T);
	check_all(0);
	while (!fuzz_done(&in)) {
		switch (fuzz_u8(&in) % 6) {
		case 0: do_set(&in); break;
		case 1: do_step(&in); break;
		case 2: do_show(&in); break;
		case 3: do_jump(&in); break;
		case 4: do_master(&in); break;
		default: check_all(targets[fuzz_u8(&in) % 9]); break;
		}
	}
	surface_destroy(S);
	return 0;
}

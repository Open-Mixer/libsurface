// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: THE BOARD SHADOW (src/surface.c): creation, the element writes (LED, ring,
 * scribble strip, counter, colour, a blanked strip), the backend that receives them, and the
 * replay of the whole board when a device comes online.
 *
 * Input: a surface (a preset, or a geometry of the input's own, which creation may refuse),
 * then a program, one operation after another until the input ends: a write with its
 * arguments (a key, a strip, a mode, a text, a colour per strip), a device that goes offline or
 * comes online. A text lives in an allocation of exactly its length, so a read past its NUL is a
 * sanitizer report. The backend is the harness's own: it keeps the board a device would show,
 * and checks every call it gets.
 *
 * The harness holds what the host asked for on its own, from surface.h's words (the latest write
 * of each element wins), and the board the device shows. Properties, each an abort() in a check_
 * function:
 *   - create: a geometry with no strips, more than 32, no fader range, or more sides, LEDs,
 *     rows or characters than the sizes allow is refused, any other made, and a surface is
 *     offline until the device comes online.
 *   - refusal: a write to a strip, row or LED state that is not there is SURFACE_E_ARG, any
 *     other is not; an LED table of max_leds entries is SURFACE_E_FULL for a key it does not hold.
 *   - offline: a device that is offline gets no call, whatever is written.
 *   - live: a write on an online device reaches the backend as the value the host wrote: an LED
 *     with its state, a ring as the mask surface.h renders, a scribble as the text of
 *     fuzz/spec.h, exactly the display's width, the counter as written up to 31 bytes, the
 *     colours up to one per strip.
 *   - args: every call the backend gets is within the geometry: a fader raw within its range, a
 *     ring mask within its LEDs, a strip, row and side that exist.
 *   - replay: after the device comes online, the board it shows is the host's whole intent, no
 *     more and no less: every LED, ring, scribble row, the counter and the colours.
 *   - blank: a blanked strip shows spaces in every row and an empty ring, and replays so.
 */
#include <surface/surface.h>

#include "fuzz.h"

#define NKEYS 24

/* The keys a program writes: strip buttons, section keys, the automation LEDs (bit 15) and the
 * ends of the 16-bit range. */
static const uint16_t keys[NKEYS] = {
	0x0000, 0x0101, 0x0218, 0x0319, 0x0408, 0x051d, 0x0607, 0x0700, 0x0801, 0x0a17, 0x8001, 0x8102,
	0x82ff, 0x8400, 0x4000, 0x7fff, 0x8000, 0xffff, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006,
};

struct board {
	/* what the device shows */
	uint8_t led_seen[NKEYS], led[NKEYS];
	uint8_t ring_seen[SPEC_MAX_STRIPS], ring_code[SPEC_MAX_STRIPS];
	uint32_t ring[SPEC_MAX_STRIPS];
	uint8_t scr_seen[SPEC_MAX_STRIPS][SPEC_MAX_SCRIBBLE_ROWS];
	char scr[SPEC_MAX_STRIPS][SPEC_MAX_SCRIBBLE_ROWS][SPEC_MAX_SCRIBBLE_CHARS];
	uint8_t counter_seen;
	char counter[SPEC_MAX_COUNTER];
	size_t ncolour;
	uint8_t colour[SPEC_MAX_STRIPS];
};

struct host {
	/* what the host asked for: the latest write of each element */
	struct board want;
	unsigned nkeys;   /* LEDs the table holds */
	uint8_t key_held[NKEYS];
};

static struct {
	struct surface_geometry g;
	unsigned max_leds;
	int online;
	unsigned calls;
	struct board dev;
	struct host host;
	int has_ring_code, has_colour;
} W;

/* the slot of a key the harness wrote, or -1 for a key it never wrote */
static int key_index(uint16_t key)
{
	for (int i = 0; i < NKEYS; i++)
		if (keys[i] == key)
			return i;
	return -1;
}

FUZZ_CHECK check_led_key(int i)
{
	/* An LED reaches the device at a key the host wrote, never at another.
	 * contract: surface.h surface_led ("latest wins"); surface_led takes the key of the LED it sets
	 * mutant: src/surface.c:363 `s->ops.led(s->backend, key, st)` -> `s->ops.led(s->backend, key ^ 0x0400, st)`
	 * input: leds */
	if (i < 0)
		abort();
}

FUZZ_CHECK check_args_fader(unsigned strip, uint16_t raw)
{
	/* A fader is written at a strip that exists, master included, within the fader's range.
	 * contract: surface.h struct surface_geometry fader_max ("16383 MCU pitch bend, 1023 DigiNet"),
	 * strips ("a master fader is one more")
	 * mutant: src/motor.c:71 `return 0;` -> `return s->g.fader_max + 1;`
	 * input: online-blank */
	if (strip >= W.g.strips + (W.g.has_master ? 1u : 0u) || raw > W.g.fader_max)
		abort();
}

FUZZ_CHECK check_args_ring(unsigned strip, uint32_t mask, int code)
{
	/* A ring is written at a strip that exists, a mask within the ring's LEDs.
	 * contract: surface.h struct surface_backend_ops ring ("ring gets the LED mask, bit 0 the
	 * leftmost LED"), struct surface_geometry ring_leds
	 * mutant: src/meter.c:96 `p ? span(0, p - 1) : 0` -> `p ? span(0, p) : 0`
	 * input: ring-full */
	if (strip >= W.g.strips || (!code && W.g.ring_leds < 32 && (mask >> W.g.ring_leds) != 0))
		abort();
}

FUZZ_CHECK check_args_scribble(unsigned strip, unsigned row, const char *text, size_t n)
{
	int bad = strip >= W.g.strips || row >= W.g.scribble_rows || n != W.g.scribble_chars;
	for (size_t i = 0; i < n && !bad; i++)
		bad = (unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e;
	/* A scribble is written at a strip and a row that exist, in the display's width, printable
	 * ASCII.
	 * contract: surface.h struct surface_geometry scribble_rows, scribble_chars
	 * mutant: src/surface.c:410 `s->g.scribble_chars) : 0;` -> `s->g.scribble_chars + 1) : 0;`
	 * input: scribbles */
	if (bad)
		abort();
}

static int op_fader(void *b, unsigned strip, uint16_t raw)
{
	(void)b;
	W.calls++;
	check_args_fader(strip, raw);
	return 1;
}

static int op_led(void *b, uint16_t key, enum surface_led st)
{
	(void)b;
	W.calls++;
	int i = key_index(key);
	check_led_key(i);
	W.dev.led_seen[i] = 1;
	W.dev.led[i] = (uint8_t)st;
	return 1;
}

static int op_ring(void *b, unsigned strip, uint32_t mask)
{
	(void)b;
	W.calls++;
	check_args_ring(strip, mask, 0);
	W.dev.ring_seen[strip] = 1;
	W.dev.ring_code[strip] = 0;
	W.dev.ring[strip] = mask;
	return 1;
}

static int op_ring_code(void *b, unsigned strip, uint32_t code)
{
	(void)b;
	W.calls++;
	check_args_ring(strip, code, 1);
	W.dev.ring_seen[strip] = 1;
	W.dev.ring_code[strip] = 1;
	W.dev.ring[strip] = code;
	return 1;
}

FUZZ_CHECK check_args_meter(unsigned strip, unsigned side, unsigned segments)
{
	/* A meter is written at a strip and a side that exist, within the segments of the bar.
	 * contract: surface.h struct surface_geometry meter_sides, meter_segments
	 * mutant: src/surface.c:453 `side, 0) > 0` -> `side, 255) > 0`
	 * input: online-blank */
	if (strip >= W.g.strips || side >= W.g.meter_sides || segments > W.g.meter_segments)
		abort();
}

static int op_meter(void *b, unsigned strip, unsigned side, unsigned segments)
{
	(void)b;
	W.calls++;
	check_args_meter(strip, side, segments);
	return 1;
}

static int op_scribble(void *b, unsigned strip, unsigned row, const char *text, size_t n)
{
	(void)b;
	W.calls++;
	/* the device reads exactly n bytes, from a block of exactly n */
	uint8_t *copy = fuzz_dup((const uint8_t *)text, n);
	check_args_scribble(strip, row, (const char *)copy, n);
	W.dev.scr_seen[strip][row] = 1;
	memcpy(W.dev.scr[strip][row], copy, n);
	free(copy);
	return 1;
}

static int op_counter(void *b, const char *text)
{
	(void)b;
	W.calls++;
	/* a text the device cannot hold is refused, and check_live_counter sees it never arrive */
	size_t n = strnlen(text, SPEC_MAX_COUNTER);
	if (n == SPEC_MAX_COUNTER)
		return -1;
	W.dev.counter_seen = 1;
	memcpy(W.dev.counter, text, n + 1);
	return 1;
}

FUZZ_CHECK check_args_colour(const uint8_t *per_strip, size_t n)
{
	/* The colours handed over are at most one per strip, from a list that is there.
	 * contract: surface.h struct surface_backend_ops colour ("colour may be NULL")
	 * mutant: src/surface.c:431 `n = s->g.strips;` -> `n = s->g.strips + 1;`
	 * input: colours */
	if (n > W.g.strips || (n && !per_strip))
		abort();
}

static int op_colour(void *b, const uint8_t *per_strip, size_t n)
{
	(void)b;
	W.calls++;
	check_args_colour(per_strip, n);
	W.dev.ncolour = n;
	if (n)
		memcpy(W.dev.colour, per_strip, n);
	return 1;
}

static const struct surface_backend_ops ops = {
	.fader = op_fader, .led = op_led, .ring = op_ring, .meter = op_meter,
	.scribble = op_scribble, .counter = op_counter, .colour = op_colour, .ring_code = op_ring_code,
};

/* ---- the expected creation, from surface.h's sizes ---------------------------------------- */

static int spec_geometry_ok(const struct surface_geometry *g)
{
	return g->strips >= 1 && g->strips <= SPEC_MAX_STRIPS && g->fader_max >= 1 && g->meter_sides <= 2 &&
		g->ring_leds <= SPEC_MAX_RING_LEDS && g->scribble_rows <= SPEC_MAX_SCRIBBLE_ROWS &&
		g->scribble_chars <= SPEC_MAX_SCRIBBLE_CHARS;
}

FUZZ_CHECK check_create(const struct surface_geometry *g, int rc, const struct surface *s)
{
	/* A geometry inside the sizes makes a surface, any other is refused with SURFACE_E_ARG and
	 * leaves no surface.
	 * contract: surface.h "Sizes", surface_create ("*out is NULL on failure")
	 * mutant: src/surface.c:63 `g->strips <= SURFACE_MAX_STRIPS` -> `g->strips <= SURFACE_MAX_STRIPS + 1`
	 * input: geometry-1 */
	if (spec_geometry_ok(g) ? rc != SPEC_OK || !s : rc != SPEC_E_ARG || s)
		abort();
	/* A new surface is offline, and its geometry is the one it was given.
	 * contract: surface.h surface_create ("A new surface is offline until surface_in_online()"),
	 * surface_geometry_of
	 * mutant: src/surface.c:116 `return s ? s->online : 0;` -> `return s ? !s->online : 0;`
	 * input: geometry-2
	 * mutant: src/surface.c:92 `s->g = c->geometry;` -> `s->g = c->geometry; s->g.meter_keepalive_ms = 0;`
	 * input: online-blank */
	if (s && (surface_online(s) || surface_geometry_of(s)->strips != g->strips ||
		  surface_geometry_of(s)->fader_max != g->fader_max))
		abort();
}

/* ---- the host's intent -------------------------------------------------------------------- */

FUZZ_CHECK check_refusal(int rc, int bad)
{
	/* An argument that is not there (a strip off the geometry, an LED state past FLASH, a NULL
	 * text) is SURFACE_E_ARG; the same call with arguments that are there is not.
	 * contract: surface.h SURFACE_E_ARG ("a NULL pointer, a strip off the geometry, an
	 * impossible value")
	 * mutant: src/surface.c:369 `strip >= s->g.strips)` -> `strip > s->g.strips)`
	 * input: refusals */
	if (bad ? rc != SPEC_E_ARG : rc == SPEC_E_ARG)
		abort();
}

FUZZ_CHECK check_offline(unsigned calls_before)
{
	/* A device that is offline gets no call, whatever is written.
	 * contract: surface.h surface_in_online ("A new surface is offline until
	 * surface_in_online()"), the board shadow ("latest wins, kept in the board shadow")
	 * mutant: src/surface.c:375 `return s->online ? s->ops.ring(s->backend, strip, st->ring) : 0;` -> `return s->ops.ring(s->backend, strip, st->ring);`
	 * input: offline-writes */
	if (!W.online && W.calls != calls_before)
		abort();
}

FUZZ_CHECK check_full(int rc, int is_new, unsigned held)
{
	/* A key the LED table does not hold, when it holds max_leds, is SURFACE_E_FULL; a key it
	 * holds never is.
	 * contract: surface.h SURFACE_E_FULL ("a fixed table is full (targets, LEDs)"),
	 * struct surface_config max_leds ("keys in the LED shadow")
	 * mutant: src/surface.c:364 `return l ? rc : SURFACE_E_FULL;` -> `return rc;`
	 * input: led-full */
	if ((is_new && held >= W.max_leds) != (rc == SPEC_E_FULL))
		abort();
}

FUZZ_CHECK check_live_led(uint16_t key, enum surface_led st, unsigned calls_before, int led_before, int led_value_before)
{
	(void)led_before; (void)led_value_before;
	int i = key_index(key);
	/* An LED written on an online device reaches it once, as written.
	 * contract: surface.h surface_led ("latest wins")
	 * mutant: src/surface.c:363 `s->ops.led(s->backend, key, st)` -> `s->ops.led(s->backend, key, SURFACE_LED_ON)`
	 * input: leds */
	if (W.calls != calls_before + 1 || !W.dev.led_seen[i] || W.dev.led[i] != (uint8_t)st)
		abort();
}

FUZZ_CHECK check_live_ring(unsigned strip, int mode, double v, unsigned calls_before)
{
	/* A ring written on an online device reaches it once, as the mask surface.h renders.
	 * contract: surface.h enum surface_ring_mode; spec.h spec_ring
	 * mutant: src/surface.c:374 `st->ring = surface_ring_mask(m, v, s->g.ring_leds);` -> `st->ring = surface_ring_mask(m, v, s->g.ring_leds - 1);`
	 * input: ring-modes */
	if (W.calls != calls_before + 1 || !W.dev.ring_seen[strip] || W.dev.ring_code[strip] ||
	    W.dev.ring[strip] != spec_ring(mode, v, W.g.ring_leds))
		abort();
}

FUZZ_CHECK check_live_code(unsigned strip, uint32_t code, unsigned calls_before)
{
	/* A ring in the device's own code reaches it once, as it is.
	 * contract: surface.h surface_ring_code ("takes the code as it is")
	 * mutant: src/surface.c:386 `ring_code(s->backend, strip, code)` -> `ring_code(s->backend, strip, code & 0xff)`
	 * input: ring-code */
	if (W.calls != calls_before + 1 || !W.dev.ring_seen[strip] || !W.dev.ring_code[strip] ||
	    W.dev.ring[strip] != code)
		abort();
}

FUZZ_CHECK check_live_scribble(unsigned strip, unsigned row, const char *utf8, unsigned calls_before)
{
	char want[SPEC_MAX_SCRIBBLE_CHARS];
	spec_scribble(want, W.g.scribble_chars, utf8);
	/* A scribble reaches the device as the display shows it: one character per code point,
	 * '?' for what is not ASCII, a space for a control character, padded and cut to the width.
	 * contract: surface.h surface_scribble; spec.h spec_scribble
	 * mutant: src/surface.c:397 `? '?'` -> `? '.'`
	 * input: scribbles
	 * mutant: src/surface.c:395 `(*p & 0xc0) == 0x80` -> `(*p & 0xc0) == 0xc0`
	 * input: scribbles */
	if (W.calls != calls_before + 1 || !W.dev.scr_seen[strip][row] ||
	    memcmp(W.dev.scr[strip][row], want, W.g.scribble_chars))
		abort();
}

FUZZ_CHECK check_live_counter(const char *text, unsigned calls_before)
{
	size_t n = strlen(text) < SPEC_MAX_COUNTER - 1 ? strlen(text) : SPEC_MAX_COUNTER - 1;
	/* The counter reaches the device as written, cut to 31 bytes.
	 * contract: surface.h SURFACE_MAX_COUNTER ("bytes of counter text, dots included")
	 * mutant: src/surface.c:420 `memcpy(s->counter, text, n);` -> `memcpy(s->counter, text, n ? n - 1 : 0);`
	 * input: counters */
	if (W.calls != calls_before + 1 || !W.dev.counter_seen || strlen(W.dev.counter) != n ||
	    memcmp(W.dev.counter, text, n))
		abort();
}

FUZZ_CHECK check_live_colour(const uint8_t *per_strip, size_t n, unsigned calls_before)
{
	size_t want = n < W.g.strips ? n : W.g.strips;
	int bad = W.has_colour ? W.calls != calls_before + 1 || W.dev.ncolour != want ||
				 (want && memcmp(W.dev.colour, per_strip, want))
			       : W.calls != calls_before;
	/* The colours reach the device as written, at most one per strip, and not at all where the
	 * backend has no colour.
	 * contract: surface.h surface_colour; struct surface_backend_ops colour ("may be NULL")
	 * mutant: src/surface.c:434 `s->ncolour = n;` -> `s->ncolour = n ? n - 1 : 0;`
	 * input: colours */
	if (bad)
		abort();
}

FUZZ_CHECK check_blank(unsigned strip)
{
	int bad = W.dev.ring[strip] != 0 || W.dev.ring_code[strip];
	for (unsigned r = 0; r < W.g.scribble_rows; r++)
		for (unsigned i = 0; i < W.g.scribble_chars; i++)
			bad |= W.dev.scr[strip][r][i] != ' ';
	/* A blanked strip shows spaces in every scribble row and an empty ring.
	 * contract: surface.h surface_blank_strip ("its scribble rows, ring and meter blank")
	 * mutant: src/surface.c:448 `surface_scribble(s, strip, r, "");` -> `surface_scribble(s, strip, r, "x");`
	 * input: online-blank */
	if (bad)
		abort();
}

FUZZ_CHECK check_replay(void)
{
	const struct board *w = &W.host.want, *d = &W.dev;
	int led = 0, ring = 0, scr = 0;
	for (int i = 0; i < NKEYS; i++)
		led |= d->led_seen[i] != w->led_seen[i] || (w->led_seen[i] && d->led[i] != w->led[i]);
	for (unsigned k = 0; k < W.g.strips; k++) {
		ring |= d->ring_seen[k] != w->ring_seen[k] ||
			(w->ring_seen[k] && (d->ring_code[k] != w->ring_code[k] || d->ring[k] != w->ring[k]));
		for (unsigned r = 0; r < W.g.scribble_rows; r++)
			scr |= d->scr_seen[k][r] != w->scr_seen[k][r] ||
			       (w->scr_seen[k][r] && memcmp(d->scr[k][r], w->scr[k][r], W.g.scribble_chars));
	}
	size_t want = W.has_colour ? w->ncolour : 0;
	/* After the device comes online it shows every LED the host wrote, in the state it last
	 * wrote, and none the host did not.
	 * contract: surface.h surface_in_online ("replays the board"), "Element writes ... latest
	 * wins, kept in the board shadow"
	 * mutant: src/surface.c:490 `if (s->leds[i].used)` -> `if (s->leds[i].used && i)`
	 * input: replay */
	if (led)
		abort();
	/* Every ring, in the form the host last wrote it: a mask, or the device's own code.
	 * contract: surface.h surface_ring, surface_ring_code ("kept in the board shadow and
	 * replayed as it is")
	 * mutant: src/surface.c:494 `if (st->ring_set && st->ring_is_code)` -> `if (st->ring_set && !st->ring_is_code)`
	 * input: replay */
	if (ring)
		abort();
	/* Every scribble row the host wrote.
	 * contract: surface.h surface_in_online ("replays the board")
	 * mutant: src/surface.c:499 `if (st->scribble_set[r])` -> `if (st->scribble_set[r] && r == 0)`
	 * input: replay */
	if (scr)
		abort();
	/* The counter.
	 * contract: surface.h surface_in_online ("replays the board")
	 * mutant: src/surface.c:502 `if (s->counter_set)` -> `if (0)`
	 * input: replay */
	if (d->counter_seen != w->counter_seen || (w->counter_seen && strcmp(d->counter, w->counter)))
		abort();
	/* The colours, when the backend takes them and the host wrote any.
	 * contract: surface.h surface_in_online ("replays the board")
	 * mutant: src/surface.c:504 `if (s->ncolour && s->ops.colour)` -> `if (0)`
	 * input: replay */
	if (d->ncolour != want || (want && memcmp(d->colour, w->colour, want)))
		abort();
}

static void host_led(uint16_t key, enum surface_led st)
{
	int i = key_index(key);
	if (!W.host.want.led_seen[i])
		W.host.nkeys++;
	W.host.want.led_seen[i] = 1;
	W.host.want.led[i] = (uint8_t)st;
}

/* ---- the program -------------------------------------------------------------------------- */

static unsigned pick_strip(struct fuzz_in *in)
{
	/* one past the geometry's strips is off it */
	return fuzz_u8(in) % (W.g.strips + 2);
}

static void do_led(struct surface *s, struct fuzz_in *in)
{
	uint16_t key = keys[fuzz_u8(in) % NKEYS];
	unsigned st = fuzz_u8(in) % 4;
	int i = key_index(key);
	int is_new = !W.host.want.led_seen[i];
	unsigned before = W.calls, held = W.host.nkeys;
	int rc = surface_led(s, key, (enum surface_led)st);
	check_refusal(rc, st > SURFACE_LED_FLASH);
	check_offline(before);
	if (st > SURFACE_LED_FLASH)
		return;
	check_full(rc, is_new, held);
	if (rc != SPEC_E_FULL)
		host_led(key, (enum surface_led)st);
	if (W.online)
		check_live_led(key, (enum surface_led)st, before, 0, 0);
}

static void do_ring(struct surface *s, struct fuzz_in *in)
{
	unsigned strip = pick_strip(in);
	int mode = fuzz_u8(in) % 7;
	double v = fuzz_double(in);
	unsigned before = W.calls;
	int rc = surface_ring(s, strip, (enum surface_ring_mode)mode, v);
	check_refusal(rc, strip >= W.g.strips);
	check_offline(before);
	if (strip >= W.g.strips)
		return;
	struct board *w = &W.host.want;
	w->ring_seen[strip] = 1;
	w->ring_code[strip] = 0;
	w->ring[strip] = spec_ring(mode, v, W.g.ring_leds);
	if (W.online)
		check_live_ring(strip, mode, v, before);
}

static void do_ring_code(struct surface *s, struct fuzz_in *in)
{
	unsigned strip = pick_strip(in);
	uint32_t code = fuzz_u32(in);
	unsigned before = W.calls;
	int rc = surface_ring_code(s, strip, code);
	check_refusal(rc, strip >= W.g.strips || !W.has_ring_code);
	check_offline(before);
	if (strip >= W.g.strips || !W.has_ring_code)
		return;
	struct board *w = &W.host.want;
	w->ring_seen[strip] = 1;
	w->ring_code[strip] = 1;
	w->ring[strip] = code;
	if (W.online)
		check_live_code(strip, code, before);
}

static void do_scribble(struct surface *s, struct fuzz_in *in)
{
	unsigned strip = pick_strip(in), row = fuzz_u8(in) % (W.g.scribble_rows + 2);
	int null = (fuzz_u8(in) & 15) == 0;
	char *text = fuzz_str(in, 40);
	unsigned before = W.calls;
	int bad = null || strip >= W.g.strips || row >= W.g.scribble_rows;
	int rc = surface_scribble(s, strip, row, null ? NULL : text);
	check_refusal(rc, bad);
	check_offline(before);
	if (!bad) {
		struct board *w = &W.host.want;
		w->scr_seen[strip][row] = 1;
		spec_scribble(w->scr[strip][row], W.g.scribble_chars, text);
		if (W.online)
			check_live_scribble(strip, row, text, before);
	}
	free(text);
}

static void do_counter(struct surface *s, struct fuzz_in *in)
{
	int null = (fuzz_u8(in) & 15) == 0;
	char *text = fuzz_str(in, 48);
	unsigned before = W.calls;
	int rc = surface_counter(s, null ? NULL : text);
	check_refusal(rc, null);
	check_offline(before);
	if (!null) {
		size_t n = strlen(text) < SPEC_MAX_COUNTER - 1 ? strlen(text) : SPEC_MAX_COUNTER - 1;
		memcpy(W.host.want.counter, text, n);
		W.host.want.counter[n] = 0;
		W.host.want.counter_seen = 1;
		if (W.online)
			check_live_counter(text, before);
	}
	free(text);
}

static void do_colour(struct surface *s, struct fuzz_in *in)
{
	uint8_t form = fuzz_u8(in);
	size_t n = fuzz_u8(in) % (SPEC_MAX_STRIPS + 6);
	uint8_t *c = fuzz_alloc(n);
	for (size_t i = 0; i < n; i++)
		c[i] = fuzz_u8(in);
	/* form 0: NULL with n = 0 (valid), form 1: NULL with n > 0 (refused), else the bytes */
	int null = (form & 7) == 0 || (form & 7) == 1;
	if ((form & 7) == 0)
		n = 0;
	else if ((form & 7) == 1 && n == 0)
		n = 1;
	unsigned before = W.calls;
	int rc = surface_colour(s, null ? NULL : c, n);
	check_refusal(rc, null && n > 0);
	check_offline(before);
	if (!(null && n > 0)) {
		size_t kept = n < W.g.strips ? n : W.g.strips;
		W.host.want.ncolour = kept;
		if (kept)
			memcpy(W.host.want.colour, c, kept);
		if (W.online)
			check_live_colour(c, n, before);
	}
	free(c);
}

static void do_blank(struct surface *s, struct fuzz_in *in)
{
	unsigned strip = pick_strip(in);
	unsigned before = W.calls;
	int rc = surface_blank_strip(s, strip);
	check_refusal(rc, strip >= W.g.strips);
	check_offline(before);
	if (strip >= W.g.strips)
		return;
	struct board *w = &W.host.want;
	for (unsigned r = 0; r < W.g.scribble_rows; r++) {
		w->scr_seen[strip][r] = 1;
		spec_scribble(w->scr[strip][r], W.g.scribble_chars, "");
	}
	w->ring_seen[strip] = 1;
	w->ring_code[strip] = 0;
	w->ring[strip] = 0;
	if (W.online)
		check_blank(strip);
}

static void do_online(struct surface *s, struct fuzz_in *in)
{
	(void)in;
	/* the device comes back blank (docs: a new session blanks every scribble, LED, ring) */
	memset(&W.dev, 0, sizeof W.dev);
	surface_in_online(s, 0);
	W.online = 1;
	check_replay();
}

static void do_offline(struct surface *s)
{
	surface_in_offline(s, 0);
	W.online = 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	memset(&W, 0, sizeof W);

	struct surface_config c = { .ops = &ops };
	uint8_t model = fuzz_u8(&in) % 4;
	if (model == 3) {
		struct surface_geometry *g = &c.geometry;
		g->strips = fuzz_u8(&in) % 36;
		g->has_master = fuzz_u8(&in) & 1;
		g->fader_max = fuzz_u16(&in) % 3 ? fuzz_u16(&in) : 0;
		g->meter_sides = fuzz_u8(&in) % 4;
		g->meter_segments = fuzz_u8(&in);
		g->ring_leds = fuzz_u8(&in) % 34;
		g->scribble_rows = fuzz_u8(&in) % 4;
		g->scribble_chars = fuzz_u8(&in) % 19;
	} else {
		surface_geometry_preset(model == 0 ? SURFACE_MODEL_XTOUCH : SURFACE_MODEL_CONTROL24, &c.geometry);
	}
	uint8_t shape = fuzz_u8(&in);
	W.has_ring_code = shape & 1;
	W.has_colour = shape >> 1 & 1;
	struct surface_backend_ops o = ops;
	if (!W.has_ring_code)
		o.ring_code = NULL;
	if (!W.has_colour)
		o.colour = NULL;
	c.ops = &o;
	c.max_leds = shape >> 2 & 15;
	W.max_leds = c.max_leds ? c.max_leds : 256;
	W.g = c.geometry;

	struct surface *s = NULL;
	int rc = surface_create(&s, &c);
	check_create(&c.geometry, rc, s);
	if (!s)
		return 0;
	while (!fuzz_done(&in)) {
		switch (fuzz_u8(&in) % 10) {
		case 0: do_led(s, &in); break;
		case 1: do_ring(s, &in); break;
		case 2: do_ring_code(s, &in); break;
		case 3: do_scribble(s, &in); break;
		case 4: do_counter(s, &in); break;
		case 5: do_colour(s, &in); break;
		case 6: do_blank(s, &in); break;
		case 7: do_online(s, &in); break;
		case 8: do_offline(s); break;
		default: break;
		}
	}
	surface_destroy(s);
	return 0;
}

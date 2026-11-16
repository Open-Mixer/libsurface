// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Writes fuzz/corpus/<harness>/<seed> (make fuzz-corpus). Each seed is a program of its harness
 * (the input form is in the header of fuzz/fuzz_<harness>.c) that reaches one thing the harness
 * checks: the edge of a law, a refusal, a timer. Nothing is fetched and nothing is random; the
 * corpus is committed, and `make fuzz-corpus-check` is red when it is not what this writes.
 * The key table's seed is made from the table itself.
 *
 * Usage: gen-corpus <directory> */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <surface/surface_diginet_keys.h>

struct buf {
	uint8_t b[4096];
	size_t n;
};

static struct buf B;
static const char *dir;

static void u8(unsigned v)
{
	if (B.n >= sizeof B.b) {
		fprintf(stderr, "gen-corpus: seed too long\n");
		exit(1);
	}
	B.b[B.n++] = (uint8_t)v;
}

static void u16(unsigned v)
{
	u8(v & 0xff);
	u8(v >> 8);
}

static void u32(uint32_t v)
{
	u16(v & 0xffff);
	u16(v >> 16);
}

/* fuzz_double(): the edge table, NaN, the infinities, a fraction n/d (negative: odd first byte) */
enum { D_ZERO, D_ONE, D_MINUS_ONE, D_HALF, D_MINUS_HALF, D_QUARTER, D_3QUARTER, D_TWO, D_MINUS_TWO,
       D_HUGE, D_MINUS_HUGE, D_TINY, D_JUST_UNDER, D_JUST_OVER, D_MINUS_JUST_UNDER, D_MINUS_JUST_OVER,
       D_NAN, D_INF, D_MINUS_INF };

static void dbl(int edge)
{
	u8(edge);
}

static void frac(unsigned n, unsigned d, int neg)
{
	u8(neg ? 21 : 20);
	u8(n);
	u8(d - 1);
}

static void str(const char *s)
{
	u8((unsigned)strlen(s));
	for (; *s; s++)
		u8((unsigned char)*s);
}

static void begin(void)
{
	B.n = 0;
}

static void emit(const char *harness, const char *name)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", dir, harness);
	if (mkdir(dir, 0777) && errno != EEXIST) { perror(dir); exit(1); }
	if (mkdir(path, 0777) && errno != EEXIST) { perror(path); exit(1); }
	snprintf(path, sizeof path, "%s/%s/%s", dir, harness, name);
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(B.b, 1, B.n, f) != B.n || fclose(f)) {
		perror(path);
		exit(1);
	}
	begin();
}

/* ---- pure: op 0 meter (steps, prev, fill, hysteresis index), 1 ring (mode, value, leds),
 * 2 preset (model), 3 strerror (code) ---- */

static void p_meter(unsigned steps, unsigned prev, int edge, unsigned h)
{
	u8(0); u8(steps); u8(prev); dbl(edge); u8(h);
}

static void p_meter_f(unsigned steps, unsigned prev, unsigned n, unsigned d, unsigned h)
{
	u8(0); u8(steps); u8(prev); frac(n, d, 0); u8(h);
}

static void p_ring(unsigned mode, int edge, unsigned leds)
{
	u8(1); u8(mode); dbl(edge); u8(leds);
}

static void p_ring_f(unsigned mode, unsigned n, unsigned d, int neg, unsigned leds)
{
	u8(1); u8(mode); frac(n, d, neg); u8(leds);
}

static void gen_pure(void)
{
	/* plain rounding on a half edge, the clamps, NaN, the range, a count that holds */
	p_meter_f(12, 0xff, 1, 24, 0);
	p_meter_f(12, 0xff, 1, 8, 0);
	p_meter(12, 0xff, D_ONE, 0);
	p_meter(12, 0xff, D_TWO, 0);
	p_meter(12, 0xff, D_MINUS_ONE, 0);
	p_meter(12, 0xff, D_NAN, 0);
	p_meter(12, 0xff, D_INF, 0);
	p_meter(0, 0xff, D_HALF, 0);
	p_meter(12, 0, D_NAN, 1);
	p_meter(12, 7, D_NAN, 1);
	p_meter(12, 0, D_NAN, 0);
	p_meter(12, 1, D_QUARTER, 1);
	emit("pure", "edges");
	/* the previous count holds exactly up to 0.5 + hysteresis, and rounds past it */
	p_meter_f(4, 0, 3, 16, 1);
	p_meter_f(4, 0, 7, 32, 1);
	p_meter_f(4, 1, 5, 16, 1);
	p_meter_f(8, 3, 1, 2, 0);
	p_meter_f(8, 3, 9, 16, 2);
	p_meter_f(64, 20, 1, 3, 3);
	emit("pure", "hold-edge");
	/* every mode at its centre, its ends and its edges, the widths 1, 2, 11, 15, 31 and the refused */
	for (unsigned mode = 0; mode <= 7; mode++) {
		static const unsigned leds[] = { 0, 1, 2, 11, 15, 31, 32, 35 };
		for (unsigned l = 0; l < 8; l++) {
			p_ring(mode, D_ZERO, leds[l]);
			p_ring(mode, D_ONE, leds[l]);
			p_ring(mode, D_MINUS_ONE, leds[l]);
			p_ring(mode, D_HALF, leds[l]);
			p_ring_f(mode, 1, 22, 0, leds[l]);
			p_ring_f(mode, 3, 22, 1, leds[l]);
		}
		p_ring(mode, D_NAN, 11);
		p_ring(mode, D_HUGE, 15);
		p_ring(mode, D_MINUS_HUGE, 15);
		p_ring(mode, D_INF, 15);
	}
	emit("pure", "ring-edges");
	for (unsigned m = 0; m < 4; m++) {
		u8(2); u8(m);
	}
	emit("pure", "presets");
	for (int c = -6; c <= 4; c++) {
		u8(3); u8((unsigned)c & 0xff);
	}
	emit("pure", "strerror");
}

/* ---- keys ---- */

static void gen_keys(void)
{
	/* every entry of the table, as a decoded key on strips 0 and 23 (a section key on its zone),
	 * then the misses: the ids between the strips and the zones, a button past the table, bit 15 */
	for (size_t i = 0; i < surface_c24_nkeys; i++) {
		const struct surface_c24_key *k = &surface_c24_keys[i];
		if (k->zone) {
			u16(SURFACE_DIGINET_KEY(k->button, k->zone));
		} else {
			u16(SURFACE_DIGINET_KEY(k->button, 0));
			u16(SURFACE_DIGINET_KEY(k->button, 23));
		}
		if (B.n > sizeof B.b - 16)
			break;
	}
	emit("keys", "table");
	begin();
	u16(0x0017); u16(0x011e); u16(0x7f18); u16(0x7f00); u16(0x0100 | 0x1d); u16(0x0100 | 0x1e);
	u16(0x8001); u16(0x8118); u16(0xffff); u16(0x0000); u16(0x0118); u16(0x0019);
	emit("keys", "misses");
	for (size_t i = 0; i < surface_c24_nkeys; i++) {
		const struct surface_c24_key *k = &surface_c24_keys[i];
		u16(0x8000 | SURFACE_DIGINET_KEY(k->button, k->zone ? k->zone : 5));
	}
	emit("keys", "bit15");
	begin();
	u16(0x7f00);
	emit("keys", "unknown");
	begin();
	u16(0);
	emit("keys", "keys");
}

/* ---- board: model, shape, then ops: 0 led (key idx, st), 1 ring (strip, mode, v), 2 ring_code
 * (strip, code), 3 scribble (strip, row, null, text), 4 counter (null, text), 5 colour (form, n, bytes),
 * 6 blank (strip), 7 online, 8 offline ---- */

enum { XT = 0, C24 = 1, CUSTOM = 3 };

static void b_head(unsigned model, unsigned shape)
{
	u8(model); u8(shape);
}

static void b_led(unsigned idx, unsigned st) { u8(0); u8(idx); u8(st); }
static void b_ring(unsigned strip, unsigned mode, int edge) { u8(1); u8(strip); u8(mode); dbl(edge); }
static void b_code(unsigned strip, uint32_t code) { u8(2); u8(strip); u32(code); }
static void b_scr(unsigned strip, unsigned row, unsigned nul, const char *t) { u8(3); u8(strip); u8(row); u8(nul); str(t); }
static void b_counter(unsigned nul, const char *t) { u8(4); u8(nul); str(t); }
static void b_colour(unsigned form, unsigned n)
{
	u8(5); u8(form); u8(n);
	for (unsigned i = 0; i < n; i++)
		u8(1 + i);
}
static void b_blank(unsigned strip) { u8(6); u8(strip); }
static void b_online(void) { u8(7); }
static void b_offline(void) { u8(8); }

static void gen_board(void)
{
	/* custom geometries: each refusal and the edges that are made */
	static const struct { unsigned strips, master, fmax, sides, seg, ring, rows, chars; } geo[] = {
		{ 0, 0, 1, 1, 12, 11, 1, 4 },    /* no strips */
		{ 33, 0, 1, 1, 12, 11, 1, 4 },   /* too many */
		{ 32, 1, 1, 2, 255, 31, 2, 16 }, /* the largest, made */
		{ 1, 0, 1, 0, 0, 0, 0, 0 },      /* the smallest, made */
		{ 4, 0, 0, 1, 12, 11, 1, 4 },    /* no fader range */
		{ 4, 0, 1, 3, 12, 11, 1, 4 },    /* three sides */
		{ 4, 0, 1, 1, 12, 32, 1, 4 },    /* 32 LEDs */
		{ 4, 0, 1, 1, 12, 11, 3, 4 },    /* three rows */
		{ 4, 0, 1, 1, 12, 11, 1, 17 },   /* 17 characters */
	};
	/* one seed each, the first named */
	for (size_t i = 0; i < sizeof geo / sizeof geo[0]; i++) {
		char name[32];
		u8(CUSTOM);
		u8(geo[i].strips); u8(geo[i].master);
		if (geo[i].fmax)
			u16(1), u16(geo[i].fmax);
		else
			u16(3);
		u8(geo[i].sides); u8(geo[i].seg); u8(geo[i].ring); u8(geo[i].rows); u8(geo[i].chars);
		u8(0);   /* the shape: a backend with neither a ring code nor colours */
		b_online();
		b_led(1, 1);
		b_ring(0, 1, D_HALF);
		b_scr(0, 0, 1, "ab");
		b_blank(0);
		b_online();
		snprintf(name, sizeof name, "geometry-%zu", i);
		emit("board", i == 0 ? "create-refusals" : name);
	}
	/* a blanked strip, online, on both presets, with a meter and a master */
	for (unsigned m = 0; m < 2; m++) {
		b_head(m, 0);
		b_online();
		b_scr(0, 0, 1, "abcdefgh");
		b_ring(0, 3, D_HALF);
		b_blank(0);
		b_blank(7);
		b_blank(m ? 23 : 8);
		b_blank(m ? 24 : 9);
		b_online();
		b_blank(2);
		emit("board", m ? "online-blank-c24" : "online-blank");
	}
	/* the refusals: a strip off the geometry, an LED state past FLASH, a NULL text, a NULL list */
	b_head(XT, 3);
	b_led(0, 3);
	b_led(0, 2);
	b_ring(8, 1, D_HALF);
	b_ring(7, 1, D_HALF);
	b_code(7, 5);
	b_code(8, 5);
	b_scr(8, 0, 1, "x");
	b_scr(0, 2, 1, "x");
	b_scr(0, 1, 1, "x");
	b_scr(0, 0, 0, "x");
	b_counter(0, "x");
	b_colour(1, 3);
	b_colour(0, 3);
	b_blank(8);
	b_blank(7);
	b_online();
	b_led(0, 3);
	b_ring(8, 1, D_HALF);
	b_ring(7, 1, D_HALF);
	b_scr(8, 0, 1, "x");
	b_scr(0, 2, 1, "x");
	b_counter(0, "x");
	b_colour(1, 3);
	b_blank(8);
	b_head(C24, 0);   /* a second program: a backend with no ring code */
	emit("board", "refusals");
	/* writes while offline reach nothing, and are all replayed */
	b_head(XT, 3);
	b_led(1, 1); b_led(2, 2);
	b_ring(0, 1, D_HALF);
	b_code(1, 0x1234);
	b_scr(0, 0, 1, "hi");
	b_counter(1, "12:34");
	b_colour(2, 4);
	b_blank(3);
	b_online();
	b_offline();
	b_led(1, 0); b_ring(0, 2, D_ONE); b_scr(0, 1, 1, "x"); b_counter(1, "9"); b_colour(2, 2);
	b_online();
	emit("board", "offline-writes");
	/* the LED table of 1, 2 and 3 entries full: a new key refused, a held key rewritten */
	for (unsigned cap = 1; cap <= 3; cap++) {
		b_head(XT, 1 | (cap << 2));
		for (unsigned k = 0; k < 6; k++)
			b_led(k, 1);
		b_online();
		for (unsigned k = 0; k < 6; k++)
			b_led(k, 2);
		b_offline();
		b_led(5, 1);
		b_online();
		emit("board", cap == 1 ? "led-full" : cap == 2 ? "led-full-2" : "led-full-3");
	}
	b_head(C24, 0);
	b_online();
	for (unsigned k = 0; k < 24; k++)
		b_led(k, k % 3);
	b_online();
	emit("board", "leds");
	/* every ring mode, online and not */
	b_head(XT, 0);
	for (unsigned online = 0; online < 2; online++) {
		for (unsigned mode = 0; mode < 7; mode++) {
			b_ring(mode, mode, D_HALF);
			b_ring(mode, mode, D_ONE);
			b_ring(mode, mode, D_MINUS_ONE);
			b_ring(mode, mode, D_NAN);
		}
		b_online();
	}
	b_head(C24, 0);
	emit("board", "ring-modes");
	b_head(XT, 0);
	b_online();
	b_ring(3, 3, D_ONE);
	emit("board", "ring-full");
	b_head(XT, 1);
	b_online();
	b_code(0, 0xdeadbeef); b_code(7, 0x5a); b_ring(1, 1, D_HALF); b_code(1, 7);
	b_offline(); b_online();
	emit("board", "ring-code");
	/* text: ASCII, a multi-byte character, a control character, a lone continuation, long, empty */
	b_head(C24, 0);
	b_online();
	b_scr(0, 0, 1, "");
	b_scr(0, 0, 1, "ab");
	b_scr(0, 0, 1, "abcdefgh");
	b_scr(1, 0, 1, "a\xc3\xa9z");
	b_scr(1, 0, 1, "\x80\x80z");
	b_scr(2, 0, 1, "a\tb\x7f");
	b_scr(3, 0, 1, "\xf0\x9f\x98\x80x");
	b_scr(4, 0, 1, "\xc0\xbf\xff");
	b_online();
	b_head(XT, 0);
	emit("board", "scribbles");
	b_head(XT, 0);
	b_online();
	b_scr(0, 0, 1, "abcdefghijklmnop");
	b_scr(0, 1, 1, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");
	b_scr(7, 1, 1, "\x01\x02");
	b_offline(); b_online();
	emit("board", "scribbles-xtouch");
	b_head(XT, 0);
	b_online();
	b_counter(1, "");
	b_counter(1, "12:34:56:12");
	b_counter(1, "0123456789012345678901234567890");
	b_counter(1, "01234567890123456789012345678901");
	b_counter(1, "012345678901234567890123456789012345");
	b_offline(); b_online();
	emit("board", "counters");
	b_head(XT, 2);
	b_online();
	b_colour(2, 0);
	b_colour(2, 3);
	b_colour(2, 8);
	b_colour(2, 9);
	b_colour(2, 30);
	b_colour(0, 0);
	b_colour(1, 2);
	b_offline(); b_colour(2, 5); b_online();
	b_head(C24, 0);
	b_head(XT, 0);   /* a backend with no colour: nothing is sent */
	emit("board", "colours");
	/* the replay of everything written: online first and then back */
	b_head(XT, 3);
	b_led(3, 1); b_led(9, 2); b_led(3, 0);
	b_ring(1, 4, D_QUARTER); b_ring(2, 5, D_3QUARTER); b_code(3, 0x81); b_code(2, 3);
	b_scr(0, 0, 1, "ch 1"); b_scr(0, 1, 1, "vol"); b_scr(5, 0, 1, "\xc3\xa9");
	b_counter(1, "01:02:03:04");
	b_colour(2, 8);
	b_blank(4);
	b_online();
	b_led(3, 2);
	b_offline();
	b_online();
	b_head(XT, 3);
	emit("board", "replay");
	b_head(C24, 1);
	b_led(1, 1); b_ring(0, 2, D_ONE); b_scr(0, 0, 1, "hey"); b_counter(1, "1"); b_colour(2, 24);
	b_online(); b_led(2, 1); b_online();
	emit("board", "replay-c24");
}

/* ---- bank: model (0 X-Touch, 1 Control|24, 2 custom: strips, master, sides), cap, then ops: 0 set (n,
 * null flag, n target bytes), 1 step (b), 2 show (t), 3 jump (t), 4 master (t), 5 query (t) ---- */

static void k_head(unsigned model, unsigned cap) { u8(model); u8(cap); }
static void k_set(unsigned n, unsigned nul)
{
	u8(0); u8(n); u8(nul);
	for (unsigned i = 0; i < n; i++)
		u8(1 + i % 8);
}
static void k_step(unsigned b) { u8(1); u8(b); }
static void k_show(unsigned t) { u8(2); u8(t); }
static void k_jump(unsigned t) { u8(3); u8(t); }
static void k_master(unsigned t) { u8(4); u8(t); }
static void k_query(unsigned t) { u8(5); u8(t); }

static void gen_bank(void)
{
	/* a null list with a length is refused; the table full */
	k_head(0, 0);
	k_set(5, 0);
	k_set(0, 0);
	k_set(0, 1);
	k_set(3, 1);
	k_query(1);
	emit("bank", "set-refusal");
	k_head(0, 3);
	k_set(10, 1);
	k_set(3, 1);
	k_set(4, 1);
	k_set(1, 1);
	k_head(1, 0);
	emit("bank", "set-long");
	/* paging by a bank and by one, the extremes of a delta, the clamp at both ends */
	k_head(0, 0);
	k_set(43, 1);
	k_step(0); k_step(3); k_step(1); k_step(1); k_step(3);
	k_step(4); k_step(4); k_step(4); k_step(4); k_step(4); k_step(4); k_step(1);
	k_step(10); k_step(11); k_step(10); k_step(11); k_step(2); k_step(2); k_step(0xff); k_step(0x80);
	k_step(5); k_step(6); k_step(7); k_step(8); k_step(9);
	k_set(12, 1);
	k_step(3);
	k_step(4);
	k_jump(8); k_step(0); k_step(1);
	k_head(1, 0);
	emit("bank", "steps");
	/* repeats and zeros in the list: the first strip that shows a target, a target off the window */
	k_head(2, 0);
	u8(5); u8(1); u8(0);
	u8(0); u8(12); u8(1);
	for (unsigned i = 0; i < 12; i++)
		u8(i % 2 ? 0 : 1);    /* targets 1 and 2 alternating */
	for (unsigned t = 0; t < 9; t++) {
		k_query(t); k_show(t); k_jump(t); k_query(t);
	}
	emit("bank", "repeats");
	/* a master fader, and none */
	for (unsigned m = 0; m < 2; m++) {
		k_head(m ? 1 : 0, 0);
		k_set(6, 1);
		for (unsigned t = 0; t < 9; t++) {
			k_master(t); k_query(t);
		}
		k_jump(5); k_show(0); k_show(8);
		emit("bank", m ? "master-c24" : "master");
	}
	/* show and jump, on and off the window, near the end of the list (the window overhangs) */
	k_head(2, 0);
	u8(3); u8(0); u8(1);
	k_set(12, 1);
	k_show(3); k_show(1); k_show(8); k_show(5);
	k_jump(5); k_jump(5); k_jump(8); k_jump(0); k_jump(1);
	k_show(2); k_step(0x01); k_show(7); k_set(7, 1);
	emit("bank", "show-jump");
	/* meters: a level written to every strip and side, then a move that clears them */
	k_head(0, 0);
	k_set(30, 1);
	k_step(3); k_step(2); k_step(2); k_step(4); k_step(4); k_step(4); k_step(4);
	k_show(8); k_jump(2); k_jump(2); k_set(30, 1);
	emit("bank", "meters");
	k_head(2, 0);
	u8(24); u8(0); u8(2);
	k_set(40, 1);
	k_step(3); k_step(4); k_jump(7); k_show(1); k_show(8);
	emit("bank", "meters-c24");
}

/* ---- motor: shape (bit 0: Control|24; bits 1..3: the table), start (u16), then each op and a
 * step byte (b >> 2 units; b & 3: 0 us, 1 ms, 2 s, 3 100 ms). Ops: 0 fader (strip, raw u16),
 * 3 touch (strip, on), 5 served (t, v, form), 7 refused (t), 8 paint (t, v), 9 paint strip (strip,
 * v), 10 suspend (t, on), 11 strip fader (strip, v), 12 note commanded (strip, raw), 13 is report
 * (strip, raw), 14 offline, 48 online, 15 bank (form...), 33 passthrough (form...), 67 toggle
 * the backend, 16 tick, 135 run to the deadline ---- */

static void m_head(unsigned c24, unsigned table, unsigned start)
{
	u8(c24 | (table << 1)); u16(start);
}
static void m_step(unsigned units, unsigned unit) { u8((units << 2) | unit); }
static void m_ms(unsigned ms) { m_step(ms > 63 ? 63 : ms, 1); }
static void m_fader(unsigned strip, unsigned raw) { u8(0); u8(strip); u16(raw); m_ms(1); }
static void m_touch(unsigned strip, unsigned on) { u8(3); u8(strip); u8(on); m_ms(1); }
static void m_served(unsigned t, int edge, unsigned form) { u8(5); u8(t); dbl(edge); u8(form); m_ms(1); }
static void m_refused(unsigned t) { u8(7); u8(t); m_ms(1); }
static void m_paint(unsigned t, int edge) { u8(8); u8(t); dbl(edge); m_ms(1); }
static void m_paint_strip(unsigned strip, int edge) { u8(9); u8(strip); dbl(edge); m_ms(1); }
static void m_suspend(unsigned t, unsigned on) { u8(10); u8(t); u8(on); m_ms(1); }
static void m_strip_fader(unsigned strip, int edge) { u8(11); u8(strip); dbl(edge); m_ms(1); }
static void m_note(unsigned strip, unsigned raw) { u8(12); u8(strip); u16(raw); m_ms(1); }
static void m_isreport(unsigned strip, unsigned raw) { u8(13); u8(strip); u16(raw); m_ms(1); }
static void m_offline(void) { u8(14); m_ms(1); }
static void m_online(void) { u8(48); m_ms(1); }
static void m_bank_set(unsigned n) { u8(15); u8(1); u8(n); for (unsigned i = 0; i < n; i++) u8(i); m_ms(1); }
static void m_bank_step(int d) { u8(15); u8(2); u8((unsigned)d & 0xff); m_ms(1); }
static void m_master(unsigned t) { u8(15); u8(4); u8(t); m_ms(1); }
static void m_toggle(void) { u8(67); m_ms(1); }
static void m_tick(unsigned after_ms) { u8(16); m_ms(after_ms); }
static void m_deadline(unsigned after_ms) { u8(135); m_ms(after_ms); }
static void m_key(unsigned key, unsigned pressed) { u8(33); u8(0); u16(key); u8(pressed); m_ms(1); }
static void m_vpot(unsigned strip, unsigned delta) { u8(33); u8(1); u8(strip); u16(delta); m_ms(1); }
static void m_jog(unsigned delta) { u8(33); u8(2); u16(delta); m_ms(1); }
static void m_warn(unsigned code) { u8(33); u8(3); u8(code); m_ms(1); }

static void gen_motor(void)
{
	/* the hand: a cap written under a touch (X-Touch: nothing; and after the release, the drive) */
	m_head(0, 0, 0);
	m_online();
	m_strip_fader(0, D_HALF);
	m_touch(0, 1);
	m_strip_fader(0, D_HALF);
	m_strip_fader(8, D_HALF);
	m_served(1, D_QUARTER, 0);
	m_served(1, D_3QUARTER, 0);
	m_paint(1, D_HALF);
	m_paint_strip(0, D_QUARTER);
	m_fader(0, 9000);
	m_tick(100);
	m_deadline(1); m_deadline(1); m_deadline(1);
	m_touch(0, 0);
	m_deadline(1);
	m_fader(3, 100);
	m_served(4, D_ONE, 0);
	m_deadline(1); m_deadline(1);
	emit("motor", "hand");
	/* the Control|24 holds its own set-point: the hand's position is echoed, the release echoes once */
	m_head(1, 0, 0);
	m_online();
	m_touch(0, 1);
	m_fader(0, 500);
	m_fader(0, 600);
	m_fader(0, 2000);
	m_touch(0, 0);
	m_deadline(1); m_deadline(1);
	m_fader(0, 100);
	m_touch(3, 1); m_fader(3, 1023); m_touch(3, 1); m_touch(3, 0); m_touch(3, 1); m_deadline(1); m_deadline(1);
	emit("motor", "c24-hand");
	/* a device that is offline is written nothing, nor is a surface never online */
	m_head(0, 0, 0);
	m_strip_fader(0, D_HALF);
	m_served(1, D_HALF, 0);
	m_paint(1, D_ONE);
	m_fader(0, 100);
	m_bank_set(4);
	m_served(1, D_QUARTER, 0);
	m_deadline(1); m_deadline(1);
	m_online();
	m_offline();
	m_strip_fader(1, D_HALF);
	m_served(2, D_3QUARTER, 0);
	m_deadline(1); m_deadline(1);
	m_online();
	emit("motor", "offline");
	/* raw at, over and far over the fader's range */
	for (unsigned c = 0; c < 2; c++) {
		unsigned max = c ? 1023 : 16383;
		m_head(c, 0, 0);
		m_online();
		m_fader(0, max); m_fader(0, max + 1); m_fader(0, max + 5); m_fader(0, max + 39);
		m_fader(0, 0); m_fader(8, max + 1); m_fader(24, max); m_fader(31, 5);
		m_served(1, D_ONE, 0); m_served(1, D_ZERO, 0); m_strip_fader(0, D_HUGE); m_strip_fader(0, D_MINUS_HUGE);
		m_strip_fader(0, D_NAN); m_strip_fader(1, D_INF); m_strip_fader(40, D_HALF);
		emit("motor", c ? "report-edges-c24" : "report-edges");
	}
	/* the touch: on, repeated, a dropout, off, lapse; the sensor repeating itself */
	m_head(0, 0, 0);
	m_online();
	m_touch(0, 1); m_touch(0, 1); m_touch(0, 0); m_touch(0, 0);
	m_touch(1, 1); m_tick(100); m_tick(150); m_tick(100); m_deadline(1); m_touch(1, 1); m_touch(1, 1);
	m_touch(2, 0); m_touch(8, 1); m_touch(9, 1); m_touch(8, 0);
	m_online(); m_touch(3, 1); m_online(); m_touch(3, 0); m_offline(); m_touch(3, 1);
	m_tick(10); m_deadline(1);
	emit("motor", "touches");
	/* the debounce on the Control|24: a release that stands, one a touch cancels */
	m_head(1, 0, 0);
	m_online();
	m_touch(0, 1); m_touch(0, 0); m_touch(0, 0); m_deadline(1);
	m_touch(0, 1); m_touch(0, 0); m_tick(100); m_touch(0, 1); m_deadline(1); m_deadline(1);
	m_touch(1, 1); m_touch(1, 0); m_tick(249); m_tick(1); m_tick(1);
	m_touch(2, 1); m_touch(2, 0); m_online(); m_deadline(1);
	m_touch(5, 0); m_touch(5, 1); m_touch(5, 1); m_touch(5, 0); m_touch(5, 0); m_deadline(1); m_deadline(1);
	emit("motor", "c24-touches");
	/* a release says where the hand left the cap: after a report, at once and on a lapsed hold */
	m_head(0, 0, 0);
	m_online();
	m_touch(0, 1); m_fader(0, 4000); m_touch(0, 0);
	m_touch(1, 1); m_fader(1, 12000); m_deadline(1); m_deadline(1);
	emit("motor", "release-position");
	m_head(1, 0, 0);
	m_online();
	m_touch(0, 1); m_fader(0, 300); m_touch(0, 0); m_deadline(1); m_deadline(1);
	emit("motor", "release-position-c24");
	/* the motor's report: a frame at where a write put the cap is not a gesture */
	m_head(0, 0, 0);
	m_online();
	m_isreport(0, 0);
	m_strip_fader(0, D_HALF);
	m_isreport(0, 8192); m_isreport(0, 8192 + 16); m_isreport(0, 8192 + 17); m_isreport(0, 8192 - 16);
	m_fader(0, 8192); m_fader(0, 8192 + 16); m_fader(0, 8192 + 17); m_fader(0, 8192 + 100);
	m_touch(0, 1); m_isreport(0, 8192 + 100); m_fader(0, 8192 + 100); m_touch(0, 0);
	m_note(0, 5000); m_fader(0, 5000); m_isreport(0, 5000); m_isreport(0, 5017); m_isreport(9, 5000);
	m_toggle(); m_strip_fader(1, D_ONE); m_fader(1, 16383); m_toggle(); m_isreport(1, 16383);
	m_head(1, 0, 0);
	m_online();
	m_strip_fader(0, D_HALF); m_isreport(0, 512); m_fader(0, 512); m_fader(0, 513); m_fader(0, 514); m_fader(0, 515);
	emit("motor", "report");
	/* passthrough: keys, V-Pots (on and off the bank), jog, warning */
	m_head(0, 0, 0);
	m_online();
	m_bank_set(6);
	m_key(0x0101, 1); m_key(0x0101, 0); m_key(0x0101, 2); m_key(0xffff, 255); m_key(0x8001, 128);
	m_vpot(0, 3); m_vpot(2, 0xfffd); m_vpot(7, 100); m_vpot(8, 1); m_vpot(9, 1);
	m_jog(5); m_jog(0xfff0); m_jog(0x7fff); m_jog(0x8000);
	m_warn(1); m_warn(0xff); m_warn(0);
	emit("motor", "passthrough");
	/* the clock: nothing waits; a flush waits 80 ms; a hold lapses; a tick early does nothing */
	m_head(0, 0, 0);
	m_online();
	m_deadline(1);
	m_served(1, D_HALF, 0);
	m_served(1, D_QUARTER, 0);
	m_served(1, D_3QUARTER, 0);
	m_tick(10); m_tick(10); m_deadline(1); m_deadline(1);
	m_touch(1, 1); m_served(2, D_HALF, 0); m_served(2, D_ONE, 0);
	m_tick(10); m_deadline(1); m_deadline(1); m_deadline(1); m_deadline(1);
	m_fader(2, 3000); m_served(3, D_HALF, 0); m_served(3, D_ZERO, 0); m_tick(1); m_tick(10);
	m_deadline(1); m_deadline(1); m_deadline(1); m_deadline(1); m_deadline(1); m_deadline(1);
	m_head(0, 0, 0);
	emit("motor", "clock");
	m_head(1, 0, 0);
	m_online();
	m_touch(0, 1); m_touch(0, 0);
	for (unsigned i = 0; i < 8; i++)
		m_tick(63);
	m_deadline(1); m_deadline(1);
	m_touch(1, 1); m_touch(1, 0); m_deadline(1); m_deadline(1);
	m_served(1, D_HALF, 0); m_served(1, D_ONE, 0); m_fader(0, 100); m_deadline(1); m_deadline(1); m_deadline(1);
	emit("motor", "clock-c24");
	/* a strip off the geometry is refused */
	m_head(0, 0, 0);
	m_online();
	m_strip_fader(0, D_HALF); m_strip_fader(8, D_HALF); m_strip_fader(9, D_HALF); m_strip_fader(10, D_HALF);
	m_head(1, 0, 0);
	emit("motor", "refusal");
	/* the host: served frames old and new, refused, repainted, suspended, with a table of 1 to 6
	 * motor states and the targets banked past it */
	for (unsigned table = 0; table < 7; table += 3) {
		m_head(table & 1, table, 1000);
		m_online();
		m_bank_set(8);
		m_fader(0, 2000); m_served(1, D_HALF, 0); m_served(1, D_QUARTER, 1 | (3 << 1)); m_served(1, D_QUARTER, 1 | (100 << 1) | 0x80);
		m_refused(1); m_served(1, D_ONE, 0); m_deadline(1); m_deadline(1); m_deadline(1);
		m_paint(1, D_HALF); m_paint(2, D_ONE); m_paint(0, D_HALF); m_paint_strip(0, D_ZERO); m_paint_strip(9, D_ONE);
		m_suspend(1, 1); m_served(1, D_ZERO, 0); m_fader(0, 100); m_suspend(1, 0); m_suspend(2, 1);
		m_bank_step(8); m_bank_step(-8); m_master(9); m_master(0); m_bank_set(0);
		for (unsigned t = 1; t < 14; t++)
			m_served(t, D_HALF, 0);
		m_served(0, D_HALF, 0); m_served(14, D_HALF, 0);
		m_note(2, 100); m_note(30, 1);
		m_online(); m_deadline(1); m_deadline(1);
		char name[16];
		snprintf(name, sizeof name, "host-%u", table);
		emit("motor", name);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: gen-corpus <directory>\n");
		return 2;
	}
	dir = argv[1];
	gen_pure();
	gen_keys();
	gen_board();
	gen_bank();
	gen_motor();
	return 0;
}

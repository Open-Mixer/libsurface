// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: THE MOTOR POLICY, THE TOUCH AND THE TIMERS (src/motor.c, and the inputs, the
 * touch and the clock in src/surface.c): a fader report from the device, a touch, a served value
 * from the host, a repaint, a refusal, a tick, the device going offline and online.
 *
 * Input: a surface (the X-Touch or the Control|24, with a table of 1 to 6 motor states or the
 * default), a list of targets, then a program, one operation after another until the input
 * ends. A position the host serves or paints is within 0..1, as surface.h says; a cap written
 * outside the policy (surface_strip_fader) is given any number. The clock is the program's: it starts where the input says, and every operation, then
 * every run to a deadline, moves it on by a step the input chooses. It never goes back.
 *
 * The backend answers every write; the harness keeps, from surface.h's words, what a hand holds
 * (a touch lasts SURFACE_TOUCH_TIMEOUT_MS where the geometry says so, a release on the Control|24
 * waits its debounce for a touch-on to cancel it), the position it last saw written to each cap,
 * and what each input call owes the host. Properties, each an abort() in a check_ function:
 *   - hand: the motor never writes a cap a hand holds. On the X-Touch no write at all; on the
 *     Control|24, which holds its own set-point, only the position the hand just reported.
 *   - offline: a device that is offline gets no write.
 *   - range: a write is within the fader's range, at a strip that exists.
 *   - touch: a touch-on that finds no hand holding the cap says so (SURFACE_EV_FADER_TOUCHED),
 *     once; a release says so (SURFACE_EV_FADER_RELEASED) once when it takes effect: at once
 *     on the X-Touch, after the debounce on the Control|24, on the tick that finds a held flag
 *     lapsed; never twice for one touch.
 *   - move: a fader report is one SURFACE_EV_FADER_MOVED at raw / fader_max (a raw past the range
 *     is the range's end), unless it is the motor reporting where it put the cap: no hand on the
 *     cap, a device that reports its motor, a frame within the tolerance of the last write; and
 *     surface_fader_is_report() says the same.
 *   - release position: a release of a strip that had a report is at the last reported raw /
 *     fader_max, where the hand left the cap.
 *   - passthrough: a key, a V-Pot, a jog and a warning reach the host once, as they came, the
 *     V-Pot only for a strip of the bank, with the target the harness's own bank window shows.
 *   - clock: surface_deadline() is never in the past and is UINT64_MAX when nothing waits (no
 *     hand, no pending release, no flush that a served value, a paint or a report could have
 *     armed); a tick before it does nothing; a tick at it leaves nothing due.
 */
#include <surface/surface.h>

#include "fuzz.h"

#define NS (SPEC_MAX_STRIPS + 1)
#define MS_US 1000ull

struct tcount { unsigned touched[NS], released[NS], moved, online, offline; };

static struct {
	struct surface_geometry g;
	unsigned nstrips;
	uint64_t now;
	int online, accept;
	/* the hand, as the harness holds it */
	uint8_t flag[NS], pending[NS];
	uint64_t until[NS], release_at[NS];
	uint16_t last_raw[NS];
	uint8_t have_report[NS];   /* a fader report reached this strip */
	/* the bank, as the harness holds it: the list, the window's offset, the master */
	unsigned cap;
	surface_target list[16];
	size_t nlist, offset;
	surface_target master;
	/* a served value, a paint or a fader report was ever given: a trailing flush may be armed */
	int flushable;
	/* the last position written to each cap */
	uint8_t cmd_set[NS];
	uint16_t cmd[NS];
	unsigned writes;
	/* events */
	struct tcount obs;
	struct surface_event last;
	unsigned nevents;
} W;

static struct surface *S;

static int held(unsigned k, uint64_t now)
{
	return W.flag[k] && (!W.g.touch_hold_ms || now < W.until[k]);
}

FUZZ_CHECK check_hand(unsigned strip, uint16_t raw)
{
	/* The motor writes no cap a hand holds. The Control|24 holds its own set-point and is
	 * written the position the hand reports, nothing else.
	 * contract: surface.h "never fight the hand", struct surface_geometry holds_setpoint ("the
	 * motor holds its own set-point: echo the hand (C24)")
	 * mutant: src/motor.c:114 `!s->online || touch_held(s, strip, now_us)` -> `!s->online || touch_held(s, strip, now_us + 1000000)`
	 * input: hand
	 * mutant: src/surface.c:163 `s->ops.fader(s->backend, strip, raw) > 0` -> `s->ops.fader(s->backend, strip, (uint16_t)(raw ^ 1)) > 0`
	 * input: c24-hand */
	if (held(strip, W.now) && (!W.g.holds_setpoint || raw != W.last_raw[strip]))
		abort();
}

FUZZ_CHECK check_offline(void)
{
	/* A device that is offline is written nothing.
	 * contract: surface.h surface_in_online ("device ready, board replayed"), SURFACE_EV_OFFLINE
	 * ("session or port lost; device still present")
	 * mutant: src/motor.c:114 `!s->online ||` -> ``
	 * input: offline */
	if (!W.online)
		abort();
}

FUZZ_CHECK check_range(unsigned strip, uint16_t raw)
{
	/* A write is within the fader's range, at a strip or the master.
	 * contract: surface.h struct surface_geometry strips, has_master, fader_max
	 * mutant: src/motor.c:73 `return s->g.fader_max;` -> `return s->g.fader_max + 1;`
	 * input: report-edges */
	if (strip >= W.nstrips || raw > W.g.fader_max)
		abort();
}

static int op_fader(void *b, unsigned strip, uint16_t raw)
{
	(void)b;
	check_range(strip, raw);
	check_offline();
	check_hand(strip, raw);
	W.writes++;
	if (!W.accept)
		return 0;
	W.cmd_set[strip] = 1;
	W.cmd[strip] = raw;
	return 1;
}

static int op_led(void *b, uint16_t k, enum surface_led st) { (void)b; (void)k; (void)st; return 1; }
static int op_ring(void *b, unsigned s, uint32_t m) { (void)b; (void)s; (void)m; return 1; }
static int op_meter(void *b, unsigned s, unsigned side, unsigned seg) { (void)b; (void)s; (void)side; (void)seg; return 1; }
static int op_scribble(void *b, unsigned s, unsigned r, const char *t, size_t n) { (void)b; (void)s; (void)r; (void)t; (void)n; return 1; }
static int op_counter(void *b, const char *t) { (void)b; (void)t; return 1; }

static const struct surface_backend_ops ops = {
	.fader = op_fader, .led = op_led, .ring = op_ring, .meter = op_meter,
	.scribble = op_scribble, .counter = op_counter,
};

FUZZ_CHECK check_event_position(const struct surface_event *ev)
{
	/* A fader report and a release say where the cap is, 0..1; a move is raw / fader_max, from the
	 * raw do_fader reported, held to the range.
	 * contract: surface.h SURFACE_EV_FADER_MOVED ("position 0..1"), SURFACE_EV_FADER_RELEASED
	 * ("position: where the hand left it")
	 * mutant: src/surface.c:267 `if (raw > s->g.fader_max)` -> `if (raw > s->g.fader_max + 1)`
	 * input: report-edges
	 * mutant: src/surface.c:283 `.position = pos,` -> `.position = 1 - pos,`
	 * input: report-edges */
	if (!(ev->position >= 0 && ev->position <= 1) || ev->strip >= W.nstrips ||
	    (ev->kind == SURFACE_EV_FADER_MOVED && ev->position != (double)W.last_raw[ev->strip] / W.g.fader_max))
		abort();
}

FUZZ_CHECK check_release_position(const struct surface_event *ev)
{
	/* A release of a strip that had a fader report says where the hand left the cap: the last
	 * raw the device reported, held to the range, over fader_max.
	 * contract: surface.h SURFACE_EV_FADER_RELEASED ("position: where the hand left it")
	 * mutant: src/surface.c:148 `return (double)st->last_raw / s->g.fader_max;` -> `return 1 - (double)st->last_raw / s->g.fader_max;`
	 * input: release-position */
	if (W.have_report[ev->strip] && ev->position != (double)W.last_raw[ev->strip] / W.g.fader_max)
		abort();
}

static void on_event(void *ctx, const struct surface_event *ev)
{
	(void)ctx;
	W.last = *ev;
	W.nevents++;
	switch (ev->kind) {
	case SURFACE_EV_FADER_TOUCHED:
		check_event_position(&(struct surface_event){ .strip = ev->strip, .position = 0 });
		W.obs.touched[ev->strip]++;
		break;
	case SURFACE_EV_FADER_RELEASED:
		check_event_position(ev);
		check_release_position(ev);
		W.obs.released[ev->strip]++;
		break;
	case SURFACE_EV_FADER_MOVED:
		check_event_position(ev);
		W.obs.moved++;
		break;
	case SURFACE_EV_ONLINE: W.obs.online++; break;
	case SURFACE_EV_OFFLINE: W.obs.offline++; break;
	default: break;
	}
}

/* ---- what an input call owes the host ----------------------------------------------------- */

static struct tcount owe;
static unsigned owe_events;

FUZZ_CHECK check_touch(void)
{
	int bad = 0;
	for (unsigned k = 0; k < NS; k++)
		bad |= W.obs.touched[k] != owe.touched[k] || W.obs.released[k] != owe.released[k];
	/* The host hears each touch begin once and each release once, when it takes effect.
	 * contract: surface.h SURFACE_EV_FADER_TOUCHED ("debounced"), SURFACE_C24_RELEASE_DEBOUNCE_MS
	 * ("a release becomes a release only if no touch-on follows within this window; a touch-on
	 * inside it cancels it, and the host sees one continuous touch"), SURFACE_TOUCH_TIMEOUT_MS
	 * mutant: src/surface.c:201 `if (was)` -> `if (0)`
	 * input: touches
	 * mutant: src/surface.c:196 `if (st->release_pending) {` -> `if (0) {`
	 * input: c24-touches */
	if (bad)
		abort();
}

FUZZ_CHECK check_move(void)
{
	/* A fader report is one move, unless it is the motor reporting where it put the cap.
	 * contract: surface.h SURFACE_EV_FADER_MOVED ("a hand, never a motor report"),
	 * struct surface_geometry reports_motor_moves
	 * mutant: src/surface.c:274 `if (report)` -> `if (0 && report)`
	 * input: report */
	if (W.obs.moved != owe.moved)
		abort();
}

FUZZ_CHECK check_is_report(int got, unsigned strip, uint16_t raw, uint64_t now)
{
	int diff = (int)raw - (int)W.cmd[strip];
	int want = strip < W.nstrips && W.g.reports_motor_moves && !held(strip, now) && W.cmd_set[strip] &&
		(diff < 0 ? -diff : diff) <= (int)W.g.motor_epsilon_q;
	/* A frame is the motor's report where the device reports its motor, no hand holds the cap, a
	 * write was seen and the frame is within the tolerance of it.
	 * contract: surface.h surface_fader_is_report ("what surface_in_fader() would drop"),
	 * struct surface_geometry motor_epsilon_q, reports_motor_moves
	 * mutant: src/surface.c:259 `<= s->g.motor_epsilon_q` -> `< s->g.motor_epsilon_q`
	 * input: report */
	if (got != want)
		abort();
}

FUZZ_CHECK check_strip_fader(int rc, int bad)
{
	/* A cap written outside the policy at a strip that is not there is refused.
	 * contract: surface.h surface_strip_fader; SURFACE_E_ARG ("a strip off the geometry")
	 * mutant: src/motor.c:315 `return SURFACE_E_ARG;` -> `return SURFACE_OK;`
	 * input: refusal */
	if (bad != (rc == SPEC_E_ARG))
		abort();
}

FUZZ_CHECK check_tick(int rc)
{
	/* A tick succeeds.
	 * contract: surface.h surface_tick
	 * mutant: src/surface.c:569 `return SURFACE_OK;` -> `return SURFACE_E_ARG;`
	 * input: clock */
	if (rc != SPEC_OK)
		abort();
}

FUZZ_CHECK check_passthrough(int ok, unsigned want)
{
	/* A key, a V-Pot, a jog and a warning reach the host once, as they came.
	 * contract: surface.h SURFACE_EV_KEY, SURFACE_EV_VPOT, SURFACE_EV_JOG, SURFACE_EV_WARNING
	 * mutant: src/surface.c:304 `.key = key, .on = pressed ? 1 : 0` -> `.key = key, .on = pressed & 1`
	 * input: passthrough
	 * mutant: src/surface.c:315 `.target = surface_strip_target(s, strip)` -> `.target = 0`
	 * input: passthrough
	 * mutant: src/bank.c:113 `return i < s->nlist ? s->list[i] : 0;` -> `return i + 1 < s->nlist ? s->list[i + 1] : 0;`
	 * input: passthrough */
	if (!ok || owe_events != want)
		abort();
}

FUZZ_CHECK check_clock(uint64_t d, uint64_t now)
{
	/* The deadline is never in the past.
	 * contract: surface.h surface_deadline ("the latest time at which tick must run again (now_us
	 * when it has work now)")
	 * mutant: src/surface.c:545 `return d < now_us ? now_us : d;` -> `return d < now_us ? now_us - 1 : d;`
	 * input: clock-c24 */
	if (d < now)
		abort();
}

FUZZ_CHECK check_idle(unsigned writes, unsigned events)
{
	/* A tick before the deadline does nothing: the deadline is the latest time one must run, and
	 * until then no timer is due.
	 * contract: surface.h surface_deadline, surface_tick
	 * mutant: src/motor.c:328 `f->flush_at > now_us` -> `f->flush_at > now_us + 1000000`
	 * input: clock */
	if (W.writes != writes || W.nevents != events)
		abort();
}

FUZZ_CHECK check_due(uint64_t d, uint64_t now)
{
	/* A tick at the deadline leaves nothing due: the deadline moves past it, or nothing waits.
	 * contract: surface.h surface_deadline ("or UINT64_MAX when nothing waits"), surface_tick
	 * mutant: src/surface.c:554 `st->release_at <= now_us` -> `st->release_at < now_us`
	 * input: clock-c24 */
	if (d != UINT64_MAX && d <= now)
		abort();
}

FUZZ_CHECK check_quiet(uint64_t d)
{
	/* A surface with nothing in flight waits for nothing: on the new surface, and after any
	 * operation that leaves no hand on a cap, no release waiting out its debounce, and no
	 * trailing flush that could be armed (no served value, paint or fader report yet).
	 * contract: surface.h surface_deadline ("or UINT64_MAX when nothing waits")
	 * mutant: src/motor.c:343 `UINT64_MAX` -> `0`
	 * input: clock
	 * mutant: src/surface.c:542 `st->touch && s->g.touch_hold_ms` -> `(st->touch || st->touch_until > now_us) && s->g.touch_hold_ms`
	 * input: touches */
	if (d != UINT64_MAX)
		abort();
}

/* ---- the bank, as surface.h describes it (fuzz_bank holds the same window) ---------------- */

static size_t m_max(void)
{
	return W.nlist > W.g.strips ? W.nlist - W.g.strips : 0;
}

static void m_move(long to)
{
	W.offset = to < 0 ? 0 : (size_t)to;
	if (W.offset > m_max())
		W.offset = m_max();
}

static void m_set(const surface_target *l, size_t n)
{
	W.nlist = n < W.cap ? n : W.cap;
	memcpy(W.list, l, W.nlist * sizeof *l);
	m_move((long)W.offset);
}

static surface_target m_target(unsigned strip)
{
	if (strip < W.g.strips)
		return W.offset + strip < W.nlist ? W.list[W.offset + strip] : 0;
	if (strip == W.g.strips && W.g.has_master)
		return W.master;
	return 0;
}

/* nothing waits: no hand, no release pending, no flush that could be armed */
static int m_quiet(void)
{
	if (W.flushable)
		return 0;
	for (unsigned k = 0; k < NS; k++)
		if (W.flag[k] || W.pending[k])
			return 0;
	return 1;
}

/* ---- the program -------------------------------------------------------------------------- */

static void begin(void)
{
	memset(&W.obs, 0, sizeof W.obs);
	memset(&owe, 0, sizeof owe);
	W.nevents = 0;
	owe_events = 0;
}

static void settle(void)
{
	check_touch();
	check_move();
}

/* the model of a touch-on at `now`: what the host is owed, the flags after */
static void m_touch(unsigned k, int on)
{
	int was = held(k, W.now);
	if (on) {
		W.flag[k] = 1;
		if (W.g.touch_hold_ms)
			W.until[k] = W.now + (uint64_t)W.g.touch_hold_ms * MS_US;
		if (W.pending[k]) {
			W.pending[k] = 0;
			return;
		}
		if (!was)
			owe.touched[k]++;
		return;
	}
	if (!was) {
		W.flag[k] = 0;
		return;
	}
	if (W.g.touch_release_debounce_ms) {
		if (!W.pending[k]) {
			W.pending[k] = 1;
			W.release_at[k] = W.now + (uint64_t)W.g.touch_release_debounce_ms * MS_US;
		}
		return;
	}
	W.flag[k] = 0;
	W.pending[k] = 0;
	owe.released[k]++;
}

/* the model of a tick at `now`: a release whose debounce has passed, a held flag that lapsed */
static void m_tick(void)
{
	for (unsigned k = 0; k < W.nstrips; k++) {
		if (W.pending[k] && W.release_at[k] <= W.now) {
			W.flag[k] = 0;
			W.pending[k] = 0;
			owe.released[k]++;
		} else if (W.flag[k] && W.g.touch_hold_ms && W.now >= W.until[k]) {
			W.flag[k] = 0;
			owe.released[k]++;
		}
	}
}

static unsigned pick_strip(struct fuzz_in *in)
{
	return fuzz_u8(in) % (W.nstrips + 2);
}

static surface_target pick_target(struct fuzz_in *in)
{
	uint8_t b = fuzz_u8(in) % 16;
	return b < 14 ? b : b == 14 ? 0xffffffffu : 0;
}

static void do_fader(struct fuzz_in *in)
{
	unsigned strip = pick_strip(in);
	uint16_t raw = (uint16_t)(fuzz_u16(in) % (W.g.fader_max + 40u));
	begin();
	if (strip < W.nstrips) {
		uint16_t c = raw > W.g.fader_max ? W.g.fader_max : raw;
		int diff = (int)c - (int)W.cmd[strip];
		int report = W.g.reports_motor_moves && !held(strip, W.now) && W.cmd_set[strip] &&
			(diff < 0 ? -diff : diff) <= (int)W.g.motor_epsilon_q;
		W.last_raw[strip] = c;
		W.have_report[strip] = 1;
		W.flushable = 1;
		if (!report)
			owe.moved = 1;
	}
	W.nevents = 0;
	surface_in_fader(S, strip, raw, W.now);
	settle();
}

static void do_touch(struct fuzz_in *in)
{
	unsigned strip = pick_strip(in);
	int on = fuzz_u8(in) & 1;
	begin();
	if (strip < W.nstrips)
		m_touch(strip, on);
	surface_in_touch(S, strip, on, W.now);
	settle();
}

static void do_served(struct fuzz_in *in)
{
	surface_target t = pick_target(in);
	double pos = spec_unit(fuzz_double(in));   /* contract: surface.h "A served value ..., 0..1" */
	uint8_t form = fuzz_u8(in);
	begin();
	W.flushable = 1;
	if (form & 1) {
		uint64_t back = (uint64_t)(form >> 1) * 7 * MS_US;
		uint64_t sent = back > W.now ? 0 : form & 0x80 ? W.now + back : W.now - back;
		surface_fader_served_at(S, t, pos, sent, W.now);
	} else {
		surface_fader_served(S, t, pos, W.now);
	}
	settle();
}

static void do_refused(struct fuzz_in *in)
{
	begin();
	surface_fader_refused(S, pick_target(in));
	settle();
}

static void do_paint(struct fuzz_in *in)
{
	surface_target t = pick_target(in);
	double pos = spec_unit(fuzz_double(in));
	begin();
	W.flushable = 1;
	surface_fader_paint(S, t, pos, W.now);
	settle();
}

static void do_paint_strip(struct fuzz_in *in)
{
	unsigned strip = pick_strip(in);
	double pos = spec_unit(fuzz_double(in));
	begin();
	W.flushable = 1;
	surface_fader_paint_strip(S, strip, pos, W.now);
	settle();
}

static void do_suspend(struct fuzz_in *in)
{
	surface_target t = pick_target(in);
	begin();
	surface_fader_suspend(S, t, fuzz_u8(in) & 1);
	settle();
}

static void do_strip_fader(struct fuzz_in *in)
{
	unsigned strip = pick_strip(in);
	double pos = fuzz_double(in);
	begin();
	check_strip_fader(surface_strip_fader(S, strip, pos, W.now), strip >= W.nstrips);
	settle();
}

static void do_note(struct fuzz_in *in)
{
	unsigned strip = pick_strip(in);
	uint16_t raw = fuzz_u16(in) % (W.g.fader_max + 1u);
	surface_note_commanded(S, strip, raw);
	if (strip < W.nstrips) {
		W.cmd_set[strip] = 1;
		W.cmd[strip] = raw;
	}
}

static void do_is_report(struct fuzz_in *in)
{
	unsigned strip = pick_strip(in);
	uint16_t raw = fuzz_u16(in) % (W.g.fader_max + 40u);
	check_is_report(surface_fader_is_report(S, strip, raw, W.now), strip, raw, W.now);
}

static void do_online(int on)
{
	begin();
	if (on) {
		W.online = 1;
		for (unsigned k = 0; k < NS; k++)
			W.flag[k] = W.pending[k] = 0;
		surface_in_online(S, W.now);
	} else {
		surface_in_offline(S, W.now);
		W.online = 0;
	}
	settle();
}

static void do_bank(struct fuzz_in *in)
{
	uint8_t form = fuzz_u8(in);
	if (form & 1) {
		surface_target l[16];
		size_t n = fuzz_u8(in) % 17;
		for (size_t i = 0; i < n; i++)
			l[i] = (surface_target)(1 + fuzz_u8(in) % 13);
		surface_bank_set(S, l, n);
		m_set(l, n);
	} else if (form & 2) {
		int delta = (int)(int8_t)fuzz_u8(in);
		surface_bank_step(S, delta);
		m_move((long)W.offset + delta);
	} else {
		surface_target t = pick_target(in);
		surface_master_set(S, t);
		if (W.g.has_master)
			W.master = t;
	}
}

static void do_passthrough(struct fuzz_in *in)
{
	uint8_t form = fuzz_u8(in) % 4;
	begin();
	unsigned want = 1;
	int ok = 1;
	if (form == 0) {
		uint16_t key = fuzz_u16(in);
		int pressed = (int)(int8_t)fuzz_u8(in);
		surface_in_key(S, key, pressed, W.now);
		ok = W.last.kind == SURFACE_EV_KEY && W.last.key == key && W.last.on == (pressed ? 1 : 0);
	} else if (form == 1) {
		unsigned strip = pick_strip(in);
		int delta = (int)(int16_t)fuzz_u16(in);
		want = strip < W.g.strips;
		surface_in_vpot(S, strip, delta, W.now);
		ok = !want || (W.last.kind == SURFACE_EV_VPOT && W.last.strip == strip && W.last.delta == delta &&
			       W.last.target == m_target(strip));
	} else if (form == 2) {
		int delta = (int)(int16_t)fuzz_u16(in);
		surface_in_jog(S, delta, W.now);
		ok = W.last.kind == SURFACE_EV_JOG && W.last.delta == delta;
	} else {
		static const char text[] = "careful";
		int code = (int)(int8_t)fuzz_u8(in);
		surface_in_warning(S, code, text, W.now);
		ok = W.last.kind == SURFACE_EV_WARNING && W.last.code == code && W.last.text == text;
	}
	owe_events = W.nevents;
	check_passthrough(ok, want);
}

/* A tick at the time the clock reads, or at the deadline. */
static void do_tick(int at_deadline)
{
	uint64_t d = surface_deadline(S, W.now);
	check_clock(d, W.now);
	if (at_deadline) {
		if (d == UINT64_MAX)
			return;
		W.now = d;
	} else if (d > W.now) {
		/* before the deadline a tick does nothing */
		unsigned writes = W.writes;
		begin();
		check_tick(surface_tick(S, W.now));
		check_idle(writes, W.nevents);
		return;
	}
	begin();
	m_tick();
	check_tick(surface_tick(S, W.now));
	check_due(surface_deadline(S, W.now), W.now);
	settle();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	memset(&W, 0, sizeof W);

	struct surface_config c = { .ops = &ops, .event = on_event };
	uint8_t shape = fuzz_u8(&in);
	surface_geometry_preset(shape & 1 ? SURFACE_MODEL_CONTROL24 : SURFACE_MODEL_XTOUCH, &c.geometry);
	c.max_targets = shape >> 1 & 7 ? (uint16_t)(1 + (shape >> 1 & 7) % 6) : 0;
	W.g = c.geometry;
	W.nstrips = W.g.strips + (W.g.has_master ? 1u : 0u);
	W.accept = 1;
	W.cap = c.max_targets ? c.max_targets : 256;
	W.now = 1000000ull + (uint64_t)fuzz_u16(&in) * 1000ull;
	if (surface_create(&S, &c) != SPEC_OK)
		return 0;
	/* a surface with nothing in flight waits for nothing */
	check_quiet(surface_deadline(S, W.now));
	surface_target l[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	surface_bank_set(S, l, 8);
	m_set(l, 8);
	surface_master_set(S, 9);
	if (W.g.has_master)
		W.master = 9;
	while (!fuzz_done(&in)) {
		uint8_t op = fuzz_u8(&in);
		switch (op % 17) {
		case 0: case 1: case 2: do_fader(&in); break;
		case 3: case 4: do_touch(&in); break;
		case 5: case 6: do_served(&in); break;
		case 7: do_refused(&in); break;
		case 8: do_paint(&in); break;
		case 9: do_paint_strip(&in); break;
		case 10: do_suspend(&in); break;
		case 11: do_strip_fader(&in); break;
		case 12: do_note(&in); break;
		case 13: do_is_report(&in); break;
		case 14: do_online(op >> 5 & 1); break;
		case 15: do_bank(&in); break;
		default:
			if (op & 0x20)
				do_passthrough(&in);
			else if (op & 0x40)
				W.accept = !W.accept;
			else
				do_tick(op >> 7);
			break;
		}
		if (m_quiet())
			check_quiet(surface_deadline(S, W.now));
		W.now = fuzz_step(&in, W.now);
	}
	surface_destroy(S);
	return 0;
}

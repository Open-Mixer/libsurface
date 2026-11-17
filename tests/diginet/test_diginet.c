// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libsurface-diginet against libdiginet's desk emulator (libdiginet-emu's, installed: no copy of it
 * lives in this tree, tests/diginet/no-emu-copy.sh forbids one), in process,
 * on a virtual clock: the backend's mux sends each frame to the emulator, the emulator's frames
 * come back through surface_diginet_rx(), and what the desk received is decoded from its log. The
 * host is a console that echoes every move as its served value, after the call that reported it.
 * Strip k shows target k + 1. The desk announces every 2 s.
 *
 *  1. ONLINE     the desk appears, is attached, comes online, and the whole board goes to it:
 *                scribbles, LEDs, automation LEDs, ring, meters on both sides, counter, faders.
 *  2. SET-POINT  a touched cap's reports are each written back as its set-point.
 *  3. RELEASE    the release writes the last report once more, and the cap is never sent
 *                anywhere else: no snap back to the target it showed before the ride.
 *  4. HOST MOVE  a served value moves an untouched cap; never a touched one.
 *  5. INPUTS     keys, section keys, V-Pot, jog and touch come up as surface events.
 *  6. DEAF       a desk that answers no wake while it announces is a DESK_DEAF warning.
 *  7. FOREIGN    a desk in session with another host is one FOREIGN warning, and is not woken.
 *  8. DESK RESTART  a desk that drops its session is repainted when it comes back.
 *  9. HOST RESTART  a host that closes and opens again repaints the desk within 2 s.
 * 10. GONE       a silent desk goes offline and gone, and gets its session back when it returns.
 * 11. OTHER NAME  an attached desk that comes back as something else gets no session and no
 *                write, its surface waits offline, and the real desk's return repaints it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <diginet/testsupport/desk_emu.h>

#include <diginet/diginet_codec.h>
#include <surface/surface_diginet.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)
#define T(name) static void name(void)
#define RUN(name) do { \
	int before_ = fails; \
	name(); \
	if (fails != before_) fprintf(stderr, "  ^ in %s\n", #name); \
} while (0)

#define MS 1000ull
#define S  1000000ull
#define IFX 2

static const uint8_t DESK[6] = { 0x00, 0xa0, 0x7e, 0x24, 0x00, 0x0a };
static const uint8_t HOST[6] = { 0x02, 0x00, 0x00, 0x00, 0x0a, 0x01 };
static const uint8_t OTHER[6] = { 0x02, 0x00, 0x00, 0x00, 0x0c, 0x01 };

static struct desk_emu E;
static struct surface_diginet *D;
static struct surface *SF;
static uint64_t now, next_ann;
static int auto_attach;
static int appeared, gone;
static const char *desk_name;      /* what on_desk expects the announce to say */
static int desk_model;

#define EVMAX 4096
static struct surface_event EV[EVMAX];
static char EVTEXT[EVMAX][100];
static int EV_SHOWN[EVMAX];        /* the desk was announcing e1 naming us when the event went up */
static unsigned EV_MARK[EVMAX];    /* the desk's log position then: what it gets after is the replay */
static int nev;
static int echo_t[EVMAX];
static double echo_p[EVMAX];
static int nechoes;

static const struct diginet_personality *P;

/* ---- the wire ---------------------------------------------------------------------------- */

static int io_send(void *ctx, int ifindex, const uint8_t *f, size_t len)
{
	(void)ctx;
	if (ifindex != IFX)
		return -ENXIO;
	emu_from_host(&E, f, len, now);
	return 0;
}

static int io_if_mac(void *ctx, int ifindex, uint8_t mac[6])
{
	(void)ctx;
	if (ifindex != IFX)
		return -ENODEV;
	memcpy(mac, HOST, 6);
	return 0;
}

static uint64_t io_now(void *ctx) { (void)ctx; return now; }

static const struct diginet_mux_io IO = {
	.send = io_send, .recv = NULL, .if_mac = io_if_mac, .now_us = io_now,
};

/* ---- the host ---------------------------------------------------------------------------- */

static void on_event(void *ctx, const struct surface_event *ev)
{
	(void)ctx;
	if (nev < EVMAX) {
		EV[nev] = *ev;
		snprintf(EVTEXT[nev], sizeof EVTEXT[nev], "%s", ev->text ? ev->text : "");
		EV[nev].text = EVTEXT[nev];
		EV_SHOWN[nev] = E.shows_session;
		EV_MARK[nev] = E.nlog;
		nev++;
	}
	/* a console takes the move and serves it back, after the call that reported it */
	if (ev->kind == SURFACE_EV_FADER_MOVED && ev->target && nechoes < EVMAX) {
		echo_t[nechoes] = (int)ev->target;
		echo_p[nechoes++] = ev->position;
	}
}

static void host_answer(void)
{
	for (int i = 0; i < nechoes && SF; i++)
		surface_fader_served(SF, (surface_target)echo_t[i], echo_p[i], now);
	nechoes = 0;
}

static void board_targets(void)
{
	surface_target list[24];
	for (int i = 0; i < 24; i++)
		list[i] = (surface_target)(i + 1);
	surface_bank_set(SF, list, 24);
}

static void on_desk(void *ctx, const struct surface_desk_info *d, int present)
{
	(void)ctx;
	CHK(!memcmp(d->mac, DESK, 6) && d->ifindex == IFX);
	if (!present) {
		gone++;
		return;
	}
	appeared++;
	CHK(!strcmp(d->name, desk_name) && !strcmp(d->version, "1.08"));
	CHK(d->model == desk_model);
	if (auto_attach) {
		struct surface *s = NULL;
		CHK(surface_diginet_attach(D, d->mac, on_event, NULL, &s) == SURFACE_OK && s);
		if (s && s != SF) {
			SF = s;
			board_targets();
		}
	}
}

/* ---- the clock --------------------------------------------------------------------------- */

static void pump(void)
{
	uint8_t b[DIGINET_MAX_FRAME_LEN];
	size_t n;
	if (!D) {   /* no host: the desk talks to no one */
		while (emu_pop(&E, b, sizeof b))
			;
		return;
	}
	while ((n = emu_pop(&E, b, sizeof b))) {
		surface_diginet_rx(D, b, n, IFX, now);
		host_answer();
	}
	CHK(surface_diginet_service(D, now) == 0);
	host_answer();
}

static void run(uint64_t us)
{
	uint64_t end = now + us;
	while (now < end) {
		now += MS;
		emu_tick(&E, now);
		if (now >= next_ann) {
			emu_announce(&E, now);
			next_ann = now + 2 * S;
		}
		pump();
	}
}

static void open_backend(void)
{
	SF = NULL;
	CHK(surface_diginet_open_io(&D, &IO, on_desk, NULL) == SURFACE_OK && D);
	CHK(surface_diginet_fd(D) == -1);
}

static void start(void)
{
	emu_init(&E, DESK, "CNTRL|24", "1.08");
	P = diginet_personality_of(DIGINET_DESK_CONTROL24);
	now = 1000 * S;
	next_ann = now + MS;
	nev = nechoes = appeared = gone = 0;
	auto_attach = 1;
	desk_name = "CNTRL|24";
	desk_model = DIGINET_DESK_CONTROL24;
	open_backend();
}

static void stop(void)
{
	if (D)
		surface_diginet_close(D, now);
	D = NULL;
	SF = NULL;
}

/* ---- reading what the desk got ----------------------------------------------------------- */

/* The decoded commands of every first send since log entry `from`, in order. */
#define WMAX 8192
static struct diginet_event W[WMAX];
static int wire(unsigned from)
{
	int n = 0;
	for (unsigned i = from; i < E.nlog; i++) {
		const struct emu_rx *r = &E.log[i];
		if (r->cmd != DIGINET_CMD_DATA || r->retry)
			continue;
		struct diginet_decoder dec;
		diginet_decoder_init(&dec, P, r->body, r->body_len);
		for (int rc; (rc = diginet_decoder_next(&dec, &W[n])) != 0 && n < WMAX - 1; )
			if (rc > 0)
				n++;
	}
	return n;
}

/* The fader values the desk got for strip since `from`. */
static int fader_writes(unsigned from, unsigned strip, unsigned *out, int max)
{
	int n = wire(from), k = 0;
	for (int i = 0; i < n; i++)
		if (W[i].kind == DIGINET_EV_FADER && W[i].strip == strip) {
			if (k < max)
				out[k] = W[i].value;
			k++;
		}
	return k;
}

static int count_wire(unsigned from, enum diginet_event_kind kind)
{
	int n = wire(from), k = 0;
	for (int i = 0; i < n; i++)
		k += W[i].kind == kind;
	return k;
}

static int count_ev(enum surface_event_kind k)
{
	int n = 0;
	for (int i = 0; i < nev; i++)
		n += EV[i].kind == k;
	return n;
}

/*
 * A Control|24 comes up in two steps (libdiginet#48): ONLINE on the a0, with no e1 from the desk yet,
 * then ONLINE again, RENEWED, on the first e1 naming this host. The desk opened a new session on the
 * e2 before that e1, which blanked it, so the board has to go out again after RENEWED. By value: after
 * the event at index `from`, exactly these two ONLINEs, in that order; answers where RENEWED's replay starts.
 */
static unsigned online_then_renewed(int from)
{
	int at[3], n = 0;
	for (int i = from; i < nev; i++)
		if (EV[i].kind == SURFACE_EV_ONLINE && n < 3)
			at[n++] = i;
	CHK(n == 2);
	if (n != 2)
		return E.nlog;
	CHK(EV_SHOWN[at[0]] == 0);   /* on the a0 */
	CHK(EV_SHOWN[at[1]] == 1);   /* on the e1 */
	return EV_MARK[at[1]];
}

static const struct surface_event *last_ev(enum surface_event_kind k)
{
	for (int i = nev - 1; i >= 0; i--)
		if (EV[i].kind == k)
			return &EV[i];
	return NULL;
}

static void desk_says(int rc, const struct diginet_cmd *c)
{
	CHK(rc == DIGINET_OK);
	emu_send_data(&E, c->bytes, c->len, 1, 0, 0);
}

static void desk_touch(unsigned strip, int on)
{
	struct diginet_cmd c;
	desk_says(diginet_touch_encode(P, strip, on, &c), &c);
}

static void desk_fader(unsigned strip, unsigned v)
{
	struct diginet_cmd c;
	desk_says(diginet_fader_encode(P, strip, v, &c), &c);
}

/* Up and online, the board painted with names, and the log marked after it. */
static unsigned online(void)
{
	start();
	run(500 * MS);
	CHK(SF != NULL);
	char name[8];
	for (unsigned k = 0; SF && k < 24; k++) {
		snprintf(name, sizeof name, "F%u", k + 1);
		surface_scribble(SF, k, 0, name);
	}
	run(2 * S);
	CHK(SF && surface_online(SF));
	nev = 0;
	return E.nlog;
}

/* ---- 1 ----------------------------------------------------------------------------------- */

T(online_replays_board)
{
	start();
	run(500 * MS);
	CHK(appeared == 1 && SF);
	if (!SF)
		return;
	/* the board, written while the desk is still waking: kept in the shadow */
	char name[8];
	for (unsigned k = 0; k < 24; k++) {
		snprintf(name, sizeof name, "F%u", k + 1);
		surface_scribble(SF, k, 0, name);
	}
	surface_led(SF, SURFACE_DIGINET_KEY(SURFACE_DIGINET_BTN_MUTE, 0), SURFACE_LED_ON);
	surface_led(SF, SURFACE_DIGINET_KEY(SURFACE_DIGINET_BTN_SOLO, 3), SURFACE_LED_FLASH);
	surface_led(SF, SURFACE_DIGINET_AUTO_KEY(2, DIGINET_AUTO_READ), SURFACE_LED_ON);
	surface_led(SF, SURFACE_DIGINET_AUTO_KEY(2, DIGINET_AUTO_TOUCH), SURFACE_LED_ON);
	surface_ring(SF, 5, SURFACE_RING_FILL, 1.0);
	surface_meter(SF, 6, 0, 1.0, now);
	surface_meter(SF, 6, 1, 0.5, now);
	surface_counter(SF, "12345678");
	surface_fader_served(SF, 8, 0.5, now);   /* strip 7 */
	unsigned mark = E.nlog;
	run(3 * S);
	unsigned renewed = online_then_renewed(0);
	CHK(count_ev(SURFACE_EV_OFFLINE) == 0);
	CHK(renewed >= mark);
	/* what RENEWED replays: the whole shadow, once. A level is the host's next frame, not shadow. */
	int n = wire(renewed);
	int scrib = 0, mute = 0, solo = 0, autos = 0, ring = 0, ml = 0, mr = 0, counter = 0, fader = 0;
	for (int i = 0; i < n; i++) {
		const struct diginet_event *w = &W[i];
		if (w->kind == DIGINET_EV_SCRIBBLE && w->addr < 24) {
			snprintf(name, sizeof name, "F%u", w->addr + 1u);
			char want[8];
			snprintf(want, sizeof want, "%-4s", name);
			scrib += !strcmp(w->text, want);
		}
		mute += w->kind == DIGINET_EV_STRIP_BUTTON && w->button == 0x08 && w->strip == 0 && w->on;
		solo += w->kind == DIGINET_EV_STRIP_BUTTON && w->button == 0x07 && w->strip == 3 && w->on;
		autos += w->kind == DIGINET_EV_STRIP_LEDS && w->addr == 2 &&
			w->mask == (DIGINET_AUTO_READ | DIGINET_AUTO_TOUCH);
		ring += w->kind == DIGINET_EV_RING && w->strip == 5 && w->mask == 0x7fff;
		ml += w->kind == DIGINET_EV_METER && !w->right && w->addr == 6 && w->segments == 14;
		mr += w->kind == DIGINET_EV_METER && w->right && (w->addr & 0x1f) == 6 && w->segments == 7;
		counter += w->kind == DIGINET_EV_COUNTER;
		fader += w->kind == DIGINET_EV_FADER && w->strip == 7 && w->value == 512;
	}
	CHK(scrib == 24);
	CHK(mute == 1 && solo == 1 && autos == 1 && ring == 1 && ml == 0 && mr == 0);
	CHK(counter == 1);
	CHK(fader == 1);
	/* only the strip the host served is driven: the others' positions are unknown */
	CHK(count_wire(renewed, DIGINET_EV_FADER) == 1);
	/* the exact total: the board as written once on the a0's session, and that replay */
	CHK(count_wire(mark, DIGINET_EV_FADER) == 2);
	CHK(count_wire(mark, DIGINET_EV_SCRIBBLE) == 48);
	CHK(count_wire(mark, DIGINET_EV_METER) == 2);
	stop();
}

/* ---- 2, 3 -------------------------------------------------------------------------------- */

T(touched_reports_echo_setpoint)
{
	unsigned mark = online();
	desk_touch(4, 1);
	run(20 * MS);
	static const unsigned v[] = { 100, 200, 300, 400 };
	for (int i = 0; i < 4; i++) {
		desk_fader(4, v[i]);
		run(20 * MS);
	}
	unsigned w[32];
	int n = fader_writes(mark, 4, w, 32);
	CHK(n == 4);
	for (int i = 0; i < n && i < 4; i++)
		CHK(w[i] == v[i]);
	CHK(count_ev(SURFACE_EV_FADER_TOUCHED) == 1 && count_ev(SURFACE_EV_FADER_MOVED) == 4);
	stop();
}

T(release_echoes_once_no_snap)
{
	unsigned mark0 = online();
	(void)mark0;
	surface_fader_served(SF, 5, 0.5, now);   /* strip 4 shows 512 before the ride */
	run(200 * MS);
	unsigned mark = E.nlog;
	desk_touch(4, 1);
	run(10 * MS);
	for (unsigned v = 520; v >= 220; v -= 60) {
		desk_fader(4, v);
		run(15 * MS);
		/* the sense chatters: a dropout inside the release debounce is one touch */
		if (v == 400) {
			desk_touch(4, 0);
			run(120 * MS);
			desk_touch(4, 1);
			run(10 * MS);
		}
	}
	unsigned w[64];
	int during = fader_writes(mark, 4, w, 64);
	CHK(during == 6);
	desk_touch(4, 0);
	run(SURFACE_C24_RELEASE_DEBOUNCE_MS * MS - 20 * MS);
	CHK(fader_writes(mark, 4, w, 64) == during);   /* nothing before the debounce ends */
	run(3 * S);
	int n = fader_writes(mark, 4, w, 64);
	CHK(n == during + 1);                           /* the release echo, once */
	for (int i = 0; i < n && i < 64; i++)
		CHK(w[i] != 512);                       /* never the target before the ride */
	CHK(n > 0 && w[n - 1] == 220);
	CHK(count_ev(SURFACE_EV_FADER_RELEASED) == 1);
	const struct surface_event *r = last_ev(SURFACE_EV_FADER_RELEASED);
	CHK(r && r->strip == 4 && r->target == 5 && r->position > 0.21 && r->position < 0.22);
	stop();
}

/* ---- 4 ----------------------------------------------------------------------------------- */

T(host_move_drives_untouched_cap)
{
	unsigned mark = online();
	surface_fader_served(SF, 10, 0.25, now);   /* strip 9 */
	run(200 * MS);
	unsigned w[16];
	CHK(fader_writes(mark, 9, w, 16) == 1 && w[0] == 256);
	desk_touch(9, 1);
	run(20 * MS);
	mark = E.nlog;
	surface_fader_served(SF, 10, 0.75, now);   /* never under a hand */
	run(200 * MS);
	CHK(fader_writes(mark, 9, w, 16) == 0);
	stop();
}

/* ---- 5 ----------------------------------------------------------------------------------- */

T(inputs_map_up)
{
	online();
	struct diginet_cmd c;
	desk_says(diginet_button_encode(0x08, 3, 1, &c), &c);     /* MUTE on strip 3 */
	run(10 * MS);
	desk_says(diginet_button_encode(0x08, 3, 0, &c), &c);
	run(10 * MS);
	desk_says(diginet_button_encode(0x02, 0x18, 1, &c), &c);  /* a section key, zone 18 */
	run(10 * MS);
	desk_says(diginet_vpot_encode(P, 11, -3, &c), &c);
	run(10 * MS);
	desk_says(diginet_jog_encode(5, &c), &c);
	run(10 * MS);
	desk_touch(2, 1);
	run(10 * MS);
	int keys = 0;
	for (int i = 0; i < nev; i++) {
		const struct surface_event *e = &EV[i];
		if (e->kind != SURFACE_EV_KEY)
			continue;
		keys++;
		if (keys == 1)
			CHK(e->key == SURFACE_DIGINET_KEY(0x08, 3) && e->on == 1);
		if (keys == 2)
			CHK(e->key == SURFACE_DIGINET_KEY(0x08, 3) && e->on == 0);
		if (keys == 3)
			CHK(e->key == SURFACE_DIGINET_KEY(0x02, 0x18) && e->on == 1);
	}
	CHK(keys == 3);
	const struct surface_event *e = last_ev(SURFACE_EV_VPOT);
	CHK(e && e->strip == 11 && e->target == 12 && e->delta == -3);
	e = last_ev(SURFACE_EV_JOG);
	CHK(e && e->delta == 5);
	e = last_ev(SURFACE_EV_FADER_TOUCHED);
	CHK(e && e->strip == 2 && e->target == 3);
	stop();
}

/* ---- 6, 7 -------------------------------------------------------------------------------- */

T(desk_deaf_is_a_warning)
{
	start();
	E.ack_wake = 0;   /* announces, answers no e2: booted on a noisy link */
	run(20 * S);
	CHK(SF && E.wakes >= 3);
	CHK(count_ev(SURFACE_EV_ONLINE) == 0);
	int deaf = 0;
	for (int i = 0; i < nev; i++)
		if (EV[i].kind == SURFACE_EV_WARNING && EV[i].code == SURFACE_WARN_DESK_DEAF) {
			deaf++;
			CHK(strstr(EV[i].text, "deaf") != NULL);
		}
	CHK(deaf >= 1);
	stop();
}

T(foreign_is_one_warning)
{
	start();
	E.foreign = 1;    /* in session with another host: announces e1 naming it */
	memcpy(E.session_mac, OTHER, 6);
	run(20 * S);
	CHK(SF != NULL);
	CHK(E.wakes == 0);
	CHK(count_ev(SURFACE_EV_ONLINE) == 0);
	int foreign = 0;
	for (int i = 0; i < nev; i++)
		if (EV[i].kind == SURFACE_EV_WARNING && EV[i].code == SURFACE_WARN_FOREIGN) {
			foreign++;
			CHK(strstr(EV[i].text, "02:00:00:00:0c:01") != NULL);
		}
	CHK(foreign == 1);   /* the desk announces it every 2 s: one warning per run */
	/* the other host lets go: the desk is woken and comes online */
	emu_drop_session(&E);
	run(5 * S);
	CHK(count_ev(SURFACE_EV_ONLINE) == 1);
	stop();
}

/* ---- 8, 9 -------------------------------------------------------------------------------- */

static int scribbles_named(unsigned from)
{
	int n = wire(from), k = 0;
	for (int i = 0; i < n; i++)
		k += W[i].kind == DIGINET_EV_SCRIBBLE && W[i].text[0] == 'F';
	return k;
}

T(desk_restart_repaints)
{
	online();
	surface_fader_served(SF, 3, 0.5, now);
	run(200 * MS);
	emu_drop_session(&E);   /* a desk reboot: the next host frame goes unanswered */
	surface_fader_served(SF, 3, 0.6, now);
	run(10 * S);
	CHK(count_ev(SURFACE_EV_OFFLINE) == 1);
	unsigned mark = online_then_renewed(0);
	CHK(scribbles_named(mark) == 24);
	unsigned w[16];
	int n = fader_writes(mark, 2, w, 16);
	CHK(n >= 1 && w[n - 1] == 614);   /* the latest target, 0.6 */
	stop();
}

T(host_restart_repaints_in_2s)
{
	online();
	E.holds_after_e3 = 1;   /* after our e3 the desk keeps announcing e1 naming us */
	surface_diginet_close(D, now);
	D = NULL;
	SF = NULL;
	run(500 * MS);
	open_backend();         /* the host again, on the same interface: the same MAC */
	nev = 0;
	unsigned mark = E.nlog;
	next_ann = now + MS;
	run(10 * MS);           /* the next announce: e1 naming us */
	CHK(SF != NULL);
	if (!SF) {
		stop();
		return;
	}
	char name[8];
	for (unsigned k = 0; k < 24; k++) {
		snprintf(name, sizeof name, "F%u", k + 1);
		surface_scribble(SF, k, 0, name);
	}
	uint64_t t0 = now;
	while (scribbles_named(mark) < 24 && now - t0 < 5 * S)
		run(10 * MS);
	CHK(scribbles_named(mark) == 24);
	CHK(now - t0 < 2 * S);
	CHK(count_ev(SURFACE_EV_ONLINE) == 1);
	stop();
}

/* ---- 10 ---------------------------------------------------------------------------------- */

T(gone_and_back)
{
	online();
	struct surface *was = SF;
	E.mute = 1;
	run(40 * S);
	CHK(gone == 1);
	CHK(count_ev(SURFACE_EV_OFFLINE) == 1);
	CHK(SF == was && !surface_online(SF));
	unsigned back = E.nlog;
	E.mute = 0;
	emu_drop_session(&E);
	run(5 * S);
	CHK(appeared == 2);
	CHK(SF == was);
	unsigned mark = online_then_renewed(0);
	CHK(scribbles_named(mark) == 24);   /* the names, without the host asking */
	CHK(scribbles_named(back) == 48);   /* the replay on the a0, and the one on RENEWED: no more */
	stop();
}

/* ---- 11 ---------------------------------------------------------------------------------- */

/* The attached desk goes silent and the same MAC comes back announcing this name. */
static void back_as(const char *name, int model)
{
	int was_gone = gone, was_appeared = appeared;
	E.mute = 1;
	run(40 * S);
	CHK(gone == was_gone + 1);
	/* what emu_init derives from the name: a desk no personality describes announces its session */
	const struct diginet_personality *p = diginet_personality_from_name(name);
	snprintf(E.name, sizeof E.name, "%s", name);
	E.announces_session = !p || p->announces_session;
	E.acks_offline = p && p->desk == DIGINET_DESK_CONTROL24;
	desk_name = name;
	desk_model = model;
	E.mute = 0;
	emu_drop_session(&E);
	run(5 * S);
	CHK(appeared == was_appeared + 1);
}

static void other_name_waits_offline(const char *name, int model)
{
	online();
	struct surface *was = SF;
	back_as(name, model);
	unsigned mark = E.nlog;
	int online_events = count_ev(SURFACE_EV_ONLINE);
	CHK(SF == was && !surface_online(SF));
	/* every host call a console makes keeps working, and none reaches the wire */
	CHK(surface_scribble(SF, 0, 0, "HELD") == 0);
	surface_fader_served(SF, 1, 0.25, now);
	CHK(surface_diginet_ring_mask(D, SF, 0, 1) == 0);
	CHK(surface_diginet_part(D, "Bye") == 0);
	run(5 * S);
	CHK(!surface_online(SF));
	CHK(count_ev(SURFACE_EV_ONLINE) == online_events);
	CHK(E.nlog == mark);
	/* the desk the surface was built for returns: its session, and the board as the host left it */
	int from = nev;
	back_as("CNTRL|24", DIGINET_DESK_CONTROL24);
	CHK(SF == was && surface_online(SF));
	mark = online_then_renewed(from);
	CHK(scribbles_named(mark) == 23);   /* F2..F24: strip 0 now holds what the host set above */
	stop();
}

T(back_under_an_unknown_name) { other_name_waits_offline("NOT|A|DK", DIGINET_DESK_UNKNOWN); }
T(back_as_another_model) { other_name_waits_offline("MAINUNIT", DIGINET_DESK_PROCONTROL); }

T(detached_while_another_name_refuses_attach)
{
	online();
	back_as("NOT|A|DK", DIGINET_DESK_UNKNOWN);
	auto_attach = 0;
	surface_diginet_detach(D, SF, now);
	SF = NULL;
	struct surface *s = (struct surface *)1;
	CHK(surface_diginet_attach(D, DESK, NULL, NULL, &s) == SURFACE_E_ARG && s == NULL);
	stop();
}

/* ---- the API's refusals ------------------------------------------------------------------ */

T(refusals)
{
	struct surface_diginet *d = (struct surface_diginet *)1;
	CHK(surface_diginet_open_io(&d, NULL, on_desk, NULL) == SURFACE_E_ARG && d == NULL);
	CHK(surface_diginet_open(&d, -1, on_desk, NULL) == SURFACE_E_ARG && d == NULL);
	start();
	auto_attach = 0;
	struct surface *s = (struct surface *)1;
	CHK(surface_diginet_attach(D, DESK, NULL, NULL, &s) == SURFACE_E_ARG && s == NULL);
	run(500 * MS);
	CHK(appeared == 1);
	CHK(surface_diginet_attach(D, OTHER, NULL, NULL, &s) == SURFACE_E_ARG);
	CHK(surface_diginet_attach(D, DESK, NULL, NULL, &s) == SURFACE_OK && s);
	struct surface *again = NULL;
	CHK(surface_diginet_attach(D, DESK, NULL, NULL, &again) == SURFACE_OK && again == s);
	CHK(surface_diginet_ring_mask(D, s, 24, 1) == SURFACE_E_ARG);
	CHK(surface_diginet_ring_mask(D, s, 0, 1) == 0);   /* not online yet */
	surface_diginet_detach(D, s, now);
	stop();
}

int main(void)
{
	RUN(online_replays_board);
	RUN(touched_reports_echo_setpoint);
	RUN(release_echoes_once_no_snap);
	RUN(host_move_drives_untouched_cap);
	RUN(inputs_map_up);
	RUN(desk_deaf_is_a_warning);
	RUN(foreign_is_one_warning);
	RUN(desk_restart_repaints);
	RUN(host_restart_repaints_in_2s);
	RUN(gone_and_back);
	RUN(back_under_an_unknown_name);
	RUN(back_as_another_model);
	RUN(detached_while_another_name_refuses_attach);
	RUN(refusals);
	if (fails) {
		fprintf(stderr, "test_diginet: %d check(s) failed\n", fails);
		return 1;
	}
	printf("test_diginet: ok\n");
	return 0;
}

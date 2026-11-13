/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * libsurface: the device behaviour of a motorised control surface, written once for every
 * surface and every host. The motor-fader policy (never fight the hand), touch with its
 * self-heal and release debounce, the bank window, the board shadow and its replay, the meter
 * step and ring rendering. Every fader is treated the same.
 *
 * What it is not: it speaks no wire (a backend encodes, through struct surface_backend_ops),
 * and it knows no mixing semantics. A host names what a strip shows by an opaque
 * surface_target, maps a key id to a verb in its own table, and owns names, colours and
 * dB laws. The design is FreeMixer/libdiginet#29, sections 4 and 5.
 *
 * No thread, no clock: every call that needs time takes the caller's now_us, and
 * surface_deadline() / surface_tick() run the timers, as libdiginet's session does. One
 * allocation, in surface_create(); nothing after it.
 */
#ifndef SURFACE_H
#define SURFACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURFACE_VERSION_MAJOR  0
#define SURFACE_VERSION_MINOR  2
#define SURFACE_VERSION_PATCH  2
#define SURFACE_VERSION_STRING "0.2.4"
/* The soname's major: libsurface.so.SURFACE_ABI. packaging/libsurface.spec and debian/ carry
 * copies; tests/conformance-packaging.sh holds them to this, and the version to VERSION. */
#define SURFACE_ABI            0

/* ---- The timing constants, declared once (tests/constants-check.sh holds that) -------------
 *
 * Each value carries the reason it has. They came with the policy from the X-Touch adapter
 * that drove a desk on a rig for months, and the history in these comments is that rig's.
 */

/*
 * Implicit-touch window (ms). Divergent inbound fader reports mean the operator physically has
 * the cap even when the capacitive touch message is momentarily absent: the X-Touch's sensor
 * drops out intermittently mid-move and lags at grab-start. While the window is open, served
 * value -> motor drive is held off for that fader so the motor never fights a moving hand; it
 * is refreshed on every divergent report and lapses once reports stop, restoring normal
 * tracking. This is the canonical DAW "implicit touch" (bjoluc/cubase-mcu-midiremote's
 * motorized-fader handling; DrivenByMoss's MCU touch logic): a per-move debounce keyed to real
 * fader motion, not a fixed grace/reconcile clock.
 *
 * 600 (was 200) since 2026-08-09: the operator reported the fader FIGHTING the hand on the rig,
 * twice. A slow, deliberate ride produces inter-report gaps well past 200 ms, and each gap
 * closed the window; the lagged echo burst then drove the motor into the moving finger, which
 * reads as exaggerated mechanical resistance. 600 covers a musician's slowest deliberate
 * gesture; the cost is only that an EXTERNAL move (another client) reaching a just-released cap
 * is deferred by at most this long, which the eye does not notice and the hand cannot.
 */
#define SURFACE_IMPLICIT_TOUCH_MS 600

/*
 * Served value -> motor drive coalescing (trailing). A gesture's echo comes back as a BURST of
 * frames; driving the motor once per frame walks the cap through every stale intermediate
 * position: with a working touch sensor that only costs noise after release, and with a flaky
 * one it is the fight itself. The motor only ever wants the LATEST truth, so served frames
 * update the target and one trailing deadline drives it once the burst quiets. This is the
 * debouncing asked for, applied on the served -> motor side; surface -> host stays
 * per report, because the engine riding every report is what makes the fader feel connected to
 * the sound.
 */
#define SURFACE_MOTOR_COALESCE_MS 80

/*
 * Fader-touch self-heal timeout (ms), for a surface whose geometry sets touch_hold_ms to it. The
 * capacitive touch flag is normally cleared by the release edge, but that edge can be LOST
 * (dropped across a host/MIDI reconnect), leaving a strip stuck "touched" so its motor never
 * tracks the host again (observed live: a fader changed the audio from another client but its
 * cap did not move; only a bridge restart cleared it). So the flag also lapses this long after
 * the last touch message for the strip: a genuinely held cap keeps refreshing it (the X-Touch
 * repeats the touch NOTE while held, and a moving cap additionally holds the implicit-touch
 * window open), while a lost release self-heals once the finger is long gone. It may sit below
 * SURFACE_IMPLICIT_TOUCH_MS: a moving cap is held by the implicit window, not by this flag, and
 * a still one by the repeated NOTE.
 *
 * A surface that does not repeat its touch message while held (the Control|24 is assumed not
 * to, libdiginet#29 Q3) sets touch_hold_ms to 0: edges only. That is safe there because DigiNet
 * delivers desk data reliably (the desk retransmits until ACKed), so the lost release this
 * heals does not happen.
 */
#define SURFACE_TOUCH_TIMEOUT_MS 300

/*
 * Touch release debounce on the Control|24 (ms). Its touch sense CHATTERS: it drops out for
 * 30-240 ms every few positions during a move (libdiginet docs/protocol.md R4). A release
 * becomes a release only if no touch-on follows within this window; a touch-on inside it cancels
 * it, and the host sees one continuous touch.
 */
#define SURFACE_C24_RELEASE_DEBOUNCE_MS 250

/*
 * Keep-alive for a HELD meter level (ms), on a surface that decays a lit meter on its own. An
 * MCU strip meter treats a channel-pressure level as a momentary peak and lets it fall unless
 * the host repeats it (no rate is published in the Logic Control/MCU documentation, and the
 * X-Touch's own fall has not been measured). A host that sends a meter only when its LED count
 * changes must therefore re-send the held count inside that window. 250 ms is the measure-free
 * default: short enough that a held level never visibly sags (observed MCU fall is on the order
 * of 300 ms per segment), four messages a second per strip instead of ~19. The Control|24 holds
 * a lit meter for 10 s and more (R7), so its geometry sets 0: no keep-alive.
 */
#define SURFACE_METER_KEEPALIVE_MS 250

/*
 * The hysteresis of a stepped (LED-ladder) meter, as a fraction of ONE step: a level must move
 * past the current step's own rounding edge by this much more before the step changes, so a
 * steady source sitting on a segment edge holds one LED count instead of toggling N <-> N-1
 * every frame (2026-09-25: X-Touch meters redrawn every frame look like blinking).
 */
#define SURFACE_METER_STEP_HYSTERESIS 0.25

/* ---- Sizes --------------------------------------------------------------------------------- */

#define SURFACE_MAX_STRIPS         32  /* fader strips; a master fader is one more */
#define SURFACE_MAX_SCRIBBLE_ROWS  2
#define SURFACE_MAX_SCRIBBLE_CHARS 16
#define SURFACE_MAX_COUNTER        32  /* bytes of counter text, dots included */
#define SURFACE_MAX_METER_SIDES    2
#define SURFACE_MAX_RING_LEDS      31

/* ---- Return codes: 0 is success, every failure negative -------------------------------- */
enum surface_status {
	SURFACE_OK     = 0,
	SURFACE_E_ARG  = -1,  /* a NULL pointer, a strip off the geometry, an impossible value */
	SURFACE_E_FULL = -2,  /* a fixed table is full (targets, LEDs) */
	SURFACE_E_NOMEM = -3, /* surface_create() could not allocate */
};

/* A host's opaque name for what a strip shows (a channel, a chain). 0 = nothing. */
typedef uint32_t surface_target;

/* What a device is: everything below the host that differs between surfaces. */
struct surface_geometry {
	uint8_t  strips;               /* 8 X-Touch, 24 Control|24, 8 ProControl */
	uint8_t  has_master;           /* a dedicated master fader: strip index `strips` */
	uint16_t fader_max;            /* 16383 MCU pitch bend, 1023 DigiNet */
	uint8_t  motor_epsilon_q;      /* motor no-op tolerance, in device quanta (see below) */
	uint8_t  meter_segments;       /* 12 X-Touch, 14 Control|24 (R7) */
	uint8_t  meter_sides;          /* 1 X-Touch, 2 Control|24 */
	uint32_t meter_keepalive_ms;   /* SURFACE_METER_KEEPALIVE_MS, or 0: the device holds a level */
	uint8_t  ring_leds;            /* 11 MCU, 15 DigiNet */
	uint8_t  scribble_rows, scribble_chars;   /* 2 x 7 X-Touch, 1 x 4 Control|24 */
	uint8_t  counter_digits;       /* 10 X-Touch timecode, 8 Control|24 */
	uint32_t touch_hold_ms;        /* self-heal of a lost release; 0 = edges only */
	uint32_t touch_release_debounce_ms; /* 0 X-Touch, SURFACE_C24_RELEASE_DEBOUNCE_MS Control|24 */
	uint8_t  reports_motor_moves;  /* the device reports where its motor put a cap (X-Touch) */
	uint8_t  holds_setpoint;       /* the motor holds its own set-point: echo the hand (C24) */
};

/* The geometries this library knows. */
enum surface_model {
	SURFACE_MODEL_XTOUCH = 1,
	SURFACE_MODEL_CONTROL24,
};

/* Fills *g with the model's geometry; SURFACE_E_ARG for an unknown model. */
int surface_geometry_preset(enum surface_model m, struct surface_geometry *g);

enum surface_led { SURFACE_LED_OFF = 0, SURFACE_LED_ON, SURFACE_LED_FLASH };

/* How a ring shows a value; see surface_ring_mask(). */
enum surface_ring_mode {
	SURFACE_RING_OFF = 0,
	SURFACE_RING_DOT,      /* v 0..1: one LED at round(v * leds), 0 = all off */
	SURFACE_RING_PAN,      /* v -1..1: one LED, centred */
	SURFACE_RING_FILL,     /* v 0..1: from the left up to the value */
	SURFACE_RING_BIPOLAR,  /* v -1..1: from the centre out to the value (boost/cut) */
	SURFACE_RING_SPREAD,   /* v 0..1: from the centre out both ways (width) */
};

enum surface_event_kind {
	SURFACE_EV_ONLINE = 1,      /* device ready, board replayed */
	SURFACE_EV_OFFLINE,         /* session or port lost; device still present */
	SURFACE_EV_FADER_MOVED,     /* strip, target, position 0..1 (a hand, never a motor report) */
	SURFACE_EV_FADER_TOUCHED,   /* strip, target (debounced) */
	SURFACE_EV_FADER_RELEASED,  /* strip, target, position: where the hand left it */
	SURFACE_EV_KEY,             /* key, on = pressed */
	SURFACE_EV_VPOT,            /* strip, target, delta */
	SURFACE_EV_JOG,             /* delta */
	SURFACE_EV_WARNING,         /* code + text */
};

enum surface_warning {
	SURFACE_WARN_DESK_DEAF = 1,     /* raised by a backend (libdiginet#29 section 3.3) */
	SURFACE_WARN_FOREIGN,           /* raised by a backend: another host owns the device */
};

struct surface_event {
	enum surface_event_kind kind;
	uint8_t strip, on;
	surface_target target;
	double position;
	uint16_t key;               /* backend key id: MCU note, or DigiNet (button << 8 | zone-or-strip) */
	int16_t delta;
	int code;
	const char *text;
};
typedef void (*surface_event_fn)(void *ctx, const struct surface_event *ev);

/*
 * The backend: the device driver under libsurface. libsurface calls it for each element it
 * writes; every op returns 1 written, 0 skipped, or a negative error. ring gets the LED mask,
 * bit 0 the leftmost LED. colour may be NULL (a device with no scribble colour).
 *
 * An op may call back into libsurface (a write, an input); libsurface keeps no pointer across
 * an op call that such a call could invalidate.
 */
struct surface_backend_ops {
	int (*fader)(void *b, unsigned strip, uint16_t raw);
	int (*led)(void *b, uint16_t key, enum surface_led st);
	int (*ring)(void *b, unsigned strip, uint32_t mask);
	int (*meter)(void *b, unsigned strip, unsigned side, unsigned segments);
	int (*scribble)(void *b, unsigned strip, unsigned row, const char *text, size_t n);
	int (*counter)(void *b, const char *text);
	int (*colour)(void *b, const uint8_t *per_strip, size_t n);
	/* A device that renders its rings itself (an MCU surface draws mode, position and centre)
	 * takes surface_ring_code()'s code as it is. May be NULL: surface_ring_code() then refuses. */
	int (*ring_code)(void *b, unsigned strip, uint32_t code);
};

struct surface_config {
	struct surface_geometry geometry;
	const struct surface_backend_ops *ops;  /* copied */
	void *backend;
	surface_event_fn event;                 /* may be NULL */
	void *event_ctx;
	uint16_t max_targets;  /* motor states and bank list length; 0 = 256 */
	uint16_t max_leds;     /* keys in the LED shadow; 0 = 256 */
};

struct surface;   /* opaque */

/* The one allocation. *out is NULL on failure. A new surface is offline until
 * surface_in_online(). */
int  surface_create(struct surface **out, const struct surface_config *c);
void surface_destroy(struct surface *s);
const struct surface_geometry *surface_geometry_of(const struct surface *s);
int  surface_online(const struct surface *s);

/* ---- The bank window (FaderBank) -------------------------------------------------------------
 * A movable window of `strips` targets over the host's ordered list. Every move clears the
 * meter steps: a strip's ballistic hold belongs to the target it showed. The host repaints
 * what the new strips show (surface_fader_paint and the element writes). */
int  surface_bank_set(struct surface *s, const surface_target *list, size_t n); /* kept count */
int  surface_bank_step(struct surface *s, int delta);       /* +-strips a bank, +-1 a channel: 1 moved */
int  surface_bank_show(struct surface *s, surface_target t); /* bring t's bank into view: 1 moved */
int  surface_bank_jump(struct surface *s, surface_target t); /* put t on strip 0: 1 moved */
int  surface_bank_offset(const struct surface *s);
surface_target surface_strip_target(const struct surface *s, unsigned strip); /* master included */
int  surface_target_strip(const struct surface *s, surface_target t);  /* bank strip, -1 off-window */
void surface_master_set(struct surface *s, surface_target t);  /* what the master fader shows */

/* ---- Fader writes (host -> device), through the motor policy ------------------------------- */

/* A served value (the host's truth) for t, 0..1. The one motor rule. A served frame older than
 * the hand's last move of t never becomes its target (never fight the hand), and "older" is read
 * from the frame's send time: sent_us, on the caller's clock, is when the host committed the
 * value (for the echo of a surface move, the time that move reached it). surface_fader_served()
 * is surface_fader_served_at() with sent_us = now_us: a host that gives no send time is ordered
 * by arrival, which cannot see an echo the host reordered behind a newer one. */
void surface_fader_served(struct surface *s, surface_target t, double pos, uint64_t now_us);
void surface_fader_served_at(struct surface *s, surface_target t, double pos, uint64_t sent_us,
			     uint64_t now_us);
/* The host refused the surface's own move of t: the echo it awaited never comes. */
void surface_fader_refused(struct surface *s, surface_target t);
/* A repaint of t at pos (bank move, page change): held inside an implicit-touch window,
 * else seeded and written even where the motor was last driven, never under a hand. */
void surface_fader_paint(struct surface *s, surface_target t, double pos, uint64_t now_us);
/* One strip's repaint, for a host that paints strip by strip (t may also sit on the master). */
void surface_fader_paint_strip(struct surface *s, unsigned strip, double pos, uint64_t now_us);
/* While on, served frames for t only move its target (the caps show something else, a send
 * layer); the host writes those caps with surface_strip_fader(). A move of a cap showing t goes up
 * as SURFACE_EV_FADER_MOVED and leaves t's motor state alone, and a release drives nothing: what
 * the cap carries now is the host's. */
void surface_fader_suspend(struct surface *s, surface_target t, int on);
/* A cap written outside the policy (a layer, a flip); skipped under a hand. */
int  surface_strip_fader(struct surface *s, unsigned strip, double pos, uint64_t now_us);

/* Whether a fader report at raw is the motor reporting where it put the cap, not a hand: what
 * surface_in_fader() would drop. For a host that routes some moves away from the surface (a learned
 * control) and must not take a motor report for a gesture. */
int  surface_fader_is_report(const struct surface *s, unsigned strip, uint16_t raw, uint64_t now_us);

/* No finger on any cap, whatever the flags say (a host that reconnected: a release may have been
 * lost across the gap). surface_in_online() does this as part of the replay. */
void surface_touch_reset(struct surface *s);

/* ---- Element writes (host -> device): latest wins, kept in the board shadow ------------- */
int surface_led(struct surface *s, uint16_t key, enum surface_led st);
int surface_ring(struct surface *s, unsigned strip, enum surface_ring_mode m, double v);
/* A ring in the device's own code (an MCU ring's mode, position and centre bit), for a device that
 * renders its ring itself; kept in the board shadow and replayed as it is. SURFACE_E_ARG when the
 * backend takes no code. The last of surface_ring() and surface_ring_code() for a strip is shown. */
int surface_ring_code(struct surface *s, unsigned strip, uint32_t code);
int surface_meter(struct surface *s, unsigned strip, unsigned side, double fill, uint64_t now_us);
/* The next level paints at once, on every strip: a strip's hold belongs to what it showed. */
void surface_meters_reset(struct surface *s);
int surface_scribble(struct surface *s, unsigned strip, unsigned row, const char *utf8);
int surface_counter(struct surface *s, const char *text);
int surface_colour(struct surface *s, const uint8_t *per_strip, size_t n);
int surface_blank_strip(struct surface *s, unsigned strip);

/* ---- Inputs (backend -> libsurface) ----------------------------------------------------- */
void surface_in_fader(struct surface *s, unsigned strip, uint16_t raw, uint64_t now_us);
void surface_in_touch(struct surface *s, unsigned strip, int on, uint64_t now_us);
void surface_in_key(struct surface *s, uint16_t key, int pressed, uint64_t now_us);
void surface_in_vpot(struct surface *s, unsigned strip, int delta, uint64_t now_us);
void surface_in_jog(struct surface *s, int delta, uint64_t now_us);
void surface_in_online(struct surface *s, uint64_t now_us);   /* replays the board */
void surface_in_offline(struct surface *s, uint64_t now_us);
void surface_in_warning(struct surface *s, int code, const char *text, uint64_t now_us);
/* A backend wrote raw to strip's cap itself: record it as the cap's commanded position. */
void surface_note_commanded(struct surface *s, unsigned strip, uint16_t raw);

/* ---- Time -------------------------------------------------------------------------------- */
/* The latest time at which tick must run again (now_us when it has work now), or UINT64_MAX
 * when nothing waits. */
uint64_t surface_deadline(const struct surface *s, uint64_t now_us);
int      surface_tick(struct surface *s, uint64_t now_us);

/* ---- Pure helpers ------------------------------------------------------------------------ */
/* A display fill 0..1 -> a step 0..steps with hysteresis against prev (-1 = none: plain
 * rounding). The meter law shared with every host that draws the same bar. */
unsigned surface_meter_step(int prev, double fill, unsigned steps, double hysteresis);
/* A ring value rendered into `leds` bits, bit 0 the leftmost. */
uint32_t surface_ring_mask(enum surface_ring_mode m, double v, unsigned leds);
const char *surface_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif

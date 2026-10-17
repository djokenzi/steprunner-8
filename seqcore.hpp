/* =============================================================================
 * seqcore.hpp - StepRunner-8 core: a trig sequencer, no VST / ALSA in here.
 *
 *   8 independent lines ("tracks"), 64 steps each (default notes 36..43 = pads A01..A08)
 *   per track: length 1..64, speed multiplier, MIDI channel, note number, default velocity,
 *              default gate, mute
 *   per step : trig on/off, trig condition, probability, and parameter locks: velocity, gate length
 *              (0 = no lock, use the track default), pan (on the track's PAN_CC) and five CC locks (the track's own CC numbers); 0 = no lock, nothing sent
 *   a step sends only the CCs it has locked; after it, a CC with a base value goes back to that value (v1.37)
 *   random generators (randomize()) write trigs, velocity / length locks or one CC's locks into one track
 *
 * Time is in quarter notes ("ppq"). The wrapper calls process(start, end) once per audio block with a
 * free-running position, so a short looping MPC sequence does not restart the 64 steps or the A:B counters.
 * Everything here is allocation-free and safe on the audio thread.
 * ========================================================================== */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace seqcore {

enum { NTRACKS = 8, NSTEPS = 64, NCC = 5 };
/* Each track sends its five CC locks on its own block of five consecutive controller numbers, ccbase .. ccbase + 4, so
 * all tracks can share one MIDI channel. Defaults: T1-T6 sit in ranges the MIDI spec leaves undefined (20-31, 85-90,
 * 102-119), T7 and T8 in the general purpose ones; none touches bank select, mod wheel, volume, pan, pedals or the
 * channel mode messages. CC_BASE_OLD: what every track used before v1.11.0, and what older projects load with. */
static const uint8_t CC_BASE_DEFAULT[8] = {20, 25, 85, 102, 107, 112, 14, 80};
/* v1.93: each track's pan goes out on a controller of its own (all MIDI "undefined" ones), mapped to its pad's Pan (517)
 * by CREATE DEFAULT MAP. Until v1.92 it was CC 10, which MPC takes as the whole track's pan (all pads at once). */
static const uint8_t PAN_CC[8] = {30, 31, 90, 117, 118, 119, 3, 9};
enum { CC_BASE_OLD = 20, CC_BASE_MAX = 127 - (NCC - 1) };

/* trig conditions as saved in projects: fixed codes, append only. The order the LIST shows them in is COND_ORDER. */
enum { C_NONE = 0, C_FILL, C_NFILL, C_PRE, C_NPRE, C_NEI, C_NNEI, C_1ST, C_N1ST, C_AB0 };
enum { NCOND = C_AB0 + 35 };   /* A:B for B = 2..8, A = 1..B */

static inline bool cond_ab(int c, int *a, int *b) {   /* condition index -> A, B */
    int k = c - C_AB0;
    if (k < 0) return false;
    for (int B = 2; B <= 8; B++) {
        if (k < B) { *a = k + 1; *b = B; return true; }
        k -= B;
    }
    return false;
}
static inline void cond_name(int c, char *out, size_t n) {
    static const char *const base[] = {"--", "FILL", "NO FILL", "PRE", "NO PRE", "NEI", "NO NEI", "1ST", "NO 1ST"};
    int a, b;
    if (c >= 0 && c < C_AB0) std::snprintf(out, n, "%s", base[c]);
    else if (cond_ab(c, &a, &b)) std::snprintf(out, n, "%d:%d", a, b);
    else std::snprintf(out, n, "?");
}

/* list position -> condition code: "--", 1ST, NO 1ST, the A:B conditions, then FILL / PRE / NEI and their negations */
static inline int cond_at(int pos) {
    static const uint8_t tail[] = {C_FILL, C_NFILL, C_PRE, C_NPRE, C_NEI, C_NNEI};
    if (pos <= 0) return C_NONE;
    if (pos == 1) return C_1ST;
    if (pos == 2) return C_N1ST;
    if (pos < 3 + 35) return C_AB0 + (pos - 3);
    return pos - 38 < 6 ? (int)tail[pos - 38] : (int)C_NONE;
}
static inline int cond_pos(int code) {
    for (int p = 0; p < NCOND; p++) if (cond_at(p) == code) return p;
    return 0;
}

static const double SPEEDS[] = {0.125, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};   /* append only */
enum { NSPEEDS = 7, SPEED_1X = 4, SHUFFLE_MAX = 80 };
enum { DIR_FWD = 1, DIR_BWD = 2, DIR_PP = 3 };

struct Step {
    uint8_t on;     /* trig */
    uint8_t cond;   /* C_* */
    uint8_t prob;   /* 0..100 % */
    uint8_t vel;    /* lock: 0 = none, 1..127 */
    uint8_t len;    /* lock: 0 = none, 1..64 quarter-steps (4 = one step) */
    uint8_t cc[NCC];   /* locks for CC ccbase + j of the track: 0 = none, 1..128 = CC value 0..127 */
    uint8_t pan;       /* lock: 0 = none, 1..128 = pan value 0..127 (sent on PAN_CC of the track), 64 = centre */
    int8_t micro;      /* TIMING (v1.77): -23..+23 24ths of a step off the grid, 0 = on the grid */
    uint8_t ratch;     /* RATCHET (v1.77): 0..7 = 1..8 hits spread over the step */
    uint8_t rres;      /* RATCHET RESOLUTION (v2.1): 0 AUTO = the hits spread over the step; 1..5 = 1/16, 1/32, 1/48, 1/64,
                        * 1/80: that many hits per step, so more hits than that go on over the next steps */
};
enum { RRES_MAX = 5 };
enum { MICRO_MAX = 23, RATCH_MAX = 8, SCATTER_MAX = 8, HUMAN_T = 4, HUMAN_V = 16, ECHO_N = 3, ECHO_Q = 192, GHOST_PCT = 25, AFILL_PCT = 50, AFILL_VEL = 80 };
static const double ECHO_DT = 0.5;    /* ECHO (v1.91): the repeats are an eighth note apart */
static const double FADE_BEATS = 4.0; /* FADE OUT (v1.91): one 4/4 bar down to silence */
static const double BUILD_BEAT = 1.0; /* BUILD (v1.91): one hit more every beat, up to RATCH_MAX */
enum { STEP_BYTES_V2 = 5 + NCC, STEP_BYTES = STEP_BYTES_V2 + 1, CC_PAN = 10 };   /* saved as: on cond prob vel len cc[5] pan (pan since chunk TSq3) */

struct Track {
    Step st[NSTEPS];
    uint8_t len;      /* 1..64 */
    uint8_t speed;    /* index into SPEEDS */
    uint8_t ch;       /* 0..15 */
    uint8_t num;      /* note number */
    uint8_t vel;      /* default velocity 1..127 */
    uint8_t gate;     /* default gate, 1..64 quarter-steps */
    uint8_t mute;
    uint8_t rnd_dens, rnd_min, rnd_max;   /* this track's random generator: density %, value range 0..127 */
    uint8_t shuf;     /* 1 = this track follows the global shuffle */
    uint8_t dir;      /* play direction (v1.76): DIR_FWD 1, DIR_BWD 2, both = back and forth; never 0 */
    uint8_t panb;     /* the pad's own pan + 1, read from the project (v1.93); 0 = not known (the centre, 64) */
    uint8_t ccbase;   /* first of the five CC numbers this track's CC locks are sent on, 0..CC_BASE_MAX */
    uint8_t ccn[NCC]; /* each CC lock's own controller number + 1 (EDIT CC); 0 = ccbase + j */
    uint8_t ccb[NCC]; /* base value + 1 of each CC (what the controlled parameter is set to); 0 = none: locks are never taken back */
    /* Euclidean generator (Seq::euclid_*): while eu_on, the trigs of steps 1..len are generated from these */
    uint8_t eu_on;
    uint8_t eu_pl1, eu_pl2;   /* pulses of generator 1 / 2, 0..64 (capped at the track length) */
    uint8_t eu_ro1, eu_ro2;   /* rotation of generator 1 / 2, in steps */
    uint8_t eu_tro;           /* rotation of the combined pattern */
    uint8_t eu_op;            /* how the two generators combine: E_OR, E_AND, E_XOR, E_SUB */
    uint8_t eu_len;           /* length of the generated rhythm, 1..64, repeated over the track; 0 = the track's own length */
    uint64_t eu_backup;       /* the trigs (bit k = step k) from before it was enabled, for Discard */
    /* runtime */
    int64_t next_k;   /* next absolute step index to fire */
    int cur;          /* play-head step, -1 before the first one */
    bool evaluated, last_res;   /* last conditional trig's outcome (PRE / NEI) */
    bool sounding;
    int rt_left;      /* ratchet hits still to come on this step; then rt_at, every rt_iv, each rt_gate long, velocity rt_vel */
    double rt_at, rt_iv, rt_gate; uint8_t rt_vel;
    int64_t ph;       /* the phase of the track's path (v1.80): REVERSE turns round where it is, RESTART puts it on step 1 */
    bool backw;       /* REVERSE: the path runs the other way from ph */
    int64_t sc_k; int sc_v;   /* SCATTER: the step it was drawn for, and the draw (24ths of a step) */
    uint8_t snd_note, snd_ch;
    double off_at;
};

struct Event { uint8_t status, d1, d2; };

struct Seq {
    Track t[NTRACKS];
    uint32_t gen_rng;                     /* their own RNG, separate from playback probability */
    bool fill;
    /* PERFORM tab switches (none of them is saved):
     *   loopn    4 / 2 / 1: every track keeps repeating the last n steps it played before the loop was switched on
     *            (0 = off). The song position runs on underneath, so switching it off drops every track back where it
     *            would have been. Changing n while it is on keeps the same end point.
     *   no_locks every trig plays plain: no conditions, no probability, no velocity / length / CC / pan locks
     *   rnd_step every track plays a random one of its steps instead of the next
     *   loopn 8: LOOP 8 (v1.79, the first pad of the second row); reverse: REVERSE (v1.79) */
    int loopn;
    bool no_locks, rnd_step;
    bool reverse = false;   /* REVERSE pad (v1.79): every track plays the other way; v1.80: from the step it is on (set_reverse) */
    bool roll = false;      /* ROLL pad (v1.80): every trig at least 4 hits (as RATCHET 4) */
    bool scatter = false;   /* SCATTER pad (v1.80): every trig a little off the grid at random, up to SCATTER_MAX 24ths */
    /* v1.91 pads (none saved; t0s on the tracks' clock):
     *   humanize  every trig a little off the grid (up to HUMAN_T 24ths) and its velocity up to +-HUMAN_V, at random
     *   build     from when it is switched on, every trig gets one hit more each BUILD_BEAT (as RATCHET), up to RATCH_MAX,
     *             and its velocity climbs from 60 % to full over the same time
     *   echo      every trig is repeated ECHO_N times, ECHO_DT apart, each one at 60 % of the one before
     *   fade      from when it is switched on, the velocity goes down to nothing over FADE_BEATS, then stays silent */
    bool humanize = false, build = false, echo = false, fade = false;
    /* v1.94 pads (not saved):
     *   ghost     every empty step plays a quiet hit (GHOST_PCT % of the track's velocity, a short note)
     *   poly      track n plays only its first 16 - n steps (T1 16, T2 15 ... T8 9; never longer than its own length),
     *             forwards, going on from the step it was on; off: back where it would have been
     *   downbeat  only the trigs on steps 1, 5, 9, 13 ... play */
    bool ghost = false, poly = false, downbeat = false;
    /* v2.2 pads (not saved):
     *   alt_bar   every second bar (4 beats) is silent, counted from the bar it was switched on in (that one plays)
     *   auto_fill on the empty steps of the last 4 of each track's length, an extra hit at random (AFILL_PCT % of them,
     *             at AFILL_VEL % of the track's velocity) */
    bool alt_bar = false, auto_fill = false;
    /* v1.2.0 STOP CC pad (not saved): while it is lit no CC goes out (locks, base values, pan, PERFORM ALL); notes
     * play on. Switched off: every track's base values and pan rest go out again on its next step (set_stop_cc). */
    bool stop_cc = false;
    void set_stop_cc(bool on) {
        if (!on && stop_cc)
            for (int i = 0; i < NTRACKS; i++) {
                for (int j = 0; j < NCC; j++) cc_dirty[i][j] = true;
                pan_dirty[i] = t[i].panb != 0;
            }
        stop_cc = on;
    }
    int64_t alt_b0 = 0;
    void set_alt(bool on, double song) { if (on && !alt_bar) alt_b0 = (int64_t)std::floor(virt(song) / 4.0); alt_bar = on; }
    int64_t pl_k0[NTRACKS] = {0}; int pl_s0[NTRACKS] = {0};
    static int poly_len(const Track &tr, int ti) { return std::max(1, std::min(eff_len(tr), 16 - ti)); }
    void set_poly(bool on) {
        if (on && !poly) for (int i = 0; i < NTRACKS; i++) {
            int64_t lp = 0;
            pl_k0[i] = t[i].next_k;
            pl_s0[i] = path_step(t[i], path_pos(t[i], loop_k(i, t[i].next_k), lp)) % poly_len(t[i], i);
        }
        poly = on;
    }
    double build_t0 = 0.0, fade_t0 = 0.0;
    struct Pending { double at; uint8_t status, d1, d2; };
    Pending eq[ECHO_Q]; int neq = 0;   /* ECHO: notes to come (on and off), on the tracks' clock */
    void set_build(bool on, double song) { if (on && !build) build_t0 = virt(song); build = on; }
    void set_fade(bool on, double song) { if (on && !fade) fade_t0 = virt(song); fade = on; }
    void echo_add(double at, uint8_t st, uint8_t d1, uint8_t d2) { if (neq < ECHO_Q) eq[neq++] = Pending{at, st, d1, d2}; }
    /* HALF TIME / DOUBLE TIME pads (v1.80): the tracks run on a clock of their own, warp times the song's; it is the song's
     * again (and every track where it would be) when they are off. virt(): song position -> the tracks' clock. */
    double warp = 1.0, w_t0 = 0.0, w_v0 = 0.0;
    double virt(double t) const { return w_v0 + (t - w_t0) * warp; }
    void set_warp(double f, double song) {
        if (f == warp) return;
        const double v = virt(song);
        if (f == 1.0) { warp = 1.0; w_t0 = w_v0 = song; if (playing) for (int i = 0; i < NTRACKS; i++) align(t[i], song); return; }
        w_v0 = v; w_t0 = song; warp = f;
    }
    /* A CC lock was sent and its base value not yet: the next step without that lock sends the base once (v1.37.0).
     * So a lock on step 8 holds for step 8 only; while there are no locks nothing is sent at all. */
    bool cc_dirty[NTRACKS][NCC] = {};
    int64_t lp_k0[NTRACKS];   /* per track: the first step index that was not played normally */
    void set_loop(int n) { if (n && !loopn) for (int i = 0; i < NTRACKS; i++) lp_k0[i] = t[i].next_k; loopn = n; }
    void invert_mutes() { for (int i = 0; i < NTRACKS; i++) { t[i].mute = t[i].mute ? 0 : 1; if (t[i].mute) note_off(t[i]); } }
    void set_all_ch(int ch) { for (int i = 0; i < NTRACKS; i++) { note_off(t[i]); t[i].ch = (uint8_t)(ch & 15); } }
    bool same_ch() const { for (int i = 1; i < NTRACKS; i++) if (t[i].ch != t[0].ch) return false; return true; }
    void unmute_all() { for (int i = 0; i < NTRACKS; i++) t[i].mute = 0; }
    bool any_muted() const { for (int i = 0; i < NTRACKS; i++) if (t[i].mute) return true; return false; }
    void mute_toggle_all() {   /* something muted: unmute everything; nothing muted: mute everything */
        if (any_muted()) { unmute_all(); return; }
        for (int i = 0; i < NTRACKS; i++) { t[i].mute = 1; note_off(t[i]); }
    }
    /* PERFORM ALL (v1.53.0): one offset per parameter, added to every track's OWN value of
     * it, so the tracks keep their distance from each other; step locks are left alone and still win on their step.
     *   PF_VEL    the track velocity          PF_LEN  the track gate, in quarter-steps
     *   PF_CC0+j  the base value of the track's CC j (tracks with no base value have nothing to move)
     *   PF_PROB   the probability of the plain steps: 100 % and no condition. A step with its own probability or a
     *             condition is locked and is left alone (v1.54.0; before: every step). PF_PAN (back in v1.72.0) moves the
     *             pan of the trigs with no pan lock from the centre (64): every such trig sends its pan CC while it is not 0.
     * The pattern itself is not touched until perform_commit(), so perform_discard() gets the old state back.
     * Before v1.53.0 the offsets were added to the step locks instead. */
    int prob_eff(const Step &s) const { return (s.cond == C_NONE && s.prob >= 100) ? pf_cl(100 + pf(PF_PROB), 0, 100) : s.prob; }
    /* what the track's CC j rests on right now: its base value with the PERFORM offset, 0..127; -1: it has no base value */
    int base_eff(const Track &tr, int j) const { return tr.ccb[j] ? pf_cl(tr.ccb[j] + pf(PF_CC0 + j), 1, 128) - 1 : -1; }
    enum { PF_PROB = 0, PF_VEL, PF_LEN, PF_PAN, PF_CC0, PF_COUNT = PF_CC0 + NCC };
    bool pf_on = false;
    bool pan_dirty[NTRACKS] = {false};   /* a PERFORM pan went out on the track: the centre is sent again when it is back at 0 */
    int pf_off[PF_COUNT] = {0};
    static int pf_cl(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
    int pf(int k) const { return pf_on ? pf_off[k] : 0; }
    Track pf_bak[NTRACKS];   /* everything the tracks held when PERFORM ALL was switched on */
    void perform_begin() {
        if (pf_on) return;
        pf_undo_ok = false;
        std::memcpy(pf_bak, t, sizeof t);
        for (int k = 0; k < PF_COUNT; k++) pf_off[k] = 0;
        pf_on = true;
    }
    void pf_clear() { pf_on = false; for (int k = 0; k < PF_COUNT; k++) pf_off[k] = 0; }
    /* back to the state from perform_begin(): steps, locks, track setup, mutes, Euclidean, random settings.
     * Whatever was changed meanwhile, on either tab, is dropped. The play position carries on. */
    bool pf_undo_ok = false;   /* a COMMIT can still be taken back (until PERFORM ALL is switched on again, or a project is loaded) */
    void perform_discard() { if (pf_on) pf_restore(); pf_clear(); }
    /* UNDO: while performing the same as switching PERFORM ALL off; after COMMIT it takes the commit back,
     * together with anything edited since. One level, one shot. */
    void perform_undo() {
        if (pf_on) { perform_discard(); return; }
        if (pf_undo_ok) { pf_restore(); pf_undo_ok = false; }
    }
    void pf_restore() { restore_from(pf_bak, false); }
    /* Put back saved tracks (PERFORM ALL backup, undo history). The play position and what is sounding carry on;
     * keep_mute leaves the mutes as they are now. */
    void restore_from(const Track *src, bool keep_mute) {
        for (int i = 0; i < NTRACKS; i++) {
            Track now = t[i];
            uint8_t mute = keep_mute ? now.mute : src[i].mute;
            if (now.mute != mute || now.num != src[i].num || now.ch != src[i].ch) note_off(t[i]), now = t[i];
            double was = step_dur(now);
            t[i] = src[i];
            t[i].mute = mute;
            keep_runtime(t[i], now);
            double is = step_dur(t[i]);
            if (is != was) { t[i].next_k = (int64_t)std::ceil((double)now.next_k * was / is - 1e-9); if (t[i].next_k < 0) t[i].next_k = 0; }
        }
    }
    void perform_commit() {   /* write the offsets into the pattern, exactly as they were heard */
        if (pf_on) pf_undo_ok = true;
        if (pf_on) for (int ti = 0; ti < NTRACKS; ti++) {
            Track &tr = t[ti];
            for (int k = 0; k < NSTEPS; k++) {
                Step &s = tr.st[k];
                if (s.on) s.prob = (uint8_t)prob_eff(s);   /* only trigs, and only the plain ones (see prob_eff) */
                if (s.on && !s.pan && pf_off[PF_PAN]) s.pan = (uint8_t)(pf_cl(64 + pf_off[PF_PAN], 0, 127) + 1);   /* the pan heard: a lock on the trig */
            }
            tr.vel = (uint8_t)pf_cl(tr.vel + pf_off[PF_VEL], 1, 127);
            tr.gate = (uint8_t)pf_cl(tr.gate + pf_off[PF_LEN], 1, 64);
            for (int j = 0; j < NCC; j++) if (tr.ccb[j]) tr.ccb[j] = (uint8_t)pf_cl(tr.ccb[j] + pf_off[PF_CC0 + j], 1, 128);
        }
        pf_clear();
    }
    bool playing;
    uint32_t rng;
    Event out[1024];   /* v2.11: was 256; when full, events (note-offs too) were dropped */
    int nout;

    Seq() { init(); }

    void init() {
        static const uint8_t notes[NTRACKS] = {36, 37, 38, 39, 40, 41, 42, 43};   /* pads A01..A08 of an MPC drum program (v1.63; until v1.62: 37 36 42 82 40 38 46 44) */
        std::memset(t, 0, sizeof t);
        for (int i = 0; i < NTRACKS; i++) {
            Track &tr = t[i];
            tr.len = 16; tr.speed = SPEED_1X; tr.ch = 0; tr.gate = 4;
            tr.num = notes[i]; tr.vel = 100; tr.ccbase = CC_BASE_DEFAULT[i];
            tr.eu_pl1 = 4;
            tr.rnd_dens = 50; tr.rnd_min = 0; tr.rnd_max = 127; tr.shuf = 1; tr.dir = DIR_FWD;
            lp_k0[i] = 0;
            for (int s = 0; s < NSTEPS; s++) tr.st[s].prob = 100;
            tr.cur = -1;
        }
        shuffle = 50; loopn = 0; no_locks = false; rnd_step = false; gen_rng = 0x9E3779B9u;
        fill = false; playing = false; rng = 0x12345678u; nout = 0;
    }

    /* The controller number of a track's CC lock j: its own (EDIT CC), or the old block ccbase + j. */
    static int cc_num(const Track &tr, int j) {
        if (tr.ccn[j]) return (tr.ccn[j] - 1) & 127;
        return (tr.ccbase > CC_BASE_MAX ? (int)CC_BASE_MAX : (int)tr.ccbase) + j;
    }
    /* where a track is in its playback (not part of the pattern): kept when the pattern under it is swapped */
    static void keep_runtime(Track &d, const Track &now) {
        d.next_k = now.next_k; d.cur = now.cur; d.evaluated = now.evaluated; d.last_res = now.last_res;
        d.sounding = now.sounding; d.snd_note = now.snd_note; d.snd_ch = now.snd_ch; d.off_at = now.off_at;
        d.rt_left = now.rt_left; d.rt_at = now.rt_at; d.rt_iv = now.rt_iv; d.rt_gate = now.rt_gate; d.rt_vel = now.rt_vel;
        d.ph = now.ph; d.backw = now.backw; d.sc_k = now.sc_k; d.sc_v = now.sc_v;
    }
    /* TRACK EDIT copy: everything a track holds except its MIDI channel, note and CC numbers (and the mute). */
    void paste_track(int i, const Track &src) {
        if (i < 0 || i >= NTRACKS) return;
        Track now = t[i];
        double was = step_dur(now);
        t[i] = src;
        t[i].ch = now.ch; t[i].num = now.num; t[i].ccbase = now.ccbase; std::memcpy(t[i].ccn, now.ccn, sizeof now.ccn); std::memcpy(t[i].ccb, now.ccb, sizeof now.ccb); t[i].mute = now.mute; t[i].panb = now.panb;
        keep_runtime(t[i], now);   /* v2.11: the ratchet, REVERSE and SCATTER state too (a copy taken mid-ratchet fired a burst) */
        double is = step_dur(t[i]);
        if (is != was) { t[i].next_k = (int64_t)std::ceil((double)now.next_k * was / is - 1e-9); if (t[i].next_k < 0) t[i].next_k = 0; }
    }
    /* steps in one round of the track: back and forth takes 2 x len - 2 */
    static int round_len(const Track &tr) { const int n = eff_len(tr); return (tr.dir & DIR_PP) == DIR_PP && n > 1 ? 2 * n - 2 : n; }
    /* The length a track plays: its own, or the Euclidean rhythm's while the generator is on and that is longer. */
    static int eff_len(const Track &tr) {
        int n = tr.len < 1 ? 1 : tr.len > NSTEPS ? (int)NSTEPS : (int)tr.len;
        if (tr.eu_on && tr.eu_len > n) n = tr.eu_len > NSTEPS ? (int)NSTEPS : (int)tr.eu_len;
        return n;
    }
    static double step_dur(const Track &tr) { return 0.25 / SPEEDS[tr.speed < NSPEEDS ? (int)tr.speed : (int)SPEED_1X]; }

    void emit(uint8_t status, uint8_t d1, uint8_t d2) {
        if (stop_cc && (status & 0xF0) == 0xB0) return;   /* STOP CC (v1.2.0) */
        if (nout < (int)(sizeof out / sizeof out[0])) { out[nout].status = status; out[nout].d1 = d1; out[nout].d2 = d2; nout++; }
    }
    uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

    void note_off(Track &tr) {
        if (!tr.sounding) return;
        emit((uint8_t)(0x80 | tr.snd_ch), tr.snd_note, 0);
        tr.sounding = false;
    }
    void all_off() {
        for (int i = 0; i < NTRACKS; i++) { note_off(t[i]); t[i].rt_left = 0; }
        for (int q = 0; q < neq; q++) if ((eq[q].status & 0xF0) == 0x80) emit(eq[q].status, eq[q].d1, 0);   /* ECHO: nothing left hanging */
        neq = 0;
    }

    /* SHUFFLE (global): where the second step of every pair sits, in % of the pair: 50 = straight, 66 = triplet feel,
     * up to SHUFFLE_MAX. Each track shuffles on its own step grid (its own speed). */
    uint8_t shuffle;
    double step_time(const Track &tr, int64_t k) const {
        double dur = step_dur(tr);
        int sw = !tr.shuf ? 50 : shuffle < 50 ? 50 : shuffle > SHUFFLE_MAX ? (int)SHUFFLE_MAX : (int)shuffle;
        return (double)k * dur + ((k & 1) ? (sw - 50) / 50.0 * dur : 0.0);
    }
    /* (re)align a track's next step to position pos; used at start and when its speed changes */
    void align(Track &tr, double pos) {
        tr.next_k = (int64_t)std::ceil(pos / step_dur(tr) - 1e-9);
        if (tr.next_k > 0 && step_time(tr, tr.next_k - 1) >= pos - 1e-9) tr.next_k--;   /* a shuffled step that is still to come */
        if (tr.next_k < 0) tr.next_k = 0;
    }

    void start(double pos) {
        all_off();
        w_t0 = w_v0 = pos;   /* HALF / DOUBLE TIME go on from here */
        build_t0 = fade_t0 = pos; neq = 0;   /* BUILD / FADE OUT start again with the transport */
        for (int i = 0; i < NTRACKS; i++) {
            align(t[i], pos); t[i].cur = -1; t[i].evaluated = false; t[i].last_res = false; lp_k0[i] = t[i].next_k;
            t[i].backw = reverse; t[i].ph = reverse ? round_len(t[i]) - 1 : 0;   /* REVERSE from the start: the mirror */
            t[i].sc_k = -1;
        }
        if (poly) { poly = false; set_poly(true); }   /* POLY on at the start: from step 1 */
        playing = true;
    }
    void stop() {
        all_off();
        for (int i = 0; i < NTRACKS; i++) t[i].cur = -1;
        playing = false;
    }
    void set_speed(int track, int speed, double song) {
        const double pos = virt(song);
        Track &tr = t[track];
        if (speed < 0) speed = 0;
        if (speed >= NSPEEDS) speed = NSPEEDS - 1;
        if (tr.speed == speed) return;
        tr.speed = (uint8_t)speed;
        if (playing) align(tr, pos);
    }

    bool eval(int ti, const Step &s, int64_t loop) {
        Track &tr = t[ti];
        int prob = prob_eff(s);
        bool conditional = s.cond != C_NONE || prob < 100;
        bool r = true;
        int a, b;
        switch (s.cond) {
        case C_NONE: break;
        case C_FILL: r = fill; break;
        case C_NFILL: r = !fill; break;
        case C_PRE: r = tr.evaluated && tr.last_res; break;
        case C_NPRE: r = tr.evaluated && !tr.last_res; break;
        case C_NEI: r = ti > 0 && t[ti - 1].evaluated && t[ti - 1].last_res; break;
        case C_NNEI: r = ti > 0 && t[ti - 1].evaluated && !t[ti - 1].last_res; break;
        case C_1ST: r = loop == 0; break;
        case C_N1ST: r = loop != 0; break;
        default: r = cond_ab(s.cond, &a, &b) ? (loop % b) == (a - 1) : true; break;
        }
        if (r && prob < 100) r = (int)(rnd() % 100) < prob;
        /* PRE and NEI look at conditional trigs only; a PRE/NEI trig does not overwrite what it reads */
        if (conditional && s.cond != C_PRE && s.cond != C_NPRE && s.cond != C_NEI && s.cond != C_NNEI) { tr.evaluated = true; tr.last_res = r; }
        return r;
    }

    int64_t loop_k(int ti, int64_t k) const {   /* the LOOP pads: the index played instead of k */
        if (loopn && k >= lp_k0[ti]) {
            int64_t base = lp_k0[ti] >= loopn ? lp_k0[ti] - loopn : 0;
            k = base + (k - lp_k0[ti]) % loopn;
        }
        return k;
    }
    static int64_t fdiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
    /* where on its path (0 .. round - 1) the track is at index k: its phase, either way round */
    int64_t path_pos(const Track &tr, int64_t k, int64_t &loop) const {
        const int64_t P = round_len(tr), x = tr.backw ? tr.ph - k : k - tr.ph;
        loop = tr.backw ? -fdiv(x, P) : fdiv(x, P);
        if (loop < 0) loop = -loop;
        return ((x % P) + P) % P;
    }
    static int path_step(const Track &tr, int64_t m) {   /* the step at path position m: FWD, BWD, back and forth */
        const int len = eff_len(tr), d = tr.dir & DIR_PP;
        if (d == DIR_PP && len > 1) return (int)(m < len ? m : 2 * len - 2 - m);
        return d == DIR_BWD ? len - 1 - (int)m : (int)m;
    }
    /* the step absolute step k plays (LOOP pads and direction), no random step: for the TIMING of a step not yet fired */
    int step_of(int ti, int64_t k, int64_t &loop) const {
        if (poly) {   /* POLY (v1.94): forwards over the track's poly length, from where it was */
            const int pl = poly_len(t[ti], ti);
            const int64_t x = pl_s0[ti] + (loop_k(ti, k) - pl_k0[ti]);
            loop = fdiv(x, pl); if (loop < 0) loop = -loop;
            return (int)(((x % pl) + pl) % pl);
        }
        return path_step(t[ti], path_pos(t[ti], loop_k(ti, k), loop));
    }
    /* REVERSE (v1.80.0): each track turns round on the step it is on and goes on from there the other way */
    void set_reverse(bool on) {
        if (on == reverse) return;
        reverse = on;
        for (int i = 0; i < NTRACKS; i++) {
            Track &tr = t[i];
            const int64_t K = loop_k(i, tr.next_k);
            int64_t lp = 0;
            const int64_t mc = path_pos(tr, K - 1, lp);   /* where the step just played is on the path */
            if (on) { tr.ph = mc - 1 + K; tr.backw = true; }
            else { tr.ph = K - (mc + 1); tr.backw = false; }
        }
    }
    /* RESTART (v1.80.0): every track plays step 1 next (the end, going back, while REVERSE is on) */
    void restart() {
        for (int i = 0; i < NTRACKS; i++) {
            Track &tr = t[i];
            const int64_t K = loop_k(i, tr.next_k);
            tr.ph = tr.backw ? K + round_len(tr) - 1 : K;
        }
    }
    /* when step k sounds: its place on the grid (with the shuffle) moved by its TIMING (NO LOCKS and RANDOM STEP: on the grid) */
    double fire_time(int ti, int64_t k) {
        Track &tr = t[ti];
        const double g = step_time(tr, k);
        if ((no_locks || rnd_step) && !humanize) return g;
        int64_t loop = 0;
        int m = no_locks || rnd_step ? 0 : tr.st[step_of(ti, k, loop)].micro;
        if (scatter || humanize) {   /* drawn once per step (this is asked again every block until the step is played) */
            const int r = scatter ? SCATTER_MAX : HUMAN_T;
            if (tr.sc_k != k) { tr.sc_k = k; tr.sc_v = (int)(rnd() % (uint32_t)(2 * r + 1)) - r; }
            m = m + tr.sc_v < -MICRO_MAX ? -MICRO_MAX : m + tr.sc_v > MICRO_MAX ? MICRO_MAX : m + tr.sc_v;
        }
        return g + m * step_dur(tr) / 24.0;
    }
    void fire(int ti, int64_t k, double when) {
        Track &tr = t[ti];
        int len = eff_len(tr);
        int64_t loop = 0;
        int step = rnd_step ? (int)(rnd() % (uint32_t)len) : step_of(ti, k, loop);
        if (rnd_step) loop = k / len;
        tr.cur = step;
        const Step &s = tr.st[step];
        const bool lk = !no_locks;
        bool fires = false;
        if (s.on) fires = no_locks ? !tr.mute : (eval(ti, s, loop) && !tr.mute);
        if (downbeat && step % 4 != 0) fires = false;   /* DOWNBEAT ONLY (v1.94) */
        for (int j = 0; j < NCC; j++) {   /* locks first, then the note; a lock that is over goes back to its base */
            if (fires && lk && s.cc[j]) {
                emit((uint8_t)(0xB0 | (tr.ch & 15)), (uint8_t)cc_num(tr, j), (uint8_t)((s.cc[j] - 1) & 127));
                cc_dirty[ti][j] = true;
            } else if (cc_dirty[ti][j] && tr.ccb[j]) {
                emit((uint8_t)(0xB0 | (tr.ch & 15)), (uint8_t)cc_num(tr, j), (uint8_t)base_eff(tr, j));
                cc_dirty[ti][j] = false;
            }
        }
        if (alt_bar && (((int64_t)std::floor(when / 4.0) - alt_b0) & 1)) return;   /* ALT BAR (v2.2): a silent bar (the CC locks above still go out) */
        if (!fires && auto_fill && !s.on && !tr.mute && step >= len - 4 && (int)(rnd() % 100u) < AFILL_PCT) {   /* AUTO FILL (v2.2) */
            note_off(tr);
            emit((uint8_t)(0x90 | (tr.ch & 15)), tr.num & 127, (uint8_t)std::max(1, tr.vel * AFILL_VEL / 100));
            tr.sounding = true; tr.snd_note = tr.num & 127; tr.snd_ch = tr.ch & 15;
            tr.off_at = when + 0.5 * step_dur(tr); tr.rt_left = 0;
            return;
        }
        if (!fires && ghost && !s.on && !tr.mute) {   /* GHOST (v1.94): a quiet, short hit on an empty step */
            note_off(tr);
            emit((uint8_t)(0x90 | (tr.ch & 15)), tr.num & 127, (uint8_t)std::max(1, tr.vel * GHOST_PCT / 100));
            tr.sounding = true; tr.snd_note = tr.num & 127; tr.snd_ch = tr.ch & 15;
            tr.off_at = when + 0.5 * step_dur(tr); tr.rt_left = 0;
            return;
        }
        if (!fires) return;
        note_off(tr);   /* same note still sounding: release before the re-trigger */
        const int pbase = tr.panb ? tr.panb - 1 : 64;   /* v1.93: the track's own pan controller, and the pad's own pan as the rest */
        if (lk && s.pan) { emit((uint8_t)(0xB0 | (tr.ch & 15)), PAN_CC[ti], (uint8_t)((s.pan - 1) & 127)); pan_dirty[ti] = tr.panb != 0; }   /* a known pan comes back after the lock */
        else if (pf(PF_PAN) || pan_dirty[ti]) {   /* PERFORM pan (v1.72): the rest plus the offset; back to the rest once it is 0 again */
            emit((uint8_t)(0xB0 | (tr.ch & 15)), PAN_CC[ti], (uint8_t)pf_cl(pbase + pf(PF_PAN), 0, 127));
            pan_dirty[ti] = pf(PF_PAN) != 0;
        }
        uint8_t vel = lk && s.vel ? s.vel : (uint8_t)pf_cl(tr.vel + pf(PF_VEL), 1, 127);   /* a lock is not moved by PERFORM ALL */
        int gate = lk && s.len ? s.len : pf_cl(tr.gate + pf(PF_LEN), 1, 64);
        if (humanize) vel = (uint8_t)pf_cl(vel + (int)(rnd() % (2 * HUMAN_V + 1)) - HUMAN_V, 1, 127);
        int bhits = 1;
        if (build) {
            const double el = when - build_t0 < 0.0 ? 0.0 : when - build_t0;
            bhits = std::min((int)RATCH_MAX, 1 + (int)(el / BUILD_BEAT));
            const double up = std::min(1.0, el / (BUILD_BEAT * (RATCH_MAX - 1)));
            vel = (uint8_t)pf_cl((int)(vel * (0.6 + 0.4 * up) + 0.5), 1, 127);
        }
        if (fade) {
            const double f = 1.0 - (when - fade_t0) / FADE_BEATS;
            if (f <= 0.0) return;   /* faded out: silent (the locks above still went out) */
            if (f < 1.0) vel = (uint8_t)pf_cl((int)(vel * f + 0.5), 1, 127);
        }
        if (echo) {   /* the repeats, each 60 % of the one before; as short as the note but no longer than the gap */
            const double eg = std::min(gate * 0.25 * step_dur(tr), ECHO_DT * 0.9);
            double ev = vel;
            for (int e = 1; e <= ECHO_N; e++) {
                ev *= 0.6;
                if (ev < 1.0) break;
                echo_add(when + e * ECHO_DT, (uint8_t)(0x90 | (tr.ch & 15)), tr.num & 127, (uint8_t)ev);
                echo_add(when + e * ECHO_DT + eg, (uint8_t)(0x80 | (tr.ch & 15)), tr.num & 127, 0);
            }
        }
        emit((uint8_t)(0x90 | (tr.ch & 15)), tr.num & 127, vel);
        tr.sounding = true; tr.snd_note = tr.num & 127; tr.snd_ch = tr.ch & 15;
        tr.off_at = when + gate * 0.25 * step_dur(tr);
        tr.rt_left = 0;
        const int hits = std::max(std::max(lk ? s.ratch + 1 : 1, roll ? 4 : 1), bhits);   /* ROLL: at least 4; BUILD: more and more */
        if (hits > 1) {   /* RATCHET: hits evenly over the step (or RESOLUTION's per step), each as long as the gate but no longer than its share */
            const int per = lk && s.rres ? s.rres : hits;
            tr.rt_iv = step_dur(tr) / per;
            tr.rt_gate = std::min(gate * 0.25 * step_dur(tr), tr.rt_iv);
            tr.off_at = when + tr.rt_gate;
            tr.rt_left = hits - 1; tr.rt_at = when + tr.rt_iv; tr.rt_vel = vel;
        }
    }
    void ratchet_hit(Track &tr) {
        note_off(tr);
        emit((uint8_t)(0x90 | (tr.ch & 15)), tr.num & 127, tr.rt_vel);
        tr.sounding = true; tr.snd_note = tr.num & 127; tr.snd_ch = tr.ch & 15;
        tr.off_at = tr.rt_at + tr.rt_gate;
        tr.rt_left--; tr.rt_at += tr.rt_iv;
    }

    /* everything that falls in [start, end) quarter notes; events are appended to out[] */
    void process(double start, double end_song) {
        (void)start;
        if (!playing) return;
        const double end = virt(end_song);   /* the tracks' clock (HALF / DOUBLE TIME) */
        for (int pass = 0; pass < 2; pass++)   /* ECHO repeats in this block, before the tracks' own notes: offs, then ons */
            for (int q = 0; q < neq; ) {
                const bool off = (eq[q].status & 0xF0) == 0x80;
                if (eq[q].at < end && off == (pass == 0)) { emit(eq[q].status, eq[q].d1, eq[q].d2); eq[q] = eq[--neq]; } else q++;
            }
        for (int ti = 0; ti < NTRACKS; ti++) {
            Track &tr = t[ti];
            for (;;) {
                const double when = fire_time(ti, tr.next_k);
                const double rat = tr.rt_left > 0 && !tr.mute ? tr.rt_at : 1e300;
                if (std::min(when, rat) >= end) break;
                if (rat < when) {   /* a ratchet hit of the step before comes first */
                    if (tr.sounding && tr.off_at <= rat + 1e-9) note_off(tr);
                    ratchet_hit(tr);
                    continue;
                }
                if (tr.sounding && tr.off_at <= when + 1e-9) note_off(tr);
                /* v2.1: a ratchet goes on over the next steps until one of them plays (RESOLUTION); fire() starts it again */
                fire(ti, tr.next_k, when);
                tr.next_k++;
            }
            if (tr.sounding && tr.off_at < end) note_off(tr);
        }
    }

    /* Random generators, one-shot: they write into the steps 1..length of one track.
     *   R_TRIG       each step gets a trig with probability rnd_dens %
     *   R_VEL        steps with a trig: rnd_dens % of them get a velocity lock in rnd_min..rnd_max (at least 1)
     *   R_LEN        the same for the length lock: rnd_min..rnd_max (0..127) maps to 0.25 .. 16 steps
     *   R_CC0 + j    the same for the track's CC lock j (CC ccbase + j), value rnd_min..rnd_max
     * Steps that are not picked keep what they had. Every step draws two numbers whether used or not. */
    enum { R_TRIG = 0, R_VEL, R_LEN, R_CC0, R_PAN = R_CC0 + NCC, R_COUNT };   /* R_PAN: the pan lock, value rnd_min..rnd_max */
    /* mix fresh entropy into both RNGs (the wrapper passes clock ticks); xorshift must never be 0 */
    void reseed(uint32_t e) {
        gen_rng ^= e * 2654435761u; if (!gen_rng) gen_rng = 0x9E3779B9u;
        rng ^= (e ^ (e >> 15)) * 2246822519u; if (!rng) rng = 0x12345678u;
        gen_rnd(); gen_rnd(); rnd();
    }
    uint32_t gen_rnd() { gen_rng ^= gen_rng << 13; gen_rng ^= gen_rng >> 17; gen_rng ^= gen_rng << 5; return gen_rng; }
    void randomize(int track, int what) {
        if (track < 0 || track >= NTRACKS || what < 0 || what >= R_COUNT) return;
        Track &tr = t[track];
        int len = eff_len(tr);
        for (int k = 0; k < len; k++) {
            Step &s = tr.st[k];
            bool hit = (int)(gen_rnd() % 100u) < (int)tr.rnd_dens;
            int v = rnd_value(tr);
            if (what == R_TRIG) { s.on = hit ? 1 : 0; continue; }
            if (s.on && hit) rnd_write(s, what, v);
        }
    }
    /* The same generators on chosen steps only (bit k of mask = step k): STEP EDIT mode. Every chosen step gets a new
     * value, with or without a trig and whatever the density; only R_TRIG still rolls the density. */
    void randomize_steps(int track, int what, uint64_t mask) {
        if (track < 0 || track >= NTRACKS || what < 0 || what >= R_COUNT) return;
        Track &tr = t[track];
        for (int k = 0; k < NSTEPS; k++) {
            if (!((mask >> k) & 1)) continue;
            Step &s = tr.st[k];
            bool hit = (int)(gen_rnd() % 100u) < (int)tr.rnd_dens;
            int v = rnd_value(tr);
            if (what == R_TRIG) s.on = hit ? 1 : 0;
            else rnd_write(s, what, v);
        }
    }
    int rnd_value(const Track &tr) {   /* one value in the track's rnd_min..rnd_max (either order) */
        int lo = tr.rnd_min, hi = tr.rnd_max;
        if (lo > hi) { int x = lo; lo = hi; hi = x; }
        if (hi > 127) hi = 127;
        return lo + (int)(gen_rnd() % (uint32_t)(hi - lo + 1));
    }
    static void rnd_write(Step &s, int what, int v) {
        if (what == R_VEL) s.vel = (uint8_t)(v < 1 ? 1 : v);
        else if (what == R_LEN) s.len = (uint8_t)(1 + v * 63 / 127);
        else if (what == R_PAN) s.pan = (uint8_t)(v + 1);
        else if (what >= R_CC0 && what < R_CC0 + NCC) s.cc[what - R_CC0] = (uint8_t)(v + 1);
    }

    /* ---- Euclidean generator -------------------------------------------------------------------------------------
     * The UI has one generator since v1.29 (PULSE eu_pl1, ROTATE eu_ro1; ENABLE and RND clear the rest); the second one,
     * TRO and the operator are still played for old projects. Two generators spread eu_pl1 / eu_pl2 pulses as evenly as possible over the track's length, each rotated by its
     * own amount; the two are combined (OR, AND, XOR, SUB = 1 and not 2) and the result is rotated by eu_tro. While
     * eu_on, that result IS the track's trigs for steps 1..len (locks and conditions of the steps are left alone);
     * euclid_apply() must be called again after any of the settings or the length changes. */
    enum { E_OR = 0, E_AND, E_XOR, E_SUB, E_NOPS };
    static bool eu_pulse(int k, int pulses, int rot, int n) {
        if (pulses <= 0 || n <= 0) return false;
        if (pulses > n) pulses = n;
        int j = ((k - rot) % n + n) % n;
        return (j * pulses) % n < pulses;   /* the Bresenham form of the Euclidean rhythm, first pulse on step 1 */
    }
    /* PATTERN LENGTH (16 / 32 / 48 / 64; set_len_all: every track at once). A track that grows gets its existing pages repeated
     * onto the new ones, steps with all their locks (16 -> 64: page 1 three more times; 32 -> 64: pages 1-2 again).
     * A new page that already holds trigs is left as it is, so going 64 -> 16 -> 64 loses nothing. */
    void set_len_track(int i, int n) {   /* the same for one track (TRACK EDIT length buttons) */
        if (i < 0 || i >= NTRACKS) return;
        n = n < 16 ? 16 : n > NSTEPS ? (int)NSTEPS : (n / 16) * 16;
        Track &tr = t[i];
        int oldp = (tr.len + 15) / 16;
        if (oldp < 1) oldp = 1;
        for (int p = oldp; p < n / 16; p++) {
            bool used = false;
            for (int j = 0; j < 16; j++) used |= tr.st[p * 16 + j].on != 0;
            if (used) continue;
            for (int j = 0; j < 16; j++) tr.st[p * 16 + j] = tr.st[((p - oldp) % oldp) * 16 + j];
        }
        tr.len = (uint8_t)n;
        euclid_apply(i);
    }
    void set_len_all(int n) { for (int i = 0; i < NTRACKS; i++) set_len_track(i, n); }
    bool all_len(int n) const { for (int i = 0; i < NTRACKS; i++) if (t[i].len != n) return false; return true; }
    void euclid_apply(int track) {
        Track &tr = t[track];
        if (!tr.eu_on) return;
        int n = eff_len(tr);
        int m = tr.eu_len ? (tr.eu_len > NSTEPS ? (int)NSTEPS : (int)tr.eu_len) : n;   /* the rhythm's own length: it repeats over the track */
        for (int k = 0; k < n; k++) {
            int j = (((k % m) - tr.eu_tro) % m + m) % m;
            bool a = eu_pulse(j, tr.eu_pl1, tr.eu_ro1, m), b = eu_pulse(j, tr.eu_pl2, tr.eu_ro2, m), r;
            switch (tr.eu_op) {
            case E_AND: r = a && b; break;
            case E_XOR: r = a != b; break;
            case E_SUB: r = a && !b; break;
            default: r = a || b; break;
            }
            tr.st[k].on = r ? 1 : 0;
        }
    }
    void euclid_enable(int track) {      /* remember the trigs as they are, then let the generator write them */
        Track &tr = t[track];
        if (tr.eu_on) return;
        tr.eu_backup = 0;
        for (int k = 0; k < NSTEPS; k++) if (tr.st[k].on) tr.eu_backup |= (uint64_t)1 << k;
        tr.eu_on = 1;
        euclid_apply(track);
    }
    void euclid_discard(int track) {     /* back to the trigs from before Enable */
        Track &tr = t[track];
        if (!tr.eu_on) return;
        for (int k = 0; k < NSTEPS; k++) tr.st[k].on = (tr.eu_backup >> k) & 1;
        tr.eu_on = 0;
    }
    void euclid_random(int track) {      /* a random rhythm: length 3..64, 1..length-1 pulses (always fewer than the length), any rotation; enables the generator */
        Track &tr = t[track];
        int len = (gen_rnd() & 1u) ? 3 + (int)(gen_rnd() % 14u)                       /* half of the time 3..16 */
                                   : 17 + (int)(gen_rnd() % (uint32_t)(NSTEPS - 16));   /* the other half 17..64 */
        tr.eu_len = (uint8_t)len;
        tr.eu_pl1 = (uint8_t)(1 + (int)(gen_rnd() % (uint32_t)(len - 1)));
        tr.eu_ro1 = (uint8_t)(gen_rnd() % (uint32_t)len);
        tr.eu_pl2 = 0; tr.eu_ro2 = 0; tr.eu_tro = 0; tr.eu_op = E_OR;
        if (tr.eu_on) euclid_apply(track); else euclid_enable(track);
    }
    void euclid_commit(int track) { t[track].len = (uint8_t)eff_len(t[track]); t[track].eu_on = 0; }   /* a longer rhythm keeps its length as the track's */   /* keep what the generator wrote; it stops following the settings */

    void clear_track(int track) {
        Track &tr = t[track];
        std::memset(tr.st, 0, sizeof tr.st);
        for (int s = 0; s < NSTEPS; s++) tr.st[s].prob = 100;
    }
    void clear_locks(int track, int step) {
        Step &s = t[track].st[step];
        s.cond = C_NONE; s.prob = 100; s.vel = 0; s.len = 0; s.pan = 0; std::memset(s.cc, 0, sizeof s.cc);   /* TIMING and RATCHET stay (v1.78: they have their own CLR) */
    }
};

}   /* namespace seqcore */

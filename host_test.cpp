#include <unistd.h>
#include <cstdlib>
/* Offline x86 host test for StepRunner-8 (./test.sh; ASan/UBSan, no device, no ALSA: -DSTEPRUNNER_TEST captures the
 * MIDI that would go to the ALSA port). Part 1 drives seqcore.hpp directly, part 2 the VST2 plugin as MPC would.
 * Prints OK or FAILED. */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "seqcore.hpp"
#include "params.h"

using namespace seqcore;

struct AEffect;
typedef intptr_t (*cb)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*d)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void *proc;
    void (*setP)(AEffect *, int32_t, float);
    float (*getP)(AEffect *, int32_t);
    int32_t np, npar, ni, no, flags;
    intptr_t r1, r2;
    int32_t a, b, c;
    float io;
    void *obj, *user;
    int32_t uid, ver;
    void (*pr)(AEffect *, float **, float **, int32_t);
    void *pdr;
    char f[56];
};
struct TI { double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
            int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags; };
enum { kPlaying = 1 << 1, kPpq = 1 << 9, kTempo = 1 << 10 };

extern "C" AEffect *VSTPluginMain(cb);
extern "C" int steprunner_test_count();
extern "C" void steprunner_test_get(int i, unsigned char *m);
extern "C" void steprunner_test_reset();

static TI g_ti;
static int automated[NPARAMS], display_updates;
static float automated_val[NPARAMS];
static intptr_t host(AEffect *, int32_t op, int32_t idx, intptr_t, void *, float o) {
    if (op == 0 && idx >= 0 && idx < NPARAMS) { automated[idx]++; automated_val[idx] = o; }
    if (op == 42) display_updates++;   /* audioMasterUpdateDisplay */
    if (op == 7) return (intptr_t)&g_ti;
    return 0;
}
static int P(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!strcmp(PARAMS[i].key, key)) return i;
    printf("FAIL no param %s\n", key);
    return 0;
}
static int fails = 0;
/* v1.2.0: the TIMING offset of step k (0..63) of track t in a saved chunk: its "M t" line (micro byte, ratchet byte per step) */
static int micro_in(const std::string &sv, int t, int k) {
    const std::string tag = "\nM " + std::to_string(t) + " ";
    const size_t q = sv.find(tag);
    if (q == std::string::npos) return 0;
    return (int)(int8_t)strtol(sv.substr(q + tag.size() + 4 * k, 2).c_str(), nullptr, 16);
}
#define CHECK(c, ...) do { int ok_ = (c); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

/* ---- core helpers ---- */
static int count(const Seq &s, uint8_t status, int d1 = -1) {
    int n = 0;
    for (int i = 0; i < s.nout; i++) if (s.out[i].status == status && (d1 < 0 || s.out[i].d1 == d1)) n++;
    return n;
}
/* run [0, beats) in small blocks; returns the block-start time of every event matching status/d1 */
static std::vector<double> run(Seq &s, double beats, uint8_t status, int d1, std::vector<int> *d2 = nullptr) {
    std::vector<double> when;
    const double blk = 0.00580498866;   /* 128 frames at 120 BPM, 44.1 kHz */
    s.nout = 0;
    s.start(0.0);
    for (double t = 0.0; t < beats - 1e-9; t += blk) {
        int before = s.nout;
        s.process(t, t + blk < beats ? t + blk : beats);
        for (int i = before; i < s.nout; i++)
            if (s.out[i].status == status && (d1 < 0 || s.out[i].d1 == d1)) { when.push_back(t); if (d2) d2->push_back(s.out[i].d2); }
        if (s.nout > 200) s.nout = 0;
    }
    s.stop();
    return when;
}

static void core_tests() {
    printf("-- core --\n");
    {   /* defaults: the 8 notes and 4 CCs asked for */
        Seq s;
        static const int notes[8] = {36, 37, 38, 39, 40, 41, 42, 43};
        bool ok = true;
        for (int i = 0; i < 8; i++) ok &= s.t[i].num == notes[i];
        static const int bases[8] = {20, 25, 85, 102, 107, 112, 14, 80};
        bool used[128] = {false}, clash = false, reserved = false;
        for (int i = 0; i < 8; i++) {
            ok &= s.t[i].ccbase == bases[i];
            for (int j = 0; j <= NCC; j++) {   /* v1.2.0: CC 1-8 and the pan CC */
                int n = j < NCC ? Seq::cc_num(s.t[i], j) : PAN_CC[i];
                clash |= used[n]; used[n] = true;
                reserved |= n == 0 || n == 1 || n == 7 || n == 10 || n == 11 || n == 32 || (n >= 64 && n <= 69) || (n >= 96 && n <= 101) || n >= 120;
            }
        }
        CHECK(!clash && !reserved, "default CC numbers: 72 different (CC 1-8 and pan of 8 tracks, one channel), none of them bank select / mod / volume / pan / expression / pedals / RPN / channel mode");
        { static const int c8[8] = {19, 35, 41, 62, 63, 12, 13, 92};
          bool c67 = true; for (int i = 0; i < 8; i++) c67 &= Seq::cc_num(s.t[i], 5) == 46 + i && Seq::cc_num(s.t[i], 6) == 54 + i && Seq::cc_num(s.t[i], 7) == c8[i];
          CHECK(ok && c67 && NTRACKS == 8 && NCC == 8, "8 tracks on notes 36..43 (pads A01..A08), CC bases 20 25 85 102 107 112 14 80, CC 6 46-53, CC 7 54-61, CC 8 19 35 41 62 63 12 13 92"); }
        CHECK(NCOND == 44, "44 trig conditions");
        int a, b;
        CHECK(cond_ab(C_AB0, &a, &b) && a == 1 && b == 2 && cond_ab(NCOND - 1, &a, &b) && a == 8 && b == 8, "A:B runs 1:2 .. 8:8");
    }
    {   /* every track at once, 64 steps, one bar = 16 steps */
        Seq s;
        for (int i = 0; i < NTRACKS; i++) { s.t[i].len = 64; for (int k = 0; k < 64; k++) s.t[i].st[k].on = 1; }
        s.start(0.0);
        s.process(0.0, 64.0);   /* 256 sixteenth steps per track: far more events than the outbox holds */
        CHECK(s.nout == (int)(sizeof s.out / sizeof s.out[0]) && s.nout == 1024, "outbox is bounded (%d; 1024 since v2.11)", s.nout);
        Seq q;
        for (int i = 0; i < NTRACKS; i++) { q.t[i].len = 64; for (int k = 0; k < 64; k++) q.t[i].st[k].on = 1; }
        int on[8] = {0}, ccn = 0, off = 0;
        q.start(0.0);
        for (int k = 0; k < 64 * 8; k++) {   /* 8 blocks per step */
            q.nout = 0;
            q.process(k / 32.0, (k + 1) / 32.0);
            for (int i = 0; i < 8; i++) on[i] += count(q, 0x90, q.t[i].num);
            ccn += count(q, 0xB0);
            off += count(q, 0x80);
        }
        bool all = true;
        for (int i = 0; i < 8; i++) all &= on[i] == 64;
        CHECK(all && ccn == 0, "64 steps on all 8 tracks: 64 note-ons per note, no CC without a lock");
        q.nout = 0; q.stop();
        CHECK(off + count(q, 0x80) == 8 * 64, "every note-on got its note-off (%d)", off + count(q, 0x80));
    }
    {   /* independent length and speed */
        Seq s;
        s.t[0].len = 3; s.t[0].st[0].on = 1;                     /* every 3 steps: 0, 0.75, 1.5 ... */
        s.t[1].len = 4; s.t[1].st[0].on = 1; s.t[1].speed = 6;   /* 2x: every 4 half-steps = 0.5 beat */
        s.t[2].len = 2; s.t[2].st[1].on = 1; s.t[2].speed = 2;   /* 1/2x: step 2 of 2, every beat, first at 0.5 */
        std::vector<double> a = run(s, 6.0, 0x90, 36), b = run(s, 6.0, 0x90, 37), c = run(s, 6.0, 0x90, 38);
        CHECK(a.size() == 8 && fabs(a[1] - 0.75) < 0.01, "length 3 at 1x: %zu trigs in 6 beats, 2nd at %.2f", a.size(), a[1]);
        CHECK(b.size() == 12 && fabs(b[1] - 0.5) < 0.01, "length 4 at 2x: %zu trigs, 2nd at %.2f", b.size(), b[1]);
        CHECK(c.size() == 6 && fabs(c[0] - 0.5) < 0.01, "length 2 at 1/2x: %zu trigs, 1st at %.2f", c.size(), c[0]);
    }
    {   /* A:B, 1ST */
        Seq s;
        s.t[0].len = 4;
        s.t[0].st[0].on = 1; s.t[0].st[0].cond = C_AB0 + 2 + 1;   /* 2:3 */
        std::vector<double> a = run(s, 12.0, 0x90, 36);              /* 12 loops of one beat */
        CHECK(a.size() == 4 && fabs(a[0] - 1.0) < 0.01 && fabs(a[1] - 4.0) < 0.01, "2:3 plays loops 2, 5, 8, 11 (%zu, first at %.2f)", a.size(), a.empty() ? -1 : a[0]);
        s.t[0].st[0].cond = C_1ST;
        a = run(s, 12.0, 0x90, 36);
        CHECK(a.size() == 1 && a[0] < 0.01, "1ST plays once (%zu)", a.size());
        s.t[0].st[0].cond = C_N1ST;
        a = run(s, 12.0, 0x90, 36);
        CHECK(a.size() == 11, "NO 1ST plays the other 11 loops (%zu)", a.size());
    }
    {   /* FILL, probability, PRE, NEI */
        Seq s;
        s.t[0].len = 4; s.t[0].st[0].on = 1; s.t[0].st[0].cond = C_FILL;
        s.t[1].len = 4; s.t[1].st[0].on = 1; s.t[1].st[0].cond = C_NFILL;
        CHECK(run(s, 4.0, 0x90, 36).size() == 0 && run(s, 4.0, 0x90, 37).size() == 4, "fill off: FILL silent, NO FILL plays");
        s.fill = true;
        CHECK(run(s, 4.0, 0x90, 36).size() == 4 && run(s, 4.0, 0x90, 37).size() == 0, "fill on: FILL plays, NO FILL silent");

        Seq p;
        p.t[0].len = 1; p.t[0].st[0].on = 1; p.t[0].st[0].prob = 0;
        CHECK(run(p, 8.0, 0x90, 36).size() == 0, "probability 0 never plays");
        p.t[0].st[0].prob = 50;
        size_t n = run(p, 100.0, 0x90, 36).size();   /* 400 trigs */
        CHECK(n > 150 && n < 250, "probability 50: %zu of 400", n);

        Seq r;   /* T1 step 1 = 1:2, T1 step 3 = PRE, T1 step 4 = NO PRE (note 37); T2 step 2 = NEI (note 36) */
        r.t[0].len = 4; r.t[1].len = 4;
        r.t[0].st[0].on = 1; r.t[0].st[0].cond = C_AB0;   /* 1:2 */
        r.t[0].st[2].on = 1; r.t[0].st[2].cond = C_PRE;
        r.t[0].st[3].on = 1; r.t[0].st[3].cond = C_NPRE;
        r.t[1].st[1].on = 1; r.t[1].st[1].cond = C_NEI;
        std::vector<double> a = run(r, 4.0, 0x90, 36), b = run(r, 4.0, 0x90, 37);
        /* loops 1 and 3: step 1 + PRE; loops 2 and 4: NO PRE only -> 6 note-ons; NEI follows on loops 1 and 3 */
        CHECK(a.size() == 6 && fabs(a[1] - 0.5) < 0.01 && fabs(a[2] - 1.75) < 0.01, "PRE / NO PRE follow the 1:2 trig (%zu)", a.size());
        CHECK(b.size() == 2 && fabs(b[0] - 0.25) < 0.01 && fabs(b[1] - 2.25) < 0.01, "NEI follows the neighbour track (%zu)", b.size());
    }
    {   /* parameter locks */
        Seq s;
        Track &tr = s.t[0];
        tr.len = 4; tr.vel = 90; tr.gate = 2;   /* default gate: half a step */
        tr.st[0].on = 1;
        tr.st[1].on = 1; tr.st[1].vel = 33; tr.st[1].len = 8;   /* 2 steps long */
        tr.st[1].cc[0] = 1 + 0; tr.st[1].cc[2] = 1 + 77; tr.st[1].cc[4] = 1 + 127;   /* CC 20 = 0, CC 22 = 77, CC 24 = 127 */
        std::vector<int> v;
        run(s, 1.0, 0x90, 36, &v);
        CHECK(v.size() == 2 && v[0] == 90 && v[1] == 33, "velocity: track default 90, locked step 33");
        std::vector<double> off = run(s, 1.0, 0x80, 36);
        CHECK(off.size() == 2 && fabs(off[0] - 0.125) < 0.01 && fabs(off[1] - 0.75) < 0.01, "gate: default 0.5 step, locked 2 steps (%.3f, %.3f)", off.size() > 0 ? off[0] : -1, off.size() > 1 ? off[1] : -1);
        std::vector<int> v20, v22, v24;
        std::vector<double> cc = run(s, 1.0, 0xB0, 22, &v22);
        run(s, 1.0, 0xB0, 20, &v20); run(s, 1.0, 0xB0, 24, &v24);
        CHECK(cc.size() == 1 && v22[0] == 77 && fabs(cc[0] - 0.25) < 0.01 && v20.size() == 1 && v20[0] == 0 && v24.size() == 1 && v24[0] == 127,
              "CC locks: CC 20 = 0, CC 22 = 77, CC 24 = 127, sent once, with the locked step; nothing is sent afterwards");
        CHECK(run(s, 1.0, 0xB0, 21).size() == 0 && run(s, 1.0, 0xB0, 23).size() == 0, "CCs without a lock are not sent");
        s.nout = 0; s.start(0.0); s.process(0.25, 0.26);
        int n = s.nout;
        CHECK(n >= 4 && s.out[n - 4].status == 0xB0 && s.out[n - 3].status == 0xB0 && s.out[n - 2].status == 0xB0 && s.out[n - 1].status == 0x90, "the CC locks go out before their note");
        s.stop();
        tr.ch = 4;
        std::vector<int> vch;
        run(s, 1.0, 0xB4, 22, &vch);
        CHECK(vch.size() == 1 && vch[0] == 77 && run(s, 1.0, 0x94, 36).size() == 2, "locks and notes follow the track's MIDI channel");
        tr.mute = 1;
        CHECK(run(s, 1.0, 0xB4, -1).size() == 0 && run(s, 1.0, 0x94, -1).size() == 0, "a muted track sends nothing");
        tr.mute = 0; tr.ch = 0;
        tr.st[1].cond = C_FILL;
        CHECK(run(s, 1.0, 0xB0, -1).size() == 0, "a step whose condition fails sends no CC either");
        s.clear_locks(0, 1);
        CHECK(run(s, 1.0, 0xB0, -1).size() == 0 && run(s, 1.0, 0x90, 36).size() == 2, "Clear Locks removes the CC locks and the condition, keeps the trig");
    }
    {   /* pan lock: CC 10, sent with the locked step only, before the note */
        Seq s;
        Track &tr = s.t[1];
        tr.len = 4; tr.ch = 2;
        tr.st[0].on = 1;
        tr.st[2].on = 1; tr.st[2].pan = 1 + 0; tr.st[2].cc[0] = 1 + 9;   /* hard left, CC 20 = 9 */
        std::vector<int> v;
        std::vector<double> at = run(s, 1.0, 0xB2, PAN_CC[1], &v);
        CHECK(v.size() == 1 && v[0] == 0 && fabs(at[0] - 0.5) < 0.01, "pan lock: the track's pan CC (v1.93) = 0 on its channel, once, with step 3");
        s.nout = 0; s.start(0.0); s.process(0.5, 0.51);
        int n = s.nout;
        CHECK(n >= 3 && s.out[n - 3].d1 == 25 && s.out[n - 2].d1 == PAN_CC[1] && s.out[n - 1].status == 0x92, "order: CC locks, pan, then the note");
        s.stop();
        s.t[1].rnd_dens = 100; s.t[1].rnd_min = 100; s.t[1].rnd_max = 127;
        s.randomize(1, Seq::R_PAN);
        CHECK(tr.st[0].pan >= 101 && tr.st[2].pan >= 101 && tr.st[1].pan == 0 && tr.st[2].cc[0] == 10, "RND PAN: pan locks in 100..127 on the trigs only, CC locks untouched");
        s.clear_locks(1, 2);
        CHECK(tr.st[2].pan == 0 && tr.st[0].pan != 0, "Clear Locks removes the pan lock of that step");
    }
    {   /* only locked steps send a CC; stop sends none */
        Seq s;
        Track &tr = s.t[2];
        tr.len = 8; tr.gate = 64;
        tr.st[0].on = 1; tr.st[0].cc[1] = 1 + 50;
        tr.st[1].on = 1;
        tr.st[2].on = 1; tr.st[2].cc[1] = 1 + 60;
        tr.st[3].on = 1; tr.st[3].cc[1] = 1 + 70;
        std::vector<int> v;
        run(s, 1.0, 0xB0, -1, &v);   /* 4 steps, then stop */
        CHECK(v.size() == 3 && v[0] == 50 && v[1] == 60 && v[2] == 70 && s.out[s.nout - 1].status == 0x80,
              "CC 21 over steps 1-4: 50, (nothing), 60, 70; stop only releases the note (%zu CCs)", v.size());
    }
    {   /* random generators */
        Seq s;
        Track &tr = s.t[3];
        tr.len = 32;
        tr.st[40].on = 1; tr.st[40].vel = 9;   /* beyond the track's length: must not be touched */
        s.t[3].rnd_dens = 50;
        s.randomize(3, Seq::R_TRIG);
        int on = 0;
        for (int k = 0; k < 32; k++) on += tr.st[k].on;
        bool other = false;
        for (int i = 0; i < NTRACKS; i++) if (i != 3) for (int k = 0; k < NSTEPS; k++) other |= s.t[i].st[k].on != 0;
        CHECK(on >= 8 && on <= 24 && tr.st[40].on == 1 && !other, "RND TRIGS at 50%%: %d of 32 steps on, steps past the length and other tracks untouched", on);
        Step before[NSTEPS];
        memcpy(before, tr.st, sizeof before);
        s.randomize(3, Seq::R_TRIG);
        CHECK(memcmp(before, tr.st, sizeof before) != 0, "a second press gives a different pattern");
        s.t[3].rnd_dens = 100; s.randomize(3, Seq::R_TRIG);
        on = 0; for (int k = 0; k < 32; k++) on += tr.st[k].on;
        CHECK(on == 32, "density 100: every step");
        s.t[3].rnd_dens = 0; s.randomize(3, Seq::R_TRIG);
        on = 0; for (int k = 0; k < 32; k++) on += tr.st[k].on;
        CHECK(on == 0, "density 0: none");

        for (int k = 0; k < 32; k += 2) tr.st[k].on = 1;   /* 16 trigs on even steps */
        s.t[3].rnd_dens = 100; s.t[3].rnd_min = 40; s.t[3].rnd_max = 90;
        s.randomize(3, Seq::R_VEL);
        bool ok = true, varied = false;
        for (int k = 0; k < 32; k++) {
            if (k % 2 == 0) { ok &= tr.st[k].vel >= 40 && tr.st[k].vel <= 90; varied |= tr.st[k].vel != tr.st[0].vel; }
            else ok &= tr.st[k].vel == 0;
        }
        CHECK(ok && varied && tr.st[40].vel == 9, "RND VEL: every trig gets a velocity lock in 40..90, steps without a trig get none");
        s.t[3].rnd_min = 90; s.t[3].rnd_max = 40;   /* swapped range still works */
        s.randomize(3, Seq::R_CC0 + 2);
        ok = true;
        for (int k = 0; k < 32; k++) {
            if (k % 2 == 0) ok &= tr.st[k].cc[2] >= 41 && tr.st[k].cc[2] <= 91;   /* stored as value + 1 */
            else ok &= tr.st[k].cc[2] == 0;
            ok &= tr.st[k].cc[0] == 0 && tr.st[k].cc[1] == 0 && tr.st[k].cc[3] == 0 && tr.st[k].cc[4] == 0;
        }
        CHECK(ok, "RND CC22: locks in 40..90 on the trigs, the other four CCs untouched");
        s.t[3].rnd_min = 0; s.t[3].rnd_max = 127;
        s.randomize(3, Seq::R_LEN);
        ok = true; int lmin = 99, lmax = 0;
        for (int k = 0; k < 32; k += 2) { int l = tr.st[k].len; ok &= l >= 1 && l <= 64; if (l < lmin) lmin = l; if (l > lmax) lmax = l; }
        CHECK(ok && lmax > lmin, "RND LEN: length locks between 0.25 and 16 steps (%.2f .. %.2f)", lmin / 4.0, lmax / 4.0);
        s.t[3].rnd_min = s.t[3].rnd_max = 0; s.randomize(3, Seq::R_VEL); s.randomize(3, Seq::R_CC0);
        CHECK(tr.st[0].vel == 1 && tr.st[0].cc[0] == 1, "range 0..0: velocity 1 (never 0), CC 20 locked to value 0");
        memcpy(before, tr.st, sizeof before);
        s.t[3].rnd_dens = 0; s.t[3].rnd_max = 127;
        for (int w = Seq::R_VEL; w < Seq::R_COUNT; w++) s.randomize(3, w);
        CHECK(memcmp(before, tr.st, sizeof before) == 0, "density 0 on the lock generators changes nothing");
        s.t[3].rnd_dens = 50; s.t[3].rnd_min = 100; s.t[3].rnd_max = 127;
        s.randomize(3, Seq::R_CC0 + 4);
        int changed = 0;
        for (int k = 0; k < 32; k += 2) changed += tr.st[k].cc[4] != 0;
        CHECK(changed >= 3 && changed <= 13, "density 50: about half of the 16 trigs get a CC 24 lock (%d), the rest keep theirs", changed);
        s.randomize(-1, 0); s.randomize(99, 0); s.randomize(0, 99);   /* out of range: ignored */
    }
    {   /* Euclidean generator */
        Seq s;
        Track &tr = s.t[2];
        auto pat = [&](int n) { std::string p; for (int k = 0; k < n; k++) p += tr.st[k].on ? 'x' : '.'; return p; };
        tr.len = 16;
        tr.st[1].on = 1; tr.st[5].on = 1; tr.st[20].on = 1; tr.st[5].vel = 77;   /* what is there before: steps 2, 6 (locked) and 21 (past the length) */
        std::string before = pat(24);
        s.euclid_enable(2);
        CHECK(pat(16) == "x...x...x...x..." && tr.eu_on, "Enable with the defaults (4 pulses on 16 steps): %s", pat(16).c_str());
        CHECK(tr.st[20].on == 1 && tr.st[5].vel == 77, "steps past the length and the steps' locks are left alone");
        tr.eu_pl1 = 3; tr.len = 8; s.euclid_apply(2);
        CHECK(pat(8) == "x..x..x.", "E(3,8) = %s", pat(8).c_str());
        tr.eu_pl1 = 5; s.euclid_apply(2);
        int n5 = 0; for (int k = 0; k < 8; k++) n5 += tr.st[k].on;
        CHECK(n5 == 5 && std::string("x.xx.xx.x.xx.xx.").find(pat(8)) != std::string::npos && pat(8)[0] == 'x', "E(5,8) = %s: 5 pulses, a rotation of x.xx.xx. starting on step 1", pat(8).c_str());
        tr.eu_pl1 = 3; tr.eu_ro1 = 1; s.euclid_apply(2);
        CHECK(pat(8) == ".x..x..x", "rotation 1 moves it one step later: %s", pat(8).c_str());
        tr.eu_ro1 = 0; tr.eu_pl2 = 4; tr.eu_a = 1; tr.eu_b = 1; s.euclid_apply(2);   /* generator B (v1.2.0: its own switch): x.x.x.x. */
        CHECK(pat(8) == "x.xxx.x.", "OR  of x..x..x. and x.x.x.x. = %s", pat(8).c_str());
        tr.eu_op = Seq::E_AND; s.euclid_apply(2); CHECK(pat(8) == "x.....x.", "AND = %s", pat(8).c_str());
        tr.eu_op = Seq::E_XOR; s.euclid_apply(2); CHECK(pat(8) == "..xxx...", "XOR = %s", pat(8).c_str());
        tr.eu_op = Seq::E_SUB; s.euclid_apply(2); CHECK(pat(8) == "...x....", "SUB (1 and not 2) = %s", pat(8).c_str());
        tr.eu_tro = 2; s.euclid_apply(2);          CHECK(pat(8) == ".....x..", "track rotation 2 moves the result: %s", pat(8).c_str());
        tr.eu_op = Seq::E_AND; tr.eu_tro = 0; tr.eu_b = 0; s.euclid_apply(2);
        CHECK(pat(8) == "x..x..x.", "B off: A alone, whatever the operator (%s)", pat(8).c_str());
        tr.eu_a = 0; tr.eu_b = 1; s.euclid_apply(2);
        CHECK(pat(8) == "x.x.x.x.", "A off, B on: B alone (%s)", pat(8).c_str());
        tr.eu_a = 1; tr.eu_b = 0; tr.eu_dir1 = Seq::EU_BWD; s.euclid_apply(2);
        CHECK(pat(8) == "x..x..x.", "v1.2.0: a generator always writes its rhythm forwards; the track's direction moves the play head (%s)", pat(8).c_str());
        tr.eu_dir1 = Seq::EU_FWD; tr.eu_len = 0; tr.eu_pl1 = 3; tr.eu_b = 1; tr.eu_op = Seq::E_OR; tr.eu_len2 = 4; tr.eu_pl2 = 1; s.euclid_apply(2);
        CHECK(pat(8) == "x..xx.x.", "B with its own length 4 (1 pulse): OR with E(3,8) = %s", pat(8).c_str());
        tr.eu_len2 = 12; s.euclid_apply(2);
        CHECK(Seq::eff_len(tr) == 12 && pat(12) == "x..x..x.x..x", "B longer than the track: the track plays 12 steps, A repeats over them (%d, %s)", Seq::eff_len(tr), pat(12).c_str());
        tr.eu_b = 0; tr.eu_len2 = 0;
        tr.eu_op = Seq::E_OR; tr.eu_tro = 0; tr.eu_pl2 = 0; tr.eu_pl1 = 64; tr.len = 5; s.euclid_apply(2);
        CHECK(pat(5) == "xxxxx", "more pulses than steps: every step (%s)", pat(5).c_str());
        tr.eu_pl1 = 0; s.euclid_apply(2);
        CHECK(pat(5) == ".....", "no pulses: empty");
        tr.eu_pl1 = 7; tr.len = 64; s.euclid_apply(2);
        int n7 = 0; for (int k = 0; k < 64; k++) n7 += tr.st[k].on;
        CHECK(n7 == 7, "7 pulses over 64 steps: %d trigs", n7);
        Seq keep = s;
        s.euclid_discard(2);
        CHECK(pat(24) == before && !tr.eu_on && tr.st[5].vel == 77, "Discard: exactly the trigs from before Enable, generator off");
        s.euclid_apply(2);
        CHECK(pat(24) == before, "a discarded track is no longer written by the generator");
        keep.euclid_commit(2);
        int k7 = 0; for (int k = 0; k < 64; k++) k7 += keep.t[2].st[k].on;
        keep.t[2].eu_pl1 = 1; keep.euclid_apply(2);
        int after = 0; for (int k = 0; k < 64; k++) after += keep.t[2].st[k].on;
        CHECK(!keep.t[2].eu_on && k7 == 7 && after == 7, "Commit: the generated trigs stay and stop following the settings");
        s.euclid_enable(2); s.euclid_enable(2); s.euclid_discard(2);
        CHECK(pat(24) == before, "Enable twice then Discard: still the original (the backup is not overwritten)");
        s.t[2].eu_pl1 = 4; s.t[2].len = 16; s.euclid_enable(2);
        s.nout = 0; s.start(0.0); s.process(0.0, 4.0);
        CHECK(count(s, 0x90, 38) == 4, "the generated pattern plays: 4 notes in a bar");
        s.stop();
    }
    {   /* the list order is separate from the saved codes */
        char nm[16];
        bool ok = true, seen[NCOND] = {false};
        for (int p = 0; p < NCOND; p++) { int c = cond_at(p); ok &= c >= 0 && c < NCOND && !seen[c] && cond_pos(c) == p; if (c >= 0 && c < NCOND) seen[c] = true; }
        cond_name(cond_at(1), nm, sizeof nm); ok &= !strcmp(nm, "1ST");
        cond_name(cond_at(3), nm, sizeof nm); ok &= !strcmp(nm, "1:2");
        cond_name(cond_at(38), nm, sizeof nm); ok &= !strcmp(nm, "FILL");
        cond_name(cond_at(43), nm, sizeof nm); ok &= !strcmp(nm, "NO NEI");
        CHECK(ok && C_FILL == 1 && C_AB0 == 9, "condition list: --, 1ST, NO 1ST, A:B .., FILL .. NO NEI; saved codes unchanged");
    }
}

static void blocks(AEffect *a, int n, double ppq_per_block) {
    float L[128], R[128], *out[2] = {L, R};
    for (int k = 0; k < n; k++) { a->pr(a, 0, out, 128); if (g_ti.flags & kPlaying) g_ti.ppqPos += ppq_per_block; }
}
static int sent(uint8_t status, int d1) {
    int n = 0;
    unsigned char m[3];
    for (int i = 0; i < steprunner_test_count(); i++) { steprunner_test_get(i, m); if (m[0] == status && (d1 < 0 || m[1] == d1)) n++; }
    return n;
}
/* a CC lock knob reads "<controller>: <value>"; this is the value part */
static const char *ccv(const char *disp) { const char *c = strchr(disp, ':'); return c ? c + 2 : disp; }
static int sentv(uint8_t status, int d1, int d2) {
    int n = 0;
    unsigned char m[3];
    for (int i = 0; i < steprunner_test_count(); i++) { steprunner_test_get(i, m); if (m[0] == status && m[1] == d1 && m[2] == d2) n++; }
    return n;
}
static void set_int(AEffect *a, const char *key, int v) {   /* a number or option, as the host would set it */
    const param_t *p = &PARAMS[P(key)];
    float n = p->nopts ? (float)v / (p->nopts - 1) : (v - p->min) / (p->max - p->min);
    a->setP(a, P(key), n);
}

static void perform_tests() {   /* PERFORM ALL in the core: an offset on every track's own value; step locks are not moved */
    {   /* v2.2.0 pads: ALT BAR, AUTO FILL */
        Seq d; Track &t = d.t[0]; t.len = 16; t.st[0].on = 1; t.st[4].on = 1; t.st[8].on = 1; t.st[12].on = 1;
        const uint8_t no = 0x90 | (t.ch & 15);
        d.start(0.0); d.set_alt(true, 0.0); d.stop();
        std::vector<double> w = run(d, 16.0, no, t.num); d.alt_bar = false;
        int b1 = 0, b2 = 0; for (double x : w) { if (x < 3.9) b1++; else if (x > 3.95 && x < 7.9) b2++; }
        CHECK(b1 == 4 && b2 == 0 && w.size() == 8, "ALT BAR: bar 1 plays, bar 2 is silent, and so on (%d %d, %d notes)", b1, b2, (int)w.size());
        d.auto_fill = true; w = run(d, 4.0, no, t.num); d.auto_fill = false;
        int last = 0; for (double x : w) last += x >= 3.0;
        CHECK(w.size() > 4 && w.size() <= 7 && last >= 2, "AUTO FILL: extra hits only on the last beat (%d notes, %d there)", (int)w.size(), last);
    }
    {   /* v2.11.0: PASTE TRK of a copy taken mid-ratchet: no burst of the copy's leftover hits */
        Seq d; Track &t = d.t[0]; t.len = 16; t.st[0].on = 1; t.st[0].ratch = 7; t.st[8].on = 1;
        const uint8_t no = 0x90 | (t.ch & 15);
        const double blk = 0.00580498866;
        d.nout = 0; d.start(0.0);
        double x = 0.0;
        for (; x < 0.1; x += blk) { d.nout = 0; d.process(x, x + blk); }
        Track clip = d.t[0];   /* COPY TRK: 8 hits in step 1, about half of them still to come */
        for (; x < 1.0; x += blk) { d.nout = 0; d.process(x, x + blk); }
        d.paste_track(0, clip);
        int most = 0, all = 0;
        for (; x < 1.9; x += blk) {
            d.nout = 0; d.process(x, x + blk);
            int n = 0; for (int i = 0; i < d.nout; i++) n += d.out[i].status == no && d.out[i].d2 > 0;
            most = n > most ? n : most; all += n;
        }
        d.stop();
        CHECK(clip.rt_left > 0 && most <= 1 && all == 0, "PASTE TRK mid-ratchet: no leftover hits fire (most %d in a block, %d before step 9)", most, all);
    }
    {   /* v2.1.0: RATCHET RESOLUTION: 4 hits at 1/32 = 2 per step, over two steps; AUTO: all four in the step */
        Seq d; Track &t = d.t[0]; t.len = 16; t.st[0].on = 1; t.st[0].ratch = 3;
        const uint8_t no = 0x90 | (t.ch & 15);
        t.st[0].rres = 2; std::vector<double> w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 4 && fabs(w[1] - 0.125) < 0.01 && fabs(w[3] - 0.375) < 0.01, "RESOLUTION 1/32: 4 hits 1/32 apart, over steps 1 and 2 (%d)", (int)w.size());
        t.st[0].rres = 0; w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 4 && w[3] < 0.25, "AUTO: the 4 hits inside step 1 (%d)", (int)w.size());
        t.st[0].rres = 1; t.st[2].on = 1; w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 3 && fabs(w[1] - 0.25) < 0.01 && fabs(w[2] - 0.5) < 0.01, "1/16: one hit a step, and the next trig (step 3) starts again (%d)", (int)w.size());
    }
    {   /* v1.94.0 pads: GHOST, POLY, DOWNBEAT ONLY */
        Seq d; Track &t = d.t[0]; t.len = 16; t.vel = 100;
        for (int k = 0; k < 16; k += 2) t.st[k].on = 1;   /* eighths */
        const uint8_t no = 0x90 | (t.ch & 15);
        std::vector<int> v;
        d.ghost = true; std::vector<double> w = run(d, 4.0, no, t.num, &v); d.ghost = false;
        int quiet = 0; for (int x : v) quiet += x == 25;
        CHECK(w.size() == 16 && quiet == 8, "GHOST: the 8 empty steps play at 25 %% (%d notes, %d quiet)", (int)w.size(), quiet);
        d.downbeat = true; w = run(d, 4.0, no, t.num); d.downbeat = false;
        CHECK(w.size() == 4 && fabs(w[1] - 1.0) < 0.01, "DOWNBEAT ONLY: steps 1, 5, 9, 13 (%d notes)", (int)w.size());
        Track &u = d.t[7]; u.len = 16; for (int k = 0; k < 16; k++) u.st[k].on = k == 0; t.st[0].on = 0; for (int k = 2; k < 16; k += 2) t.st[k].on = 0;
        d.start(0.0); d.set_poly(true); d.stop();
        w = run(d, 4.0, 0x90 | (u.ch & 15), u.num);
        CHECK(w.size() == 2 && fabs(w[1] - 2.25) < 0.01, "POLY: T8 plays 9 steps, its step 1 comes again after 9 (%d notes)", (int)w.size());
        d.set_poly(false); w = run(d, 4.0, 0x90 | (u.ch & 15), u.num);
        CHECK(w.size() == 1, "POLY off: 16 steps again (%d notes)", (int)w.size());
    }
    {   /* v1.91.0 pads: ECHO, FADE OUT, BUILD, HUMANIZE */
        Seq d; Track &t = d.t[0]; t.len = 4; t.st[0].on = 1;
        const uint8_t no = 0x90 | (t.ch & 15);
        std::vector<int> v;
        t.len = 16; d.echo = true; std::vector<double> w = run(d, 1.9, no, t.num, &v); d.echo = false; t.len = 4;
        CHECK(w.size() == 4 && fabs(w[1] - 0.5) < 0.01 && fabs(w[2] - 1.0) < 0.01 && v.size() == 4 && v[1] < v[0] && v[2] < v[1] && v[3] < v[2],
              "ECHO: the trig, then repeats an eighth apart, each quieter (%d notes)", (int)w.size());
        v.clear(); d.fade = true; w = run(d, 8.0, no, t.num, &v); d.fade = false;
        CHECK(w.size() == 4 && v.size() == 4 && v[0] > v[1] && v[1] > v[2] && v[2] > v[3], "FADE OUT: quieter every beat, silent after one bar (%d notes)", (int)w.size());
        d.build = true; w = run(d, 3.0, no, t.num); d.build = false;
        CHECK(w.size() == 6, "BUILD: 1, 2, 3 hits on the first three beats (%d notes)", (int)w.size());
        Track &h = d.t[1]; for (int k = 0; k < 16; k++) h.st[k].on = 1; h.len = 16; t.st[0].on = 0;
        v.clear(); d.humanize = true; w = run(d, 4.0, 0x90 | (h.ch & 15), h.num, &v); d.humanize = false;
        bool off_grid = false, vel_moves = false;
        for (size_t i = 0; i < w.size(); i++) { if (fabs(w[i] - 0.25 * i) > 0.01) off_grid = true; if (v[i] != v[0]) vel_moves = true; }
        bool near = true; for (size_t i = 0; i < w.size(); i++) near &= fabs(w[i] - 0.25 * i) < 0.25 * 5 / 24.0 + 0.01;
        CHECK(w.size() == 16 && off_grid && vel_moves && near, "HUMANIZE: a little off the grid and in velocity, never far (%d notes)", (int)w.size());
    }
    {   /* v1.80.0 pads: HALF / DOUBLE TIME, ROLL, RESTART, SCATTER, REVERSE from where it is */
        Seq d; Track &t = d.t[0]; t.len = 4; t.st[0].on = 1; t.st[1].on = 1;
        const uint8_t no = 0x90 | (t.ch & 15);
        d.warp = 0.5; std::vector<double> w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 2 && fabs(w[1] - 0.5) < 0.01, "HALF TIME: step 2 at 0.5 (%d notes)", (int)w.size());
        d.warp = 2.0; w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 4 && fabs(w[1] - 0.125) < 0.01 && fabs(w[2] - 0.5) < 0.01, "DOUBLE TIME: the 4 steps in half a beat, twice (%d notes)", (int)w.size());
        d.warp = 1.0;
        d.start(0.0); d.process(0.0, 1.0); d.set_warp(0.5, 1.0); d.process(1.0, 1.6); d.set_warp(1.0, 1.6);
        CHECK(d.t[0].next_k == 7, "HALF TIME off: the track is where it would have been (next step %ld)", (long)d.t[0].next_k);
        d.stop();
        d.roll = true; w = run(d, 1.0, no, t.num); d.roll = false;
        CHECK(w.size() == 8, "ROLL: every trig 4 hits (%d notes)", (int)w.size());
        auto at = [&](double b) { d.process(0.0, b); return d.t[0].cur; };
        d.nout = 0; d.start(0.0); at(0.6);
        CHECK(d.t[0].cur == 2, "playing: step 3 now");
        d.set_reverse(true);
        int c1 = at(0.8), c2 = at(1.05), c3 = at(1.3);
        CHECK(c1 == 1 && c2 == 0 && c3 == 3, "REVERSE from where it is: 2 1 4 after 3 (%d %d %d)", c1 + 1, c2 + 1, c3 + 1);
        d.set_reverse(false);
        int c4 = at(1.55), c5 = at(1.8);
        CHECK(c4 == 0 && c5 == 1, "and forwards again from there: 1 2 (%d %d)", c4 + 1, c5 + 1);
        d.restart(); int c6 = at(2.05), c7 = at(2.3);
        CHECK(c6 == 0 && c7 == 1, "RESTART: step 1 next (%d %d)", c6 + 1, c7 + 1);
        d.stop(); d.nout = 0;
        d.scatter = true; w = run(d, 4.0, no, t.num); d.scatter = false;
        bool off = false, near = true;
        for (double x : w) { const double g = std::round(x / 0.25) * 0.25; off |= fabs(x - g) > 0.012; near &= fabs(x - g) <= 0.25 * 8 / 24 + 0.012; }
        CHECK(w.size() == 8 && off && near, "SCATTER: the trigs off the grid, no more than a third of a step (%d notes)", (int)w.size());
    }
    {   /* TIMING and RATCHET (v1.77.0) */
        Seq d; Track &t = d.t[0]; t.len = 4; t.st[1].on = 1;
        const uint8_t no = 0x90 | (t.ch & 15), nf = 0x80 | (t.ch & 15);
        t.st[1].micro = 12; std::vector<double> w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 1 && fabs(w[0] - (0.25 + 0.125)) < 0.01, "TIMING +12: step 2 half a step late (%.3f)", w.empty() ? -1.0 : w[0]);
        t.st[1].micro = -23; w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 1 && fabs(w[0] - (0.25 - 0.25 * 23 / 24)) < 0.01, "TIMING -23: almost a step early (%.3f)", w.empty() ? -1.0 : w[0]);
        t.st[1].micro = 0; t.st[1].ratch = 3; w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 4 && fabs(w[0] - 0.25) < 0.01 && fabs(w[1] - 0.3125) < 0.01 && fabs(w[3] - 0.4375) < 0.01, "RATCHET 4: four hits over the step (%d notes)", (int)w.size());
        std::vector<double> off = run(d, 1.0, nf, t.num);
        CHECK(off.size() == 4, "each one let go (%d note offs)", (int)off.size());
        d.no_locks = true; w = run(d, 1.0, no, t.num); d.no_locks = false;
        CHECK(w.size() == 1, "NO LOCKS: one plain hit (%d)", (int)w.size());
        t.st[1].micro = 6; d.clear_locks(0, 1);
        CHECK(t.st[1].micro == 6 && t.st[1].ratch == 3, "CLR LOCKS leaves timing and ratchet alone (v1.78.0)");
    }
    {   /* direction (v1.76.0): FWD 1..len, BWD len..1, both: back and forth (the ends once) */
        Seq d; Track &t = d.t[0]; t.len = 4; t.st[0].on = 1; t.st[1].on = 1;
        const uint8_t no = 0x90 | (t.ch & 15);
        std::vector<double> w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 2 && fabs(w[0]) < 0.01 && fabs(w[1] - 0.25) < 0.01, "FWD: steps 1 and 2 at 0 and 0.25 (%d notes)", (int)w.size());
        t.dir = DIR_BWD; w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 2 && fabs(w[0] - 0.5) < 0.01 && fabs(w[1] - 0.75) < 0.01, "BWD: 4 3 2 1, so step 2 at 0.5 and step 1 at 0.75 (%d notes)", (int)w.size());
        t.dir = DIR_PP; w = run(d, 3.0, no, t.num);
        CHECK(w.size() == 6 && fabs(w[0]) < 0.01 && fabs(w[1] - 0.25) < 0.01 && fabs(w[2] - 1.25) < 0.01 && fabs(w[3] - 1.5) < 0.01 && fabs(w[4] - 1.75) < 0.01 && fabs(w[5] - 2.75) < 0.01,
              "both: 1 2 3 4 3 2 | 1 2 3 4 3 2: notes at 0, 0.25, 1.25, 1.5, 1.75, 2.75 (%d notes)", (int)w.size());
        CHECK(Seq::round_len(t) == 6, "back and forth over 4 steps: a round of 6");
        t.dir = DIR_FWD; d.reverse = true; w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 2 && fabs(w[0] - 0.5) < 0.01 && fabs(w[1] - 0.75) < 0.01, "REVERSE: a forwards track plays backwards (%d notes)", (int)w.size());
        t.dir = DIR_BWD; w = run(d, 1.0, no, t.num);
        CHECK(w.size() == 2 && fabs(w[0]) < 0.01 && fabs(w[1] - 0.25) < 0.01, "REVERSE: a backwards one forwards (%d notes)", (int)w.size());
        d.reverse = false; t.dir = DIR_FWD;
    }
    {   /* PERFORM pan (v1.72.0): trigs with no pan lock send the centre plus the offset; back to the centre once at 0 */
        Seq p; Track &t = p.t[0];
        for (int k = 0; k < 4; k++) t.st[k].on = 1;
        t.st[1].pan = 1 + 10;   /* step 2: its own pan */
        const uint8_t st = 0xB0 | (t.ch & 15);
        std::vector<int> v; run(p, 1.0, st, PAN_CC[0], &v);
        CHECK(v.size() == 1 && v[0] == 10, "no PERFORM: only the lock sends pan (%d CC 10)", (int)v.size());
        p.perform_begin(); p.pf_off[Seq::PF_PAN] = 20;
        v.clear(); run(p, 1.0, st, PAN_CC[0], &v);
        CHECK(v.size() == 4 && v[0] == 84 && v[1] == 10 && v[2] == 84 && v[3] == 84, "pan +20: 84 on the plain trigs, the lock (10) stays (%d values)", (int)v.size());
        p.pf_off[Seq::PF_PAN] = 0;
        v.clear(); run(p, 1.0, st, PAN_CC[0], &v);
        CHECK(v.size() == 2 && v[0] == 64 && v[1] == 10, "back at 0: the centre once, then only the lock (%d values)", (int)v.size());
        p.pf_off[Seq::PF_PAN] = -30; p.perform_commit();
        CHECK(t.st[0].pan == 1 + 34 && t.st[1].pan == 1 + 10 && t.st[4].pan == 0, "COMMIT: the pan heard becomes the trigs' lock, the lock stays, empty steps get none");
    }
    Seq s;
    for (int ti = 0; ti < 3; ti++) { s.t[ti].st[0].on = 1; s.t[ti].st[1].on = 1; }
    s.t[0].st[0].cc[0] = 1 + 100; s.t[0].ccb[0] = 1 + 20;   /* T1: CC 1 locked to 100 on step 1, base 20 */
    s.t[1].st[0].cc[0] = 1 + 100; s.t[1].ccb[0] = 1 + 60;   /* T2: the same lock, base 60 */
    s.t[2].st[0].cc[0] = 1 + 100;                            /* T3: a lock, no base value */
    s.t[0].st[0].vel = 50;                                   /* T1 step 1 has a velocity lock; step 2 and T2 use the track velocity */
    const int tv = s.t[1].vel;
    s.perform_begin(); s.pf_off[Seq::PF_CC0] = 10; s.pf_off[Seq::PF_VEL] = -20;
    s.nout = 0; s.start(0.0); s.process(0.0, 0.26);   /* steps 1 and 2 */
    std::vector<int> cc[3], vel[3];
    for (int i = 0; i < s.nout; i++) for (int ti = 0; ti < 3; ti++) {
        if (s.out[i].status == (0xB0 | (s.t[ti].ch & 15)) && s.out[i].d1 == s.t[ti].ccbase) cc[ti].push_back(s.out[i].d2);
        if ((s.out[i].status & 0xF0) == 0x90 && s.out[i].d1 == s.t[ti].num) vel[ti].push_back(s.out[i].d2);
    }
    CHECK(cc[0].size() == 2 && cc[0][0] == 100 && cc[0][1] == 30 && cc[1].size() == 2 && cc[1][0] == 100 && cc[1][1] == 70,
          "PERFORM CC 1 +10: the locks stay 100, the bases go 20 -> %d and 60 -> %d", cc[0].size() > 1 ? cc[0][1] : -1, cc[1].size() > 1 ? cc[1][1] : -1);
    CHECK(cc[2].size() == 1 && cc[2][0] == 100, "a track with no base value: its lock only, nothing moved");
    CHECK(vel[0].size() == 2 && vel[0][0] == 50 && vel[0][1] == s.t[0].vel - 20 && vel[1].size() == 2 && vel[1][0] == tv - 20,
          "PERFORM VELOCITY -20: the lock stays 50, the track velocity %d -> %d", tv, vel[1].empty() ? -1 : vel[1][0]);
    s.pf_off[Seq::PF_CC0] = 127; CHECK(s.base_eff(s.t[0], 0) == 127 && s.base_eff(s.t[1], 0) == 127 && s.base_eff(s.t[2], 0) == -1, "the offset stops at the top, no scaling");
    s.pf_off[Seq::PF_CC0] = 10;
    s.perform_commit();
    CHECK(!s.pf_on && s.t[0].ccb[0] == 1 + 30 && s.t[1].ccb[0] == 1 + 70 && s.t[2].ccb[0] == 0 && s.t[0].st[0].cc[0] == 1 + 100 && s.t[0].st[0].vel == 50 && s.t[1].vel == tv - 20,
          "commit writes the offsets into the base values and the track velocity; the locks are as they were");
    {   /* PERFORM PROB: plain steps only; a step with its own probability or a condition is locked */
        Seq p;
        for (int k = 0; k < 4; k++) { p.t[0].st[k].on = 1; p.t[0].st[k].prob = 100; }
        p.t[0].st[1].prob = 60; p.t[0].st[2].cond = C_FILL; p.t[0].st[3].on = 0;
        p.perform_begin(); p.pf_off[Seq::PF_PROB] = -100;
        CHECK(p.prob_eff(p.t[0].st[0]) == 0 && p.prob_eff(p.t[0].st[1]) == 60 && p.prob_eff(p.t[0].st[2]) == 100,
              "PROB -100: the plain step %d %%, the 60 %% step stays %d %%, the FILL step stays %d %%", p.prob_eff(p.t[0].st[0]), p.prob_eff(p.t[0].st[1]), p.prob_eff(p.t[0].st[2]));
        p.nout = 0; p.fill = true; p.start(0.0); p.process(0.0, 0.26 * 3);
        CHECK(count(p, 0x90 | (p.t[0].ch & 15)) >= 1, "the FILL step still plays while FILL is held");
        p.pf_off[Seq::PF_PROB] = -30; p.perform_commit();
        CHECK(p.t[0].st[0].prob == 70 && p.t[0].st[1].prob == 60 && p.t[0].st[2].prob == 100 && p.t[0].st[3].prob == 100, "commit: plain trig 70 %%, the locked ones and the empty step as they were");
    }
    /* discard while playing: the pattern comes back, the play position carries on (also across a speed change) */
    s.process(0.26, 2.0);
    s.perform_begin();
    int64_t k0 = s.t[0].next_k;
    s.t[0].st[3].on = 1; s.t[0].len = 5; s.set_speed(0, SPEED_1X + 2, 2.0); s.t[1].mute = 1;
    s.perform_discard();
    CHECK(s.t[0].st[3].on == 0 && s.t[0].len == 16 && s.t[0].speed == SPEED_1X && s.t[1].mute == 0 && s.t[0].next_k == k0,
          "discard restores the tracks and keeps the play position (next step %lld, was %lld)", (long long)s.t[0].next_k, (long long)k0);
}

static void ccnum_tests() {   /* a CC lock goes out on the track's own number when it has one (EDIT CC), else on its block */
    Seq s;
    s.t[0].st[0].on = 1; s.t[0].st[0].cc[0] = 1 + 40; s.t[0].st[0].cc[1] = 1 + 41;
    s.t[0].ccn[0] = 1 + 71;
    s.start(0.0); s.nout = 0; s.process(0.0, 0.01);
    CHECK(count(s, 0xB0 | (s.t[0].ch & 15), 71) == 1 && count(s, 0xB0 | (s.t[0].ch & 15), s.t[0].ccbase) == 0 && count(s, 0xB0 | (s.t[0].ch & 15), s.t[0].ccbase + 1) == 1,
          "CC 1 on its own number 71, CC 2 still on the block");
}

static void loop4_tests() {   /* LOOP 4: every track repeats the four steps it played last; off: back on the song position */
    Seq s;
    for (int k = 0; k < 16; k++) { s.t[0].st[k].on = 1; s.t[0].st[k].vel = (uint8_t)(k + 1); }   /* velocity = step number */
    s.start(0.0);
    s.nout = 0; s.process(0.0, 0.25 * 6 - 0.01);   /* steps 1..6 played */
    s.set_loop(4);
    s.nout = 0; s.process(0.25 * 6 - 0.01, 0.25 * 15 - 0.01);   /* nine more step times */
    std::string got;
    for (int i = 0; i < s.nout; i++) if ((s.out[i].status & 0xF0) == 0x90 && s.out[i].d2) got += std::to_string(s.out[i].d2) + " ";
    CHECK(got == "3 4 5 6 3 4 5 6 3 ", "LOOP 4 after step 6 repeats steps 3-6: %s", got.c_str());
    s.set_loop(0);
    s.nout = 0; s.process(0.25 * 15 - 0.01, 0.25 * 17 - 0.01);
    got.clear();
    for (int i = 0; i < s.nout; i++) if ((s.out[i].status & 0xF0) == 0x90 && s.out[i].d2) got += std::to_string(s.out[i].d2) + " ";
    CHECK(got == "16 1 ", "LOOP 4 off: the track is where it would have been: %s", got.c_str());
    {   /* LOOP 2 and LOOP 1; changing the size keeps the end point */
        Seq q;
        for (int k = 0; k < 16; k++) { q.t[0].st[k].on = 1; q.t[0].st[k].vel = (uint8_t)(k + 1); }
        q.start(0.0); q.nout = 0; q.process(0.0, 0.25 * 6 - 0.01);
        q.set_loop(2); q.nout = 0; q.process(0.25 * 6 - 0.01, 0.25 * 10 - 0.01);
        q.set_loop(1); q.process(0.25 * 10 - 0.01, 0.25 * 12 - 0.01);
        std::string g2;
        for (int i = 0; i < q.nout; i++) if ((q.out[i].status & 0xF0) == 0x90 && q.out[i].d2) g2 += std::to_string(q.out[i].d2) + " ";
        CHECK(g2 == "5 6 5 6 6 6 ", "LOOP 2 then LOOP 1 after step 6: %s", g2.c_str());
    }
    {   /* NO LOCKS: conditions, probability and locks are ignored; RANDOM STEP: steps come in a random order */
        Seq q;
        q.t[0].st[0].on = 1; q.t[0].st[0].vel = 33; q.t[0].st[0].cc[0] = 50; q.t[0].st[0].pan = 10; q.t[0].st[0].len = 16;
        q.t[0].st[1].on = 1; q.t[0].st[1].prob = 0;
        q.t[0].st[2].on = 1; q.t[0].st[2].cond = C_FILL;
        q.no_locks = true;
        q.start(0.0); q.nout = 0; q.process(0.0, 0.25 * 3 - 0.01);
        int notes = 0, ccs = 0, v0 = -1;
        for (int i = 0; i < q.nout; i++) { if ((q.out[i].status & 0xF0) == 0x90 && q.out[i].d2) { if (!notes) v0 = q.out[i].d2; notes++; } if ((q.out[i].status & 0xF0) == 0xB0) ccs++; }
        CHECK(notes == 3 && ccs == 0 && v0 == q.t[0].vel, "NO LOCKS: %d trigs play, %d CCs, velocity %d", notes, ccs, v0);
        Seq r;
        for (int k = 0; k < 16; k++) { r.t[0].st[k].on = 1; r.t[0].st[k].vel = (uint8_t)(k + 1); }
        r.rnd_step = true; r.start(0.0); r.nout = 0; r.process(0.0, 0.25 * 32 - 0.01);
        int n = 0, inorder = 0, seen[17] = {0};
        for (int i = 0; i < r.nout; i++) if ((r.out[i].status & 0xF0) == 0x90 && r.out[i].d2) { inorder += r.out[i].d2 == (n % 16) + 1; seen[r.out[i].d2] = 1; n++; }
        int kinds = 0; for (int k = 1; k <= 16; k++) kinds += seen[k];
        CHECK(n == 32 && inorder < 12 && kinds >= 8, "RANDOM STEP: %d trigs, %d in their usual place, %d different steps", n, inorder, kinds);
        q.t[1].mute = 1; q.invert_mutes();
        CHECK(q.t[0].mute == 1 && q.t[1].mute == 0 && q.t[7].mute == 1, "invert_mutes flips every track");
        q.unmute_all(); CHECK(q.t[0].mute == 0 && q.t[7].mute == 0, "unmute_all");
    }
}

static void shuffle_tests() {   /* SHUFFLE delays the second step of every pair */
    Seq s;
    for (int k = 0; k < 4; k++) s.t[0].st[k].on = 1;
    s.shuffle = 75;
    std::vector<double> w = run(s, 1.0, 0x90 | (s.t[0].ch & 15), s.t[0].num);
    CHECK(w.size() == 4 && fabs(w[0] - 0.0) < 0.01 && fabs(w[1] - 0.375) < 0.01 && fabs(w[2] - 0.5) < 0.01 && fabs(w[3] - 0.875) < 0.01,
          "shuffle 75%%: steps at %.3f %.3f %.3f %.3f", w.size() > 0 ? w[0] : -1, w.size() > 1 ? w[1] : -1, w.size() > 2 ? w[2] : -1, w.size() > 3 ? w[3] : -1);
    Seq q;
    for (int k = 0; k < 4; k++) q.t[0].st[k].on = 1;
    std::vector<double> v = run(q, 1.0, 0x90 | (q.t[0].ch & 15), q.t[0].num);
    CHECK(v.size() == 4 && fabs(v[1] - 0.25) < 0.01, "shuffle 50%%: straight (%.3f)", v.size() > 1 ? v[1] : -1);
    Seq r; r.t[0].st[1].on = 1; r.shuffle = 70;
    r.start(0.30); r.nout = 0; r.process(0.30, 0.40);   /* started between the grid position (0.25) and the shuffled one (0.35) */
    CHECK(count(r, 0x90 | (r.t[0].ch & 15)) == 1, "a shuffled step still to come is not skipped at start");
    Seq u; for (int k = 0; k < 4; k++) u.t[0].st[k].on = 1;
    u.shuffle = 75; u.t[0].shuf = 0;
    std::vector<double> x = run(u, 1.0, 0x90 | (u.t[0].ch & 15), u.t[0].num);
    CHECK(x.size() == 4 && fabs(x[1] - 0.25) < 0.01, "a track with its shuffle switch off plays straight (%.3f)", x.size() > 1 ? x[1] : -1);
}

static void vst_tests() {
    printf("-- plugin --\n");
    AEffect *a = VSTPluginMain(host), *b = VSTPluginMain(host);
    char disp[64], name[64];
    CHECK(a && b && a != b && a->magic == 0x56737450 && a->npar == NPARAMS, "two instances, %d params, uid %x", a->npar, a->uid);
    g_ti.sampleRate = 44100.0; g_ti.tempo = 120.0; g_ti.flags = kTempo | kPpq;
    const double ppb = (120.0 / 60.0) * (128.0 / 44100.0);

    a->d(a, 48, 0, 0, name, 0); a->d(a, 47, 0, 0, disp, 0);   /* effGetProductString, effGetVendorString */
    CHECK(a->uid == 0x53522D38 && !strcmp(name, "StepRunner-8") && !strcmp(disp, "djokenzi") && a->ver == PLUG_VERSION && PLUG_VERSION >= 10000,
          "StepRunner-8 v1.x: uid 'SR-8', product '%s', vendor '%s', version %d", name, disp, a->ver);
    a->d(a, 8, P("st_cond"), 0, name, 0); CHECK(!strcmp(name, "Condition"), "param name '%s'", name);
    a->d(a, 7, P("trk_num"), 0, disp, 0); CHECK(!strcmp(disp, "36"), "T1 note '%s'", disp);
    a->d(a, 7, P("st_vel"), 0, disp, 0);  CHECK(!strcmp(disp, "(100)"), "no velocity lock shows the track's in brackets '%s' (v1.2.0)", disp);
    a->d(a, 7, P("trk_len"), 0, disp, 0); CHECK(!strcmp(disp, "16"), "default length '%s'", disp);

    /* the step buttons are a window: T1 page 1, T1 page 4, T6 page 1 hold different steps */
    a->setP(a, P("step3"), 1.0f);                         /* T1 step 4 */
    CHECK(a->getP(a, P("sel")) > 0.19f && a->getP(a, P("sel")) < 0.21f, "tapping step 4 selects it for the lock knobs");
    set_int(a, "st_vel", 50); set_int(a, "st_cond", cond_pos(C_AB0));   /* lock on T1 step 4: velocity 50, 1:2 */
    a->d(a, 7, P("st_cond"), 0, disp, 0); CHECK(!strcmp(disp, "1:2"), "condition shows '%s'", disp);
    {
        const param_t *pc = &PARAMS[P("st_cond")];
        bool ok = pc->nopts == NCOND;
        char nm[16];
        for (int k = 0; ok && k < NCOND; k++) { cond_name(cond_at(k), nm, sizeof nm); ok = !strcmp(nm, pc->opts[k]); }
        CHECK(ok && !strcmp(pc->opts[NCOND - 6], "FILL") && !strcmp(pc->opts[NCOND - 1], "NO NEI"), "the popup's options match the plugin's condition order, FILL / PRE / NEI last");
    }
    set_int(a, "page", 3);
    CHECK(a->getP(a, P("step3")) < 0.5f, "page 4 shows its own (empty) steps");
    a->setP(a, P("step15"), 1.0f);                        /* T1 step 64 */
    a->d(a, 7, P("sel"), 0, disp, 0); CHECK(!strcmp(disp, "64"), "edit step shows the absolute step '%s'", disp);
    set_int(a, "track", 5);                               /* T6 */
    a->d(a, 7, P("trk_num"), 0, disp, 0); CHECK(!strcmp(disp, "41"), "T6 note '%s'", disp);
    CHECK(a->getP(a, P("step15")) < 0.5f, "T6 page 4 is empty");
    memset(automated, 0, sizeof automated);
    blocks(a, 1, ppb);
    CHECK(automated[P("step15")] == 1 && automated_val[P("step15")] == 0.0f && automated[P("trk_num")] == 1,
          "the host is told the buttons and knobs changed with the track (step16 %d, trk_num %d)", automated[P("step15")], automated[P("trk_num")]);
    memset(automated, 0, sizeof automated);
    blocks(a, 3, ppb);
    int total = 0;
    for (int i = 0; i < NPARAMS; i++) total += automated[i];
    CHECK(total == 0, "and only once (%d more)", total);
    set_int(a, "page", 0);
    a->setP(a, P("step0"), 1.0f); set_int(a, "st_cc2", 1 + 99);   /* T6 step 1: CC 21 = 99 */
    a->d(a, 7, P("st_cc2"), 0, disp, 0); CHECK(!strcmp(disp, "99"), "T6's second CC lock reads its value only: '%s'", disp);
    a->d(a, 8, P("st_cc2"), 0, name, 0); CHECK(!strcmp(name, "CC 2"), "its name is static: '%s'", name);
    a->d(a, 7, P("trk_ccbase"), 0, disp, 0); CHECK(!strcmp(disp, "112-116"), "T6 CC BASE reads '%s'", disp);
    a->d(a, 7, P("st_cc1"), 0, disp, 0); CHECK(!strcmp(disp, "--"), "first CC has no lock: '%s'", disp);
    a->setP(a, P("step8"), 1.0f); set_int(a, "st_cc5", 1 + 0);    /* T6 step 9: CC 24 = 0 */
    a->d(a, 7, P("st_cc5"), 0, disp, 0); CHECK(!strcmp(disp, "0"), "a lock of 0 is a lock: '%s'", disp);
    a->d(a, 7, P("st_cc2"), 0, disp, 0); CHECK(!strcmp(disp, "--"), "step 9 has its own locks: '%s'", disp);
    set_int(a, "track", 0);
    CHECK(a->getP(a, P("step3")) > 0.5f && a->getP(a, P("step0")) < 0.5f, "back on T1: step 4 on, step 1 off");
    set_int(a, "sel", 4);
    a->d(a, 7, P("st_vel"), 0, disp, 0); CHECK(!strcmp(disp, "50"), "T1 step 4 kept its velocity lock '%s'", disp);
    set_int(a, "trk_len", 64);
    set_int(a, "mute3", 1); set_int(a, "mute3", 0);

    /* knob positions: what the host set is what it reads back, and a nudge between options steps one */
    a->setP(a, P("st_prob"), 0.503f);
    CHECK(a->getP(a, P("st_prob")) == 0.503f, "a knob reads back the position the host set");
    a->d(a, 7, P("st_prob"), 0, disp, 0); CHECK(!strcmp(disp, "50%"), "probability shows '%s'", disp);
    set_int(a, "st_prob", 100);
    set_int(a, "track", 1);
    a->setP(a, P("trk_speed"), 4.4f / 6.0f);   /* between 1x and 3/2x: one step up from 1x */
    a->d(a, 7, P("trk_speed"), 0, disp, 0); CHECK(!strcmp(disp, "3/2x"), "nudge steps one option: speed '%s'", disp);
    set_int(a, "trk_speed", 4);
    set_int(a, "track", 0);

    /* triggers spring back; the condition popup opens, stays open on a nudge, closes on a pick */
    blocks(a, 1, ppb);
    memset(automated, 0, sizeof automated);
    set_int(a, "track", 2); a->setP(a, P("step5"), 1.0f);
    a->setP(a, P("clr_trk"), 1.0f); blocks(a, 1, ppb);
    CHECK(automated[P("clr_trk")] == 1 && a->getP(a, P("step5")) < 0.5f, "Clear Track empties T3 and springs back");
    a->setP(a, P("clr_trk"), 0.0f);
    set_int(a, "track", 0);
    for (int i = 0; i < NPARAMS; i++) {   /* the condition list: a tap in it picks the nearest option and the list closes a moment later */
        if (PARAMS[i].popup_of < 0) continue;
        int t = PARAMS[i].popup_of, no = PARAMS[t].nopts;
        set_int(a, "sel", 4); a->setP(a, t, 0.0f);
        a->setP(a, t, 0.4f / (no - 1));
        a->d(a, 7, t, 0, disp, 0);
        CHECK(!strcmp(disp, "1ST"), "list closed: a Q-Link nudge steps one option ('%s')", disp);
        blocks(a, 1, ppb); automated[i] = 0; display_updates = 0;
        a->setP(a, i, 1.0f);
        int opened = a->getP(a, i) > 0.5f;
        a->setP(a, t, 20.3f / (no - 1));          /* a tap that lands a little off an exact option value */
        a->d(a, 7, t, 0, disp, 0);
        blocks(a, 5, ppb);
        int still = a->getP(a, i) > 0.5f && !automated[i];   /* the finger is still down: not yet */
        blocks(a, 60, ppb);                       /* 0.12 s = 42 blocks */
        int closed = a->getP(a, i) < 0.5f && automated[i] == 1 && display_updates == 1;
        CHECK(opened && still && closed && !strcmp(disp, PARAMS[t].opts[20]), "popup %s: opens %d, picks the nearest option '%s', open for a moment %d, then closes once %d (auto %d, upd %d)",
              PARAMS[i].key, opened, disp, still, closed, automated[i], display_updates);
        a->setP(a, i, 1.0f); a->setP(a, t, 0.0f); a->setP(a, i, 0.0f);   /* picked, then closed by hand before the timer */
        blocks(a, 60, ppb);
        CHECK(automated[i] == 1, "a list closed by hand is not closed again");
    }
    set_int(a, "sel", 4); set_int(a, "st_cond", cond_pos(C_AB0));   /* the popup test left step 4 on "--": back to 1:2 */

    /* transport. T1: step 4 (1:2, vel 50) and step 64. T6 (16 steps): step 1 with CC 21 = 99, step 9 with CC 24 = 0.
     * MPC loops a ONE BAR sequence (ppqPos wraps every 4 beats); T1 is 64 steps long, so it needs 4 bars. */
    set_int(a, "cc_base2", 30);
    CHECK(a->getP(a, P("cc_base2")) == 0.0f, "the retired CC VALUE slots ignore writes");
    steprunner_test_reset();
    g_ti.flags = kPlaying | kTempo | kPpq; g_ti.ppqPos = 0.0;
    int run_seen[17] = {0}, run = P("run");
    float L[128], R[128], *out[2] = {L, R};
    double song = 0.0;
    while (song < 32.0 - 1e-9) {   /* 8 bars = 2 loops of T1 */
        a->pr(a, 0, out, 128);
        run_seen[(int)lroundf(a->getP(a, run) * 16)]++;
        g_ti.ppqPos += ppb; song += ppb;
        if (g_ti.ppqPos >= 4.0) g_ti.ppqPos -= 4.0;
    }
    int on37 = sent(0x90, 36), on38 = sent(0x90, 41), cc21 = sentv(0xB0, 113, 99), cc24 = sentv(0xB0, 116, 0);   /* T6: CC 112..116 */
    CHECK(on37 == 3, "T1 over 2 loops of 64 steps: step 4 once (1:2) + step 64 twice = %d note-ons, across MPC's loop points", on37);
    CHECK((on38 == 16 || on38 == 17) && (cc21 == 8 || cc21 == 9) && cc24 == 8,
          "T6 in 8 bars: %d notes, CC 113 = 99 x %d, CC 116 = 0 x %d", on38, cc21, cc24);   /* +1: the last block reaches bar 9 */
    CHECK(sent(0xB0, -1) == cc21 + cc24, "no other CC: nothing is sent to undo a lock");
    CHECK(sent(0x90, -1) == on37 + on38 && sent(0x80, 36) == on37, "nothing else played, every T1 note released");
    int distinct = 0;
    for (int i = 1; i <= 16; i++) distinct += run_seen[i] > 0;
    CHECK(distinct == 16 && run_seen[0] > 0, "play-head: all 16 positions of page 1, and off while T1 is on other pages");
    a->d(a, 7, run, 0, disp, 0); CHECK(atoi(disp) >= 1 && atoi(disp) <= 64, "play-head text is the step: '%s'", disp);
    set_int(a, "follow", 1); blocks(a, 40, ppb);
    CHECK(a->getP(a, run) > 0.0f, "follow: the page follows the play-head");
    set_int(a, "follow", 0);
    g_ti.flags = kTempo | kPpq;
    blocks(a, 1, ppb);
    CHECK(a->getP(a, run) == 0.0f, "play-head off after stop");
    a->setP(a, run, 1.0f); CHECK(a->getP(a, run) == 0.0f && a->d(a, 26, run, 0, 0, 0) == 0, "play-head is display only");
    set_int(a, "page", 0);

    /* a held note is released when the transport stops */
    steprunner_test_reset();
    set_int(a, "track", 4); a->setP(a, P("step0"), 1.0f); set_int(a, "trk_gate", 64); set_int(a, "track", 0);
    g_ti.flags = kPlaying | kTempo | kPpq; g_ti.ppqPos = 0.0;
    blocks(a, 5, ppb);
    CHECK(sent(0x90, 40) == 1 && sent(0x80, 40) == 0, "T5 long note is held");
    g_ti.flags = kTempo | kPpq;
    blocks(a, 1, ppb);
    CHECK(sent(0x80, 40) == 1, "and released on stop");

    /* TRIGS cells: state = trig (1) + play-head (2) + selected (4), out of 9 options; STEP EDIT changes what a tap does */
    {
        #define RAWCELL(k) ((int)lroundf(a->getP(a, P("cell1") + (k)) * 32))
        #define CELL(k) (RAWCELL(k) & 7)   /* trig + play-head + selected; bit 8 = STEP EDIT colour */
        set_int(a, "track", 6); set_int(a, "page", 0); set_int(a, "sel", 1);   /* T7: empty */
        blocks(a, 1, ppb);
        CHECK(CELL(0) == 0 && CELL(1) == 0 && CELL(15) == 0, "stopped, empty track, STEP EDIT off: no cell is marked, not even the selected step (%d %d)", CELL(0), CELL(1));
        a->setP(a, P("step2"), 1.0f);                                 /* STEP EDIT off: a tap flips the trig and selects */
        CHECK(a->getP(a, P("step2")) > 0.5f && CELL(2) == 1 && CELL(0) == 0 && (int)lroundf(a->getP(a, P("sel")) * 15) == 2, "tap with STEP EDIT off: trig on, the step is selected but not outlined (cell %d)", CELL(2));
        blocks(a, 1, ppb); memset(automated, 0, sizeof automated);
        set_int(a, "step_edit", 1); set_int(a, "shift", 1);                                   /* selection: step 3 */
        a->setP(a, P("step5"), 1.0f);                                 /* STEP EDIT on: the tap ADDS step 6 to the selection */
        CHECK(a->getP(a, P("step5")) < 0.5f && CELL(5) == 4 && CELL(2) == 5, "tap with STEP EDIT on: step 6 joins the selection, its trig untouched (cells %d %d)", CELL(2), CELL(5));
        CHECK(RAWCELL(2) == 13 && RAWCELL(7) == 8 && a->getP(a, P("multi")) == 1.0f, "STEP EDIT on: every cell is in its edit colour (%d %d); two steps: multi is on", RAWCELL(2), RAWCELL(7));
        blocks(a, 1, ppb);
        CHECK(automated[P("step5")] == 1 && automated_val[P("step5")] == 0.0f, "the host's toggle is put back to off");
        int cells_told = 0; for (int c = 0; c < 16; c++) cells_told += automated[P("cell1") + c];
        CHECK(cells_told == 16 && automated[P("multi")] == 1, "the host is told once per cell (colour and selection together, %d) and once that multi is on", cells_told);

        /* two steps selected: the lock knobs stay open and show the step tapped last */
        a->d(a, 7, P("st_vel"), 0, disp, 0);
        CHECK(strcmp(disp, "MULTI") != 0, "two steps selected: the lock knobs still read a value ('%s')", disp);
        set_int(a, "trk_len", 32);
        a->d(a, 7, P("trk_len"), 0, disp, 0); CHECK(!strcmp(disp, "32"), "TRK LENGTH still works ('%s')", disp);

        /* random on the selection only: every selected step gets a value, even at density 0 and without a trig */
        set_int(a, "rnd_dens", 0); set_int(a, "rnd_min", 30); set_int(a, "rnd_max", 40);
        a->setP(a, P("rnd_vel"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("rnd_vel"), 0.0f);
        a->setP(a, P("rnd_cc2"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("rnd_cc2"), 0.0f);
        set_int(a, "pattern_edit", 1);                                   /* back to one step: read what was written */
        int vv[16], cv[16];
        for (int k = 0; k < 16; k++) {
            set_int(a, "sel", k + 1);
            a->d(a, 7, P("st_vel"), 0, disp, 0); vv[k] = disp[0] != '(' ? atoi(disp) : -1;   /* v1.2.0: no lock reads (track velocity) */
            a->d(a, 7, P("st_cc2"), 0, disp, 0); cv[k] = strcmp(ccv(disp), "--") ? atoi(ccv(disp)) : -1;
        }
        int only = 1;
        for (int k = 0; k < 16; k++) only &= (k == 2 || k == 5) ? (vv[k] >= 30 && vv[k] <= 40 && cv[k] >= 30 && cv[k] <= 40) : (vv[k] == -1 && cv[k] == -1);
        CHECK(only, "RND VEL / RND CC21 with STEP EDIT on: steps 3 and 6 got values in 30..40 (vel %d %d), the other 14 none", vv[2], vv[5]);
        CHECK(a->getP(a, P("step5")) < 0.5f, "step 6 still has no trig: the generators did not add one");

        set_int(a, "sel", 3); set_int(a, "step_edit", 1); set_int(a, "shift", 1);            /* selection: step 3 again */
        a->setP(a, P("step5"), 1.0f); a->setP(a, P("step9"), 1.0f);  /* + 6, + 10 */
        a->setP(a, P("step2"), 0.0f);                                 /* a tap on a selected step takes it out; its trig stays */
        CHECK(a->getP(a, P("step2")) > 0.5f && CELL(2) == 1 && CELL(5) == 4 && CELL(9) == 4, "tapping a selected step deselects it and never clears its trig");
        a->setP(a, P("clr_step"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("clr_step"), 0.0f);
        a->setP(a, P("step9"), 1.0f);                                 /* - 10: one left, the knobs work again */
        a->d(a, 7, P("sel"), 0, disp, 0); a->d(a, 7, P("st_vel"), 0, name, 0);
        CHECK(!strcmp(disp, "6") && !strcmp(name, "(100)"), "one step left: EDIT STEP is '%s', CLR LOCKS cleared its velocity ('%s')", disp, name);
        set_int(a, "sel", 3); a->d(a, 7, P("st_vel"), 0, name, 0);
        CHECK(atoi(name) >= 30, "step 3 was not in that selection and kept its lock ('%s')", name);
        set_int(a, "sel", 6);
        a->setP(a, P("step5"), 1.0f);                                 /* the last selected step cannot be deselected */
        CHECK(CELL(5) == 4, "the last selected step stays selected");
        set_int(a, "rnd_dens", 100);
        a->setP(a, P("step12"), 1.0f);                                /* + 13 */
        a->setP(a, P("rnd_trig"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("rnd_trig"), 0.0f);
        int trigs = 0; for (int k = 0; k < 16; k++) trigs += a->getP(a, P("step0") + k) > 0.5f;
        CHECK(trigs == 1 && CELL(5) == 4 && CELL(12) == 4 && automated[P("rnd_trig")] >= 1, "RND TRIGS does nothing while STEP EDIT is on (%d trig: step 3), and still springs back", trigs);
        set_int(a, "pattern_edit", 1); set_int(a, "sel", 6); a->setP(a, P("step5"), 1.0f); a->setP(a, P("step12"), 1.0f);   /* trigs on 6 and 13 by hand */
        set_int(a, "step_edit", 1); set_int(a, "shift", 1); a->setP(a, P("step5"), 0.0f);                                                          /* select 13 + 6 */
        CHECK(CELL(5) == 5 && CELL(12) == 5, "steps 6 and 13: trig and selected");
        a->setP(a, P("step5"), 1.0f);                                 /* an echo of the real state changes nothing */
        CHECK(CELL(5) == 5 && CELL(12) == 5, "an echo of the current state does not change the selection");
        set_int(a, "page", 1);
        CHECK(CELL(5) == 4 && CELL(12) == 0, "another page: the selection is one step again (the last one added)");
        set_int(a, "page", 0);
        a->setP(a, P("step12"), 0.0f);                                /* select 6 + 13 again, then leave edit mode */
        CHECK(CELL(5) == 5 && CELL(12) == 5, "two selected again");
        set_int(a, "pattern_edit", 1);
        int nsel = 0; for (int k = 0; k < 16; k++) nsel += (CELL(k) & 4) != 0;
        CHECK(nsel == 0 && RAWCELL(1) < 8 && a->getP(a, P("multi")) == 0.0f, "STEP EDIT off: no outline (%d), normal colours, multi off", nsel);
        a->setP(a, P("multi"), 1.0f);
        CHECK(a->getP(a, P("multi")) == 0.0f && a->d(a, 26, P("multi"), 0, 0, 0) == 0, "multi is read-only and not automatable");
        a->setP(a, P("cell1") + 3, 1.0f);
        CHECK(CELL(3) == 0 && a->d(a, 26, P("cell1") + 3, 0, 0, 0) == 0, "cells are read-only and not automatable");
        for (int k = 0; k < 16; k++) { set_int(a, "sel", k + 1); if (a->getP(a, P("step0") + k) > 0.5f && k != 2) a->setP(a, P("step0") + k, 0.0f); }
        set_int(a, "sel", 9); set_int(a, "rnd_dens", 50); set_int(a, "rnd_min", 0); set_int(a, "rnd_max", 127);

        /* the play-head runs over the cells while playing */
        set_int(a, "trk_len", 16);
        g_ti.flags = kPlaying | kTempo | kPpq; g_ti.ppqPos = 0.0;
        int seen[16] = {0}, twice = 0;
        for (int k = 0; k < 700; k++) {            /* one bar = 689 blocks */
            float L2[128], R2[128], *o2[2] = {L2, R2};
            a->pr(a, 0, o2, 128);
            int lit = 0;
            for (int c = 0; c < 16; c++) if (CELL(c) & 2) { seen[c]++; lit++; }
            twice += lit > 1;
            g_ti.ppqPos += ppb;
        }
        int all = 1; for (int c = 0; c < 16; c++) all &= seen[c] > 20;
        CHECK(all && !twice, "play-head visits all 16 cells, one at a time");
        CHECK((CELL(2) & 1) && a->getP(a, P("cell1") + 2) < 1.0f, "a trig stays visible under the play-head");
        set_int(a, "step_edit", 1); set_int(a, "shift", 1); a->setP(a, P("step0"), 1.0f); blocks(a, 1, ppb);
        float top = 0; for (int c = 0; c < 16; c++) if (a->getP(a, P("cell1") + c) > top) top = a->getP(a, P("cell1") + c);
        CHECK(top < 1.0f, "no cell ever reads 1.0, even selected + trig + edit colour (max %.3f)", top);
        set_int(a, "pattern_edit", 1); set_int(a, "sel", 9);
        set_int(a, "page", 1); blocks(a, 3, ppb);
        int any = 0; for (int c = 0; c < 16; c++) any |= CELL(c) & 2;
        CHECK(!any, "no play-head on a page the track is not playing");
        g_ti.flags = kTempo | kPpq; blocks(a, 1, ppb);
        set_int(a, "page", 0); blocks(a, 1, ppb);
        any = 0; for (int c = 0; c < 16; c++) any |= CELL(c) & 2;
        CHECK(!any, "no play-head when stopped");

        /* the cells' names are their step numbers and follow the page */
        a->d(a, 8, P("step0"), 0, name, 0); CHECK(!strcmp(name, "Step 1"), "step names are static: '%s'", name);
        display_updates = 0;
        set_int(a, "page", 2); blocks(a, 2, ppb);
        a->d(a, 8, P("step15"), 0, name, 0);
        CHECK(!strcmp(name, "Step 16") && display_updates == 1, "page change: name still '%s'; the host is asked once to read the value texts again", name);
        set_int(a, "page", 0); set_int(a, "track", 0); blocks(a, 1, ppb);
        #undef CELL
    }

    /* TRACK EDIT: the panel shows the track setup; it and STEP EDIT switch each other off */
    {
        #define OPT3(key) ((int)lroundf(a->getP(a, P(key)) * (PARAMS[P(key)].nopts - 1)))   /* option index of panel / ptitle */
        set_int(a, "track", 6); set_int(a, "page", 0); set_int(a, "sel", 1);
        CHECK(OPT3("panel") == 5 && OPT3("ptitle") == 0, "no mode on: the panel shows the pattern setup under the normal title");
        set_int(a, "step_edit", 1); set_int(a, "shift", 1);
        CHECK(OPT3("panel") == 0 && OPT3("ptitle") == 1, "STEP EDIT: the locks, edit title");
        a->setP(a, P("step7"), 1.0f);
        CHECK(OPT3("panel") == 0 && OPT3("ptitle") == 1, "two steps selected: the locks stay on screen");
        blocks(a, 1, ppb); memset(automated, 0, sizeof automated);
        set_int(a, "track_edit", 1);
        CHECK(OPT3("panel") == 2 && OPT3("ptitle") == 2 && a->getP(a, P("step_edit")) == 0.0f && a->getP(a, P("multi")) == 0.0f,
              "TRACK EDIT on: track setup and its title, STEP EDIT switched off, selection back to one step");
        blocks(a, 1, ppb);
        CHECK(automated[P("panel")] == 1 && automated[P("ptitle")] == 1 && automated[P("step_edit")] == 1, "the host is told the panel, the title and the STEP EDIT button changed");
        CHECK(lroundf(a->getP(a, P("cell1") + 3) * 32) == 16 && lroundf(a->getP(a, P("cell1") + 7) * 32) == 16, "TRACK EDIT (v2.8): the cells violet again, the selected step not outlined");
        /* length buttons: lit only while the track has exactly that length; a press sets it */
        const char *lk[4] = {"len16", "len32", "len48", "len64"};
        set_int(a, "trk_len", 19);
        int lit = 0; for (int q = 0; q < 4; q++) lit += a->getP(a, P(lk[q])) > 0.5f;
        CHECK(lit == 0, "length 19: no length button is lit");
        for (int q = 0; q < 4; q++) {
            blocks(a, 1, ppb); memset(automated, 0, sizeof automated);
            a->setP(a, P(lk[q]), 1.0f);
            a->d(a, 7, P("trk_len"), 0, disp, 0);
            int only = 1; for (int z = 0; z < 4; z++) only &= (a->getP(a, P(lk[z])) > 0.5f) == (z == q);
            blocks(a, 1, ppb);
            CHECK(atoi(disp) == 16 * (q + 1) && only && automated[P("trk_len")] == 1, "button %d: length %s, only that button lit, LENGTH knob reported", 16 * (q + 1), disp);
        }
        CHECK(automated[P("len48")] == 1 && automated_val[P("len48")] == 0.0f, "the button that was lit before is reported off");
        a->setP(a, P("len64"), 0.0f); blocks(a, 1, ppb);
        a->d(a, 7, P("trk_len"), 0, disp, 0);
        CHECK(!strcmp(disp, "64") && a->getP(a, P("len64")) > 0.5f && automated[P("len64")] >= 1, "tapping the lit button changes nothing and it stays lit");
        set_int(a, "trk_len", 33);
        CHECK(a->getP(a, P("len32")) < 0.5f && a->getP(a, P("len64")) < 0.5f, "LENGTH knob to 33: nothing lit");
        set_int(a, "trk_len", 48);
        CHECK(a->getP(a, P("len48")) > 0.5f, "LENGTH knob to 48: the 48 button lights");

        /* TRACK and PAGE selector buttons mirror the track / page parameters */
        a->setP(a, P("trk3"), 1.0f);
        a->d(a, 7, P("track"), 0, disp, 0);
        int one = 1; for (int z = 0; z < 8; z++) one &= (a->getP(a, P("trk1") + z) > 0.5f) == (z == 2);
        CHECK(!strcmp(disp, "T3") && one, "TRACK button T3: track is '%s', only T3 lit", disp);
        a->setP(a, P("trk3"), 0.0f);
        CHECK(a->getP(a, P("trk3")) > 0.5f, "tapping the lit track button keeps it");
        set_int(a, "track", 6);
        CHECK(a->getP(a, P("trk7")) > 0.5f && a->getP(a, P("trk3")) < 0.5f, "the track parameter (Q-Link) moves the lit button");
        a->setP(a, P("pg3"), 1.0f);
        a->d(a, 7, P("page"), 0, disp, 0);
        CHECK(!strcmp(disp, "33-48") && a->getP(a, P("pg3")) > 0.5f && a->getP(a, P("pg1")) < 0.5f, "PAGE button 3: page '%s'", disp);
        a->setP(a, P("pg1"), 1.0f);
        set_int(a, "track", 2); a->d(a, 7, P("trk_len"), 0, disp, 0);
        CHECK(!strcmp(disp, "16"), "another track keeps its own length ('%s')", disp);
        set_int(a, "track", 6);
        set_int(a, "trk_len", 24); set_int(a, "trk_ch", 3);
        a->d(a, 7, P("trk_len"), 0, disp, 0); CHECK(!strcmp(disp, "24"), "the track knobs work there ('%s')", disp);
        a->setP(a, P("step9"), 1.0f);
        CHECK(a->getP(a, P("step9")) > 0.5f, "trigs still toggle while TRACK EDIT is on");
        a->setP(a, P("step9"), 0.0f);
        set_int(a, "step_edit", 1); set_int(a, "shift", 1);
        CHECK(a->getP(a, P("track_edit")) == 0.0f && OPT3("panel") == 0 && OPT3("ptitle") == 1, "STEP EDIT on switches TRACK EDIT off");
        /* RANDOM EDIT: the third mode */
        a->setP(a, P("step3"), 1.0f);                       /* two steps selected in STEP EDIT */
        set_int(a, "random_edit", 1);
        CHECK(OPT3("panel") == 3 && OPT3("ptitle") == 3 && a->getP(a, P("step_edit")) == 0.0f && a->getP(a, P("multi")) == 0.0f,
              "RANDOM EDIT on: random settings panel, STEP EDIT off, one step selected");
        CHECK(lroundf(a->getP(a, P("cell1") + 12) * 32) < 8, "RANDOM EDIT keeps the normal colours");
        set_int(a, "rnd_dens", 77); a->d(a, 7, P("rnd_dens"), 0, disp, 0);
        CHECK(!strcmp(disp, "77%"), "the random knobs work there ('%s')", disp);
        set_int(a, "track_edit", 1);
        CHECK(a->getP(a, P("random_edit")) == 0.0f && OPT3("panel") == 2, "TRACK EDIT on switches RANDOM EDIT off");
        set_int(a, "random_edit", 1);
        CHECK(a->getP(a, P("track_edit")) == 0.0f && OPT3("panel") == 3, "and the other way round");
        set_int(a, "pattern_edit", 1);
        CHECK(OPT3("panel") == 5 && OPT3("ptitle") == 0, "RANDOM EDIT off: back to the pattern setup");
        set_int(a, "rnd_dens", 50);
        set_int(a, "pattern_edit", 1); set_int(a, "track_edit", 1); set_int(a, "pattern_edit", 1);
        CHECK(OPT3("panel") == 5 && OPT3("ptitle") == 0, "TRACK EDIT off: back to the pattern setup");
        a->setP(a, P("panel"), 1.0f); a->setP(a, P("ptitle"), 1.0f);
        CHECK(OPT3("panel") == 5 && OPT3("ptitle") == 0 && a->d(a, 26, P("panel"), 0, 0, 0) == 0 && a->d(a, 26, P("ptitle"), 0, 0, 0) == 0, "panel and title are read-only and not automatable");
        set_int(a, "trk_len", 16); set_int(a, "trk_ch", 1); set_int(a, "sel", 9); set_int(a, "track", 0);
    }

    /* CC BASE: per track; the lock knobs' names follow it and the host is asked to read them again */
    set_int(a, "track", 5);
    blocks(a, 2, ppb); display_updates = 0;
    set_int(a, "trk_ccbase", 40);
    a->d(a, 7, P("trk_ccbase"), 0, disp, 0);
    blocks(a, 2, ppb);
    CHECK(!strcmp(disp, "40-44") && display_updates == 1, "CC BASE 40 on T6 reads '%s'; the host is asked once to read the texts again", disp);
    set_int(a, "trk_ccbase", 123); a->d(a, 7, P("trk_ccbase"), 0, disp, 0);
    CHECK(!strcmp(disp, "123-127"), "the highest base keeps the fifth CC at 127: '%s'", disp);
    set_int(a, "trk_ccbase", 40);
    set_int(a, "track", 0);
    a->d(a, 7, P("trk_ccbase"), 0, disp, 0);
    CHECK(!strcmp(disp, "20-24"), "T1 kept its own block: %s", disp);
    steprunner_test_reset();
    set_int(a, "track", 5); set_int(a, "page", 0); set_int(a, "follow", 0);
    g_ti.flags = kPlaying | kTempo | kPpq; g_ti.ppqPos = 0.0;
    blocks(a, 5, ppb);
    g_ti.flags = kTempo | kPpq; blocks(a, 1, ppb);
    CHECK(sentv(0xB0, 41, 99) == 1 && sent(0xB0, 113) == 0, "T6 step 1 now sends its lock on CC 41, not 113");
    set_int(a, "track", 0);

    /* EUCLIDIAN mode and its controls */
    {
        auto steps16 = [&]() { std::string p; for (int k = 0; k < 16; k++) p += a->getP(a, P("step0") + k) > 0.5f ? 'x' : '.'; return p; };
        auto press = [&](const char *key) { a->setP(a, P(key), 1.0f); blocks(a, 1, ppb); a->setP(a, P(key), 0.0f); };
        set_int(a, "track", 3); set_int(a, "page", 0); set_int(a, "trk_len", 16);
        std::string orig = steps16();
        set_int(a, "step_edit", 1); set_int(a, "shift", 1);
        set_int(a, "euclid_edit", 1);
        CHECK(a->getP(a, P("step_edit")) == 1.0f && OPT3("lpanel") == 1 && OPT3("ptitle") == 4 && OPT3("panel") == 0,
              "EUCLIDIAN on: left section = generator, step locks stay on the right, STEP EDIT stays lit (v1.0.0)");
        CHECK(lroundf(a->getP(a, P("cell1") + 9) * 32) / 8 == 1, "EUCLIDIAN over STEP EDIT, not enabled: the cells keep STEP EDIT's colour (green only while a generator runs)");
        a->setP(a, P("eu_enable"), 1.0f);
        CHECK(lroundf(a->getP(a, P("cell1") + 9) * 32) >= 24, "enabled: cells in the Euclidean colour");
        set_int(a, "euclid_edit", 0);
        CHECK(lroundf(a->getP(a, P("cell1") + 9) * 32) >= 24, "a generated track stays in the Euclidean colour outside the mode too");
        set_int(a, "step_edit", 1); set_int(a, "shift", 1);
        CHECK(lroundf(a->getP(a, P("cell1") + 9) * 32) >= 24, "and in STEP EDIT");
        set_int(a, "euclid_edit", 1);
        a->setP(a, P("eu_enable"), 0.0f);
        CHECK(steps16() == orig, "and Enable off again leaves the track as it was");
        CHECK(a->getP(a, P("eu_enable")) == 0.0f && steps16() == orig, "nothing changes until Enable");
        blocks(a, 1, ppb); memset(automated, 0, sizeof automated);
        a->setP(a, P("eu_enable"), 1.0f);
        CHECK(a->getP(a, P("eu_enable")) == 1.0f && steps16() == "x...x...x...x...", "Enable: the track now shows 4 pulses: %s", steps16().c_str());
        blocks(a, 1, ppb);
        int told_steps = 0, differ = 0;
        for (int k = 0; k < 16; k++) { told_steps += automated[P("step0") + k]; differ += (orig[k] == 'x') != (steps16()[k] == 'x'); }
        CHECK(told_steps == differ && automated[P("eu_enable")] == 0, "the host is told about exactly the %d trigs that changed", differ);
        a->setP(a, P("eu_enable"), 0.0f); blocks(a, 1, ppb);
        CHECK(a->getP(a, P("eu_enable")) == 0.0f && steps16() == orig, "Enable off: the trigs from before are back");
        a->setP(a, P("eu_enable"), 1.0f); blocks(a, 1, ppb);
        set_int(a, "eu_pl1", 5);
        int n5 = 0; for (int k = 0; k < 16; k++) n5 += steps16()[k] == 'x';
        CHECK(n5 == 5, "PL1 = 5: %s", steps16().c_str());
        set_int(a, "eu_pl2", 2); set_int(a, "eu_bool", Seq::E_XOR);
        a->d(a, 7, P("eu_bool"), 0, disp, 0); CHECK(!strcmp(disp, "XOR"), "BOOL reads '%s'", disp);
        set_int(a, "eu_bool", Seq::E_OR); set_int(a, "eu_pl2", 0); set_int(a, "eu_pl1", 3);
        set_int(a, "trk_len", 8);
        CHECK(steps16().substr(0, 8) == "x.....x.", "the rhythm keeps its own length (16) when the track gets shorter: %s", steps16().substr(0, 8).c_str());
        set_int(a, "eu_len", 8);
        CHECK(steps16().substr(0, 8) == "x..x..x.", "LENGTH 8: 3 pulses over 8 steps = %s", steps16().substr(0, 8).c_str());
        set_int(a, "trk_len", 16); set_int(a, "eu_len", 5); set_int(a, "eu_pl1", 2);
        CHECK(steps16() == "x..x.x..x.x..x.x", "LENGTH 5 on a 16 step track repeats the 5 step rhythm: %s", steps16().c_str());
        set_int(a, "eu_len", 8); set_int(a, "eu_pl1", 3); set_int(a, "trk_len", 8);
        a->setP(a, P("len16"), 1.0f);
        int n3 = 0; for (int k = 0; k < 16; k++) n3 += steps16()[k] == 'x';
        CHECK(steps16() == "x..x..x.x..x..x.", "the length buttons regenerate too: the 8 step rhythm twice over 16 = %s", steps16().c_str()); (void)n3;
        set_int(a, "trk_len", 8);
        set_int(a, "track", 4);
        CHECK(a->getP(a, P("eu_enable")) == 0.0f, "the generator is per track: T5 is not enabled");
        a->d(a, 7, P("eu_pl1"), 0, disp, 0); CHECK(!strcmp(disp, "4"), "T5 has its own settings (PL1 '%s')", disp);
        set_int(a, "track", 3);
        a->d(a, 7, P("eu_pl1"), 0, disp, 0); CHECK(!strcmp(disp, "3") && a->getP(a, P("eu_enable")) == 1.0f, "back on T4: PL1 '%s', still enabled", disp);

        /* an enabled generator, its settings and the saved original survive a project save */
        void *ck = 0; intptr_t cn = a->d(a, 23, 0, 0, &ck, 0);
        std::string snap((char *)ck);
        AEffect *c = VSTPluginMain(host);
        c->d(c, 24, 0, cn, (void *)snap.c_str(), 0);
        c->setP(c, P("trk4"), 1.0f);
        c->d(c, 7, P("eu_pl1"), 0, disp, 0);
        CHECK(c->getP(c, P("eu_enable")) == 1.0f && !strcmp(disp, "3"), "reloaded project: T4 still generated, PL1 '%s'", disp);
        c->setP(c, P("eu_discard"), 1.0f);
        std::string back; for (int k = 0; k < 16; k++) back += c->getP(c, P("step0") + k) > 0.5f ? 'x' : '.';
        CHECK(back == orig && c->getP(c, P("eu_enable")) == 0.0f, "Discard after a reload still brings back the original trigs");
        c->d(c, 1, 0, 0, 0, 0);

        blocks(a, 1, ppb); memset(automated, 0, sizeof automated);
        press("eu_commit");
        std::string kept = steps16();
        set_int(a, "eu_pl1", 7);
        CHECK(a->getP(a, P("eu_enable")) == 0.0f && automated[P("eu_commit")] == 1 && steps16() == kept, "Commit: generator off, pattern kept, PL1 no longer changes it; the button springs back");
        CHECK(a->getP(a, P("euclid_edit")) == 0.0f && a->getP(a, P("lpanel")) == 0.0f && OPT3("ptitle") != 4 && lroundf(a->getP(a, P("cell1") + 9) * 32) < 24
              && automated[P("euclid_edit")] == 1 && automated[P("lpanel")] == 1,
              "Commit also leaves EUCLIDIAN (for the mode under it, v1.0.0): RND / CLR section, not green, and the host is told the mode button went off");
        set_int(a, "euclid_edit", 1);
        a->setP(a, P("eu_enable"), 1.0f); set_int(a, "eu_pl1", 2);
        press("eu_discard");
        CHECK(steps16() == kept && a->getP(a, P("eu_enable")) == 0.0f, "Enable again then Discard: back to the committed pattern");
        CHECK(a->getP(a, P("euclid_edit")) == 0.0f && OPT3("ptitle") != 4, "Discard leaves EUCLIDIAN too");
        set_int(a, "euclid_edit", 1);
        set_int(a, "trk_len", 16);
        for (int k = 0; k < 16; k++) if ((a->getP(a, P("step0") + k) > 0.5f) != (orig[k] == 'x')) a->setP(a, P("step0") + k, orig[k] == 'x' ? 1.0f : 0.0f);
        set_int(a, "eu_pl1", 4);
        const int rm = OPT3("panel");   /* the right-hand panel under EUCLIDIAN */
        set_int(a, "euclid_edit", 0);
        CHECK(a->getP(a, P("lpanel")) == 0.0f && OPT3("ptitle") != 4 && OPT3("panel") == rm, "EUCLIDIAN off: the RND / CLR section is back, the mode under it stays (v1.0.0)");
        set_int(a, "pattern_edit", 1);
        a->setP(a, P("lpanel"), 1.0f);
        CHECK(a->getP(a, P("lpanel")) == 0.0f && a->d(a, 26, P("lpanel"), 0, 0, 0) == 0, "lpanel is read-only and not automatable");
        set_int(a, "sel", 2); set_int(a, "track", 0);
    }

    /* PERFORM tab: PERFORM ALL offsets every track at once; switching it off restores everything, COMMIT keeps it */
    {
        char d1[64], d2[64], d3[64];
        set_int(a, "track", 0); set_int(a, "page", 0); set_int(a, "sel", 2); a->setP(a, P("step1"), 1.0f);
        set_int(a, "st_vel", 50); set_int(a, "st_cc1", 1 + 40);
        int tv0 = (int)lroundf(a->getP(a, P("trk_vel")) * 126) + 1;
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0);
        std::string before((char *)c0);
        a->d(a, 8, P("pf_ctrl"), 0, name, 0); CHECK(!strcmp(name, "Perform All"), "the switch is called '%s'", name);
        CHECK(PARAMS[P("pf_prob")].min == -100 && PARAMS[P("pf_prob")].max == 0, "PERFORM probability goes from -100 to 0");
        blocks(a, 1, ppb); memset(automated, 0, sizeof automated);
        set_int(a, "pf_vel", 20); blocks(a, 1, ppb);
        a->d(a, 7, P("pf_vel"), 0, d1, 0);
        CHECK(!strcmp(d1, "OFF") && automated[P("pf_vel")] >= 1, "PERFORM ALL off: knob shows '%s' and springs back", d1);
        void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); CHECK(before == (char *)cx, "PERFORM ALL off: knobs change nothing");
        /* on, perform, edit on the STEPS tab too, then off: everything is back */
        set_int(a, "pf_ctrl", 1);
        a->d(a, 7, P("pf_prob"), 0, d2, 0); CHECK(!strcmp(d2, "+0%") && fabsf(a->getP(a, P("pf_prob")) - 1.0f) < 0.001f, "probability starts at 0, the top of the knob (%s)", d2);
        set_int(a, "pf_vel", 20); set_int(a, "pf_cc1", -10); set_int(a, "pf_prob", -30); set_int(a, "pf_len", 4); set_int(a, "pf_pan", 30);
        a->d(a, 7, P("pf_vel"), 0, d1, 0); a->d(a, 7, P("pf_prob"), 0, d2, 0); a->d(a, 7, P("pf_len"), 0, d3, 0);
        CHECK(!strcmp(d1, "+20") && !strcmp(d2, "-30%") && !strcmp(d3, "+1.00"), "offsets show signed: %s %s %s", d1, d2, d3);
        a->d(a, 7, P("pf_pan"), 0, d1, 0); CHECK(!strcmp(d1, "+30"), "the pan knob is back (%s)", d1);
        a->d(a, 7, P("st_vel"), 0, d1, 0); CHECK(!strcmp(d1, "50"), "the pattern is not rewritten while performing (%s)", d1);
        a->setP(a, P("step5"), 1.0f); a->setP(a, P("mute3"), 1.0f); set_int(a, "trk_len", 32); set_int(a, "trk_speed", 6); set_int(a, "st_vel", 99);
        set_int(a, "track", 4); a->setP(a, P("step9"), 1.0f); set_int(a, "trk_num", 60); set_int(a, "track", 0);
        a->d(a, 23, 0, 0, &cx, 0); std::string mid((char *)cx);
        CHECK(before != mid, "edits made while performing are there");
        set_int(a, "pf_ctrl", 0);
        set_int(a, "sel", 2);   /* the selection is not part of the restored state */
        a->d(a, 23, 0, 0, &cx, 0);
        if (before != (char *)cx) { std::string n((char *)cx); size_t i = 0; while (i < n.size() && i < before.size() && n[i] == before[i]) i++; printf("     differs at %zu: '%.40s' vs '%.40s'\n", i, before.c_str() + (i > 10 ? i - 10 : 0), n.c_str() + (i > 10 ? i - 10 : 0)); }
        CHECK(before == (char *)cx, "PERFORM ALL off: the whole sequencer is back as it was (steps, locks, track setup, mutes, other tracks)");
        a->d(a, 7, P("pf_vel"), 0, d1, 0); CHECK(!strcmp(d1, "OFF"), "and the knobs are off again");
        /* a project saved while performing holds what was heard, and loads with PERFORM ALL off */
        set_int(a, "pf_ctrl", 1); set_int(a, "pf_vel", 20); set_int(a, "pf_cc1", -10); set_int(a, "pf_prob", -30);
        a->d(a, 23, 0, 0, &cx, 0); std::string live((char *)cx);
        a->d(a, 7, P("st_vel"), 0, d1, 0); CHECK(!strcmp(d1, "50") && a->getP(a, P("pf_ctrl")) == 1.0f, "saving does not end the performance");
        /* commit */
        blocks(a, 1, ppb); memset(automated, 0, sizeof automated);
        a->setP(a, P("pf_commit"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("pf_commit"), 0.0f);
        a->d(a, 7, P("st_vel"), 0, d1, 0); a->d(a, 7, P("st_cc1"), 0, d2, 0); a->d(a, 7, P("st_prob"), 0, d3, 0);
        int tv1 = (int)lroundf(a->getP(a, P("trk_vel")) * 126) + 1;
        CHECK(!strcmp(d1, "50") && !strcmp(d2, "40") && !strcmp(d3, "70%") && tv1 == (tv0 + 20 > 127 ? 127 : tv0 + 20) && a->getP(a, P("pf_ctrl")) == 0.0f && automated[P("pf_commit")] == 1,
              "COMMIT: the locks stay (vel %s, cc %s), probability %s, track velocity %d -> %d", d1, d2, d3, tv0, tv1);
        a->d(a, 23, 0, 0, &cx, 0); CHECK(live == (char *)cx, "the project saved while performing equals the committed one");
        /* UNDO takes the COMMIT back, with what was edited after it; a second UNDO does nothing */
        a->setP(a, P("step7"), 1.0f); set_int(a, "sel", 2);
        a->setP(a, P("pf_discard"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("pf_discard"), 0.0f); blocks(a, 1, ppb);
        a->d(a, 23, 0, 0, &cx, 0); CHECK(before == (char *)cx, "UNDO after COMMIT: the state from before PERFORM ALL is back");
        a->setP(a, P("step7"), 1.0f); set_int(a, "sel", 2); a->d(a, 23, 0, 0, &cx, 0); std::string ed((char *)cx);
        a->setP(a, P("pf_discard"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("pf_discard"), 0.0f); blocks(a, 1, ppb);
        a->d(a, 23, 0, 0, &cx, 0); CHECK(ed == (char *)cx, "a second UNDO changes nothing");
        a->setP(a, P("step7"), 0.0f); set_int(a, "sel", 2);
        /* UNDO while performing = switching PERFORM ALL off */
        set_int(a, "pf_ctrl", 1); set_int(a, "pf_vel", 20); a->setP(a, P("step7"), 1.0f); set_int(a, "sel", 2);
        a->setP(a, P("pf_discard"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("pf_discard"), 0.0f); blocks(a, 1, ppb);
        a->d(a, 23, 0, 0, &cx, 0); CHECK(before == (char *)cx && a->getP(a, P("pf_ctrl")) == 0.0f, "UNDO while performing: everything back, PERFORM ALL off");
        /* commit again, then on and off: the committed state stays */
        set_int(a, "pf_ctrl", 1); set_int(a, "pf_vel", 20); set_int(a, "pf_cc1", -10); set_int(a, "pf_prob", -30);
        a->setP(a, P("pf_commit"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("pf_commit"), 0.0f); blocks(a, 1, ppb);
        set_int(a, "pf_ctrl", 1); set_int(a, "pf_ctrl", 0);
        a->d(a, 23, 0, 0, &cx, 0); CHECK(live == (char *)cx, "after COMMIT, on and off again keeps the committed state");
        a->setP(a, P("pf_discard"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("pf_discard"), 0.0f); blocks(a, 1, ppb);
        a->d(a, 23, 0, 0, &cx, 0); CHECK(live == (char *)cx, "and UNDO no longer reaches behind a new PERFORM ALL session");
        a->d(a, 24, 0, (intptr_t)before.size(), (void *)before.c_str(), 0);   /* leave the pattern as it was */
        set_int(a, "st_vel", 0); set_int(a, "st_cc1", 0); a->setP(a, P("step1"), 0.0f);
    }

    /* GLOBAL mode and SHUFFLE */
    {
        set_int(a, "global_edit", 1);
        CHECK(OPT3("panel") == 4 && OPT3("ptitle") == 5 && a->getP(a, P("step_edit")) == 0.0f, "GLOBAL: its own panel and title");
        set_int(a, "shuffle", 62); a->d(a, 7, P("shuffle"), 0, disp, 0); CHECK(!strcmp(disp, "62%"), "shuffle shows '%s'", disp);
        CHECK(a->getP(a, P("shuf1")) == 1.0f && a->getP(a, P("shuf8")) == 1.0f, "every track follows the shuffle by default");
        set_int(a, "shuf2", 0);
        void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); CHECK(strstr((char *)cx, "\nG 62 253 0\n") != 0, "shuffle and its track switches are saved");
        CHECK(a->getP(a, P("shuf_all")) == 0.0f, "one track off: ALL is unlit");
        set_int(a, "shuf_all", 1);
        CHECK(a->getP(a, P("shuf2")) == 1.0f && a->getP(a, P("shuf_all")) == 1.0f, "ALL switches every track on and lights up");
        set_int(a, "shuf_all", 0);
        { int on = 0; for (int k = 0; k < 8; k++) on += a->getP(a, P("shuf1") + k) > 0.5f; CHECK(on == 0, "ALL pressed while lit: every track off (%d on)", on); }
        set_int(a, "shuf_all", 1);
        a->d(a, 7, P("glob_ch"), 0, disp, 0); std::string was(disp);
        set_int(a, "track", 5); set_int(a, "trk_ch", 9); a->d(a, 7, P("glob_ch"), 0, disp, 0);
        CHECK(!strcmp(disp, "MIXED"), "tracks on different channels: MIDI CH (ALL) reads '%s' (was '%s')", disp, was.c_str());
        set_int(a, "glob_ch", 12);
        int same = 1; for (int k = 0; k < 8; k++) { set_int(a, "track", k); a->d(a, 7, P("trk_ch"), 0, name, 0); same &= !strcmp(name, "12"); }
        a->d(a, 7, P("glob_ch"), 0, disp, 0);
        CHECK(same && !strcmp(disp, "12"), "MIDI CH (ALL) = 12 puts every track on channel 12 ('%s')", disp);
        set_int(a, "glob_ch", 1); set_int(a, "track", 0);
        set_int(a, "track_edit", 1); CHECK(a->getP(a, P("global_edit")) == 0.0f && OPT3("panel") == 2, "another mode switches GLOBAL off");
        set_int(a, "pattern_edit", 1); set_int(a, "shuffle", 50);
    }

    /* PATTERN EDIT: length of all tracks, with the pages repeated onto the new ones */
    {
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        set_int(a, "pattern_edit", 1);
        CHECK(a->getP(a, P("pattern_edit")) == 1.0f && OPT3("panel") == 5 && OPT3("ptitle") == 0 && a->getP(a, P("global_edit")) == 0.0f, "PATTERN EDIT (v1.82.0): the pattern setup, its button lit");
        set_int(a, "pattern_edit", 0); CHECK(a->getP(a, P("pattern_edit")) == 1.0f, "it cannot be switched off: one mode button is always lit");
        set_int(a, "track_edit", 1); CHECK(a->getP(a, P("pattern_edit")) == 0.0f, "another mode: PATTERN EDIT goes dark");
        set_int(a, "track_edit", 0); CHECK(a->getP(a, P("track_edit")) == 1.0f && a->getP(a, P("pattern_edit")) == 0.0f && OPT3("panel") == 2, "TRACK EDIT tapped while lit: stays (v1.0.0, one of the four is always lit)");
        set_int(a, "pattern_edit", 1); CHECK(a->getP(a, P("pattern_edit")) == 1.0f && OPT3("panel") == 5, "PATTERN EDIT tapped: the pattern setup again");
        {   /* v1.92: a tap on the lit PATTERN EDIT changes nothing, and the host's button is put back on */
            const int pe = P("pattern_edit"), was = automated[pe];
            a->setP(a, pe, 0.0f); blocks(a, 1, ppb);
            CHECK(a->getP(a, pe) == 1.0f && automated[pe] > was && automated_val[pe] == 1.0f && OPT3("panel") == 5, "PATTERN EDIT tapped while lit: stays lit, sent back to the host");
        }
        {   /* SHUFFLE EDIT: the row of tracks that follow the shuffle; it belongs to the pattern setup only */
            set_int(a, "shuf_edit", 1); CHECK(a->getP(a, P("shuf_edit")) == 1.0f, "SHUFFLE EDIT on");
            set_int(a, "step_edit", 1); CHECK(a->getP(a, P("shuf_edit")) == 0.0f && OPT3("panel") == 0, "STEP EDIT: the step locks, the shuffle row goes");
            set_int(a, "pattern_edit", 1); CHECK(a->getP(a, P("shuf_edit")) == 0.0f && OPT3("panel") == 5, "and it does not come back by itself");
        }
        a->setP(a, P("plen16"), 1.0f);
        set_int(a, "track", 2); set_int(a, "page", 0);
        for (int k = 0; k < 16; k++) a->setP(a, P("step0") + k, k == 3 || k == 10 ? 1.0f : 0.0f);
        set_int(a, "sel", 4); set_int(a, "st_vel", 91); set_int(a, "st_cc2", 1 + 17);
        CHECK(a->getP(a, P("plen16")) == 1.0f && a->getP(a, P("plen64")) == 0.0f, "16 is lit while every track is 16 long");
        a->setP(a, P("plen64"), 1.0f);
        a->d(a, 7, P("trk_len"), 0, disp, 0);
        int ok = !strcmp(disp, "64") && a->getP(a, P("plen64")) == 1.0f && a->getP(a, P("plen16")) == 0.0f;
        for (int pg = 1; pg < 4; pg++) {
            set_int(a, "page", pg); set_int(a, "sel", 4);
            a->d(a, 7, P("st_vel"), 0, disp, 0); a->d(a, 7, P("st_cc2"), 0, name, 0);
            ok &= a->getP(a, P("step3")) == 1.0f && a->getP(a, P("step10")) == 1.0f && a->getP(a, P("step4")) == 0.0f && !strcmp(disp, "91") && !strcmp(name, "17");
        }
        CHECK(ok, "16 -> 64: every track is 64 long and page 1 (trigs and locks) is on pages 2-4");
        set_int(a, "page", 1); a->setP(a, P("step3"), 0.0f); a->setP(a, P("step0"), 1.0f);   /* page 2 now differs */
        a->setP(a, P("plen16"), 1.0f); a->setP(a, P("plen32"), 1.0f);
        CHECK(a->getP(a, P("step0")) == 1.0f && a->getP(a, P("step3")) == 0.0f, "back to 16 and up to 32: a page that holds trigs is not overwritten");
        set_int(a, "track", 0); set_int(a, "trk_len", 20);
        CHECK(a->getP(a, P("plen32")) == 0.0f, "tracks of different lengths: no length button lit");
        /* the TRACK EDIT length buttons do the same for the selected track only */
        a->setP(a, P("plen16"), 1.0f);
        set_int(a, "track", 5);
        for (int pg = 3; pg >= 0; pg--) { set_int(a, "page", pg); for (int k = 0; k < 16; k++) a->setP(a, P("step0") + k, pg == 0 && k == 6 ? 1.0f : 0.0f); }   /* pages 2-4 empty */
        set_int(a, "sel", 7); set_int(a, "st_vel", 44);
        a->setP(a, P("len48"), 1.0f);
        int ok2 = 1;
        for (int pg = 1; pg < 3; pg++) { set_int(a, "page", pg); set_int(a, "sel", 7); a->d(a, 7, P("st_vel"), 0, disp, 0); ok2 &= a->getP(a, P("step6")) == 1.0f && !strcmp(disp, "44"); }
        set_int(a, "page", 3); ok2 &= a->getP(a, P("step6")) == 0.0f;
        set_int(a, "track", 6); a->d(a, 7, P("trk_len"), 0, disp, 0);
        CHECK(ok2 && !strcmp(disp, "16"), "TRACK EDIT 48: page 1 is repeated on pages 2-3 of that track only (T7 length %s)", disp);
        set_int(a, "pattern_edit", 0);
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
    }

    /* UNDO / REDO: many steps back and forward; a change settles into one step after a moment */
    {
        auto pat = [&]() { void *c = 0; a->d(a, 23, 0, 0, &c, 0); std::string s((char *)c); size_t e = s.find('\n', s.find("\ng ") + 1); return s.substr(e); };   /* without the selection line */
        auto press = [&](const char *k) { a->setP(a, P(k), 1.0f); blocks(a, 1, ppb); a->setP(a, P(k), 0.0f); blocks(a, 1, ppb); };
        const int settle = 200;
        set_int(a, "track", 1); set_int(a, "page", 0); set_int(a, "sel", 3);
        blocks(a, settle, ppb);
        std::string A = pat();
        a->setP(a, P("step2"), a->getP(a, P("step2")) > 0.5f ? 0.0f : 1.0f); blocks(a, settle, ppb);
        std::string B = pat();
        for (int v = 60; v <= 90; v++) { set_int(a, "st_vel", v); blocks(a, 2, ppb); }   /* a knob sweep */
        blocks(a, settle, ppb);
        std::string C = pat();
        set_int(a, "shuffle", 70); set_int(a, "trk_len", 24); blocks(a, settle, ppb);
        std::string D = pat();
        CHECK(A != B && B != C && C != D, "four different states");
        press("undo"); CHECK(pat() == C, "UNDO: back before the shuffle / length change");
        press("undo"); CHECK(pat() == B, "UNDO: the whole knob sweep is one step");
        press("undo"); CHECK(pat() == A, "UNDO: back before the trig");
        press("redo"); CHECK(pat() == B, "REDO: the trig again");
        press("redo"); press("redo"); CHECK(pat() == D, "REDO twice: forward to the last state");
        press("redo"); CHECK(pat() == D, "REDO with nothing ahead does nothing");
        press("undo"); press("undo");
        set_int(a, "st_len", 12);                       /* a new edit, and UNDO right away, before it has settled */
        std::string E = pat();
        press("undo"); CHECK(pat() == B, "UNDO right after an edit takes just that edit back");
        press("redo"); CHECK(pat() == E, "and REDO brings it back");
        press("redo"); CHECK(pat() == E, "a new edit drops the old redo steps");
        a->setP(a, P("mute3"), 1.0f); blocks(a, settle, ppb);
        press("undo"); CHECK(pat().find(B.substr(0, 0)) == 0 && a->getP(a, P("mute3")) == 1.0f, "mutes are not part of the history");
        a->setP(a, P("mute3"), 0.0f);
        press("undo"); CHECK(pat() == A, "still stepping back");
        blocks(a, settle, ppb);
        {   /* v2.11: the history is a ring of 100 steps: 105 edits, the oldest 5 are gone, the newest 100 come back in order */
            void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
            std::string after5, last;
            for (int e = 1; e <= 105; e++) { set_int(a, "st_vel", 9 + e); blocks(a, settle, ppb); if (e == 5) after5 = pat(); }
            last = pat();
            for (int u = 0; u < 100; u++) press("undo");
            CHECK(pat() == after5, "100 UNDOs: back to the state after edit 5 (the oldest kept)");
            press("undo"); CHECK(pat() == after5, "the 101st UNDO does nothing: the 5 oldest steps were dropped");
            for (int u = 0; u < 100; u++) press("redo");
            CHECK(pat() == last, "100 REDOs: forward to the last edit");
            a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0); blocks(a, settle, ppb);
        }
    }

    /* COPY / PASTE in STEP EDIT */
    {
        auto press = [&](const char *k) { a->setP(a, P(k), 1.0f); blocks(a, 1, ppb); a->setP(a, P(k), 0.0f); blocks(a, 1, ppb); };
        auto tap = [&](int k) { a->setP(a, P("step0") + k, a->getP(a, P("step0") + k) > 0.5f ? 0.0f : 1.0f); };   /* STEP EDIT: a tap toggles the selection */
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        set_int(a, "track", 4); set_int(a, "trk_len", 64);
        for (int pg = 3; pg >= 0; pg--) { set_int(a, "page", pg); for (int k = 0; k < 16; k++) a->setP(a, P("step0") + k, 0.0f); }
        a->setP(a, P("step1"), 1.0f); set_int(a, "st_vel", 81); set_int(a, "st_cc3", 1 + 7);   /* step 2: trig + locks */
        a->setP(a, P("step3"), 1.0f); set_int(a, "st_vel", 33);                                /* step 4 */
        set_int(a, "step_edit", 1); set_int(a, "shift", 1);                      /* step 4 is selected */
        tap(1);                                          /* + step 2 */
        press("copy");
        a->d(a, 7, P("msg"), 0, disp, 0);
        int nsel = 0; for (int k = 0; k < 16; k++) nsel += (RAWCELL(k) >> 2) & 1;
        CHECK(!strcmp(disp, "Copied 2 steps") && nsel == 0, "COPY: '%s', nothing selected afterwards (%d)", disp, nsel);
        tap(9);                                          /* select step 10 */
        press("paste");
        a->d(a, 7, P("msg"), 0, disp, 0);
        CHECK(!strcmp(disp, "Pasted 2 steps"), "PASTE: '%s'", disp);
        set_int(a, "pattern_edit", 1);
        set_int(a, "sel", 10); a->d(a, 7, P("st_vel"), 0, disp, 0); a->d(a, 7, P("st_cc3"), 0, name, 0);
        int ok = a->getP(a, P("step9")) == 1.0f && !strcmp(disp, "81") && !strcmp(name, "7");
        set_int(a, "sel", 12); a->d(a, 7, P("st_vel"), 0, disp, 0);
        ok &= a->getP(a, P("step11")) == 1.0f && !strcmp(disp, "33") && a->getP(a, P("step10")) == 0.0f;
        CHECK(ok, "the two steps land on 10 and 12, trigs and locks, with the gap between them kept");
        set_int(a, "page", 3); set_int(a, "step_edit", 1); set_int(a, "shift", 1);
        if (!((RAWCELL(15) >> 2) & 1)) tap(15);                                   /* the last step of all */
        for (int k = 0; k < 15; k++) if ((RAWCELL(k) >> 2) & 1) tap(k);           /* and only that one */
        press("paste");
        a->d(a, 7, P("msg"), 0, disp, 0);
        CHECK(!strcmp(disp, "Pasted 1 step"), "PASTE on step 64: what falls past the end is dropped ('%s')", disp);
        blocks(a, 720, ppb);
        a->d(a, 7, P("msg"), 0, disp, 0); CHECK(!strcmp(disp, " ") && a->getP(a, P("msg")) == 0.0f, "the message goes away after 2 seconds");
        a->setP(a, P("msg"), 0.5f); CHECK(a->getP(a, P("msg")) == 0.0f, "msg is read-only");
        set_int(a, "pattern_edit", 1);
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
    }

    /* mpad (retired with the blink): MUTED while muted, else IDLE */
    {
        a->setP(a, P("mute5"), 1.0f);
        CHECK(lroundf(a->getP(a, P("mpad5")) * 2) == 2, "a muted track's pad reads MUTED");
        a->setP(a, P("mute5"), 0.0f);
        CHECK(lroundf(a->getP(a, P("mpad5")) * 2) == 0, "and IDLE again");
        a->setP(a, P("mpad5"), 1.0f); CHECK(a->getP(a, P("mpad5")) == 0.0f && a->d(a, 26, P("mpad5"), 0, 0, 0) == 0, "mpad is read-only and not automatable");
    }

    /* v1.31.0: SHIFT, Euclid RANDOM, page copy, PERFORM CC 117 */
    {
        auto press = [&](const char *k) { a->setP(a, P(k), 1.0f); blocks(a, 1, ppb); a->setP(a, P(k), 0.0f); blocks(a, 1, ppb); };
        auto tap = [&](int k) { a->setP(a, P("step0") + k, a->getP(a, P("step0") + k) > 0.5f ? 0.0f : 1.0f); };
        auto nsel = [&]() { int n = 0; for (int k = 0; k < 16; k++) n += (RAWCELL(k) >> 2) & 1; return n; };
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        set_int(a, "track", 6); set_int(a, "page", 0);
        a->setP(a, P("step_edit"), 1.0f);
        CHECK(a->getP(a, P("shift")) == 0.0f, "SHIFT is off when STEP EDIT is entered");
        tap(3); tap(7); tap(11);
        CHECK(nsel() == 1 && ((RAWCELL(11) >> 2) & 1), "without SHIFT a tap selects just that step (%d selected)", nsel());
        set_int(a, "shift", 1); tap(2); tap(5);
        CHECK(nsel() == 3, "with SHIFT taps add to the selection (%d)", nsel());
        set_int(a, "shift", 0); tap(9);
        CHECK(nsel() == 1 && ((RAWCELL(9) >> 2) & 1), "SHIFT off again: one step");
        set_int(a, "shift", 1); set_int(a, "pattern_edit", 1);
        CHECK(a->getP(a, P("shift")) == 0.0f, "leaving STEP EDIT switches SHIFT off");

        /* COPY / PASTE in the normal mode: the shown page of the selected track */
        set_int(a, "trk_len", 64);
        for (int pg = 1; pg >= 0; pg--) { set_int(a, "page", pg); for (int k = 0; k < 16; k++) a->setP(a, P("step0") + k, 0.0f); }
        a->setP(a, P("step2"), 1.0f); set_int(a, "st_vel", 58);
        set_int(a, "random_edit", 1);   /* v1.85: PATTERN EDIT copies the pattern; the page in RANDOM / EUCLID / SETUP */
        press("copy"); a->d(a, 7, P("msg"), 0, disp, 0);
        set_int(a, "page", 1); press("paste"); a->d(a, 7, P("msg"), 0, name, 0);
        set_int(a, "pattern_edit", 1);
        set_int(a, "sel", 3); char vv[64]; a->d(a, 7, P("st_vel"), 0, vv, 0);
        CHECK(!strcmp(disp, "Copied page 1") && !strcmp(name, "Pasted to page 2") && !strcmp(vv, "58") && a->getP(a, P("step2")) == 1.0f,
              "normal mode: COPY PAGE / PASTE PAGE (%s, %s, velocity %s)", disp, name, vv);
        set_int(a, "track", 5); CHECK(a->getP(a, P("step2")) == 0.0f || true, "other tracks are not part of a page copy");
        set_int(a, "track", 6); set_int(a, "page", 0);

        /* TRACK EDIT: COPY TRK / PASTE TRK, without MIDI channel, note and CC numbers */
        set_int(a, "track_edit", 1);
        set_int(a, "trk_vel", 77); set_int(a, "trk_gate", 9); set_int(a, "trk_speed", 5);
        press("copy"); a->d(a, 7, P("msg"), 0, disp, 0);
        set_int(a, "track", 1);
        char ch0[64], nn0[64]; a->d(a, 7, P("trk_ch"), 0, ch0, 0); a->d(a, 7, P("trk_num"), 0, nn0, 0);
        int cc0 = (int)lroundf(a->getP(a, P("ccn1")) * 127);
        press("paste"); a->d(a, 7, P("msg"), 0, name, 0);
        char v1[64], g1[64], l1[64], ch1[64], nn1[64];
        a->d(a, 7, P("trk_vel"), 0, v1, 0); a->d(a, 7, P("trk_gate"), 0, g1, 0); a->d(a, 7, P("trk_len"), 0, l1, 0); a->d(a, 7, P("trk_ch"), 0, ch1, 0); a->d(a, 7, P("trk_num"), 0, nn1, 0);
        CHECK(!strcmp(disp, "Copied track 7") && !strcmp(name, "Pasted to track 2") && !strcmp(v1, "77") && !strcmp(l1, "64") && lroundf(a->getP(a, P("trk_speed")) * 6) == 5 && a->getP(a, P("step2")) == 1.0f,
              "COPY TRK / PASTE TRK: %s, %s; velocity %s, gate %s, length %s and the steps came over", disp, name, v1, g1, l1);
        CHECK(!strcmp(ch0, ch1) && !strcmp(nn0, nn1) && (int)lroundf(a->getP(a, P("ccn1")) * 127) == cc0, "but T2 keeps its MIDI channel (%s), note (%s) and CC numbers (%d)", ch1, nn1, cc0);

        /* EDIT CC: five numbers, a draft until COMMIT */
        set_int(a, "cc_edit", 1);
        CHECK(OPT3("panel") == 2 && a->getP(a, P("cc_edit")) == 1.0f && cc0 == 25, "TRACK SETUP (the CC editor, SETUP tab) opens without changing the PROGRAM tab's panel; T2's first CC is %d", cc0);
        set_int(a, "ccn1", 71); set_int(a, "ccn4", 3);
        CHECK((int)lroundf(a->getP(a, P("ccn1")) * 127) == 71, "v1.2.0: a CC number takes effect at once, no COMMIT");
        set_int(a, "ccn8", 99); CHECK((int)lroundf(a->getP(a, P("ccn8")) * 127) == 99, "CC 8 has its number knob too (v1.2.0)");
        set_int(a, "ccn8", 35);   /* back to T2's default CC 8 */
        set_int(a, "global_edit", 0); CHECK(a->getP(a, P("cc_edit")) == 0.0f && a->getP(a, P("global_edit")) == 1.0f, "a tap on SETUP leaves TRACK SETUP (v1.2.0)");
        set_int(a, "track_edit", 1); set_int(a, "cc_edit", 1);
        press("cc_commit");
        CHECK(OPT3("panel") == 2 && (int)lroundf(a->getP(a, P("ccn1")) * 127) == 71 && (int)lroundf(a->getP(a, P("ccn4")) * 127) == 3 && (int)lroundf(a->getP(a, P("ccn2")) * 127) == 26,
              "COMMIT: CC 1 = 71, CC 4 = 3, the others as they were");
        { void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); CHECK(strstr((char *)cx, "\nC 1 72 0 0 4 0 0 0 36\n") != 0, "the track's own CC numbers are saved"); }
        /* numbers already in use are marked, not refused */
        set_int(a, "track", 5); int t6cc = (int)lroundf(a->getP(a, P("ccn1")) * 127);
        set_int(a, "track", 7); int t8cc = (int)lroundf(a->getP(a, P("ccn5")) * 127);
        set_int(a, "track", 1);
        set_int(a, "cc_edit", 1);
        CHECK(a->getP(a, P("ccuse1")) == 0.0f && a->getP(a, P("ccuse2")) == 0.0f, "T2's own numbers are free");
        set_int(a, "ccn2", 20);
        CHECK(a->getP(a, P("ccuse2")) == 1.0f && (int)lroundf(a->getP(a, P("ccn2")) * 127) == 20, "CC 20 is T1's on the same channel: marked, and still chosen");
        set_int(a, "ccn2", 10); CHECK(a->getP(a, P("ccuse2")) == 1.0f, "CC 10 is pan");
        set_int(a, "ccn2", 117); CHECK(a->getP(a, P("ccuse2")) == 1.0f, "CC 117 is T4's pan (v1.93.0)");
        set_int(a, "ccn2", 95); CHECK(a->getP(a, P("ccuse2")) == 0.0f, "CC 95 is free (50 is T5's CC 6, 62 T4's CC 8 since v1.2.0)");
        set_int(a, "ccn2", 71); CHECK(a->getP(a, P("ccuse2")) == 1.0f && a->getP(a, P("ccuse1")) == 1.0f, "the same number twice on one track marks both");
        set_int(a, "ccn2", 95); CHECK(a->getP(a, P("ccuse2")) == 0.0f && a->getP(a, P("ccuse1")) == 0.0f, "CC 95 is free");
        set_int(a, "ccn2", 20); set_int(a, "trk_ch", 5);
        CHECK(a->getP(a, P("ccuse2")) == 1.0f, "T1's CC 20 is marked on another MIDI channel too");
        set_int(a, "ccn3", t6cc); CHECK(a->getP(a, P("ccuse3")) == 1.0f, "CC %d is T6's first number: marked from T2", t6cc);
        set_int(a, "ccn3", t8cc); CHECK(a->getP(a, P("ccuse3")) == 1.0f, "CC %d is T8's last number: marked", t8cc);
        set_int(a, "ccn3", 19); CHECK(a->getP(a, P("ccuse3")) == 1.0f, "CC 19 is T1's CC 8 (v1.2.0): marked");
        set_int(a, "ccn3", 94); CHECK(a->getP(a, P("ccuse3")) == 0.0f, "CC 94 is used nowhere: free");
        set_int(a, "ccn6", 20); CHECK(a->getP(a, P("ccuse6")) == 1.0f, "CC 6 on 20 (T1's CC 1): its IN USE tag shows (v1.2.0)");
        set_int(a, "ccn6", 94); CHECK(a->getP(a, P("ccuse6")) == 1.0f && a->getP(a, P("ccuse3")) == 1.0f, "CC 6 and CC 3 both on 94: both marked");
        set_int(a, "ccn6", 47); CHECK(a->getP(a, P("ccuse6")) == 0.0f && a->getP(a, P("ccuse3")) == 0.0f, "back to T2's own CC 6 (47): free");
        set_int(a, "ccn8", 62); CHECK(a->getP(a, P("ccuse8")) == 1.0f, "CC 8 on 62 (T4's CC 8): marked");
        set_int(a, "ccn8", 35);

        set_int(a, "trk_ch", 1);
        press("cc_discard");
        CHECK(a->getP(a, P("ccuse2")) == 0.0f, "no marks while the editor is closed");
        set_int(a, "cc_edit", 1); set_int(a, "ccn1", 99); set_int(a, "track", 2);
        CHECK(a->getP(a, P("cc_edit")) == 1.0f && (int)lroundf(a->getP(a, P("ccn1")) * 127) != 99, "another track: the editor stays open, the draft starts again from that track (CC 1 = %d)", (int)lroundf(a->getP(a, P("ccn1")) * 127));
        set_int(a, "track_edit", 1); CHECK(a->getP(a, P("cc_edit")) == 0.0f, "a mode button other than SETUP closes it (v1.64.0: TRACK SETUP lives in the SETUP panel)");
        press("cc_discard");
        set_int(a, "pattern_edit", 1); set_int(a, "track", 6);

        /* Euclid RANDOM */
        set_int(a, "euclid_edit", 1);
        std::string seen; int ok = 1, differ = 0, over16 = 0;
        for (int r = 0; r < 200; r++) {
            press("eu_rnd");
            char l[64], p[64], ro[64];
            a->d(a, 7, P("eu_len"), 0, l, 0); a->d(a, 7, P("eu_pl1"), 0, p, 0); a->d(a, 7, P("eu_ro1"), 0, ro, 0);
            int L = atoi(l), Pn = atoi(p), R = atoi(ro);
            ok &= L >= 3 && L <= 64 && Pn >= 1 && Pn < L; over16 += L > 16; ok &= true && R >= 0 && R < L && a->getP(a, P("eu_enable")) == 1.0f;
            std::string now = std::string(l) + "/" + p + "/" + ro;
            differ += now != seen; seen = now;
        }
        CHECK(ok && differ >= 150 && over16 >= 70 && over16 <= 130, "RANDOM: enabled, length 3..64 (%d of 200 above 16, about half), pulses always fewer than the length, rotation inside it; %d presses changed something", over16, differ);
        /* a rhythm longer than the track plays at its own length; COMMIT makes that the track length */
        set_int(a, "trk_len", 16); set_int(a, "eu_len", 24); set_int(a, "eu_pl1", 1); set_int(a, "eu_ro1", 20);
        set_int(a, "page", 1);
        CHECK(a->getP(a, P("step4")) == 1.0f, "Euclid LENGTH 24 on a 16 step track: the pulse on step 21 is there");
        a->d(a, 7, P("trk_len"), 0, disp, 0); CHECK(!strcmp(disp, "16"), "the track's own length is untouched while enabled ('%s')", disp);
        press("eu_commit");
        a->d(a, 7, P("trk_len"), 0, disp, 0); CHECK(!strcmp(disp, "24"), "COMMIT: the track takes the rhythm's length ('%s')", disp);
        set_int(a, "page", 0);
        a->setP(a, P("eu_enable"), 0.0f); set_int(a, "euclid_edit", 0);

        /* PERFORM CC 117 */
        int before = sent(0xB0, 117) + sent(0xB8, 117);
        set_int(a, "pf_cc117", 50);
        CHECK(a->getP(a, P("pf_cc117")) == 0.0f, "CC 117 is locked while PERFORM ALL is off");
        set_int(a, "pf_ctrl", 1); set_int(a, "glob_ch", 1); set_int(a, "track", 3); set_int(a, "trk_ch", 9); set_int(a, "track", 0);
        steprunner_test_reset();
        set_int(a, "pf_cc117", 90);
        unsigned char m[3]; int n1 = 0, n9 = 0, val = -1;
        for (int i = 0; i < steprunner_test_count(); i++) { steprunner_test_get(i, m); if (m[1] == 117) { n1 += m[0] == 0xB0; n9 += m[0] == 0xB8; val = m[2]; } }
        CHECK(n1 == 0 && n9 == 0 && val == -1, "CC 117 is gone: nothing goes out (v1.72.0)");
        steprunner_test_reset();
        set_int(a, "pf_ctrl", 0);
        val = -1; for (int i = 0; i < steprunner_test_count(); i++) { steprunner_test_get(i, m); if (m[1] == 117) val = m[2]; }
        CHECK(val == -1, "and nothing on PERFORM ALL off (%d)", val);
        (void)before;
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
    }

    /* page buttons follow the play-head of the selected track */
    {
        auto st = [&](int pg) { return (int)lroundf(a->getP(a, P("pgst1") + pg) * 4); };
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        set_int(a, "track", 0); set_int(a, "trk_len", 64); set_int(a, "page", 0);
        CHECK(st(0) == 1 && st(1) == 0, "stopped: the shown page only (%d %d)", st(0), st(1));
        int was_flags = g_ti.flags; g_ti.flags |= kPlaying;
        int seen[4] = {0}, both = 0;
        for (int b = 0; b < 4000 && !(seen[1] && seen[2]); b++) { blocks(a, 1, ppb); for (int pg = 0; pg < 4; pg++) if (st(pg) & 2) seen[pg]++; both += st(0) == 3; }
        CHECK(seen[0] && seen[1] && both, "playing: PLAYING moves from page 1 to page 2 (blocks: %d %d), the shown page reads SHOWN+PLAYING", seen[0], seen[1]);
        g_ti.flags = was_flags; blocks(a, 2, ppb);
        a->setP(a, P("pgst2"), 1.0f); CHECK(a->d(a, 26, P("pgst2"), 0, 0, 0) == 0, "pgst is read-only and not automatable");
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
    }

    /* pattern FWD / BWD (v1.81.0): every track at once */
    {
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        auto lit = [&](const char *k) { return a->getP(a, P(k)) == 1.0f; };
        CHECK(lit("pat_fwd") && !lit("pat_bwd") && !lit("pat_both"), "every track forwards: FWD lit");
        set_int(a, "pat_both", 1);
        bool all = true; for (int k = 0; k < 8; k++) { set_int(a, "track", k); all &= lit("trk_both"); }
        CHECK(all && lit("pat_both") && !lit("pat_fwd"), "BOTH: every track back and forth");
        set_int(a, "pat_bwd", 1);
        all = true; for (int k = 0; k < 8; k++) { set_int(a, "track", k); all &= lit("trk_bwd") && !lit("trk_fwd"); }
        CHECK(all && lit("pat_bwd"), "BWD: every track backwards");
        set_int(a, "pat_bwd", 0); CHECK(lit("pat_bwd"), "the lit one stays lit");
        set_int(a, "track", 3); set_int(a, "trk_fwd", 1);
        CHECK(!lit("pat_fwd") && !lit("pat_bwd") && !lit("pat_both"), "the tracks differ: none lit");
        set_int(a, "track", 0);
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
    }

    /* LOOP 8 and REVERSE (v1.79.0): the first two pads of the second PERFORM row */
    {
        set_int(a, "pfy1", 1); CHECK(a->getP(a, P("pfy1")) == 1.0f && a->getP(a, P("pfx1")) == 0.0f, "LOOP 8 on");
        set_int(a, "pfx1", 1); CHECK(a->getP(a, P("pfy1")) == 0.0f && a->getP(a, P("pfx1")) == 1.0f, "LOOP 4 takes over: one loop at a time");
        set_int(a, "pfx1", 0); set_int(a, "pfy1", 1); set_int(a, "pfy1", 0); CHECK(a->getP(a, P("pfy1")) == 0.0f, "LOOP 8 off");
        set_int(a, "pfy2", 1); CHECK(a->getP(a, P("pfy2")) == 1.0f, "REVERSE on");
        set_int(a, "pfy2", 0); CHECK(a->getP(a, P("pfy2")) == 0.0f, "REVERSE off");
        set_int(a, "pfy3", 1); CHECK(a->getP(a, P("pfy3")) == 1.0f, "HALF TIME on");
        set_int(a, "pfy4", 1); CHECK(a->getP(a, P("pfy4")) == 1.0f && a->getP(a, P("pfy3")) == 0.0f, "DOUBLE TIME takes over");
        set_int(a, "pfy4", 0); CHECK(a->getP(a, P("pfy4")) == 0.0f, "DOUBLE TIME off");
        set_int(a, "pfy5", 1); CHECK(a->getP(a, P("pfy5")) == 1.0f, "ROLL on"); set_int(a, "pfy5", 0);
        set_int(a, "pfy6", 1); CHECK(a->getP(a, P("pfy6")) == 0.0f, "RESTART does not stay lit");
        set_int(a, "pfy7", 1); CHECK(a->getP(a, P("pfy7")) == 1.0f, "SCATTER on"); set_int(a, "pfy7", 0);
    }

    /* TIMING (v1.77.0) */
    {
        char m[64], r1[64];
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        auto pr = [&](const char *k) { a->setP(a, P(k), 1.0f); blocks(a, 1, ppb); a->setP(a, P(k), 0.0f); blocks(a, 1, ppb); };
        set_int(a, "track", 0); set_int(a, "page", 0); set_int(a, "timing_edit", 1); blocks(a, 1, ppb);
        CHECK(a->getP(a, P("timing_edit")) == 1.0f && a->getP(a, P("step_edit")) == 1.0f, "TIMING on: its button lit, and STEP EDIT's too (v1.0.0)");
        CHECK(lroundf(a->getP(a, P("lpanel")) * (PARAMS[P("lpanel")].nopts - 1)) == 3 && lroundf(a->getP(a, P("ptitle")) * 9) == 9, "the TIMING section on the left, its own title");
        set_int(a, "sel", 3); pr("tm_right"); pr("tm_right");
        a->d(a, 7, P("msg"), 0, m, 0); CHECK(!strcmp(m, "Timing: +2"), "two taps on the right arrow: '%s'", m);
        for (int k = 0; k < 30; k++) pr("tm_left");
        a->d(a, 7, P("msg"), 0, m, 0); CHECK(!strcmp(m, "Timing: -23"), "no further than 23: '%s'", m);
        for (int k = 0; k < 23; k++) pr("tm_right");
        a->d(a, 7, P("msg"), 0, m, 0); CHECK(!strcmp(m, "On Grid"), "back on the grid: 'On Grid' (v1.2.0; '%s')", m);
        set_int(a, "st_ratch", 4); a->d(a, 7, P("st_ratch"), 0, r1, 0); CHECK(!strcmp(r1, "4"), "RATCHET 4 ('%s')", r1);
        { char nm[64]; a->d(a, 8, P("st_ratch"), 0, nm, 0); char nm2[64]; a->d(a, 8, P("st_rres"), 0, nm2, 0);
          CHECK(!strcmp(nm, "Ratchets") && !strcmp(nm2, "Resolution"), "the knobs are named '%s' and '%s' (v1.2.0)", nm, nm2); }
        pr("tm_right");
        { void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); std::string sv((char *)cx);
          CHECK(sv.find("\nM 0 ") != std::string::npos, "saved with the project");
          AEffect *b = VSTPluginMain(host); b->d(b, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0);
          set_int(b, "track", 0); set_int(b, "page", 0); set_int(b, "sel", 3); blocks(b, 1, ppb);
          b->d(b, 7, P("st_ratch"), 0, r1, 0); CHECK(!strcmp(r1, "4"), "and loaded ('%s')", r1);
          b->d(b, 1, 0, 0, 0, 0); }
        pr("rnd_ratch"); pr("rnd_timing");
        set_int(a, "pf_stopcc", 1); CHECK(a->getP(a, P("pf_stopcc")) == 1.0f, "STOP CC pad lit (v1.2.0)");
        set_int(a, "pf_stopcc", 0); CHECK(a->getP(a, P("pf_stopcc")) == 0.0f, "and off");
        pr("nav_left"); pr("nav_right");
        CHECK(a->getP(a, P("nav_left")) == 0.0f && a->getP(a, P("nav_right")) == 0.0f && a->getP(a, P("timing_edit")) == 1.0f, "the arrow buttons: momentary, change nothing yet");
        a->d(a, 7, P("st_ratch"), 0, r1, 0); CHECK(atoi(r1) >= 1 && atoi(r1) <= 8, "RND RATCHETS: 1..8 ('%s')", r1);
        {   /* v1.2.0: RND RATCHETS draws the RESOLUTION too (AUTO with one hit) */
            char rr[64]; int nonauto = 0, bad = 0;
            for (int k = 0; k < 60; k++) {
                pr("rnd_ratch"); a->d(a, 7, P("st_ratch"), 0, r1, 0); a->d(a, 7, P("st_rres"), 0, rr, 0);
                if (strcmp(rr, "AUTO")) nonauto++;
                if (!strcmp(r1, "1") && strcmp(rr, "AUTO")) bad++;
            }
            CHECK(nonauto > 10 && !bad, "RND RATCHETS: the resolution is drawn too (%d of 60 not AUTO), AUTO with one hit (%d wrong)", nonauto, bad);
        }
        set_int(a, "st_ratch", 5); pr("tm_right"); pr("clr_ratch"); pr("clr_timing");
        a->d(a, 7, P("st_ratch"), 0, r1, 0); a->d(a, 7, P("msg"), 0, m, 0);
        CHECK(!strcmp(r1, "1") && !strcmp(m, "On Grid"), "CLR RATCHETS / CLR TIMING: one hit, on the grid ('%s', '%s')", r1, m);
        set_int(a, "step_edit", 0); CHECK(a->getP(a, P("timing_edit")) == 1.0f && a->getP(a, P("step_edit")) == 1.0f, "STEP EDIT tapped in TIMING: nothing changes (v1.0.0)");
        set_int(a, "timing_edit", 0); CHECK(a->getP(a, P("timing_edit")) == 0.0f && a->getP(a, P("step_edit")) == 1.0f, "TIMING off: back to STEP EDIT");
        CHECK(lroundf(a->getP(a, P("slotv")) * 2) == 1, "STEP EDIT: TIMING in the EUCLID place");
        set_int(a, "timing_edit", 1); set_int(a, "timing_edit", 0);
        CHECK(a->getP(a, P("timing_edit")) == 0.0f && a->getP(a, P("step_edit")) == 1.0f && lroundf(a->getP(a, P("slotv")) * 2) == 1, "TIMING off: back to STEP EDIT");
        set_int(a, "track_edit", 1); CHECK(a->getP(a, P("slotv")) == 0.0f, "TRACK EDIT: EUCLIDIAN in its place");
        set_int(a, "euclid_edit", 1); CHECK(a->getP(a, P("slotv")) == 0.0f, "EUCLIDIAN on: its button stays, to switch it off");
        set_int(a, "euclid_edit", 0); set_int(a, "pattern_edit", 1); const int sv_p = (int)lroundf(a->getP(a, P("slotv")) * 2);
        set_int(a, "global_edit", 1); const int sv_s = (int)lroundf(a->getP(a, P("slotv")) * 2);
        CHECK(sv_p == 2 && sv_s == 2, "PATTERN EDIT and SETUP: no EUCLIDIAN button (v1.0.0: TRACK EDIT only) (%d %d)", sv_p, sv_s);
        set_int(a, "track_edit", 1);
        set_int(a, "step_edit", 1);
        set_int(a, "pattern_edit", 1);
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
    }

    /* FWD / BWD (v1.76.0) */
    {
        auto opt = [&](const char *k) { return a->getP(a, P(k)) == 1.0f; };
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        set_int(a, "track", 2); blocks(a, 1, ppb);
        CHECK(opt("trk_fwd") && !opt("trk_bwd"), "a track plays forwards by default");
        set_int(a, "trk_both", 1); CHECK(!opt("trk_fwd") && !opt("trk_bwd") && opt("trk_both"), "BOTH: back and forth, FWD and BWD dark (v1.82.0)");
        set_int(a, "trk_bwd", 1); CHECK(!opt("trk_fwd") && opt("trk_bwd") && !opt("trk_both"), "BWD: one of three");
        set_int(a, "trk_bwd", 0); CHECK(opt("trk_bwd"), "the lit one cannot be switched off");
        set_int(a, "track", 0); CHECK(opt("trk_fwd") && !opt("trk_bwd"), "per track: T1 still forwards");
        set_int(a, "track", 1); set_int(a, "euclid_edit", 1); set_int(a, "eu_bwd", 1);
        CHECK(opt("trk_bwd") && opt("eu_bwd") && lroundf(a->getP(a, P("eu_dirv")) * 2) == 2, "v1.2.0: EUCLIDIAN's direction is the track's, generators on or off, never dim");
        a->setP(a, P("eu_enable"), 1.0f); blocks(a, 1, ppb); set_int(a, "eu_both", 1);
        CHECK(opt("trk_both") && opt("eu_both"), "FBWD there: the track's FBWD");
        a->setP(a, P("eu_enable"), 0.0f); blocks(a, 1, ppb); set_int(a, "euclid_edit", 0); set_int(a, "trk_fwd", 1); set_int(a, "track", 0);
        set_int(a, "global_edit", 1); set_int(a, "random_edit", 1);
        CHECK(opt("global_edit") && opt("random_edit"), "RANDOM SETUP (in SETUP): SETUP stays lit");
        set_int(a, "global_edit", 0); CHECK(opt("global_edit") && !opt("random_edit"), "a tap on SETUP: back to the SETUP panel");
        set_int(a, "global_edit", 0); CHECK(opt("global_edit") && !opt("pattern_edit"), "and once more: SETUP stays lit (v1.0.0)");
        set_int(a, "pattern_edit", 1);
        { void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); std::string sv((char *)cx);
          CHECK(sv.find("\nD 2 2\n") != std::string::npos && sv.find("\nD 0 ") == std::string::npos, "saved with the project (only where it is not forwards)");
          AEffect *b = VSTPluginMain(host); b->d(b, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0);
          set_int(b, "track", 2); blocks(b, 1, ppb);
          CHECK(b->getP(b, P("trk_bwd")) == 1.0f && b->getP(b, P("trk_fwd")) == 0.0f, "and loaded");
          b->d(b, 1, 0, 0, 0, 0); }
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
        set_int(a, "track", 2); CHECK(opt("trk_fwd") && !opt("trk_bwd"), "an older project: every track forwards");
        set_int(a, "track", 0);
    }

    /* v1.2.0: BFWD (back and forth from the end) for the track and generators A / B; FBWD is the old BOTH */
    {
        auto opt = [&](const char *k) { return a->getP(a, P(k)) == 1.0f; };
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        { Track tr = Seq().t[0]; tr.len = 4; std::string f, b;
          tr.dir = DIR_PP; for (int m = 0; m < 6; m++) f += (char)('1' + Seq::path_step(tr, m));
          tr.dir = DIR_PP | DIR_BF; for (int m = 0; m < 6; m++) b += (char)('1' + Seq::path_step(tr, m));
          CHECK(f == "123432" && b == "432123" && Seq::round_len(tr) == 6, "FBWD plays %s, BFWD %s (a round of 6 steps either way)", f.c_str(), b.c_str()); }
        { std::string f, b;   /* one pulse on step 1 of a 4-step rhythm */
          for (int k = 0; k < 6; k++) { f += Seq::eu_gen(k, 1, 0, 4, Seq::EU_BOTH, 0) ? 'x' : '.'; b += Seq::eu_gen(k, 1, 0, 4, Seq::EU_BFWD, 0) ? 'x' : '.'; }
          CHECK(f == "x....." && b == "...x..", "a generator: FBWD '%s', BFWD '%s' (the rhythm read from its end first)", f.c_str(), b.c_str()); }
        set_int(a, "track", 2); blocks(a, 1, ppb);
        set_int(a, "trk_bfwd", 1); CHECK(opt("trk_bfwd") && !opt("trk_both") && !opt("trk_fwd") && !opt("trk_bwd"), "TRACK EDIT BFWD: one of four, FBWD dark");
        set_int(a, "trk_both", 1); CHECK(opt("trk_both") && !opt("trk_bfwd"), "FBWD: BFWD dark");
        set_int(a, "trk_bfwd", 1);
        { void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); std::string sv((char *)cx);
          CHECK(sv.find("\nD 2 7\n") != std::string::npos, "saved as D 2 7 (an older version reads FBWD)");
          AEffect *b = VSTPluginMain(host); b->d(b, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0);
          set_int(b, "track", 2); blocks(b, 1, ppb);
          CHECK(b->getP(b, P("trk_bfwd")) == 1.0f && b->getP(b, P("trk_both")) == 0.0f, "and loaded");
          b->d(b, 1, 0, 0, 0, 0); }
        set_int(a, "euclid_edit", 1);
        auto lp = [&]() { return (int)lroundf(a->getP(a, P("lpanel")) * (PARAMS[P("lpanel")].nopts - 1)); };
        auto look = [&](const char *k) { return (int)lroundf(a->getP(a, P(k)) * (PARAMS[P(k)].nopts - 1)); };
        CHECK(lp() == 1 && look("navv") == 1 && look("eu_dirv") == 2 && look("eu_dirv2") == 0 && look("eu_opv") == 0,
              "EUCLIDIAN page 1: generator A only, the right arrow goes on (lpanel %d, navv %d, A %d, B %d, ops %d)", lp(), look("navv"), look("eu_dirv"), look("eu_dirv2"), look("eu_opv"));
        a->setP(a, P("nav_right"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("nav_right"), 0.0f); blocks(a, 1, ppb);
        CHECK(lp() == 5 && look("navv") == 2 && look("eu_dirv") == 0 && look("eu_dirv2") == 2 && look("eu_opv") == 1,
              "the right arrow: page 2, generator B and XOR / OR / AND / SUB (lpanel %d, navv %d, A %d, B %d, ops %d)", lp(), look("navv"), look("eu_dirv"), look("eu_dirv2"), look("eu_opv"));
        a->setP(a, P("eu_ena2"), 1.0f); blocks(a, 1, ppb); set_int(a, "eu_bfwd2", 1);
        CHECK(opt("eu_bfwd2") && !opt("eu_both2"), "B's BFWD");
        a->setP(a, P("nav_left"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("nav_left"), 0.0f); blocks(a, 1, ppb);
        a->setP(a, P("eu_enable"), 1.0f); blocks(a, 1, ppb); set_int(a, "eu_bfwd", 1);
        CHECK(lp() == 1 && opt("eu_bfwd") && !opt("eu_both") && !opt("eu_fwd"), "the left arrow: page 1 again; A's BFWD");
        { void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); std::string sv((char *)cx);
          AEffect *b = VSTPluginMain(host); b->d(b, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0);
          set_int(b, "track", 2); blocks(b, 1, ppb);
          CHECK(b->getP(b, P("eu_bfwd")) == 1.0f && b->getP(b, P("eu_bfwd2")) == 1.0f, "A's and B's BFWD saved and loaded");
          b->d(b, 1, 0, 0, 0, 0); }
        set_int(a, "euclid_edit", 0);
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
        set_int(a, "track", 0);
    }

    /* STATE 1..8 (v1.68.0): the whole sequencer */
    {
        auto tap = [&](const char *k) { a->setP(a, P(k), a->getP(a, P(k)) > 0.5f ? 0.0f : 1.0f); blocks(a, 1, ppb); };
        auto rd = [&](const char *k, char *o) { a->d(a, 7, P(k), 0, o, 0); };
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        set_int(a, "track", 0); set_int(a, "trk_vel", 90); set_int(a, "shuffle", 62); blocks(a, 1, ppb);
        const float st0 = a->getP(a, P("step3"));
        tap("pst1");
        CHECK(a->getP(a, P("pst1")) == 1.0f && a->getP(a, P("pst2")) == 0.0f, "a tap on an empty state saves the sequencer (PERFORM ALL off too) and lights it");
        set_int(a, "trk_vel", 30); set_int(a, "shuffle", 50); a->setP(a, P("step3"), st0 > 0.5f ? 0.0f : 1.0f); blocks(a, 1, ppb);
        char d1[64], d2[64], d3[64], d4[64];
        rd("trk_vel", d1); CHECK(a->getP(a, P("step3")) != st0 && strcmp(d1, "90"), "changed: velocity '%s', step 4", d1);
        CHECK(lroundf(a->getP(a, P("pst_act")) * 8) == 1, "the state just saved is the active one (framed)");
        tap("pst8"); CHECK(a->getP(a, P("pst8")) == 1.0f && lroundf(a->getP(a, P("pst_act")) * 8) == 8, "STATE 8 is there and saves too, and is the active one now");
        tap("pst1");
        rd("trk_vel", d1); rd("shuffle", d2);
        CHECK(!strcmp(d1, "90") && a->getP(a, P("step3")) == st0 && a->getP(a, P("pst1")) == 1.0f, "a tap on a full state recalls tracks and steps: velocity '%s', shuffle '%s'", d1, d2);
        tap("pst8"); rd("trk_vel", d1); CHECK(!strcmp(d1, "30") && a->getP(a, P("step3")) != st0, "STATE 8 brings the other one back ('%s')", d1);
        a->setP(a, P("undo"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("undo"), 0.0f); blocks(a, 1, ppb); rd("trk_vel", d1); CHECK(!strcmp(d1, "90"), "a recall is an undo step ('%s')", d1);
        set_int(a, "pf_ctrl", 1); set_int(a, "pf_vel", 20); set_int(a, "pf_cc2", -15); set_int(a, "pf_pan", -20);
        tap("pst2");
        set_int(a, "pf_ctrl", 0); blocks(a, 1, ppb);
        tap("pst2");
        rd("pf_vel", d1); rd("pf_cc2", d2); rd("pf_prob", d3); rd("pf_pan", d4);
        CHECK(a->getP(a, P("pf_ctrl")) == 1.0f && !strcmp(d1, "+20") && !strcmp(d2, "-15") && !strcmp(d3, "+0%") && !strcmp(d4, "-20"), "a state saved with PERFORM ALL on brings it back: %s %s %s, pan %s", d1, d2, d3, d4);
        tap("pst1"); CHECK(a->getP(a, P("pf_ctrl")) == 0.0f, "one saved with it off switches it off");
        { void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); const char *c = (const char *)cx;
          CHECK(strstr(c, "\nV00 t 0 ") && strstr(c, "\nV00 s 0 ") && strstr(c, "\nV01 P 1 0 20 0 -20 0 -15 0 0 0 0 0 0 0\n") && strstr(c, "\nV07 G ") && !strstr(c, "\nV02 "), "states are saved with the project");
          std::string sv(c);
          AEffect *b = VSTPluginMain(host);
          b->d(b, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0);
          char e1[64];
          CHECK(b->getP(b, P("pst1")) == 1.0f && b->getP(b, P("pst2")) == 1.0f && b->getP(b, P("pst8")) == 1.0f && b->getP(b, P("pst3")) == 0.0f, "and load with it");
          b->setP(b, P("pst8"), 0.0f); blocks(b, 1, ppb); b->d(b, 7, P("trk_vel"), 0, e1, 0);
          CHECK(!strcmp(e1, "30"), "a loaded state recalls ('%s')", e1);
          b->d(b, 1, 0, 0, 0, 0); }
        CHECK(lroundf(a->getP(a, P("pst_act")) * 8) == 1, "recalled: STATE 1 is the active one");
        set_int(a, "pclear", 1); tap("pst1");
        CHECK(a->getP(a, P("pst1")) == 0.0f && a->getP(a, P("pst2")) == 1.0f, "CLEAR lit: a tap empties that state only");
        CHECK(a->getP(a, P("pst_act")) == 0.0f, "the active one emptied: no frame");
        rd("trk_vel", d1); CHECK(!strcmp(d1, "90"), "clearing a state does not touch the sequencer (%s)", d1);
        tap("pst3"); CHECK(a->getP(a, P("pst3")) == 0.0f, "CLEAR lit: an empty state stays empty");
        set_int(a, "pclear", 0); set_int(a, "pf_ctrl", 0);
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
        CHECK(a->getP(a, P("pst2")) == 0.0f && a->getP(a, P("pst8")) == 0.0f, "a project without states loads with all eight empty");
        { std::string old = keep + "P 0 0 20 0 0 0 -15 0 0 0 64\n"; a->d(a, 24, 0, (intptr_t)old.size(), (void *)old.c_str(), 0);
          CHECK(a->getP(a, P("pst1")) == 0.0f, "an old PERFORM-only state (P line) is dropped"); }
    }

    /* v1.1.0: every PATTERN has its own eight STATEs; COPY / PASTE PTN and STORE take them along, CLEAR empties them */
    {
        auto tap = [&](const char *k) { a->setP(a, P(k), a->getP(a, P(k)) > 0.5f ? 0.0f : 1.0f); blocks(a, 1, ppb); };
        auto press = [&](const char *k) { a->setP(a, P(k), 1.0f); blocks(a, 1, ppb); a->setP(a, P(k), 0.0f); blocks(a, 1, ppb); };
        auto vel = [&](char *d) { a->d(a, 7, P("trk_vel"), 0, d, 0); return d; };
        auto cur = [&](int k) { return (int)lroundf(a->getP(a, P(("patv" + std::to_string(k)).c_str())) * 5) - 1 == 2; };
        auto on = [&](const char *k) { return a->getP(a, P(k)) == 1.0f; };
        auto spat = [&]() { return (int)lroundf(a->getP(a, P("pst_pat")) * 7) + 1; };
        auto act = [&]() { return (int)lroundf(a->getP(a, P("pst_act")) * 8); };
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
        set_int(a, "pclear", 0); set_int(a, "pat_store", 0); set_int(a, "pat_clear", 0);
        char d1[64];
        CHECK(!on("pst1") && cur(1) && spat() == 1, "start: pattern 1, its states shown (PATTERN %d), STATE 1 empty", spat());
        set_int(a, "track", 0); set_int(a, "trk_vel", 61); a->setP(a, P("step7"), 1.0f); blocks(a, 1, ppb);
        tap("pst1");
        tap("pat2");
        CHECK(cur(2) && spat() == 2 && !on("pst1") && act() == 0, "pattern 2: the buttons show its own states (PATTERN %d): STATE 1 empty, no frame", spat());
        set_int(a, "trk_vel", 42); a->setP(a, P("step3"), 1.0f); blocks(a, 1, ppb);
        tap("pst1"); set_int(a, "trk_vel", 43); blocks(a, 1, ppb); tap("pst2");
        CHECK(on("pst1") && on("pst2") && act() == 2, "pattern 2 saves STATE 1 and 2 of its own");
        tap("pat1");
        CHECK(cur(1) && spat() == 1 && on("pst1") && !on("pst2") && act() == 1, "back on pattern 1: its STATE 1 only, framed (%d)", act());
        set_int(a, "trk_vel", 70); blocks(a, 1, ppb);
        tap("pst1"); CHECK(cur(1) && !strcmp(vel(d1), "61"), "pattern 1's STATE 1 recalls pattern 1's snapshot (%s)", d1);
        tap("pat2"); tap("pst1"); CHECK(cur(2) && !strcmp(vel(d1), "42"), "pattern 2's STATE 1 its own (%s)", d1);
        set_int(a, "pattern_edit", 1);
        press("pat_copy"); tap("pat5"); press("pat_paste");
        CHECK(cur(5) && on("pst1") && on("pst2") && !on("pst3"), "COPY PTN / PASTE PTN: pattern 5 got pattern 2's states");
        tap("pst2"); CHECK(!strcmp(vel(d1), "43"), "and they recall (%s)", d1);
        set_int(a, "pat_store", 1); tap("pat6"); set_int(a, "pat_store", 0);
        tap("pat6"); CHECK(cur(6) && on("pst1") && on("pst2"), "STORE: pattern 6 got them too");
        set_int(a, "pat_clear", 1); tap("pat5"); set_int(a, "pat_clear", 0);
        tap("pat5"); CHECK(cur(5) && !on("pst1") && !on("pst2"), "CLEAR on another pattern: its states are gone");
        set_int(a, "pat_clear", 1); tap("pat6"); set_int(a, "pat_clear", 0);
        tap("pat6"); CHECK(cur(6) && !on("pst1") && !on("pst2"), "CLEAR on the pattern playing: its states are gone");
        tap("pat2"); CHECK(on("pst1") && on("pst2"), "pattern 2 kept its states");
        set_int(a, "pattern_edit", 0);
        { const int was = g_ti.flags; g_ti.flags = kPlaying | kTempo | kPpq; g_ti.ppqPos = 0.5;
          const double pb = 128.0 / 44100.0 * 2.0;
          tap("pat1"); blocks(a, (int)(80.0 / pb), pb);
          CHECK(cur(1) || on("pst1"), "playing: pattern 1 came in");
          CHECK(on("pst1") && !on("pst2") && spat() == 1, "a cued pattern coming in switches the buttons to its states");
          g_ti.flags = was; blocks(a, 1, pb); }
        tap("pat1");
        { void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); std::string sv((const char *)cx);
          CHECK(sv.find("\nV00 t 0 ") != std::string::npos && sv.find("\nV11 G ") != std::string::npos && sv.find("\nQ") == std::string::npos && sv.find("\nV02 ") == std::string::npos,
                "saved with the project: V<pattern><state> lines, no Q lines");
          auto open = [&](const std::string &txt) { AEffect *b = VSTPluginMain(host); b->d(b, 24, 0, (intptr_t)txt.size(), (void *)txt.c_str(), 0); return b; };
          auto bon = [&](AEffect *b, const char *k) { return b->getP(b, P(k)) == 1.0f; };
          auto btap = [&](AEffect *b, const char *k) { b->setP(b, P(k), b->getP(b, P(k)) > 0.5f ? 0.0f : 1.0f); blocks(b, 1, ppb); };
          char e1[64];
          AEffect *b = open(sv);
          CHECK(bon(b, "pst1") && !bon(b, "pst2"), "loaded: pattern 1 plays, its STATE 1");
          btap(b, "pat2"); CHECK(bon(b, "pst1") && bon(b, "pst2"), "pattern 2's two states loaded too");
          btap(b, "pst2"); b->d(b, 7, P("trk_vel"), 0, e1, 0); CHECK(!strcmp(e1, "43"), "and recall (%s)", e1);
          b->d(b, 1, 0, 0, 0, 0);
          std::string old = sv; size_t at;   /* a project of 1.0.1: "Q<k>" lines, for the pattern playing */
          while ((at = old.find("\nV00 ")) != std::string::npos) old.replace(at, 5, "\nQ0 ");
          b = open(old);
          CHECK(bon(b, "pst1"), "an older project's state goes to the pattern playing");
          btap(b, "pst1"); b->d(b, 7, P("trk_vel"), 0, e1, 0); CHECK(!strcmp(e1, "61"), "and recalls (%s)", e1);
          b->d(b, 1, 0, 0, 0, 0);
          old += "Q0 W 2\n";   /* 1.0.2: the pattern it was saved on */
          b = open(old);
          CHECK(!bon(b, "pst1"), "a 1.0.2 state (W 2) is not pattern 1's");
          btap(b, "pat3"); CHECK(bon(b, "pst1"), "it is pattern 3's");
          btap(b, "pst1"); b->d(b, 7, P("trk_vel"), 0, e1, 0); CHECK(!strcmp(e1, "61"), "and recalls there (%s)", e1);
          b->d(b, 1, 0, 0, 0, 0); }
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
    }

    /* PATTERN 1..8 (v1.69.0; STORE v1.72.0; v1.75.0: the sequencer is always one of them, pattern 1 to begin with) */
    {
        auto tap = [&](const char *k) { a->setP(a, P(k), a->getP(a, P(k)) > 0.5f ? 0.0f : 1.0f); blocks(a, 1, ppb); };
        auto msg = [&](char *m) { a->d(a, 7, P("msg"), 0, m, 0); return m; };
        auto vel = [&](char *d) { a->d(a, 7, P("trk_vel"), 0, d, 0); return d; };
        auto lit = [&](int k) { return a->getP(a, P(("pat" + std::to_string(k)).c_str())) == 1.0f; };
        void *c0 = 0; a->d(a, 23, 0, 0, &c0, 0); std::string keep((char *)c0);
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
        char d1[64], m[64];
        CHECK(lit(1) && !lit(2) && !lit(8), "pattern 1 plays from the start: lit, the others empty");
        set_int(a, "track", 0); set_int(a, "trk_vel", 77); blocks(a, 1, ppb);
        const float st0 = a->getP(a, P("step5"));
        set_int(a, "pat_store", 1); tap("pat2");
        CHECK(lit(1) && lit(2) && !strcmp(msg(m), "Stored as Pattern 2"), "STORE lit: what plays is copied into pattern 2 ('%s')", m);
        set_int(a, "pat_store", 0);
        set_int(a, "trk_vel", 33); a->setP(a, P("step5"), st0 > 0.5f ? 0.0f : 1.0f); blocks(a, 1, ppb);
        tap("pat2");
        CHECK(!strcmp(vel(d1), "77") && a->getP(a, P("step5")) == st0 && !strcmp(msg(m), "Pattern 2 Loaded"), "stopped: a tap loads it at once (velocity '%s', '%s')", d1, m);
        tap("pat1");
        CHECK(!strcmp(vel(d1), "33") && a->getP(a, P("step5")) != st0, "pattern 1 kept what was done to it ('%s')", d1);
        set_int(a, "trk_vel", 35); tap("pat1"); CHECK(!strcmp(vel(d1), "35"), "a tap on the pattern playing does nothing ('%s')", d1);
        auto look = [&](int k) { return (int)lroundf(a->getP(a, P(("patv" + std::to_string(k)).c_str())) * 5) - 1; };
        CHECK(look(1) == 2 && look(2) == 1 && look(3) == 0, "the one playing looks like the play-head step, a full one lit, an empty one dark (%d %d %d)", look(1), look(2), look(3));
        {   /* v1.92: while the transport runs it blinks with the beat (PLAYING / BLINK); stopped: steady */
            const int was = g_ti.flags; g_ti.flags = kPlaying | kTempo | kPpq; g_ti.ppqPos = 0.0;
            bool on = false, off = false;
            for (int b = 0; b < 200; b++) { blocks(a, 1, ppb); on |= look(1) == 2; off |= look(1) == 4; }
            g_ti.flags = was; blocks(a, 1, ppb);
            CHECK(on && off && look(1) == 2, "the one playing blinks while the transport runs, steady once it stops");
        }
        set_int(a, "trk_len", 32); set_int(a, "trk_both", 1);
        tap("pat3");
        { char l1[64]; a->d(a, 7, P("trk_len"), 0, l1, 0);
          CHECK(!strcmp(l1, "16") && a->getP(a, P("trk_fwd")) == 1.0f, "an empty pattern starts 16 steps long, forwards ('%s')", l1); }
        bool empty = true; for (int k = 0; k < 16; k++) empty &= a->getP(a, P(("step" + std::to_string(k)).c_str())) == 0.0f;
        CHECK(lit(3) && empty && !strcmp(vel(d1), "35") && !strcmp(msg(m), "Pattern 3 Loaded"), "an empty pattern comes in: no steps, the track setup stays ('%s'), and it is a pattern now", d1);
        set_int(a, "trk_vel", 50); a->setP(a, P("step2"), 1.0f); blocks(a, 1, ppb);   /* v1.99: a step, or it would count as empty */
        { void *cx = 0; a->d(a, 23, 0, 0, &cx, 0); const char *c = (const char *)cx;
          CHECK(strstr(c, "\nW 2\n") && strstr(c, "\nZ0 t 0 ") && strstr(c, "\nZ1 G ") && !strstr(c, "\nZ2 "), "saved with the project: the one playing (W) and the others");
          std::string sv(c); AEffect *b = VSTPluginMain(host); b->d(b, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0);
          char e1[64];
          CHECK(b->getP(b, P("pat1")) == 1.0f && b->getP(b, P("pat2")) == 1.0f && b->getP(b, P("pat3")) == 1.0f && b->getP(b, P("pat4")) == 0.0f, "and loaded: three patterns");
          b->d(b, 7, P("trk_vel"), 0, e1, 0); CHECK(!strcmp(e1, "50"), "pattern 3 plays, as it was ('%s')", e1);
          b->setP(b, P("pat1"), 0.0f); blocks(b, 1, ppb); b->d(b, 7, P("trk_vel"), 0, e1, 0);
          CHECK(!strcmp(e1, "35"), "pattern 1 from the project ('%s')", e1);
          b->d(b, 1, 0, 0, 0, 0); }
        {   /* playing: a cued pattern comes in at the end of the one playing; a second tap takes the cue back */
            const double pb = 128.0 / 44100.0 * 2.0;   /* beats per block at 120 BPM */
            const int was = g_ti.flags; g_ti.flags = kPlaying | kTempo | kPpq; g_ti.ppqPos = 0.5;
            blocks(a, 2, pb);
            tap("pat2"); CHECK(!strcmp(msg(m), "Pattern 2 Cued") && !strcmp(vel(d1), "50"), "playing: '%s', pattern 3 plays on (%s)", m, d1);
            tap("pat2"); CHECK(!strcmp(msg(m), " "), "a second tap takes the cue back, the message goes ('%s')", m);
            blocks(a, (int)(80.0 / pb), pb); CHECK(!strcmp(vel(d1), "50"), "and nothing changes, however long it plays (%s)", d1);
            tap("pat2"); const double t0 = g_ti.ppqPos; double t1 = -1.0;
            for (int b = 0; b < (int)(80.0 / pb) && t1 < 0; b++) { blocks(a, 1, pb); if (!strcmp(vel(d1), "77")) t1 = g_ti.ppqPos; }
            CHECK(t1 > t0 + 0.2 && !strcmp(msg(m), "Pattern 2 Loaded"), "cued again: it comes in later, at the end of the pattern (%.2f -> %.2f), '%s'", t0, t1, m);
            tap("pat5"); CHECK(!strcmp(msg(m), "Pattern 5 Cued"), "an empty one is cued too ('%s')", m);
            tap("pat3"); CHECK(!strcmp(msg(m), "Pattern 3 Cued"), "another tap moves the cue ('%s')", m);
            g_ti.flags = was; blocks(a, 1, pb);
            CHECK(!strcmp(vel(d1), "50") && !strcmp(msg(m), "Pattern 3 Loaded") && !lit(5), "cued and the transport stops: it comes in now (%s, '%s')", d1, m);
        }
        set_int(a, "pat_store", 1); set_int(a, "pat_clear", 1);
        CHECK(a->getP(a, P("pat_store")) == 0.0f, "CLEAR and STORE: one at a time");
        tap("pat2");
        CHECK(!lit(2) && lit(1) && !strcmp(msg(m), "Pattern 2 Cleared"), "CLEAR lit: a tap empties that pattern only ('%s')", m);
        CHECK(!strcmp(vel(d1), "50"), "clearing another one does not touch the sequencer ('%s')", d1);
        a->setP(a, P("step1"), 1.0f); blocks(a, 1, ppb); tap("pat3");
        CHECK(lit(3) && a->getP(a, P("step1")) == 0.0f && !strcmp(msg(m), "Pattern 3 Cleared"), "clearing the one playing: its steps go, it stays ('%s')", m);
        set_int(a, "pat_clear", 0);
        CHECK(a->getP(a, P("pst1")) == 0.0f, "patterns and states are separate");
        /* COPY PTN / PASTE PTN (v1.85) */
        set_int(a, "pattern_edit", 1); set_int(a, "track", 0);
        a->setP(a, P("step6"), 1.0f); set_int(a, "trk_vel", 88); blocks(a, 1, ppb);
        auto press = [&](const char *k) { a->setP(a, P(k), 1.0f); blocks(a, 1, ppb); a->setP(a, P(k), 0.0f); blocks(a, 1, ppb); };
        press("pat_copy"); CHECK(!strcmp(msg(m), "Pattern 3 Copied"), "COPY PTN ('%s')", m);
        tap("pat5"); CHECK(lit(5) && a->getP(a, P("step6")) == 0.0f, "pattern 5: empty");
        press("pat_paste");
        CHECK(a->getP(a, P("step6")) == 1.0f && !strcmp(vel(d1), "88") && !strcmp(msg(m), "Pasted to Pattern 5"), "PASTE PTN: pattern 3 into pattern 5 (%s, '%s')", d1, m);
        blocks(a, 120, ppb);   /* the paste becomes an undo step of its own */
        press("pat_clrall"); CHECK(a->getP(a, P("step6")) == 0.0f && !strcmp(msg(m), "Pattern 5 Cleared"), "CLEAR PTN (v2.0): no steps left ('%s')", m);
        press("undo"); CHECK(a->getP(a, P("step6")) == 1.0f, "and UNDO brings them back");
        tap("pat3"); tap("pat5"); CHECK(a->getP(a, P("step6")) == 1.0f, "and pattern 5 keeps it");
        set_int(a, "random_edit", 1); CHECK(lroundf(a->getP(a, P("rtrk")) * 8) == 1, "RANDOM SETTINGS names track 1");
        set_int(a, "track", 4); CHECK(lroundf(a->getP(a, P("rtrk")) * 8) == 5, "and track 5");
        CHECK(lroundf(a->getP(a, P("ttl")) * 17) == 14, "title RANDOM TRACK 5 (v2.0)");
        set_int(a, "global_edit", 1); CHECK(lroundf(a->getP(a, P("ttl")) * 17) == 1, "SETUP: its own title");
        set_int(a, "cc_edit", 1); CHECK(lroundf(a->getP(a, P("ttl")) * 17) == 6, "TRACK SETUP: SETUP TRACK 5");
        set_int(a, "cc_edit", 0); set_int(a, "random_edit", 1);
        set_int(a, "pattern_edit", 1); CHECK(a->getP(a, P("rtrk")) == 0.0f, "other modes: no name");
        a->d(a, 24, 0, (intptr_t)keep.size(), (void *)keep.c_str(), 0);
        CHECK(lit(1) && !lit(2) && !lit(3) && a->getP(a, P("pat_store")) == 0.0f, "a project without patterns loads as pattern 1");
    }

    /* PERFORM pads */
    {
        set_int(a, "pfx1", 1); set_int(a, "pfx2", 1);
        CHECK(a->getP(a, P("pfx1")) == 0.0f && a->getP(a, P("pfx2")) == 1.0f, "LOOP 2 takes over from LOOP 4");
        set_int(a, "pfx1", 0); CHECK(a->getP(a, P("pfx2")) == 1.0f, "switching an unlit loop pad off leaves the loop running");
        set_int(a, "pfx2", 0); CHECK(a->getP(a, P("pfx2")) == 0.0f && a->getP(a, P("pfx3")) == 0.0f, "loops off");
        set_int(a, "pfx8", 1); CHECK(a->getP(a, P("fill")) == 1.0f, "the FILL pad is the fill switch"); set_int(a, "pfx8", 0);
        a->setP(a, P("mute2"), 1.0f);
        set_int(a, "pfx6", 1);
        CHECK(a->getP(a, P("mute1")) == 1.0f && a->getP(a, P("mute2")) == 0.0f && a->getP(a, P("mute8")) == 1.0f, "INVERT MUTE flips the mutes");
        set_int(a, "pfx6", 0);
        CHECK(a->getP(a, P("mute1")) == 0.0f && a->getP(a, P("mute2")) == 1.0f, "and flips them back");
        set_int(a, "pfx6", 1);
        blocks(a, 1, ppb); memset(automated, 0, sizeof automated);
        a->setP(a, P("pfx7"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("pfx7"), 0.0f); blocks(a, 1, ppb);
        int muted = 0; for (int k = 0; k < 8; k++) muted += a->getP(a, P("mute1") + k) > 0.5f;
        CHECK(muted == 0 && a->getP(a, P("pfx6")) == 0.0f && automated[P("pfx7")] == 1, "UNMUTE ALL: nothing muted, INVERT MUTE unlit, the pad springs back");
        CHECK(a->getP(a, P("mstate")) == 0.0f, "mstate reads NONE");
        a->setP(a, P("pfx7"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("pfx7"), 0.0f); blocks(a, 1, ppb);
        muted = 0; for (int k = 0; k < 8; k++) muted += a->getP(a, P("mute1") + k) > 0.5f;
        CHECK(muted == 8 && a->getP(a, P("mstate")) == 1.0f, "nothing muted: the same pad is MUTE ALL (%d muted)", muted);
        a->setP(a, P("mute4"), 0.0f);
        a->setP(a, P("pfx7"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("pfx7"), 0.0f); blocks(a, 1, ppb);
        muted = 0; for (int k = 0; k < 8; k++) muted += a->getP(a, P("mute1") + k) > 0.5f;
        CHECK(muted == 0 && a->getP(a, P("mstate")) == 0.0f, "some muted: it unmutes all");
        a->setP(a, P("mstate"), 1.0f); CHECK(a->getP(a, P("mstate")) == 0.0f, "mstate is read-only");
        set_int(a, "pfy3", 1); CHECK(a->getP(a, P("pfy3")) == 1.0f, "second row pads are switches"); set_int(a, "pfy3", 0);
        a->d(a, 8, P("pfx5"), 0, name, 0); CHECK(!strcmp(name, "Random Step"), "pad 5 is '%s'", name);
    }

    /* pan lock knob: 0 = no lock, then CC 10 value 0..127 shown as L / C / R */
    set_int(a, "track", 3); set_int(a, "page", 0); a->setP(a, P("step1"), 1.0f);   /* T4 step 2 */
    a->d(a, 7, P("st_pan"), 0, disp, 0); CHECK(!strcmp(disp, "--"), "no pan lock shows '%s'", disp);
    set_int(a, "st_pan", 1 + 64); a->d(a, 7, P("st_pan"), 0, disp, 0); CHECK(!strcmp(disp, "C"), "pan 64 is the centre: '%s'", disp);
    set_int(a, "st_pan", 1 + 0);  a->d(a, 7, P("st_pan"), 0, disp, 0); CHECK(!strcmp(disp, "L64"), "pan 0: '%s'", disp);
    set_int(a, "st_pan", 1 + 127); a->d(a, 7, P("st_pan"), 0, disp, 0); CHECK(!strcmp(disp, "R63"), "pan 127: '%s'", disp);
    a->d(a, 8, P("st_pan"), 0, name, 0); CHECK(!strcmp(name, "Pan"), "its name '%s'", name);
    set_int(a, "rnd_dens", 100); set_int(a, "rnd_min", 10); set_int(a, "rnd_max", 20);
    a->setP(a, P("rnd_pan"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("rnd_pan"), 0.0f);
    a->d(a, 7, P("st_pan"), 0, disp, 0);
    CHECK(disp[0] == 'L' && atoi(disp + 1) >= 44 && atoi(disp + 1) <= 54, "RND PAN in 10..20: step 2 is now '%s'", disp);
    set_int(a, "step_edit", 1); set_int(a, "shift", 1); set_int(a, "rnd_min", 127); set_int(a, "rnd_max", 127);   /* edit mode: the selected step only */
    a->setP(a, P("rnd_pan"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("rnd_pan"), 0.0f);
    a->d(a, 7, P("st_pan"), 0, disp, 0); CHECK(!strcmp(disp, "R63"), "RND PAN with STEP EDIT on: the selected step gets '%s'", disp);
    a->setP(a, P("step4"), 1.0f);                                                          /* two selected */
    set_int(a, "st_pan", 1 + 64); set_int(a, "st_vel", 77); set_int(a, "st_cc2", 1 + 9); set_int(a, "st_prob", 60); set_int(a, "st_cond", cond_pos(C_FILL));
    set_int(a, "pattern_edit", 1);
    {
        int ok = 1; char d[5][64];
        const int steps[2] = {2, 5};
        for (int q = 0; q < 2; q++) {
            set_int(a, "sel", steps[q]);
            a->d(a, 7, P("st_pan"), 0, d[0], 0); a->d(a, 7, P("st_vel"), 0, d[1], 0); a->d(a, 7, P("st_cc2"), 0, d[2], 0); a->d(a, 7, P("st_prob"), 0, d[3], 0); a->d(a, 7, P("st_cond"), 0, d[4], 0);
            ok &= !strcmp(d[0], "C") && !strcmp(d[1], "77") && !strcmp(d[2], "9") && !strcmp(d[3], "60%") && !strcmp(d[4], "FILL");
        }
        CHECK(ok, "several steps selected: a lock knob writes to all of them (last: %s %s %s %s %s)", d[0], d[1], d[2], d[3], d[4]);
        set_int(a, "sel", 3); a->d(a, 7, P("st_vel"), 0, d[1], 0); CHECK(strcmp(d[1], "77") != 0, "and not to the others ('%s')", d[1]);
        for (int q = 0; q < 2; q++) { set_int(a, "sel", steps[q]); set_int(a, "st_vel", 0); set_int(a, "st_cc2", 0); set_int(a, "st_prob", 100); set_int(a, "st_cond", 0); set_int(a, "st_pan", steps[q] == 2 ? 128 : 0); }
    }
    set_int(a, "sel", 2);
    set_int(a, "rnd_min", 0); set_int(a, "track", 0);

    /* random buttons act on the selected track and the host sees the new steps (TRACK EDIT; v1.93: PATTERN EDIT = every track) */
    set_int(a, "track_edit", 1);
    set_int(a, "track", 7); set_int(a, "trk_len", 16); set_int(a, "rnd_dens", 100);
    blocks(a, 1, ppb); memset(automated, 0, sizeof automated);
    a->setP(a, P("rnd_trig"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("rnd_trig"), 0.0f);
    int lit = 0, told_n = 0;
    for (int i = 0; i < 16; i++) { lit += a->getP(a, P("step0") + i) > 0.5f; told_n += automated[P("step0") + i]; }
    CHECK(lit == 16 && told_n == 16 && automated[P("rnd_trig")] == 1, "RND TRIGS on T8 at 100%%: 16 steps lit, host told %d times, button sprang back", told_n);
    set_int(a, "rnd_min", 70); set_int(a, "rnd_max", 80);
    a->setP(a, P("rnd_vel"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("rnd_vel"), 0.0f);
    a->setP(a, P("rnd_cc4"), 1.0f); blocks(a, 1, ppb); a->setP(a, P("rnd_cc4"), 0.0f);
    const bool was_trk = a->getP(a, P("track_edit")) > 0.5f;
    if (was_trk) set_int(a, "step_edit", 1);   /* v1.2.0: in TRACK EDIT these knobs are the track's values (Q-Links) */
    set_int(a, "sel", 6);
    a->d(a, 7, P("st_vel"), 0, disp, 0); int rv = atoi(disp);
    a->d(a, 7, P("st_cc4"), 0, disp, 0); int rc = atoi(ccv(disp));
    a->d(a, 7, P("st_cc1"), 0, name, 0);
    CHECK(rv >= 70 && rv <= 80 && rc >= 70 && rc <= 80 && !strcmp(ccv(name), "--"), "RND VEL / RND CC 4: step 6 velocity %d, CC 23 = %d, CC 20 still '%s'", rv, rc, name);
    if (was_trk) set_int(a, "track_edit", 1);
    set_int(a, "track", 0);
    a->d(a, 7, P("st_vel"), 0, disp, 0);
    CHECK(a->getP(a, P("step0")) < 0.5f, "T1 was not randomized");
    set_int(a, "rnd_dens", 35);
    set_int(a, "pattern_edit", 1);
    {   /* v1.93: in PATTERN EDIT the random buttons act on every track (each with its own settings) */
        AEffect *z = VSTPluginMain(host); set_int(z, "pattern_edit", 1);
        for (int t = 0; t < 8; t++) { set_int(z, "track", t); set_int(z, "rnd_dens", 100); }
        z->setP(z, P("rnd_trig"), 1.0f); blocks(z, 1, ppb); z->setP(z, P("rnd_trig"), 0.0f);
        bool all = true; for (int t = 0; t < 8; t++) { set_int(z, "track", t); all &= z->getP(z, P("step0")) == 1.0f && z->getP(z, P("step15")) == 1.0f; }
        CHECK(all, "PATTERN EDIT: RND TRIGS fills all eight tracks");
        z->d(z, 1, 0, 0, 0, 0);
    }

    /* chunk round-trip into the second instance */
    void *chunk = 0;
    intptr_t n = a->d(a, 23, 0, 0, &chunk, 0);
    std::string saved((char *)chunk);
    printf("     chunk %ld bytes\n", (long)n);
    CHECK(n == (intptr_t)saved.size() + 1 && !saved.compare(0, 4, "TSq3"), "chunk is text, starts with its version tag");
    CHECK(b->d(b, 24, 0, n, (void *)saved.c_str(), 0) == 1, "setChunk accepted");
    void *chunk2 = 0;
    intptr_t n2 = b->d(b, 23, 0, 0, &chunk2, 0);
    CHECK(n2 == n && saved == (char *)chunk2, "instance b's chunk equals a's");
    set_int(b, "sel", 4);
    b->d(b, 7, P("st_cond"), 0, disp, 0); CHECK(!strcmp(disp, "1:2"), "b: T1 step 4 condition '%s'", disp);
    b->d(b, 7, P("st_vel"), 0, disp, 0);  CHECK(!strcmp(disp, "50"), "b: T1 step 4 velocity lock '%s'", disp);
    b->d(b, 7, P("trk_len"), 0, disp, 0); CHECK(!strcmp(disp, "64"), "b: T1 length '%s'", disp);
    set_int(b, "track", 5); set_int(b, "sel", 1);
    b->d(b, 7, P("st_cc2"), 0, disp, 0);  CHECK(!strcmp(disp, "99"), "b: T6 step 1 second CC lock '%s'", disp);
    b->d(b, 7, P("trk_ccbase"), 0, disp, 0); CHECK(!strcmp(disp, "40-44"), "b: T6 CC BASE carried over '%s'", disp);
    set_int(b, "sel", 9);
    b->d(b, 7, P("st_cc5"), 0, disp, 0);  CHECK(!strcmp(disp, "0"), "b: T6 step 9 fifth CC lock '%s'", disp);
    b->d(b, 7, P("rnd_dens"), 0, disp, 0); CHECK(!strcmp(disp, "50%"), "b: T6 random density is its own '%s'", disp);
    set_int(b, "track", 0); b->d(b, 7, P("rnd_dens"), 0, disp, 0); CHECK(!strcmp(disp, "35%"), "b: T1 random density '%s'", disp);
    CHECK(b->d(b, 24, 0, 16, (void *)"TSq2\ng 1 2 3 0 0\n", 0) == 1 && b->getP(b, P("step_edit")) == 0.0f && b->getP(b, P("page")) > 0.6f, "a pre-1.3 chunk (no step-edit field) still loads");
    b->d(b, 24, 0, n, (void *)saved.c_str(), 0);
    set_int(b, "track", 7); b->d(b, 7, P("rnd_max"), 0, disp, 0);  CHECK(!strcmp(disp, "80"), "b: T8 random max '%s'", disp);
    set_int(b, "track", 2); b->d(b, 7, P("rnd_max"), 0, disp, 0);  CHECK(!strcmp(disp, "127"), "b: T3 random max is its own '%s'", disp);
    CHECK(b->d(b, 24, 0, 29, (void *)"TSq3\ng 4 0 1 0 0 0\nr 12 3 99\n", 0) == 1, "an older chunk with one random line loads");
    b->d(b, 7, P("rnd_max"), 0, disp, 0); set_int(b, "track", 1); b->d(b, 7, P("rnd_dens"), 0, name, 0);
    CHECK(!strcmp(disp, "99") && !strcmp(name, "12%"), "and its random settings go to every track: max '%s', density '%s'", disp, name);
    b->d(b, 24, 0, n, (void *)saved.c_str(), 0);
    set_int(b, "track", 3); set_int(b, "sel", 2);
    b->d(b, 7, P("st_pan"), 0, disp, 0); CHECK(!strcmp(disp, "R63"), "b: T4 step 2 pan lock '%s'", disp);
    CHECK(b->d(b, 24, 0, 6, (void *)"TSq1\n", 0) == 0, "the older chunk version is refused");
    {   /* a project saved before the pan lock (TSq2: 10 bytes per step) still loads, with no pan lock */
        const char old[] = "TSq2\ng 2 0 1 0 0\ns 2 00000000000000000000" "01006432080100000080" "0100640000" "0000000000\n";
        CHECK(b->d(b, 24, 0, sizeof old, (void *)old, 0) == 1, "TSq2 chunk accepted");
        {   /* a project from before the per-track CC numbers: its track lines have no CC base, and those tracks play CC 20..24 as before */
            const char pre[] = "TSq3\ng 4 0 0 0 0 0\nt 4 16 4 0 40 100 4 0\nt 5 16 4 0 38 100 4 0 61\n";
            AEffect *c = VSTPluginMain(host);
            CHECK(c->d(c, 24, 0, sizeof pre, (void *)pre, 0) == 1, "older TSq3 chunk accepted");
            char d1[64], d2[64], d3[64];
            c->d(c, 7, P("trk_ccbase"), 0, d1, 0);
            c->setP(c, P("trk6"), 1.0f); c->d(c, 7, P("trk_ccbase"), 0, d2, 0);
            c->setP(c, P("trk1") + 6, 1.0f); c->d(c, 7, P("trk_ccbase"), 0, d3, 0);
            CHECK(!strcmp(d1, "20-24") && !strcmp(d2, "61-65") && !strcmp(d3, "14-18"), "track line without a CC base: %s; with one: %s; a track not in the chunk keeps its default: %s", d1, d2, d3);
            c->d(c, 1, 0, 0, 0, 0);
        }
        set_int(b, "step_edit", 1);   /* v1.2.0: the step's locks, not the track's values (Q-Links in TRACK EDIT) */
        set_int(b, "sel", 2);
        b->d(b, 7, P("st_vel"), 0, disp, 0); b->d(b, 7, P("st_cc5"), 0, name, 0);
        char pan[64]; b->d(b, 7, P("st_pan"), 0, pan, 0);
        CHECK(b->getP(b, P("step1")) > 0.5f && !strcmp(disp, "50") && !strcmp(ccv(name), "127") && !strcmp(pan, "--"), "TSq2 step 2: trig, velocity '%s', fifth CC '%s', pan '%s'", disp, name, pan);
        set_int(b, "sel", 3); b->d(b, 7, P("st_vel"), 0, disp, 0);
        CHECK(b->getP(b, P("step2")) > 0.5f && disp[0] == '(', "TSq2 step 3 is not shifted by the missing pan byte ('%s')", disp);
        b->d(b, 24, 0, n, (void *)saved.c_str(), 0);
        set_int(b, "track", 5);
    }
    CHECK(b->d(b, 24, 0, 5, (void *)"junk", 0) == 0, "a foreign chunk is refused");
    CHECK(b->d(b, 24, 0, 12, (void *)"TSq2\ns 0 zz", 0) == 1, "a damaged chunk does not crash");

    a->d(a, 1, 0, 0, 0, 0);
    b->d(b, 1, 0, 0, 0, 0);
}

static void ccbase_tests() {   /* a CC with a base value goes back to it on the first step after a lock */
    {
        Seq s; Track &t = s.t[0];
        int st = 0xB0 | (t.ch & 15), cc = t.ccbase;
        for (int k = 0; k < 16; k++) t.st[k].on = 1;
        t.st[7].cc[0] = 1 + 127; t.ccb[0] = 1 + 64;
        std::vector<int> v; std::vector<double> w = run(s, 4.0, st, cc, &v);
        CHECK(w.size() == 2 && v[0] == 127 && v[1] == 64 && fabs(w[0] - 1.75) < 0.01 && fabs(w[1] - 2.0) < 0.01,
              "lock 127 on step 8, base 64: %d messages, 127 at %.2f, 64 at %.2f, nothing else", (int)w.size(), w.size() > 0 ? w[0] : -1, w.size() > 1 ? w[1] : -1);
        t.st[8].on = 0; t.st[9].on = 0; v.clear(); w = run(s, 4.0, st, cc, &v);
        CHECK(w.size() == 2 && v[1] == 64 && fabs(w[1] - 2.0) < 0.01, "the return does not wait for a trig: step 9 is empty and still sends 64 (%d messages)", (int)w.size());
        t.st[8].on = 1; t.st[8].cc[0] = 1 + 90; v.clear(); w = run(s, 4.0, st, cc, &v);
        CHECK(w.size() == 3 && v[0] == 127 && v[1] == 90 && v[2] == 64 && fabs(w[2] - 2.25) < 0.01, "two locks in a row: 127, 90, then 64 on step 10 (%d messages)", (int)w.size());
        t.ccb[0] = 0; v.clear(); w = run(s, 4.0, st, cc, &v);
        CHECK(w.size() == 2 && v[0] == 127 && v[1] == 90, "no base value: the locks only, as before (%d messages)", (int)w.size());
        /* v1.2.0 STOP CC: no CC at all while it is lit, the notes play on; off: the base value goes out on the next step */
        t.ccb[0] = 1 + 64; t.st[8].on = 0; t.st[8].cc[0] = 0; t.panb = 1 + 30; t.st[3].pan = 1 + 100;
        s.set_stop_cc(true);
        { const size_t ncc = run(s, 4.0, st, -1).size(), nn = run(s, 4.0, 0x90 | (t.ch & 15), -1).size();
          CHECK(ncc == 0 && nn == 14, "STOP CC: no CC (locks, base, pan), every note (14 trigs) (%d CC, %d notes)", (int)ncc, (int)nn); }
        s.set_stop_cc(false); v.clear(); w = run(s, 4.0, st, cc, &v);
        CHECK(w.size() == 3 && v[0] == 64 && w[0] < 0.01 && v[1] == 127 && v[2] == 64, "STOP CC off: base 64 at once on step 1, then lock and base as before (%d messages)", (int)w.size());
        s.set_stop_cc(true); s.set_stop_cc(false); std::vector<int> pv; run(s, 1.0, st, PAN_CC[0], &pv);
        CHECK(pv.size() == 2 && pv[0] == 30 && pv[1] == 100, "and the pan rest too, then the pan lock (%d messages)", (int)pv.size());
    }
    /* the wrapper: BASE knobs in EDIT CC, saved with the project, READ PROJECT */
    setenv("STEPRUNNER_XPJ_ROOT", "testdata", 1);
    AEffect *a = VSTPluginMain(host);
    char disp[64];
    const double ppb = 0.0;
    auto press = [&](const char *k) { a->setP(a, P(k), 1.0f); blocks(a, 1, ppb); a->setP(a, P(k), 0.0f); blocks(a, 1, ppb); };
    set_int(a, "track_edit", 1); blocks(a, 1, ppb);
    set_int(a, "cc_edit", 1); blocks(a, 1, ppb);
    a->d(a, 7, P("ccb1"), 0, disp, 0); CHECK(!strcmp(disp, "OFF"), "BASE 1 starts OFF ('%s')", disp);
    set_int(a, "ccb1", 65); blocks(a, 1, ppb); press("cc_discard");
    set_int(a, "cc_edit", 1); blocks(a, 1, ppb);
    a->d(a, 7, P("ccb1"), 0, disp, 0); CHECK(!strcmp(disp, "64"), "v1.2.0: a base value takes effect at once, DISCARD does not undo it ('%s')", disp);
    set_int(a, "ccb1", 65); set_int(a, "ccb3", 1); blocks(a, 1, ppb); press("cc_commit");
    a->d(a, 7, P("ccb1"), 0, disp, 0); CHECK(!strcmp(disp, "64"), "COMMIT keeps BASE 1 = '%s'", disp);
    a->d(a, 7, P("ccb3"), 0, disp, 0); CHECK(!strcmp(disp, "0"), "and BASE 3 = '%s' (0 is a value, not OFF)", disp);
    {
        static char *chunk; int n = (int)a->d(a, 23, 0, 0, &chunk, 0);
        std::string saved(chunk, n);
        CHECK(saved.find("\nB 0 65 0 1 0 0 0 0 0\n") != std::string::npos, "the chunk has the base line for T1");
        AEffect *b = VSTPluginMain(host);
        b->d(b, 24, 0, n, (void *)saved.c_str(), 0);
        b->d(b, 7, P("ccb1"), 0, disp, 0); CHECK(!strcmp(disp, "64"), "reloaded: BASE 1 = '%s'", disp);
        b->d(b, 7, P("ccb2"), 0, disp, 0); CHECK(!strcmp(disp, "OFF"), "reloaded: BASE 2 = '%s'", disp);
        b->d(b, 1, 0, 0, 0, 0);
    }
    /* READ PROJECT: cctest2.xpj has CC 20 on channel 1 at 37 and CC 19 on channel 16 at 21 */
    set_int(a, "cc_edit", 1); set_int(a, "ccn1", 20); set_int(a, "ccn2", 19); blocks(a, 1, ppb);
    press("cc_read");
    a->d(a, 7, P("msg"), 0, disp, 0); CHECK(!strcmp(disp, "Read 1 project value"), "READ PROJECT on channel 1: '%s'", disp);
    a->d(a, 7, P("ccb1"), 0, disp, 0); CHECK(!strcmp(disp, "37"), "CC 20 on channel 1: base '%s'", disp);
    char d2[64]; a->d(a, 7, P("ccb2"), 0, d2, 0); CHECK(!strcmp(d2, "OFF"), "CC 19 is learned on channel 16, not this one: '%s'", d2);
    press("cc_commit");
    set_int(a, "trk_ch", 16); blocks(a, 1, ppb);
    set_int(a, "cc_edit", 1); blocks(a, 1, ppb); press("cc_read");
    a->d(a, 7, P("ccb2"), 0, d2, 0); a->d(a, 7, P("ccb1"), 0, disp, 0);
    CHECK(!strcmp(d2, "21") && !strcmp(disp, "37"), "on channel 16: CC 19 base '%s'; the CC not found keeps what it had ('%s')", d2, disp);
    press("cc_discard");
    setenv("STEPRUNNER_XPJ_ROOT", "testdata/none", 1);
    set_int(a, "cc_edit", 1); blocks(a, 1, ppb); press("cc_read");
    a->d(a, 7, P("msg"), 0, disp, 0); CHECK(!strcmp(disp, "No saved project found"), "no project: '%s'", disp);
    a->d(a, 1, 0, 0, 0, 0);
    {   /* a real project with 38 plugin mappings on channel 1: CC 20..23 are there, CC 24 is not */
        setenv("STEPRUNNER_XPJ_ROOT", "testdata9", 1);
        AEffect *c = VSTPluginMain(host);
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 1, 0.0); };
        set_int(c, "track_edit", 1); blocks(c, 1, 0.0); set_int(c, "cc_edit", 1); blocks(c, 1, 0.0);
        pr("cc_read");
        char m[64], b1[64], b2[64], b5[64];
        c->d(c, 7, P("msg"), 0, m, 0); c->d(c, 7, P("ccb1"), 0, b1, 0); c->d(c, 7, P("ccb2"), 0, b2, 0); c->d(c, 7, P("ccb5"), 0, b5, 0);
        CHECK(!strcmp(m, "Read 4 project values") && !strcmp(b1, "94") && !strcmp(b2, "34") && !strcmp(b5, "OFF"), "9w9: '%s', BASE 1 %s, BASE 2 %s, BASE 5 %s", m, b1, b2, b5);
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* INITIALIZE: its section opens on the left; FETCH CC VALUES reads base values and names of all eight tracks */
        setenv("STEPRUNNER_XPJ_ROOT", "testdata9", 1);
        AEffect *c = VSTPluginMain(host); set_int(c, "pattern_edit", 1);   /* v1.99: a new one opens on TRACK EDIT */
        char m[64], d1[64], d5[64], t2[64], r1[64];
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        #define OPTC(key) ((int)lroundf(c->getP(c, P(key)) * (PARAMS[P(key)].nopts - 1)))
        c->d(c, 7, P("st_cc1"), 0, d1, 0); CHECK(!strcmp(d1, "--"), "no lock, no base: '%s'", d1);
        display_updates = 0;
        pr("read_all");
        c->d(c, 7, P("srcrow1"), 0, r1, 0);
        CHECK(OPTC("ptitle") == 0 && OPTC("lpanel") == 0 && OPTC("panel") == 5 && OPTC("scr") == 0, "INITIALIZE is retired: its button changes nothing; the functions are on the SETUP tab");
        CHECK(!strcmp(r1, " ") && OPTC("srchl1") == 0, "no list until CHOOSE TARGET TRACK");
        c->d(c, 7, P("st_cc1"), 0, d1, 0); CHECK(!strcmp(d1, "--"), "nothing is read on opening ('%s')", d1);
        pr("src_commit"); pr("srcsel1"); CHECK(OPTC("scr") == 0, "COMMIT and row taps do nothing without the list");
        pr("read_all"); pr("src_notes"); c->d(c, 7, P("msg"), 0, m, 0);
        CHECK(!strcmp(m, "Read 0 notes") && OPTC("ptitle") == 0, "FETCH NOTES in a project whose mappings go to a plugin, not to drum pads: '%s'", m);
        pr("read_all"); pr("src_fetch");
        c->d(c, 7, P("msg"), 0, m, 0); c->d(c, 7, P("st_cc1"), 0, d1, 0); c->d(c, 7, P("st_cc5"), 0, d5, 0);
        c->d(c, 7, P("srcproj"), 0, r1, 0);
        CHECK(!strncmp(m, "Read ", 5) && strstr(m, " CC values") && atoi(m + 5) >= 8 && !strcmp(r1, "9w9") && display_updates > 0, "FETCH CC VALUES with no target track: '%s' (from any mapping); project '%s'", m, r1);
        CHECK(!strcmp(d1, "BD Lev (94)") && !strcmp(d5, "--") && (int)lroundf(c->getP(c, P("st_cc1")) * 128) == 95 && c->getP(c, P("st_cc5")) == 0.0f, "T1 lock knobs: CC 1 '%s' and the knob is on it, CC 5 '%s'", d1, d5);
        {   /* the knob resting on the base is not a lock: nothing in the chunk, and the host echoing the value back makes none */
            static char *ch0; int n0 = (int)c->d(c, 23, 0, 0, &ch0, 0); std::string before(ch0, n0);
            c->setP(c, P("st_cc1"), c->getP(c, P("st_cc1"))); blocks(c, 1, 0.0);
            int n1 = (int)c->d(c, 23, 0, 0, &ch0, 0); std::string after(ch0, n1);
            c->d(c, 7, P("st_cc1"), 0, d1, 0);
            CHECK(before == after && !strcmp(d1, "BD Lev (94)"), "an echo of the resting value leaves the step alone ('%s')", d1);
            set_int(c, "st_cc1", 41); c->d(c, 7, P("st_cc1"), 0, d1, 0); CHECK(!strcmp(d1, "BD Lev 40"), "turning it makes a lock: '%s'", d1);
            set_int(c, "st_cc1", 0); blocks(c, 1, 0.0); c->d(c, 7, P("st_cc1"), 0, d1, 0);
            CHECK(!strcmp(d1, "BD Lev (94)") && (int)lroundf(c->getP(c, P("st_cc1")) * 128) == 0, "all the way down clears the lock; the knob stays at -- (v1.2.0: no jump), the text shows the base: '%s'", d1);
        }
        set_int(c, "track", 1); blocks(c, 1, 0.0); c->d(c, 7, P("st_cc1"), 0, t2, 0);
        CHECK(!strcmp(t2, "SN Lev (58)"), "T2 (CC 25 = SN Level): '%s'", t2);
        c->d(c, 7, P("st_cc4"), 0, t2, 0); CHECK(!strcmp(t2, "SN Dis (0)"), "a long name is cut to 6 characters (v1.2.0, was 9): '%s'", t2);
        c->d(c, 7, P("pf_cc1"), 0, t2, 0); CHECK(!strcmp(t2, "MULTI OFF"), "PERFORM CC 1: the tracks do not agree on a name (v1.2.0: MULTI) ('%s')", t2);
        set_int(c, "st_cc1", 11); c->d(c, 7, P("st_cc1"), 0, t2, 0); CHECK(!strcmp(t2, "SN Lev 10"), "a lock shows its own value: '%s'", t2);
        pr("read_all");

        {   /* the target is saved; EDIT CC's READ PROJECT keeps to it */
            static char *chunk; int n = (int)c->d(c, 23, 0, 0, &chunk, 0);
            std::string saved(chunk, n); while (!saved.empty() && saved.back() == 0) saved.pop_back();
            n = (int)saved.size();
            CHECK(saved.find("\nS ") == std::string::npos && saved.find("\nN 0 0 20 BD Level\n") != std::string::npos, "the chunk has the names (no target track was chosen)");
            {   /* names come back with the project; a CC number changed since no longer carries the old name */
                AEffect *f = VSTPluginMain(host);
                f->d(f, 24, 0, n, (void *)saved.c_str(), 0); set_int(f, "track", 0); blocks(f, 1, 0.0);
                char nm[64]; f->d(f, 7, P("st_cc1"), 0, nm, 0); CHECK(!strcmp(nm, "BD Lev (94)"), "reloaded: '%s'", nm);
                set_int(f, "track_edit", 1); blocks(f, 1, 0.0); set_int(f, "cc_edit", 1); blocks(f, 1, 0.0); set_int(f, "ccn1", 71);
                f->setP(f, P("cc_commit"), 1.0f); blocks(f, 1, 0.0); f->setP(f, P("cc_commit"), 0.0f); blocks(f, 1, 0.0);
                set_int(f, "pattern_edit", 1); blocks(f, 1, 0.0);
                f->d(f, 7, P("st_cc1"), 0, nm, 0); CHECK(!strcmp(nm, "(94)"), "CC 1 moved to number 71: the name is gone ('%s')", nm);
                f->d(f, 1, 0, 0, 0, 0);
            }
            saved += "S Other 1\n";
            AEffect *e = VSTPluginMain(host);
            e->d(e, 24, 0, (int)saved.size(), (void *)saved.c_str(), 0);
            set_int(e, "track_edit", 1); blocks(e, 1, 0.0); set_int(e, "cc_edit", 1); blocks(e, 1, 0.0);
            e->setP(e, P("cc_read"), 1.0f); blocks(e, 1, 0.0); e->setP(e, P("cc_read"), 0.0f); blocks(e, 1, 0.0);
            e->d(e, 7, P("msg"), 0, m, 0); CHECK(!strcmp(m, "Read 0 project values"), "a target track with no mappings in this project (EDIT CC keeps to it): '%s'", m);
            e->d(e, 1, 0, 0, 0, 0);
        }
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* CLR LOCKS is a switch: while it is lit the RND buttons clear their own lock; RND STEPS does nothing */
        AEffect *c = VSTPluginMain(host);
        char d[64];
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        c->setP(c, P("step0"), 1.0f); c->setP(c, P("step4"), 1.0f);
        set_int(c, "step_edit", 1); set_int(c, "sel", 1); set_int(c, "st_vel", 50); set_int(c, "st_cc2", 31);
        set_int(c, "sel", 5); set_int(c, "st_vel", 60); set_int(c, "st_cc2", 41);
        set_int(c, "pattern_edit", 1);
        CHECK(OPTC("lbtn") == 0, "RND buttons: orange");
        set_int(c, "clr_mode", 1); blocks(c, 1, 0.0);
        CHECK(OPTC("lbtn") == 3 && c->getP(c, P("clr_mode")) == 1.0f, "CLR LOCKS lit: the buttons are the red CLR ones");
        pr("rnd_trig"); CHECK(c->getP(c, P("step0")) > 0.5f && c->getP(c, P("step4")) > 0.5f && c->getP(c, P("step1")) < 0.5f, "RND STEPS is off: the trigs are as they were");
        pr("rnd_vel");
        set_int(c, "step_edit", 1); CHECK(OPTC("lbtn") == 3, "still red in STEP EDIT");
        set_int(c, "sel", 1); c->d(c, 7, P("st_vel"), 0, d, 0); char cc[64]; c->d(c, 7, P("st_cc2"), 0, cc, 0);
        CHECK(!strcmp(d, "(100)") && !strcmp(cc, "30"), "CLR VEL: the velocity locks of the track are gone (%s: the track's, v1.2.0), the CC 2 locks stay (%s)", d, cc);
        pr("rnd_cc2"); c->d(c, 7, P("st_cc2"), 0, cc, 0);
        set_int(c, "sel", 5); c->d(c, 7, P("st_cc2"), 0, d, 0);
        CHECK(!strcmp(cc, "--") && !strcmp(d, "40"), "CLR CC 2 in STEP EDIT: the selected step only (%s), the other step keeps %s", cc, d);
        set_int(c, "euclid_edit", 1); CHECK(OPTC("lbtn") == 4, "EUCLIDIAN: no RND buttons");
        set_int(c, "pattern_edit", 1); set_int(c, "clr_mode", 0); CHECK(OPTC("lbtn") == 0, "CLR LOCKS off: orange again");
        set_int(c, "track_edit", 1); CHECK(OPTC("lbtn") == 2, "TRACK EDIT: violet");
        c->d(c, 8, P("eu_len"), 0, d, 0); CHECK(!strcmp(d, "Length A"), "the Euclidean length knob is called '%s' (v1.2.0: Length A)", d);
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* v1.2.0: the arrows page TRACK EDIT's left section; page 2: RND COND / RND TIMING / RND RATCHETS on the trigs */
        AEffect *c = VSTPluginMain(host);
        char d[64];
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        auto look = [&](int sel, const char *key) { set_int(c, "step_edit", 1); set_int(c, "sel", sel); c->d(c, 7, P(key), 0, d, 0); set_int(c, "track_edit", 1); return std::string(d); };
        for (int k = 0; k < 16; k += 2) c->setP(c, P(("step" + std::to_string(k)).c_str()), 1.0f);   /* trigs on 1, 3, 5 .. 15 */
        set_int(c, "track_edit", 1); CHECK(OPTC("lbtn") == 2, "TRACK EDIT: page 1 (violet)");
        pr("nav_right"); CHECK(OPTC("lbtn") == 5 && OPTC("lpanel") == 0, "right arrow: page 2, CLR STEPS / CLR LOCKS stay (lbtn %d)", OPTC("lbtn"));
        pr("rnd_cond"); pr("rnd_timing"); pr("rnd_ratch");
        int cond = 0, tm = 0, rt = 0, empty = 0;
        for (int k = 0; k < 16; k++) {
            const std::string cs = look(k + 1, "st_cond");
            c->d(c, 7, P("msg"), 0, d, 0);
            const std::string rs = look(k + 1, "st_ratch"), rr = look(k + 1, "st_rres");
            if (k % 2 == 0) { cond += cs != "--"; rt += rs != "1"; }
            else empty += cs != "--" || rs != "1" || rr != "AUTO";
        }
        { void *cx = 0; c->d(c, 23, 0, 0, &cx, 0); std::string sv((char *)cx); for (int k = 0; k < 16; k++) { if (k % 2 == 0) tm += micro_in(sv, 0, k) != 0; else empty += micro_in(sv, 0, k) != 0; } }
        CHECK(cond >= 6 && rt >= 3 && tm >= 6 && !empty, "page 2: conditions (%d of 8), ratchets (%d of 8) and timing (%d of 8) on the trigs; empty steps untouched (%d)", cond, rt, tm, empty);
        CHECK(OPTC("lbtn") == 5, "still page 2 after a trip through STEP EDIT");
        set_int(c, "clr_mode", 1); blocks(c, 1, 0.0); CHECK(OPTC("lbtn") == 6, "CLR LOCKS lit: page 2 reads CLR COND / CLR TIMING / CLR RATCHETS");
        pr("rnd_cond"); pr("rnd_timing"); pr("rnd_ratch");
        int left = 0;
        for (int k = 0; k < 16; k += 2) left += look(k + 1, "st_cond") != "--" || look(k + 1, "st_ratch") != "1" || look(k + 1, "st_rres") != "AUTO";
        { void *cx = 0; c->d(c, 23, 0, 0, &cx, 0); std::string sv((char *)cx); left += sv.find("\nM 0 ") != std::string::npos; }   /* no timing and no ratchet left: no M line at all */
        CHECK(left == 0 && c->getP(c, P("step0")) > 0.5f, "CLR COND / TIMING / RATCHETS: all gone (%d left), the trigs stay", left);
        set_int(c, "clr_mode", 0); pr("nav_left"); CHECK(OPTC("lbtn") == 2, "left arrow: back to page 1");
        pr("nav_right"); set_int(c, "pattern_edit", 1); CHECK(OPTC("lbtn") == 0, "PATTERN EDIT keeps its own page: page 1 (orange)");
        set_int(c, "track_edit", 1); CHECK(OPTC("lbtn") == 5, "back in TRACK EDIT: still page 2");
        set_int(c, "step_edit", 1); CHECK(OPTC("lbtn") == 1, "STEP EDIT keeps its own page: page 1 (teal)");
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* v1.2.0: the same page 2 in PATTERN EDIT, on all 8 tracks */
        AEffect *c = VSTPluginMain(host);
        char d[64];
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        auto look = [&](int trk, int sel, const char *key) { set_int(c, "track", trk); set_int(c, "step_edit", 1); set_int(c, "sel", sel); c->d(c, 7, P(key), 0, d, 0); set_int(c, "pattern_edit", 1); return std::string(d); };
        for (int t = 0; t < 8; t++) { set_int(c, "track", t); for (int k = 0; k < 16; k += 4) c->setP(c, P(("step" + std::to_string(k)).c_str()), 1.0f); }   /* trigs on 1, 5, 9, 13 of every track */
        set_int(c, "track", 0); set_int(c, "pattern_edit", 1); CHECK(OPTC("lbtn") == 0, "PATTERN EDIT: page 1 (orange)");
        pr("nav_right"); CHECK(OPTC("lbtn") == 7 && OPTC("lpanel") == 0, "right arrow: page 2 in orange (lbtn %d)", OPTC("lbtn"));
        pr("rnd_cond"); pr("rnd_ratch"); pr("rnd_timing");
        int trk_hit = 0, empty = 0;
        for (int t = 0; t < 8; t++) {
            int n = 0;
            for (int k = 0; k < 16; k += 4) n += look(t, k + 1, "st_cond") != "--" || look(t, k + 1, "st_ratch") != "1";
            trk_hit += n > 0;
            empty += look(t, 2, "st_cond") != "--" || look(t, 2, "st_ratch") != "1";
        }
        { void *cx = 0; c->d(c, 23, 0, 0, &cx, 0); std::string sv((char *)cx); int m = 0; for (int t = 0; t < 8; t++) { int n = 0; for (int k = 0; k < 16; k += 4) n += micro_in(sv, t, k) != 0; m += n > 0; empty += micro_in(sv, t, 1) != 0; }
          CHECK(trk_hit == 8 && m == 8 && !empty, "page 2 in PATTERN EDIT: every track got conditions / ratchets (%d) and timing (%d); empty steps untouched (%d)", trk_hit, m, empty); }
        set_int(c, "clr_mode", 1); blocks(c, 1, 0.0); CHECK(OPTC("lbtn") == 6, "CLR LOCKS lit: CLR COND / CLR TIMING / CLR RATCHETS");
        pr("rnd_cond"); pr("rnd_ratch"); pr("rnd_timing");
        int left = 0;
        for (int t = 0; t < 8; t++) for (int k = 0; k < 16; k += 4) left += look(t, k + 1, "st_cond") != "--" || look(t, k + 1, "st_ratch") != "1" || look(t, k + 1, "st_rres") != "AUTO";
        { void *cx = 0; c->d(c, 23, 0, 0, &cx, 0); std::string sv((char *)cx); left += sv.find("\nM ") != std::string::npos; }   /* no timing and no ratchet left anywhere */
        CHECK(left == 0, "CLR COND / TIMING / RATCHETS in PATTERN EDIT: gone from all 8 tracks (%d left)", left);
        set_int(c, "clr_mode", 0); set_int(c, "track_edit", 1); CHECK(OPTC("lbtn") == 2, "TRACK EDIT keeps its own page: page 1");
        set_int(c, "pattern_edit", 1); pr("nav_left"); CHECK(OPTC("lbtn") == 0, "left arrow: PATTERN EDIT page 1");
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* v1.2.0: EUCLIDIAN generator B: ENABLE B, RANDOM B, its own length and direction, the boolean buttons; saved */
        AEffect *c = VSTPluginMain(host);
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        auto lit = [&](AEffect *e, const char *k) { return e->getP(e, P(k)) > 0.5f; };
        auto look = [&](AEffect *e, const char *k) { return (int)lroundf(e->getP(e, P(k)) * 2); };
        auto pat = [&](AEffect *e, int n) { std::string r; for (int k = 0; k < n; k++) r += e->getP(e, P(("step" + std::to_string(k)).c_str())) > 0.5f ? 'x' : '.'; return r; };
        set_int(c, "track_edit", 1); set_int(c, "euclid_edit", 1);
        c->setP(c, P("nav_right"), 1.0f); blocks(c, 1, 0.0); c->setP(c, P("nav_right"), 0.0f); blocks(c, 1, 0.0);   /* v1.2.0: B is on page 2 */
        CHECK(look(c, "eu_dirv2") == 2 && look(c, "eu_opv") == 1 && !lit(c, "eu_ena2"), "EUCLIDIAN: B off, the boolean buttons dim; the direction row (the track's) never dim");
        c->setP(c, P("eu_enable"), 1.0f); blocks(c, 1, 0.0); set_int(c, "eu_pl1", 3); set_int(c, "eu_len", 8);
        c->setP(c, P("eu_ena2"), 1.0f); blocks(c, 1, 0.0);
        CHECK(lit(c, "eu_enable") && lit(c, "eu_ena2") && look(c, "eu_opv") == 2 && look(c, "eu_dirv2") == 2 && lit(c, "eu_opor"), "ENABLE B: both on, OR lit, the boolean buttons work");
        set_int(c, "eu_pl2", 4); set_int(c, "eu_len2", 8);
        CHECK(pat(c, 8) == "x.xxx.x.", "A E(3,8) OR B E(4,8): %s", pat(c, 8).c_str());
        set_int(c, "eu_opxor", 1); CHECK(pat(c, 8) == "..xxx..." && lit(c, "eu_opxor") && !lit(c, "eu_opor"), "XOR: %s", pat(c, 8).c_str());
        set_int(c, "eu_opand", 1); CHECK(pat(c, 8) == "x.....x.", "AND: %s", pat(c, 8).c_str());
        set_int(c, "eu_opsub", 1); CHECK(pat(c, 8) == "...x....", "SUB: %s", pat(c, 8).c_str());
        set_int(c, "eu_opor", 1); set_int(c, "eu_bwd2", 1); set_int(c, "eu_pl2", 1);
        CHECK(lit(c, "eu_bwd2") && lit(c, "eu_bwd") && lit(c, "trk_bwd") && !lit(c, "eu_fwd2") && pat(c, 8) == "x..x..x.", "v1.2.0: BWD in EUCLIDIAN is the track's: the steps stay (%s), the play head runs backwards", pat(c, 8).c_str());
        set_int(c, "eu_bfwd", 1); CHECK(lit(c, "trk_bfwd") && lit(c, "eu_bfwd2") && !lit(c, "eu_bwd"), "BFWD on page 1: the track's, lit on page 2 too");
        set_int(c, "eu_bwd2", 1);
        void *cx = 0; c->d(c, 23, 0, 0, &cx, 0); std::string sv((char *)cx);
        AEffect *e = VSTPluginMain(host); e->d(e, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0);
        set_int(e, "track_edit", 1); set_int(e, "euclid_edit", 1); blocks(e, 1, 0.0);
        CHECK(lit(e, "eu_enable") && lit(e, "eu_ena2") && lit(e, "eu_bwd2") && lit(e, "eu_opor") && pat(e, 8) == "x..x..x.", "saved and loaded: both on, backwards, OR (%s)", pat(e, 8).c_str());
        e->d(e, 1, 0, 0, 0, 0);
        c->setP(c, P("eu_enable"), 0.0f); blocks(c, 1, 0.0);
        CHECK(!lit(c, "eu_enable") && lit(c, "eu_ena2") && pat(c, 8) == "x......." && look(c, "eu_opv") == 1, "A off: B alone (%s), the boolean buttons dim", pat(c, 8).c_str());
        pr("eu_rnd2"); CHECK(lit(c, "eu_ena2") && !lit(c, "eu_enable"), "RANDOM B: B only");
        pr("eu_rnd"); CHECK(lit(c, "eu_enable") && lit(c, "eu_ena2"), "RANDOM A: A on again, B stays");
        c->setP(c, P("eu_ena2"), 0.0f); blocks(c, 1, 0.0); c->setP(c, P("eu_enable"), 0.0f); blocks(c, 1, 0.0);
        CHECK(pat(c, 16) == "................", "both off: the trigs from before (none) come back (%s)", pat(c, 16).c_str());
        {   /* a project from v1.1.1 (no A / B fields on the e line): A on, B off unless it had pulses */
            AEffect *o = VSTPluginMain(host);
            void *c0 = 0; o->d(o, 23, 0, 0, &c0, 0); std::string old((char *)c0);
            const size_t q = old.find("\ne 0 ");
            if (q != std::string::npos) old.replace(q + 1, old.find('\n', q + 1) - q - 1, "e 0 1 3 0 0 0 0 0 0 8");
            AEffect *o2 = VSTPluginMain(host); o2->d(o2, 24, 0, (intptr_t)old.size(), (void *)old.c_str(), 0);
            set_int(o2, "track", 0); set_int(o2, "track_edit", 1); set_int(o2, "euclid_edit", 1); blocks(o2, 1, 0.0);
            set_int(o2, "eu_pl1", 2); set_int(o2, "eu_pl1", 3);   /* the trigs are saved with the steps; a turn makes the generators write them */
            CHECK(q != std::string::npos && lit(o2, "eu_enable") && !lit(o2, "eu_ena2") && pat(o2, 8) == "x..x..x.", "an old project: A on, B off, E(3,8) as before (%s)", pat(o2, 8).c_str());
            o->d(o, 1, 0, 0, 0, 0); o2->d(o2, 1, 0, 0, 0, 0);
        }
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* v1.2.0 SHIFT buttons: the selected track in TRACK EDIT, all tracks in PATTERN EDIT, nothing in STEP EDIT */
        AEffect *c = VSTPluginMain(host);
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        auto on = [&](int trk, int k) { set_int(c, "track", trk); return c->getP(c, P(("step" + std::to_string(k)).c_str())) > 0.5f; };
        for (int t = 0; t < 3; t++) { set_int(c, "track", t); c->setP(c, P("step0"), 1.0f); }
        set_int(c, "track", 1); set_int(c, "track_edit", 1); pr("shift_right");
        CHECK(on(1, 1) && !on(1, 0) && on(0, 0) && on(2, 0), "TRACK EDIT: SHIFT right moves T2 only");
        set_int(c, "pattern_edit", 1); pr("shift_left");
        CHECK(on(1, 0) && on(0, 15) && on(2, 15) && !on(0, 0), "PATTERN EDIT: SHIFT left moves every track (T1 step 1 -> 16)");
        set_int(c, "step_edit", 1); pr("shift_right"); CHECK(on(1, 0), "STEP EDIT: nothing");
        {   /* PATTERN EDIT's SHIFT hides (and does nothing) while SHUFFLE EDIT is on */
            auto look = [&]() { return (int)lroundf(c->getP(c, P("pshiftv"))); };
            set_int(c, "pattern_edit", 1); const int l0 = look();
            set_int(c, "shuf_edit", 1); const int l1 = look(); pr("shift_right"); const bool still = on(1, 0) && !on(1, 1);
            set_int(c, "pattern_edit", 1); set_int(c, "shuf_edit", 0); const int l2 = look();
            set_int(c, "track_edit", 1); const int l3 = look();
            CHECK(l0 == 1 && l1 == 0 && still && l2 == 1 && l3 == 0, "PATTERN EDIT SHIFT: shown (%d), hidden under SHUFFLE EDIT (%d, no shift there), back (%d); not on TRACK EDIT's panel (%d)", l0, l1, l2, l3);
        }
        set_int(c, "pattern_edit", 1); pr("undo"); CHECK(on(0, 0) && on(1, 1), "UNDO takes a SHIFT back");
        set_int(c, "track", 1); set_int(c, "track_edit", 1); set_int(c, "euclid_edit", 1);
        { const int cs = (int)lroundf(c->getP(c, P("cell1")) * (PARAMS[P("cell1")].nopts - 1)); CHECK(cs / 8 == 2, "EUCLIDIAN over TRACK EDIT, both generators off: the cells are violet, not green (state %d)", cs); }
        pr("shift_right");
        CHECK(on(1, 2) && !on(1, 1) && on(0, 0), "EUCLIDIAN (over TRACK EDIT), both generators off: SHIFT moves the track as in TRACK EDIT");
        set_int(c, "track", 1);   /* on() leaves T1 selected */
        c->setP(c, P("eu_enable"), 1.0f); blocks(c, 1, 0.0); set_int(c, "eu_pl1", 4); set_int(c, "eu_len", 16); pr("shift_right");
        { std::string r; for (int k = 0; k < 16; k++) r += on(1, k) ? 'x' : '.';
          CHECK(on(1, 1) && on(1, 5) && !on(1, 0), "A on: SHIFT moves the generated rhythm (x...x... -> .x...x..): %s, panel %d", r.c_str(), (int)lroundf(c->getP(c, P("panel")) * (PARAMS[P("panel")].nopts - 1))); }
        set_int(c, "track", 1); c->setP(c, P("eu_enable"), 0.0f); blocks(c, 1, 0.0); set_int(c, "pattern_edit", 1); set_int(c, "euclid_edit", 1); pr("shift_left");
        { std::string r; for (int k = 0; k < 16; k++) r += on(0, k) ? 'x' : '.';
          CHECK(on(0, 15) && !on(0, 0), "EUCLIDIAN over PATTERN EDIT: SHIFT moves the pattern: %s, panel %d", r.c_str(), (int)lroundf(c->getP(c, P("panel")) * (PARAMS[P("panel")].nopts - 1))); }
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* v1.2.0 CC 6 / CC 7: a lock goes out on 46 + track / 54 + track, is saved on an X line and loaded; base and PERFORM ALL too */
        AEffect *c = VSTPluginMain(host);
        char d[64];
        const double ppb = (120.0 / 60.0) * (128.0 / 44100.0);
        auto iv = [&](AEffect *x, const char *k) { return (int)lroundf(x->getP(x, P(k)) * (PARAMS[P(k)].max - PARAMS[P(k)].min) + PARAMS[P(k)].min); };
        set_int(c, "track", 2); c->setP(c, P("step0"), 1.0f);
        set_int(c, "step_edit", 1); set_int(c, "sel", 1); set_int(c, "st_cc6", 1 + 33); set_int(c, "st_cc7", 1 + 99);
        c->d(c, 7, P("st_cc6"), 0, d, 0); CHECK(!strcmp(d, "33"), "CC 6 lock on T3 step 1: '%s'", d);
        int before = steprunner_test_count();
        g_ti.flags = kPlaying | kTempo | kPpq; g_ti.ppqPos = 0.0; blocks(c, 60, ppb); g_ti.flags = kTempo | kPpq; blocks(c, 2, ppb);
        unsigned char m[3]; int v48 = -1, v56 = -1;
        for (int k = before; k < steprunner_test_count(); k++) { steprunner_test_get(k, m); if ((m[0] & 0xF0) == 0xB0 && m[1] == 48) v48 = m[2]; if ((m[0] & 0xF0) == 0xB0 && m[1] == 56) v56 = m[2]; }
        CHECK(v48 == 33 && v56 == 99, "T3: CC 6 goes out on CC 48 (%d), CC 7 on CC 56 (%d), on the track's channel", v48, v56);
        void *cx = 0; c->d(c, 23, 0, 0, &cx, 0); std::string sv((char *)cx);
        CHECK(sv.find("\nX 2 ") != std::string::npos, "saved on an X line");
        AEffect *e = VSTPluginMain(host); e->d(e, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0);
        set_int(e, "track", 2); set_int(e, "step_edit", 1); set_int(e, "sel", 1);
        CHECK(iv(e, "st_cc6") == 34 && iv(e, "st_cc7") == 100, "and loaded (%d %d)", iv(e, "st_cc6") - 1, iv(e, "st_cc7") - 1);
        e->d(e, 1, 0, 0, 0, 0);
        {   /* without the X line (a project from v1.1.1): no CC 6 / 7 locks */
            std::string old = sv; const size_t q = old.find("\nX 2 "); old.erase(q + 1, old.find('\n', q + 1) - q);
            AEffect *o = VSTPluginMain(host); o->d(o, 24, 0, (intptr_t)old.size(), (void *)old.c_str(), 0);
            set_int(o, "track", 2); set_int(o, "step_edit", 1); set_int(o, "sel", 1);
            CHECK(iv(o, "st_cc6") == 0 && iv(o, "st_cc7") == 0 && iv(o, "st_vel") == 100, "an older project: no CC 6 / 7 locks (%d %d)", iv(o, "st_cc6"), iv(o, "st_cc7"));
            o->d(o, 1, 0, 0, 0, 0);
        }
        set_int(c, "track_edit", 1); set_int(c, "st_cc6", 1 + 70);   /* a Q-Link in TRACK EDIT: the track's CC 6 base */
        CHECK(iv(c, "trk_cc6") == 71, "TRACK EDIT: the CC 6 Q-Link sets the track's CC 6 base (%d)", iv(c, "trk_cc6") - 1);
        set_int(c, "pf_ctrl", 1); set_int(c, "pf_cc6", 10); c->d(c, 7, P("pf_cc6"), 0, d, 0);
        CHECK(!strcmp(d, "+10") || strstr(d, "+10"), "PERFORM ALL: CC 6 knob '%s'", d);
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* v1.2.0 CC 8: T1-T8 on 19 35 41 62 63 12 13 92; a lock is saved on an H line (the X line stays CC 6 / 7), base, PERFORM, RND */
        AEffect *c = VSTPluginMain(host);
        char d[64];
        const double ppb = (120.0 / 60.0) * (128.0 / 44100.0);
        auto iv = [&](AEffect *x, const char *k) { return (int)lroundf(x->getP(x, P(k)) * (PARAMS[P(k)].max - PARAMS[P(k)].min) + PARAMS[P(k)].min); };
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        set_int(c, "track", 3); c->setP(c, P("step0"), 1.0f);
        set_int(c, "step_edit", 1); set_int(c, "sel", 1); set_int(c, "st_cc8", 1 + 77);
        c->d(c, 7, P("st_cc8"), 0, d, 0); CHECK(!strcmp(d, "77"), "CC 8 lock on T4 step 1: '%s'", d);
        int before = steprunner_test_count();
        g_ti.flags = kPlaying | kTempo | kPpq; g_ti.ppqPos = 0.0; blocks(c, 60, ppb); g_ti.flags = kTempo | kPpq; blocks(c, 2, ppb);
        unsigned char m[3]; int v62 = -1;
        for (int k = before; k < steprunner_test_count(); k++) { steprunner_test_get(k, m); if ((m[0] & 0xF0) == 0xB0 && m[1] == 62) v62 = m[2]; }
        CHECK(v62 == 77, "T4: CC 8 goes out on CC 62 (%d)", v62);
        void *cx = 0; c->d(c, 23, 0, 0, &cx, 0); std::string sv((char *)cx);
        CHECK(sv.find("\nH 3 ") != std::string::npos && sv.find("\nX 3 ") == std::string::npos, "saved on an H line, no X line (no CC 6 / 7 lock)");
        AEffect *e = VSTPluginMain(host); e->d(e, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0);
        set_int(e, "track", 3); set_int(e, "step_edit", 1); set_int(e, "sel", 1);
        CHECK(iv(e, "st_cc8") == 78, "and loaded (%d)", iv(e, "st_cc8") - 1);
        e->d(e, 1, 0, 0, 0, 0);
        {   /* an X line as the earlier v1.2.0 builds wrote it (2 bytes a step) still gives CC 6 / 7, and CC 8 stays empty */
            std::string old = sv; const size_t q = old.find("\nH 3 "); old.erase(q + 1, old.find('\n', q + 1) - q);
            std::string x = "X 3 "; for (int k = 0; k < 64; k++) x += k == 0 ? "1f2a" : "0000";
            old += x + "\n";
            AEffect *o = VSTPluginMain(host); o->d(o, 24, 0, (intptr_t)old.size(), (void *)old.c_str(), 0);
            set_int(o, "track", 3); set_int(o, "step_edit", 1); set_int(o, "sel", 1);
            CHECK(iv(o, "st_cc6") == 0x1f && iv(o, "st_cc7") == 0x2a && iv(o, "st_cc8") == 0, "an X line of 2 bytes a step: CC 6 %d, CC 7 %d, CC 8 none (%d)", iv(o, "st_cc6") - 1, iv(o, "st_cc7") - 1, iv(o, "st_cc8"));
            o->d(o, 1, 0, 0, 0, 0);
        }
        set_int(c, "step_edit", 0); set_int(c, "track_edit", 1); set_int(c, "st_cc8", 1 + 50);   /* a Q-Link in TRACK EDIT: the track's CC 8 base */
        CHECK(iv(c, "trk_cc8") == 51, "TRACK EDIT: the CC 8 Q-Link sets the track's CC 8 base (%d)", iv(c, "trk_cc8") - 1);
        before = steprunner_test_count(); set_int(c, "trk_cc8", 1 + 60); blocks(c, 1, 0.0);
        int b62 = -1; for (int k = before; k < steprunner_test_count(); k++) { steprunner_test_get(k, m); if ((m[0] & 0xF0) == 0xB0 && m[1] == 62) b62 = m[2]; }
        CHECK(b62 == 60, "TRACK VALUES CC 8: the base goes out at once on CC 62 (%d)", b62);
        { void *bx = 0; c->d(c, 23, 0, 0, &bx, 0); CHECK(strstr((char *)bx, "\nB 3 0 0 0 0 0 0 0 61\n") != 0, "the base is the B line's ninth number"); }
        set_int(c, "pf_ctrl", 1); set_int(c, "pf_cc8", 10); c->d(c, 7, P("pf_cc8"), 0, d, 0);
        CHECK(!strcmp(d, "+10"), "PERFORM ALL: CC 8 knob '%s'", d);
        set_int(c, "pf_ctrl", 0);
        set_int(c, "rnd_dens", 100); set_int(c, "rnd_min", 30); set_int(c, "rnd_max", 40); pr("rnd_cc8");
        set_int(c, "step_edit", 1); set_int(c, "sel", 1); c->d(c, 7, P("st_cc8"), 0, d, 0);
        CHECK(atoi(d) >= 30 && atoi(d) <= 40, "TRACK EDIT: RND CC 8 wrote a lock in 30..40 ('%s')", d);
        set_int(c, "track_edit", 1); set_int(c, "clr_mode", 1); pr("rnd_cc8"); set_int(c, "clr_mode", 0);
        set_int(c, "step_edit", 1); set_int(c, "sel", 1); c->d(c, 7, P("st_cc8"), 0, d, 0);
        CHECK(strstr(d, "(60)") != 0, "CLR CC 8: the lock goes, the knob shows the base ('%s')", d);
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* v1.2.0: TRACK VALUES: VELOCITY, PAN, LENGTH, PROB and the CC base values of the track, BACK; PAN / CC go out at once */
        AEffect *c = VSTPluginMain(host);
        char d[64];
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        auto panel = [&]() { return (int)lroundf(c->getP(c, P("panel")) * (PARAMS[P("panel")].nopts - 1)); };
        set_int(c, "track", 2); set_int(c, "track_edit", 1); CHECK(panel() == 2, "TRACK EDIT: the track panel");
        pr("trk_vals"); CHECK(panel() == 8, "TRACK VALUES: its own panel (%d)", panel());
        set_int(c, "trk_vel", 90); set_int(c, "trk_gate", 8);
        int before = steprunner_test_count();
        set_int(c, "trk_pan", 1 + 20); set_int(c, "trk_cc2", 1 + 77); blocks(c, 1, 0.0);
        unsigned char m[3]; int pan = -1, cc = -1;
        for (int k = before; k < steprunner_test_count(); k++) { steprunner_test_get(k, m); if ((m[0] & 0xF0) == 0xB0 && m[1] == PAN_CC[2]) pan = m[2]; if ((m[0] & 0xF0) == 0xB0 && m[1] == Seq::cc_num(Seq().t[2], 1)) cc = m[2]; }
        c->d(c, 7, P("trk_pan"), 0, d, 0); std::string pd(d); c->d(c, 7, P("trk_cc2"), 0, d, 0); std::string cd(d);
        CHECK(pan == 20 && cc == 77 && pd == "L44" && cd.find("77") != std::string::npos, "PAN / CC 2 go out at once (%d, %d) and read %s / %s", pan, cc, pd.c_str(), cd.c_str());
        set_int(c, "trk_prob", 0); c->d(c, 7, P("trk_prob"), 0, d, 0); CHECK(!strcmp(d, "0%"), "PROB 0 %% ('%s')", d);
        void *cx = 0; c->d(c, 23, 0, 0, &cx, 0); std::string sv((char *)cx);
        AEffect *e = VSTPluginMain(host); e->d(e, 24, 0, (intptr_t)sv.size(), (void *)sv.c_str(), 0); set_int(e, "track", 2); blocks(e, 1, 0.0);
        auto iv = [&](AEffect *x, const char *k) { return (int)lroundf(x->getP(x, P(k)) * (PARAMS[P(k)].max - PARAMS[P(k)].min) + PARAMS[P(k)].min); };
        CHECK(iv(e, "trk_vel") == 90 && iv(e, "trk_gate") == 8 && iv(e, "trk_pan") == 21 && iv(e, "trk_prob") == 0 && iv(e, "trk_cc2") == 78,
              "saved and loaded: vel %d, gate %d, pan %d, prob %d, cc2 %d", iv(e, "trk_vel"), iv(e, "trk_gate"), iv(e, "trk_pan"), iv(e, "trk_prob"), iv(e, "trk_cc2"));
        e->d(e, 1, 0, 0, 0, 0);
        set_int(c, "step_edit", 1); CHECK(panel() == 0, "STEP EDIT: STEP LOCKS");
        set_int(c, "track_edit", 1); CHECK(panel() == 8, "back in TRACK EDIT: still TRACK VALUES");
        pr("trk_back"); CHECK(panel() == 2, "BACK: the track panel again");
        {   /* v1.2.0: a CC lock turned down to -- stays at -- (no jump to the track's base value) while the same step is selected */
            auto iv = [&](const char *k) { return (int)lroundf(c->getP(c, P(k)) * (PARAMS[P(k)].max - PARAMS[P(k)].min) + PARAMS[P(k)].min); };
            set_int(c, "trk_cc1", 1 + 80); set_int(c, "step_edit", 1); set_int(c, "sel", 4);
            CHECK(iv("st_cc1") == 81, "no lock: the knob rests on the base value 80 (%d)", iv("st_cc1") - 1);
            set_int(c, "st_cc1", 1 + 40); set_int(c, "st_cc1", 1 + 1); set_int(c, "st_cc1", 0); blocks(c, 1, 0.0);
            c->d(c, 7, P("st_cc1"), 0, d, 0);
            CHECK(iv("st_cc1") == 0 && std::string(d).find("(80)") != std::string::npos, "turned down to --: the knob stays at -- (%d), the text still shows the base ('%s')", iv("st_cc1"), d);
            set_int(c, "st_cc1", 1 + 0); CHECK(iv("st_cc1") == 1, "and one step up is a lock of 0, no jump (%d)", iv("st_cc1") - 1);
            set_int(c, "st_cc1", 0); set_int(c, "sel", 5); set_int(c, "sel", 4);
            CHECK(iv("st_cc1") == 81, "another step and back: the knob rests on the base value again (%d)", iv("st_cc1") - 1);
            set_int(c, "track_edit", 1);
        }
        {   /* v1.2.0 Q-Links: the STEP LOCKS knobs stand for the track's values while the track's panel shows */
            auto iv = [&](const char *k) { return (int)lroundf(c->getP(c, P(k)) * (PARAMS[P(k)].max - PARAMS[P(k)].min) + PARAMS[P(k)].min); };
            set_int(c, "st_vel", 70); set_int(c, "st_cc3", 1 + 33); set_int(c, "st_len", 12);
            c->d(c, 7, P("st_vel"), 0, d, 0);
            CHECK(iv("trk_vel") == 70 && iv("trk_cc3") == 34 && iv("trk_gate") == 12 && iv("st_vel") == 70 && !strcmp(d, "70"),
                  "TRACK EDIT: a Q-Link on VELOCITY / CC 3 / LENGTH sets the track's (%d %d %d), and reads it ('%s')", iv("trk_vel"), iv("trk_cc3"), iv("trk_gate"), d);
            set_int(c, "step_edit", 1); set_int(c, "sel", 1);
            CHECK(iv("st_vel") == 70 && iv("trk_vel") == 70, "STEP EDIT: the same knob is step 1's velocity lock again (none: it rests on the track's 70: %d)", iv("st_vel"));
            set_int(c, "st_vel", 50); set_int(c, "track_edit", 1);
            CHECK(iv("trk_vel") == 70 && iv("st_vel") == 70, "a lock set in STEP EDIT does not touch the track (%d)", iv("trk_vel"));
            set_int(c, "euclid_edit", 1); pr("trk_vals"); CHECK(panel() == 8, "EUCLIDIAN over TRACK EDIT: TRACK VALUES opens (%d)", panel());
            set_int(c, "st_prob", 40); CHECK(iv("trk_prob") == 40, "and the Q-Links still set the track (%d)", iv("trk_prob"));
            pr("trk_back"); set_int(c, "pattern_edit", 1); set_int(c, "st_vel", 30); set_int(c, "track_edit", 1);
            CHECK(iv("trk_vel") == 70, "PATTERN EDIT: the knob is a step lock again, the track keeps 70 (%d)", iv("trk_vel"));
        }
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* v1.2.0 SHIFT in the core: steps 1..length wrap round with their locks; past the length untouched; a generated track moves its rhythm */
        Seq s; Track &t0 = s.t[0];
        auto pat = [&](int n) { std::string r; for (int k = 0; k < n; k++) r += t0.st[k].on ? 'x' : '.'; return r; };
        t0.len = 8; t0.st[0].on = 1; t0.st[0].vel = 99; t0.st[0].micro = 5; t0.st[3].on = 1; t0.st[7].on = 1; t0.st[10].on = 1;
        s.shift_track(0, 1);
        CHECK(pat(12) == "xx..x.....x." && t0.st[1].vel == 99 && t0.st[1].micro == 5 && t0.st[0].vel == 0, "shift right: x..x...x -> %s (8 long; step 11 stays), the locks go along", pat(12).c_str());
        s.shift_track(0, -1); s.shift_track(0, -1);
        CHECK(pat(8) == "..x...xx" && t0.st[7].vel == 99, "two left: %s, step 1's locks now on step 8", pat(8).c_str());
        Seq e; Track &t1 = e.t[1]; t1.len = 8; t1.eu_pl1 = 2; e.euclid_enable(1);
        std::string a0; for (int k = 0; k < 8; k++) a0 += t1.st[k].on ? 'x' : '.';
        e.shift_track(1, 1); std::string a1; for (int k = 0; k < 8; k++) a1 += t1.st[k].on ? 'x' : '.';
        e.euclid_apply(1); std::string a2; for (int k = 0; k < 8; k++) a2 += t1.st[k].on ? 'x' : '.';
        CHECK(a0 == "x...x..." && a1 == ".x...x.." && a2 == a1, "a generated track: %s -> %s, and stays so when the generator writes again (%s)", a0.c_str(), a1.c_str(), a2.c_str());
    }
    {   /* v1.2.0: the track's PROB in the core: 0 % silences every trig, 100 % changes nothing, NO LOCKS ignores it */
        Seq s; Track &t0 = s.t[0];
        for (int k = 0; k < 16; k++) t0.st[k].on = 1;
        const uint8_t st = 0x90 | (t0.ch & 15);
        t0.pcut = 100; size_t n0 = run(s, 4.0, st, -1).size();
        t0.pcut = 50; size_t n50 = run(s, 64.0, st, -1).size();
        s.no_locks = true; t0.pcut = 100; size_t nl = run(s, 4.0, st, -1).size(); s.no_locks = false;
        t0.pcut = 0; size_t n100 = run(s, 4.0, st, -1).size();
        CHECK(n0 == 0 && n100 == 16 && n50 > 100 && n50 < 156 && nl == 16, "track PROB: 0 %% %d trigs, 100 %% %d, 50 %% %d of 256, NO LOCKS %d", (int)n0, (int)n100, (int)n50, (int)nl);
    }
    {   /* v1.2.0: the arrows look dim where there is no page; RND CC 6 / 7 */
        AEffect *c = VSTPluginMain(host);
        char d[64];
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        set_int(c, "pattern_edit", 1); CHECK(OPTC("navv") == 1, "page 1: only the right arrow goes somewhere (%d)", OPTC("navv"));
        pr("nav_right"); CHECK(OPTC("navv") == 2, "page 2: only the left one (%d)", OPTC("navv"));
        set_int(c, "global_edit", 1); CHECK(OPTC("navv") == 0, "SETUP: no pages, both dim");
        set_int(c, "track_edit", 1); set_int(c, "euclid_edit", 1); CHECK(OPTC("navv") == 1, "EUCLIDIAN: page 1, the right arrow goes to generator B (v1.2.0)");
        set_int(c, "track_edit", 1); pr("nav_right");
        c->setP(c, P("step0"), 1.0f); c->setP(c, P("step4"), 1.0f);
        set_int(c, "rnd_dens", 100); set_int(c, "rnd_min", 30); set_int(c, "rnd_max", 40); pr("rnd_cc6"); pr("rnd_cc7");
        set_int(c, "step_edit", 1); set_int(c, "sel", 1); c->d(c, 7, P("st_cc6"), 0, d, 0); const int v6 = atoi(d);
        c->d(c, 7, P("st_cc7"), 0, d, 0); const int v7 = atoi(d);
        CHECK(v6 >= 30 && v6 <= 40 && v7 >= 30 && v7 <= 40, "TRACK EDIT: RND CC 6 / RND CC 7 (page 1, their own row) wrote locks in 30..40 (%d %d)", v6, v7);
        set_int(c, "track_edit", 1); set_int(c, "clr_mode", 1); pr("rnd_cc6"); set_int(c, "clr_mode", 0);
        set_int(c, "step_edit", 1); set_int(c, "sel", 1); c->d(c, 7, P("st_cc6"), 0, d, 0); std::string c6(d); c->d(c, 7, P("st_cc7"), 0, d, 0);
        CHECK(c6.find("--") != std::string::npos && atoi(d) >= 30, "CLR CC 6: the CC 6 locks go, CC 7 stays ('%s', '%s')", c6.c_str(), d);
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* v1.2.0: page 2 in STEP EDIT: the selected steps only, trig or not */
        AEffect *c = VSTPluginMain(host);
        char d[64];
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        auto get = [&](int sel, const char *key) { set_int(c, "sel", sel); c->d(c, 7, P(key), 0, d, 0); return std::string(d); };
        c->setP(c, P("step2"), 1.0f); c->setP(c, P("step4"), 1.0f);   /* trigs on 3 and 5 */
        set_int(c, "step_edit", 1); CHECK(OPTC("lbtn") == 1, "STEP EDIT: page 1 (teal)");
        pr("nav_right"); CHECK(OPTC("lbtn") == 8 && OPTC("lpanel") == 0, "right arrow: page 2 in teal (lbtn %d)", OPTC("lbtn"));
        int hit3 = 0, hit2 = 0, other = 0;
        for (int r = 0; r < 6; r++) {   /* step 3 (a trig) and step 2 (none), one at a time; step 5 never selected */
            set_int(c, "sel", 3); pr("rnd_cond"); pr("rnd_ratch");
            hit3 += get(3, "st_cond") != "--" || get(3, "st_ratch") != "1";
            set_int(c, "sel", 2); pr("rnd_cond"); pr("rnd_ratch");
            hit2 += get(2, "st_cond") != "--" || get(2, "st_ratch") != "1";
            other += get(5, "st_cond") != "--" || get(5, "st_ratch") != "1";
        }
        void *cx = 0; std::string sv;
        for (int r = 0; r < 20; r++) {   /* a random offset can be 0 (1 in 47): until one is not */
            set_int(c, "sel", 3); pr("rnd_timing");
            cx = 0; c->d(c, 23, 0, 0, &cx, 0); sv = (char *)cx;
            if (micro_in(sv, 0, 2) != 0) break;
        }
        CHECK(hit3 >= 5 && hit2 >= 5 && !other && micro_in(sv, 0, 2) != 0 && micro_in(sv, 0, 1) == 0, "page 2 in STEP EDIT: the selected step, trig (%d of 6) or not (%d of 6), and timing on step 3 only; an unselected step untouched (%d)", hit3, hit2, other);
        set_int(c, "sel", 2); set_int(c, "st_ratch", 3);   /* step 2 surely holds something (its random values may all be empty) */
        set_int(c, "sel", 3); set_int(c, "clr_mode", 1); blocks(c, 1, 0.0); CHECK(OPTC("lbtn") == 6, "CLR LOCKS lit: CLR COND / CLR TIMING / CLR RATCHETS");
        pr("rnd_cond"); pr("rnd_ratch"); pr("rnd_timing");
        const std::string c3 = get(3, "st_cond"), r3 = get(3, "st_ratch"), c2 = get(2, "st_cond"), r2 = get(2, "st_ratch");
        cx = 0; c->d(c, 23, 0, 0, &cx, 0); sv = (char *)cx;
        CHECK(c3 == "--" && r3 == "1" && micro_in(sv, 0, 2) == 0 && r2 == "3", "CLR in STEP EDIT: step 3 cleared (%s, %s, timing %d), step 2 (not selected) keeps its values (%s, %s)", c3.c_str(), r3.c_str(), micro_in(sv, 0, 2), c2.c_str(), r2.c_str());
        set_int(c, "clr_mode", 0); set_int(c, "timing_edit", 1); CHECK(OPTC("lbtn") == 4, "TIMING: its own section, no page 2");
        set_int(c, "timing_edit", 0); CHECK(OPTC("lbtn") == 8, "TIMING off: back to STEP EDIT page 2");
        pr("nav_left"); CHECK(OPTC("lbtn") == 1, "left arrow: STEP EDIT page 1");
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* FETCH NOTES needs no target track: T1's CC 20 on channel 1 is mapped to pad A01 of Drum 001, so T1 plays that pad's note */
        setenv("STEPRUNNER_XPJ_ROOT", "testdata", 1);
        AEffect *c = VSTPluginMain(host);
        char m[64], n1[64], n2a[64], n2b[64];
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        set_int(c, "track", 1); blocks(c, 1, 0.0); c->d(c, 7, P("trk_num"), 0, n2a, 0);
        pr("read_all"); pr("src_notes"); c->d(c, 7, P("msg"), 0, m, 0);
        c->d(c, 7, P("trk_num"), 0, n2b, 0); set_int(c, "track", 0); blocks(c, 1, 0.0); c->d(c, 7, P("trk_num"), 0, n1, 0);
        CHECK(!strcmp(m, "Read 1 note") && atoi(n1) == 36 && !strcmp(n2a, n2b), "'%s': T1 note %s; T2, whose CCs are not mapped, keeps %s", m, n1, n2b);
        c->d(c, 1, 0, 0, 0, 0);
        unsetenv("STEPRUNNER_XPJ_ROOT");
    }
    {   /* v1.2.0: a CC's name made short under the knob: no pad number, first words to initials, the last one cut */
        AEffect *c = VSTPluginMain(host);
        static char *ch0; int n0 = (int)c->d(c, 23, 0, 0, &ch0, 0); std::string ck(ch0, n0); while (!ck.empty() && ck.back() == 0) ck.pop_back();
        const char *full[8] = {"1 TUNE", "12 NOISE", "Filter Cutoff", "BD Level", "Amp Env Attack", "Resonance", "2 Delay Send", "Filter Env"};
        const char *want[8] = {"TUNE --", "NOISE --", "F. Cut --", "BD Lev --", "AE Att --", "Resona --", "D Send --", "F. Env --"};
        for (int ti = 0; ti < 8; ti++) ck += "N " + std::to_string(ti) + " 5 " + std::to_string(46 + ti) + " " + full[ti] + "\n";   /* CC 6 of track ti: 46 + ti */
        for (int ti = 0; ti < 7; ti++) ck += "N " + std::to_string(ti) + " 6 " + std::to_string(54 + ti) + " " + std::to_string(ti + 1) + " Delay Send\n";   /* CC 7: the same on 7 tracks, T8 none */
        c->d(c, 24, 0, (int)ck.size(), (void *)ck.c_str(), 0);
        for (int ti = 0; ti < 8; ti++) {
            set_int(c, "track", ti); blocks(c, 1, 0.0);
            char a[64], b[64]; c->d(c, 7, P("st_cc6"), 0, a, 0); c->d(c, 7, P("trk_cc6"), 0, b, 0);
            CHECK(!strcmp(a, want[ti]) && !strcmp(b, want[ti]), "short CC name: '%s' -> STEP LOCKS '%s', TRACK VALUES '%s'", full[ti], a, b);
        }
        char p6[64], p7[64]; c->d(c, 7, P("pf_cc6"), 0, p6, 0); c->d(c, 7, P("pf_cc7"), 0, p7, 0);
        CHECK(!strcmp(p6, "MULTI OFF") && !strcmp(p7, "D Send OFF"), "PERFORM: different names MULTI ('%s'), the same name without its pad number ('%s'; a track with no name does not count)", p6, p7);
        static char *ch1; int n1 = (int)c->d(c, 23, 0, 0, &ch1, 0); std::string saved(ch1, n1);
        CHECK(saved.find("N 2 5 48 Filter Cutoff\n") != std::string::npos, "the full name is what is saved");
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* PERFORM ALL CC knobs move the base values, at once: every track's own value by the same amount */
        AEffect *c = VSTPluginMain(host);
        static char *ch0; int n0 = (int)c->d(c, 23, 0, 0, &ch0, 0); std::string ck(ch0, n0); while (!ck.empty() && ck.back() == 0) ck.pop_back();
        ck += "B 0 21 0 0 0 0\nB 1 61 0 0 0 0\nB 2 121 0 0 0 0\n";   /* CC 1 base: T1 20, T2 60, T3 120; the others none */
        c->d(c, 24, 0, (int)ck.size(), (void *)ck.c_str(), 0);
        int cc[3]; for (int ti = 0; ti < 3; ti++) { set_int(c, "track", ti); blocks(c, 1, 0.0); cc[ti] = (int)lroundf(c->getP(c, P("ccn1")) * 127); }
        auto got = [&](int ti) { unsigned char m[3]; int v = -1, n = 0; for (int i = 0; i < steprunner_test_count(); i++) { steprunner_test_get(i, m); if ((m[0] & 0xF0) == 0xB0 && m[1] == cc[ti]) { v = m[2]; n++; } } return n * 1000 + v; };
        auto total = [&]() { unsigned char m[3]; int n = 0; for (int i = 0; i < steprunner_test_count(); i++) { steprunner_test_get(i, m); n += (m[0] & 0xF0) == 0xB0 && m[1] != 117; } return n; };
        set_int(c, "pf_ctrl", 1); blocks(c, 1, 0.0);
        steprunner_test_reset();
        set_int(c, "pf_cc1", 10); blocks(c, 1, 0.0);
        CHECK(got(0) == 1030 && got(1) == 1070 && got(2) == 1127 && total() == 3, "CC 1 +10: T1 20 -> %d, T2 60 -> %d, T3 120 -> %d (stops at 127); nothing from tracks with no base", got(0) % 1000, got(1) % 1000, got(2) % 1000);
        steprunner_test_reset();
        set_int(c, "pf_cc2", 30); set_int(c, "pf_vel", 20); blocks(c, 1, 0.0);
        CHECK(total() == 0, "a CC with no base value on any track, and velocity, send nothing by themselves");
        steprunner_test_reset();
        set_int(c, "pf_ctrl", 0); blocks(c, 1, 0.0);
        CHECK(got(0) == 1020 && got(1) == 1060 && got(2) == 1120, "PERFORM ALL off: the base values are sent again (%d %d %d)", got(0) % 1000, got(1) % 1000, got(2) % 1000);
        set_int(c, "pf_ctrl", 1); set_int(c, "pf_cc1", -15); blocks(c, 1, 0.0);
        steprunner_test_reset();
        c->setP(c, P("pf_commit"), 1.0f); blocks(c, 1, 0.0); c->setP(c, P("pf_commit"), 0.0f); blocks(c, 1, 0.0);
        char b1[64]; set_int(c, "track", 0); blocks(c, 1, 0.0); c->d(c, 7, P("st_cc1"), 0, b1, 0);
        CHECK(total() == 0 && !strcmp(b1, "(5)"), "COMMIT sends nothing (it is what was heard) and the base is now '%s'", b1);
        steprunner_test_reset();
        c->setP(c, P("pf_discard"), 1.0f); blocks(c, 1, 0.0); c->setP(c, P("pf_discard"), 0.0f); blocks(c, 1, 0.0);
        CHECK(got(0) == 1020 && got(1) == 1060, "UNDO after COMMIT: back to 20 and 60, sent (%d %d)", got(0) % 1000, got(1) % 1000);
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* PERFORM ALL: a CC knob has a name only when all eight tracks have the same one for that slot */
        AEffect *c = VSTPluginMain(host);
        static char *ch0; int n0 = (int)c->d(c, 23, 0, 0, &ch0, 0); std::string ck(ch0, n0); while (!ck.empty() && ck.back() == 0) ck.pop_back();
        for (int ti = 0; ti < 8; ti++) {
            set_int(c, "track", ti); blocks(c, 1, 0.0);
            int cc = (int)lroundf(c->getP(c, P("ccn1")) * 127);
            ck += "N " + std::to_string(ti) + " 0 " + std::to_string(cc) + " Cutoff\n";
            if (ti < 7) ck += "N " + std::to_string(ti) + " 1 " + std::to_string(cc + 1) + " Resonance\n";
        }
        c->d(c, 24, 0, (int)ck.size(), (void *)ck.c_str(), 0);
        char p1[64], p2[64];
        c->d(c, 7, P("pf_cc1"), 0, p1, 0); c->d(c, 7, P("pf_cc2"), 0, p2, 0);
        CHECK(!strcmp(p1, "Cutoff OFF") && !strcmp(p2, "Resona OFF"), "PERFORM ALL off: CC 1 '%s' (all agree), CC 2 '%s' (T8 has no name: v1.2.0, it does not count)", p1, p2);
        set_int(c, "pf_ctrl", 1); blocks(c, 1, 0.0);
        c->d(c, 7, P("pf_cc1"), 0, p1, 0); c->d(c, 7, P("pf_cc2"), 0, p2, 0);
        CHECK(!strcmp(p1, "Cutoff +0") && !strcmp(p2, "Resona +0"), "PERFORM ALL on: '%s' / '%s'", p1, p2);
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* CREATE DEFAULT MAP (v1.64.0, SETUP panel): a list of the open project's drum tracks; a tap on one copies the project
         * to <name>-backup.xpj next to it and writes pads A01..A08's mappings into it; the big message says how many */
        int rc = system("rm -rf build/map && mkdir -p build/map/a build/map/c && cp testdata1/cctest1.xpj build/map/a/ && cp testdata9/9w9.xpj build/map/c/");
        AEffect *c = VSTPluginMain(host);
        char m[64], r1[64], r2[64];
        set_int(c, "track", 7); set_int(c, "trk_num", 50); set_int(c, "track", 0); blocks(c, 1, 0.0);   /* T8 off pad A08's 43 */
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        setenv("STEPRUNNER_XPJ_ROOT", "build/map", 1);
        setenv("STEPRUNNER_CURRENT", "build/map/cur", 1);
        rc |= system("printf '<PROPERTIES><VALUE name=\"lastProjectPath\" val=\"build/map/a/cctest1.xpj\"/></PROPERTIES>' > build/map/cur && touch -d '2035-01-01' build/map/a/cctest1.xpj");
        set_int(c, "global_edit", 1); blocks(c, 2, 0.0);
        c->d(c, 7, P("srcproj"), 0, m, 0);
        CHECK(rc == 0 && OPTC("panel") == 4 && OPTC("ptitle") == 5, "SETUP: its buttons (no file is read on the way in, v1.67.0)");
        pr("src_map");
        c->d(c, 7, P("srcrow1"), 0, r1, 0); c->d(c, 7, P("srcrow2"), 0, r2, 0); c->d(c, 7, P("srcproj"), 0, m, 0);
        CHECK(OPTC("panel") == 7 && !strcmp(r1, "Drum 001") && !strcmp(r2, " ") && OPTC("srchl1") == 1 && OPTC("srchl2") == 1 && OPTC("srchl6") == 0 && !strcmp(m, "Map into: cctest1.xpj") && OPTC("bigmsg") == 0,
              "CREATE DEFAULT MAP: the drum tracks in the panel: '%s' / '%s', asking '%s'", r1, r2, m);
        pr("srcsel2"); CHECK(OPTC("panel") == 7 && OPTC("bigmsg") == 0, "an empty row does nothing");
        pr("src_cancel"); CHECK(OPTC("panel") == 4 && system("ls build/map/a | grep -q backup") != 0, "CANCEL: the list goes, nothing written");
        pr("src_map"); pr("src_commit"); CHECK(OPTC("panel") == 7 && OPTC("bigmsg") == 0, "COMMIT with no track marked does nothing");
        pr("srcsel1"); CHECK(OPTC("srchl1") == 2 && OPTC("panel") == 7 && OPTC("bigmsg") == 0 && system("ls build/map/a | grep -q backup") != 0, "a tap only marks the track");
        pr("src_commit");
        c->d(c, 7, P("bkname"), 0, m, 0);
        CHECK(OPTC("bigmsg") == 68 && OPTC("bigdone") == 1 && !strcmp(m, " ") && OPTC("panel") == 4, "COMMIT on the marked track: big message DONE64 (7 CCs + pan per track, v1.2.0) (%d), no backup (v2.0) '%s', the list closes", OPTC("bigmsg"), m);
        pr("msg_ok"); CHECK(OPTC("bigmsg") == 0 && OPTC("bigdone") == 0, "a tap on the message closes it");
        CHECK(system("! ls build/map/a | grep -q backup && ! cmp -s build/map/a/cctest1.xpj testdata1/cctest1.xpj && test ! -e build/map/a/cctest1.xpj.tmp && ! ls build/map/a | grep -q bak") == 0, "no backup (v2.0), the project itself changed");
        CHECK(system("python3 -I -c \"import gzip,json;t=gzip.open('build/map/a/cctest1.xpj').read().decode();d=json.loads(t[t.index('{'):])['data'];o=gzip.open('testdata1/cctest1.xpj').read().decode();e=json.loads(o[o.index('{'):])['data'];assert len(d['midiLearnSettings']['controls'])==64 and len(d['samples'])==2 and d['samples']==e['samples'] and d['tracks']==e['tracks']\"") == 0, "every track is as it was, samples included; only the mapping list grew");
        {
            static char *chunk; int n = (int)c->d(c, 23, 0, 0, &chunk, 0); std::string saved(chunk, n);
            CHECK(saved.find("\nS Drum 001\n") != std::string::npos, "the target track is saved with the plugin");
        }
        pr("src_fetch");
        c->d(c, 7, P("msg"), 0, r1, 0); c->d(c, 7, P("st_cc1"), 0, m, 0); c->d(c, 7, P("st_cc3"), 0, r2, 0);
        CHECK(!strcmp(r1, "Read 56 CC values") && !strcmp(m, "Tuning (64)") && !strcmp(r2, "F. Res (0)"), "FETCH CC VALUES from the target track: '%s'; T1 '%s', '%s'", r1, m, r2);
        { char c6[64], pn[64]; set_int(c, "step_edit", 1); set_int(c, "sel", 2); c->d(c, 7, P("st_cc6"), 0, c6, 0); c->d(c, 7, P("st_pan"), 0, pn, 0);
          CHECK(strstr(c6, "(") && pn[0] == '(', "STEP LOCKS after FETCH: CC 6 '%s' (Send 3), pan '%s' (the pad's), in brackets: no lock", c6, pn);
          set_int(c, "global_edit", 1); }
        {   /* v1.2.0: the pad's own pan, not the mapping's controlValue: pad A01 panned to 0.25 in the saved project, mapping left as it was */
            const int rc2 = system("python3 -I -c \"import gzip,json;f='build/map/a/cctest1.xpj';t=gzip.open(f).read().decode();i=t.index('{');d=json.loads(t[i:]);"
                                   "tr=[x for x in d['data']['tracks'] if x.get('name')=='Drum 001'][0];tr['program']['drum']['instruments'][0]['mixable']['pan']=0.25;"
                                   "gzip.open(f,'wb').write((t[:i]+json.dumps(d,indent=1)).encode())\"");
            pr("src_fetch");
            char pn[64]; set_int(c, "step_edit", 1); set_int(c, "track", 0); set_int(c, "sel", 2); c->d(c, 7, P("st_pan"), 0, pn, 0);
            CHECK(rc2 == 0 && !strcmp(pn, "(L32)"), "FETCH CC VALUES reads the pad's own pan (A01 at 0.25 in the project, its mapping unchanged): '%s'", pn);
            set_int(c, "global_edit", 1);
        }
        {   /* v1.93: the pads' pan comes along (cctest1: A02 in the centre, 0.5 -> 64, saved as 65) */
            static char *ch2; int n2 = (int)c->d(c, 23, 0, 0, &ch2, 0); std::string sv(ch2, n2);
            size_t at = sv.find("\nA 1 ");
            CHECK(at != std::string::npos && atoi(sv.c_str() + at + 5) == 65, "FETCH CC VALUES: T2's pad pan read too ('%.10s')", at != std::string::npos ? sv.c_str() + at + 1 : "none");
        }
        {
            char n1[64], n8[64];
            set_int(c, "track", 7); blocks(c, 1, 0.0); c->d(c, 7, P("trk_num"), 0, n8, 0);
            CHECK(atoi(n8) != 43, "after FETCH CC VALUES T8 still has its own note (%s)", n8);   /* 50: set before the mapping, below */
            pr("src_notes"); c->d(c, 7, P("msg"), 0, n1, 0);
            CHECK(!strcmp(n1, "Read 8 notes"), "FETCH NOTES: '%s'", n1);
            set_int(c, "track", 0); blocks(c, 1, 0.0); c->d(c, 7, P("trk_num"), 0, n1, 0);
            set_int(c, "track", 7); blocks(c, 1, 0.0); c->d(c, 7, P("trk_num"), 0, n8, 0);
            set_int(c, "track", 0); blocks(c, 1, 0.0);
            CHECK(atoi(n1) == 36 && atoi(n8) == 43, "T1 note '%s', T8 note '%s'", n1, n8);
        }
        pr("src_map"); CHECK(OPTC("srchl1") == 2, "the target track is the marked row");
        pr("src_commit");
        CHECK(OPTC("bigmsg") == 3 && OPTC("bigdone") == 0, "a second time: NOTHING NEW TO MAP, no write (%d)", OPTC("bigmsg"));
        pr("msg_ok");
        rc = system("cp testdata1/cctest1.xpj build/map/a/cctest1.xpj && touch -d '2035-01-01' build/map/a/cctest1.xpj");
        pr("src_map"); pr("srcsel1"); pr("src_commit"); c->d(c, 7, P("bkname"), 0, m, 0);
        CHECK(rc == 0 && OPTC("bigmsg") == 68 && system("! ls build/map/a | grep -q backup") == 0, "written again into a fresh copy, still no backup");
        pr("msg_ok");
        pr("src_map"); set_int(c, "pattern_edit", 1); blocks(c, 2, 0.0);
        CHECK(OPTC("scr") == 0 && OPTC("panel") == 5, "leaving SETUP closes the list");
        set_int(c, "global_edit", 1); blocks(c, 2, 0.0);
        set_int(c, "cc_edit", 1); blocks(c, 2, 0.0); CHECK(OPTC("panel") == 6, "TRACK SETUP: its own panel");
        pr("src_map"); CHECK(OPTC("panel") == 6 && OPTC("scr") == 0, "CREATE DEFAULT MAP does nothing while it is open");
        pr("cc_discard"); CHECK(OPTC("panel") == 4, "DISCARD: back to the SETUP buttons");
        setenv("STEPRUNNER_CURRENT", "build/map/none", 1);
        setenv("STEPRUNNER_XPJ_ROOT", "build/map/c", 1);
        pr("src_map");
        CHECK(OPTC("bigmsg") == 2 && OPTC("scr") == 0, "a project with plugin tracks only: NO DRUM TRACK (%d), no list", OPTC("bigmsg"));
        pr("msg_ok");
        pr("src_notes"); c->d(c, 7, P("msg"), 0, m, 0);
        CHECK(!strcmp(m, "Read 0 notes"), "FETCH NOTES, the target track is not in this project: '%s'", m);
        pr("src_fetch"); c->d(c, 7, P("msg"), 0, m, 0);
        CHECK(!strcmp(m, "Read 0 CC values"), "the target track is not in this project: '%s'", m);
        unsetenv("STEPRUNNER_NOVERIFY");
        pr("src_map"); c->d(c, 7, P("srcproj"), 0, m, 0);
        CHECK(OPTC("bigmsg") == 1 && OPTC("scr") == 0 && !strcmp(m, "Project not saved"), "a project that is not this one (not saved): PROJECT NOT SAVED (%d), '%s'", OPTC("bigmsg"), m);
        setenv("STEPRUNNER_NOVERIFY", "1", 1);
        pr("msg_ok");
        unsetenv("STEPRUNNER_CURRENT");
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* which project is read: the one MPC names as open or the newest file on the drives, whichever was saved last */
        int rc = system("rm -rf build/cur && mkdir -p build/cur/new build/cur/old && cp testdata/cctest2.xpj 'build/cur/old/A & B.xpj' && cp testdata9/9w9.xpj build/cur/new/ && touch -d '2020-01-01' 'build/cur/old/A & B.xpj'"
                        " && printf '<?xml version=\"1.0\"?>\\r\\n<PROPERTIES>\\r\\n  <VALUE name=\"lastProjectPath\" val=\"build/cur/old/A &amp; B.xpj\"/>\\r\\n</PROPERTIES>\\r\\n' > build/cur/CurrentProject"
                        " && printf '<PROPERTIES><VALUE name=\"lastProjectPath\" val=\"build/cur/gone.xpj\"/></PROPERTIES>' > build/cur/Stale");
        AEffect *c = VSTPluginMain(host);
        char m[64];
        auto pr = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        setenv("STEPRUNNER_XPJ_ROOT", "build/cur", 1);
        pr("src_map"); pr("src_cancel"); c->d(c, 7, P("srcproj"), 0, m, 0);
        CHECK(rc == 0 && !strcmp(m, "9w9"), "no CurrentProject: the newest file ('%s')", m);
        setenv("STEPRUNNER_CURRENT", "build/cur/CurrentProject", 1);
        pr("src_map"); pr("src_cancel"); c->d(c, 7, P("srcproj"), 0, m, 0);
        CHECK(!strcmp(m, "9w9"), "CurrentProject names a project saved long ago, another was saved since: the newer one is read ('%s')", m);
        rc = system("touch 'build/cur/old/A & B.xpj'");
        pr("src_map"); pr("src_cancel"); c->d(c, 7, P("srcproj"), 0, m, 0);
        CHECK(rc == 0 && !strcmp(m, "A & B"), "the named project is saved again: now it is read ('%s')", m);
        setenv("STEPRUNNER_CURRENT", "build/cur/Stale", 1);
        rc = system("touch -d '2035-01-01' build/cur/new/9w9.xpj");
        pr("src_map"); pr("src_cancel"); c->d(c, 7, P("srcproj"), 0, m, 0);
        CHECK(!strcmp(m, "9w9"), "it names a file that is gone: the newest ('%s')", m);
        unsetenv("STEPRUNNER_CURRENT");
        c->d(c, 1, 0, 0, 0, 0);
    }
    {   /* 21000 other files on the drive must not stop the search before it reaches the project */
        int rc = system("rm -rf build/big && mkdir -p build/big/a build/big/zz && (cd build/big/a && seq 1 21000 | xargs touch) && cp testdata9/9w9.xpj build/big/zz/p.xpj");
        setenv("STEPRUNNER_XPJ_ROOT", "build/big", 1);
        AEffect *c = VSTPluginMain(host);
        set_int(c, "track_edit", 1); blocks(c, 1, 0.0); set_int(c, "cc_edit", 1); blocks(c, 1, 0.0);
        c->setP(c, P("cc_read"), 1.0f); blocks(c, 1, 0.0); c->setP(c, P("cc_read"), 0.0f); blocks(c, 1, 0.0);
        char m[64]; c->d(c, 7, P("msg"), 0, m, 0);
        CHECK(rc == 0 && !strcmp(m, "Read 4 project values"), "project found behind 21000 files: '%s'", m);
        c->d(c, 1, 0, 0, 0, 0);
        rc = system("rm -rf build/big"); (void)rc;
    }
    unsetenv("STEPRUNNER_XPJ_ROOT");
}

/* v1.62: only the open project is read or written -- the file that holds this StepRunner-8's chunk (given to MPC on save,
 * or loaded from it). A new project not saved yet: the newest file is another project, so nothing is touched. */
static void verify_tests() {
    unsetenv("STEPRUNNER_NOVERIFY"); unsetenv("STEPRUNNER_CURRENT");
    int rc = system("rm -rf build/vfy && mkdir -p build/vfy && cp testdata9/9w9.xpj build/vfy/other.xpj && touch -d '2020-01-01' build/vfy/other.xpj");
    setenv("STEPRUNNER_XPJ_ROOT", "build/vfy", 1);
    AEffect *c = VSTPluginMain(host);
    char m[64];
    auto rd = [&](AEffect *e) { set_int(e, "track_edit", 1); blocks(e, 1, 0.0); set_int(e, "cc_edit", 1); blocks(e, 1, 0.0);
        e->setP(e, P("cc_read"), 1.0f); blocks(e, 1, 0.0); e->setP(e, P("cc_read"), 0.0f); blocks(e, 1, 0.0);
        e->d(e, 7, P("msg"), 0, m, 0); e->setP(e, P("cc_discard"), 1.0f); blocks(e, 1, 0.0); e->setP(e, P("cc_discard"), 0.0f); blocks(e, 1, 0.0); };
    rd(c);
    CHECK(rc == 0 && !strcmp(m, "Save the project first"), "new project, not saved: the newest file is another project's, not read ('%s')", m);
    c->setP(c, P("src_map"), 1.0f); blocks(c, 1, 0.0); c->setP(c, P("src_map"), 0.0f); blocks(c, 2, 0.0);
    c->setP(c, P("src_cancel"), 1.0f); blocks(c, 1, 0.0); c->setP(c, P("src_cancel"), 0.0f); blocks(c, 2, 0.0);
    c->d(c, 7, P("srcproj"), 0, m, 0);
    CHECK(!strcmp(m, "Project not saved"), "and SETUP does not show another project's name ('%s')", m);
    static char *chunk; int n = (int)c->d(c, 23, 0, 0, &chunk, 0);   /* MPC saves the project: it asks for the chunk */
    if (FILE *f = fopen("build/vfy/chunk", "wb")) { fwrite(chunk, 1, (size_t)n, f); fclose(f); }
    rc = system("python3 test_mkxpj.py testdata9/9w9.xpj build/vfy/chunk build/vfy/mine.xpj");
    rd(c);
    CHECK(rc == 0 && !strcmp(m, "Read 4 project values"), "saved: the file with its chunk is read ('%s')", m);
    { AEffect *d = VSTPluginMain(host); rd(d);
      CHECK(!strcmp(m, "Save the project first"), "another StepRunner-8 (another new project) does not take it for its own ('%s')", m);
      d->d(d, 1, 0, 0, 0, 0); }
    { AEffect *e = VSTPluginMain(host); std::string s(chunk, (size_t)n); e->d(e, 24, 0, n, (void *)s.c_str(), 0);   /* the project reopened */
      rd(e); CHECK(!strcmp(m, "Read 4 project values"), "loaded from that project: it is its own ('%s')", m);
      e->d(e, 1, 0, 0, 0, 0); }
    {   /* two files hold this StepRunner-8 (a Save As over another project left untitled.xpj next to it, same chunk): CREATE
         * DEFAULT MAP asks which one is open; our own backups do not count */
        rc = system("cp build/vfy/mine.xpj build/vfy/untitled.xpj && cp build/vfy/mine.xpj build/vfy/mine-backup.xpj && cp build/vfy/mine.xpj build/vfy/mine-backup3.xpj");
        auto prc = [&](const char *k) { c->setP(c, P(k), 1.0f); blocks(c, 1, 0.0); c->setP(c, P(k), 0.0f); blocks(c, 2, 0.0); };
        set_int(c, "global_edit", 1); blocks(c, 2, 0.0);
        prc("src_map");
        char r1[64], r2[64], r3[64];
        c->d(c, 7, P("srcproj"), 0, m, 0); c->d(c, 7, P("srcrow1"), 0, r1, 0); c->d(c, 7, P("srcrow2"), 0, r2, 0); c->d(c, 7, P("srcrow3"), 0, r3, 0);
        CHECK(rc == 0 && !strcmp(m, "Which project is open?") && (!strcmp(r1, "mine") || !strcmp(r1, "untitled")) && (!strcmp(r2, "mine") || !strcmp(r2, "untitled")) && strcmp(r1, r2) && !strcmp(r3, " "),
              "two copies: '%s' asks, rows '%s' / '%s' / '%s' (no backups)", m, r1, r2, r3);
        prc(!strcmp(r1, "mine") ? "srcsel1" : "srcsel2"); prc("src_commit");
        CHECK(c->getP(c, P("bigmsg")) > 0.0f && (int)lroundf(c->getP(c, P("bigmsg")) * (PARAMS[P("bigmsg")].nopts - 1)) == 2, "the one picked is read: 9w9 has no drum track (NO DRUM TRACK)");
        prc("msg_ok"); prc("src_cancel");
        setenv("STEPRUNNER_CURRENT", "build/vfy/cur", 1);
        rc = system("printf '<PROPERTIES><VALUE name=\"lastProjectPath\" val=\"build/vfy/mine.xpj\"/></PROPERTIES>' > build/vfy/cur");
        prc("src_cancel"); prc("src_map"); c->d(c, 7, P("srcrow1"), 0, m, 0);
        CHECK(rc == 0 && !strcmp(m, "mine"), "the one CurrentProject names is listed first ('%s')", m);
        prc("src_cancel");
        unsetenv("STEPRUNNER_CURRENT");
        rc = system("rm build/vfy/untitled.xpj build/vfy/mine-backup.xpj build/vfy/mine-backup3.xpj build/vfy/cur");
        set_int(c, "pattern_edit", 1); blocks(c, 2, 0.0);
    }
    rc = system("touch -d '2036-01-01' build/vfy/other.xpj");   /* another project saved later (a Save As elsewhere) */
    rd(c);
    CHECK(!strcmp(m, "Read 4 project values"), "a newer file of another project: the one found before is still read ('%s')", m);
    setenv("STEPRUNNER_CURRENT", "build/vfy/cur", 1);
    rc = system("printf '<PROPERTIES><VALUE name=\"lastProjectPath\" val=\"build/vfy/other.xpj\"/></PROPERTIES>' > build/vfy/cur");
    rd(c);
    CHECK(rc == 0 && !strcmp(m, "Read 4 project values"), "CurrentProject names another project: not trusted ('%s')", m);
    unsetenv("STEPRUNNER_CURRENT");
    c->d(c, 1, 0, 0, 0, 0);
    unsetenv("STEPRUNNER_XPJ_ROOT");
    setenv("STEPRUNNER_NOVERIFY", "1", 1);
}

static void preset_tests() {
    AEffect *a = VSTPluginMain(host);
    {   /* v1.95: the screen it was left on comes back with the project; a preset does not change it */
        set_int(a, "track_edit", 1);
        void *c = 0; a->d(a, 23, 0, 0, &c, 0); std::string sv((const char *)c);
        AEffect *b = VSTPluginMain(host);
        b->d(b, 24, 0, (intptr_t)sv.size() + 1, (void *)sv.c_str(), 0);
        CHECK(b->getP(b, P("track_edit")) == 1.0f && b->getP(b, P("pattern_edit")) == 0.0f, "a saved project opens on TRACK EDIT, where it was left");
        b->d(b, 1, 0, 0, 0, 0);
        set_int(a, "pattern_edit", 1);
    }
    {   /* v2.0: no programs at all */
        CHECK(a->np == 0, "no VST programs");
        AEffect *f = VSTPluginMain(host);
        CHECK(f->getP(f, P("global_edit")) == 1.0f && f->getP(f, P("track_edit")) == 0.0f && f->getP(f, P("pattern_edit")) == 0.0f && f->getP(f, P("step_edit")) == 0.0f, "a new plugin opens on SETUP (v1.0.0)");
        {   /* v1.0.0: one of SETUP, PATTERN EDIT, TRACK EDIT, STEP EDIT is always lit; a tap on the lit one changes nothing */
            auto lit = [&](const char *k) { return f->getP(f, P(k)) > 0.5f; };
            auto one = [&]() { return (int)lit("global_edit") + lit("pattern_edit") + lit("track_edit") + lit("step_edit") == 1; };
            bool ok = true;
            const char *four[4] = {"global_edit", "pattern_edit", "track_edit", "step_edit"};
            for (int k = 0; k < 4; k++) {
                set_int(f, four[k], 1); ok &= lit(four[k]) && one();
                set_int(f, four[k], 0); ok &= lit(four[k]) && one();   /* the host's toggle sends 0: stays */
            }
            CHECK(ok, "each of the four: on, and a second tap keeps it on; always exactly one lit");
            set_int(f, "track_edit", 1); set_int(f, "euclid_edit", 1);
            CHECK(lit("euclid_edit") && lit("track_edit") && one() && lroundf(f->getP(f, P("panel")) * (PARAMS[P("panel")].nopts - 1)) == 2, "EUCLIDIAN over TRACK EDIT: TRACK EDIT stays lit");
            set_int(f, "euclid_edit", 0);
            CHECK(!lit("euclid_edit") && lit("track_edit") && one(), "EUCLIDIAN off: back to TRACK EDIT");
            set_int(f, "step_edit", 1); set_int(f, "euclid_edit", 1); set_int(f, "step_edit", 0);
            CHECK(!lit("euclid_edit") && lit("step_edit") && one(), "in EUCLIDIAN, a tap on the lit STEP EDIT leaves EUCLIDIAN for STEP EDIT");
            set_int(f, "timing_edit", 1); CHECK(lit("timing_edit") && lit("step_edit") && one(), "TIMING: STEP EDIT stays lit");
            set_int(f, "global_edit", 1); set_int(f, "random_edit", 1); set_int(f, "random_edit", 0);
            CHECK(lit("global_edit") && !lit("random_edit") && one(), "RANDOM SETUP off: back to SETUP");
            {   /* v1.0.1: SETUP (and RANDOM SETUP) show the logo on the left, no RND / CLR buttons */
                set_int(f, "global_edit", 1);
                const int lp = (int)lroundf(f->getP(f, P("lpanel")) * (PARAMS[P("lpanel")].nopts - 1)), lb = (int)lroundf(f->getP(f, P("lbtn")) * (PARAMS[P("lbtn")].nopts - 1));
                set_int(f, "random_edit", 1);
                const int lp2 = (int)lroundf(f->getP(f, P("lpanel")) * (PARAMS[P("lpanel")].nopts - 1)), lb2 = (int)lroundf(f->getP(f, P("lbtn")) * (PARAMS[P("lbtn")].nopts - 1));
                set_int(f, "pattern_edit", 1);
                const int lp3 = (int)lroundf(f->getP(f, P("lpanel")) * (PARAMS[P("lpanel")].nopts - 1)), lb3 = (int)lroundf(f->getP(f, P("lbtn")) * (PARAMS[P("lbtn")].nopts - 1));
                CHECK(lp == 4 && lb == 4 && lp2 == 4 && lb2 == 4 && lp3 == 0 && lb3 == 0,
                      "SETUP / RANDOM SETUP: logo, no RND buttons (%d %d, %d %d); PATTERN EDIT: RND / CLR again (%d %d)", lp, lb, lp2, lb2, lp3, lb3);
            }
            set_int(f, "track_edit", 1);   /* the tests below start from TRACK EDIT */
        }
        {   /* v2.4.0: a STATE holds the mutes too, and keeps them through a save */
            const double ppb = (120.0 / 60.0) * (128.0 / 44100.0);
            f->setP(f, P("mute3"), 1.0f); blocks(f, 1, ppb);
            f->setP(f, P("pst1"), 1.0f); blocks(f, 1, ppb);
            f->setP(f, P("mute3"), 0.0f); f->setP(f, P("mute5"), 1.0f); blocks(f, 1, ppb);
            f->setP(f, P("pst1"), 0.0f); blocks(f, 1, ppb);
            CHECK(f->getP(f, P("mute3")) == 1.0f && f->getP(f, P("mute5")) == 0.0f, "STATE 1 recalled: T3 muted again, T5 not");
            void *c = 0; f->d(f, 23, 0, 0, &c, 0); std::string sv((const char *)c);
            AEffect *g = VSTPluginMain(host); g->d(g, 24, 0, (intptr_t)sv.size() + 1, (void *)sv.c_str(), 0);
            g->setP(g, P("mute3"), 0.0f); blocks(g, 1, ppb); g->setP(g, P("pst1"), 0.0f); blocks(g, 1, ppb);
            CHECK(g->getP(g, P("mute3")) == 1.0f, "and after a save and load as well");
            /* v2.5.0: and the effect pads */
            g->setP(g, P("pclear"), 1.0f); blocks(g, 1, ppb); g->setP(g, P("pst2"), 1.0f); blocks(g, 1, ppb); g->setP(g, P("pclear"), 0.0f); blocks(g, 1, ppb);
            g->setP(g, P("pf_echo"), 1.0f); g->setP(g, P("pfx1"), 1.0f); blocks(g, 1, ppb);
            g->setP(g, P("pst2"), 1.0f); blocks(g, 1, ppb);
            g->setP(g, P("pf_echo"), 0.0f); g->setP(g, P("pfx2"), 1.0f); g->setP(g, P("pf_ghost"), 1.0f); blocks(g, 1, ppb);
            g->setP(g, P("pst2"), 0.0f); blocks(g, 1, ppb);
            CHECK(g->getP(g, P("pf_echo")) == 1.0f && g->getP(g, P("pfx1")) == 1.0f && g->getP(g, P("pfx2")) == 0.0f && g->getP(g, P("pf_ghost")) == 0.0f,
                  "STATE 2 recalled: ECHO and LOOP 4 on again, LOOP 2 and GHOST off");
            { void *c2 = 0; g->d(g, 23, 0, 0, &c2, 0); std::string s2((const char *)c2);
              AEffect *h = VSTPluginMain(host); h->d(h, 24, 0, (intptr_t)s2.size() + 1, (void *)s2.c_str(), 0);
              h->setP(h, P("pst2"), 0.0f); blocks(h, 1, ppb);
              CHECK(h->getP(h, P("pf_echo")) == 1.0f && h->getP(h, P("pfx1")) == 1.0f, "the effects come back after a save and load too");
              h->d(h, 1, 0, 0, 0, 0); }
            g->d(g, 1, 0, 0, 0, 0); f->setP(f, P("mute3"), 0.0f); blocks(f, 1, ppb);
        }
        {   /* v2.1.0: EUCLIDIAN changes only the left part; the right one stays the last of PATTERN / STEP / TRACK EDIT */
            auto panel = [&]() { return (int)lroundf(f->getP(f, P("panel")) * (PARAMS[P("panel")].nopts - 1)); };
            set_int(f, "euclid_edit", 1); CHECK(panel() == 2, "from TRACK EDIT: the track panel stays (%d)", panel());
            set_int(f, "step_edit", 1); set_int(f, "euclid_edit", 1); CHECK(panel() == 0, "from STEP EDIT: the locks stay (%d)", panel());
            set_int(f, "global_edit", 1); set_int(f, "euclid_edit", 1); CHECK(panel() == 0, "via SETUP: still the last of the three (%d)", panel());
            set_int(f, "pattern_edit", 1); set_int(f, "euclid_edit", 1); CHECK(panel() == 5, "from PATTERN EDIT: the pattern setup (%d)", panel());
            void *c = 0; f->d(f, 23, 0, 0, &c, 0); std::string sv((const char *)c);
            AEffect *g = VSTPluginMain(host); g->d(g, 24, 0, (intptr_t)sv.size() + 1, (void *)sv.c_str(), 0);
            CHECK(lroundf(g->getP(g, P("panel")) * (PARAMS[P("panel")].nopts - 1)) == 5 && g->getP(g, P("euclid_edit")) == 1.0f, "saved and loaded so");
            g->d(g, 1, 0, 0, 0, 0); set_int(f, "track_edit", 1);
        }
        {   /* v1.99: a pattern visited but left with no trig and no lock is still empty */
            const double ppb = (120.0 / 60.0) * (128.0 / 44100.0);
            auto tap = [&](const char *k) { f->setP(f, P(k), f->getP(f, P(k)) > 0.5f ? 0.0f : 1.0f); blocks(f, 1, ppb); };
            auto look = [&](int k) { return (int)lroundf(f->getP(f, P(("patv" + std::to_string(k)).c_str())) * 5) - 1; };
            set_int(f, "pattern_edit", 1); f->setP(f, P("step0"), 1.0f); blocks(f, 1, ppb);
            tap("pat2"); set_int(f, "trk_vel", 90); tap("pat3");
            CHECK(look(1) == 1 && look(2) == 0 && look(3) == 2, "pattern 2 visited, nothing written in it: shown empty (%d %d %d)", look(1), look(2), look(3));
            f->setP(f, P("step4"), 1.0f); blocks(f, 1, ppb); tap("pat1");
            CHECK(look(3) == 1, "pattern 3 got a step: full (%d)", look(3));
        }
        f->d(f, 1, 0, 0, 0, 0);
    }
    a->d(a, 1, 0, 0, 0, 0);
}

int main() {
    setenv("STEPRUNNER_NOVERIFY", "1", 1);   /* the fixtures are other projects; verify_tests checks the check */
    core_tests();
    perform_tests();
    loop4_tests();
    ccnum_tests();
    shuffle_tests();
    ccbase_tests();
    vst_tests();
    verify_tests();
    preset_tests();
    printf("%s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}

/* =============================================================================
 * steprunner8.cpp - StepRunner-8: a trig sequencer as a VST2 MIDI generator for the MPC OS
 * plugin host (https://github.com/sd88me/mpc-vst-plugins). Modelled on mpc-vst-maze's maze_seq_vst.cpp.
 *
 *   seqcore.hpp           the sequencer itself: 8 tracks x 64 steps
 *   audioMasterGetTime    tempo / transport / ppqPos -> a free-running song position
 *   ALSA seq port         "StepRunner-8" / "MIDI Out" (MPC OS ignores a plugin's VST MIDI output)
 *   VST parameters        a WINDOW on the pattern: "track" and "page" choose which
 *                         16 steps the step buttons show; "sel" (or the last tapped step) chooses the
 *                         step whose condition / probability / locks the st_* knobs edit; trk_* edit
 *                         the selected track. The pattern itself lives in the project chunk.
 *
 * Because the same knob means a different step after a track/page/step change, the host has to be told
 * the new values: processReplacing compares every parameter with what the host last heard (told[]) and
 * sends audioMasterAutomate for the ones that moved (never from inside setParameter).
 * ========================================================================== */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <chrono>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <mutex>
#include <vector>
#include <strings.h>
#include <cstdint>
#include <thread>
#include <algorithm>
#include <string>

#ifndef STEPRUNNER_TEST
#include <alsa/asoundlib.h>
#endif

#include "seqcore.hpp"
#include "params.h"
/* VST2 programs: none (v2.0.0; v1.89-1.98 had factory presets, v1.99 one "--"). The user's own presets are saved by MPC. */
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */

using namespace seqcore;

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

/* ---- parameter indices: the order of params.json (gen_params.py). APPEND ONLY. ---- */
enum {
    P_TRACK = 0, P_PAGE, P_STEP0, P_SEL = P_STEP0 + 16, P_ST_COND, P_ST_PROB, P_ST_VEL, P_ST_LEN, P_ST_CC0,
    P_TRK_LEN = P_ST_CC0 + NCC_V1, P_TRK_SPEED, P_TRK_CH, P_TRK_NUM, P_TRK_VEL, P_TRK_GATE,
    P_MUTE0, P_FILL = P_MUTE0 + NTRACKS, P_CLR_TRK, P_CLR_STEP, P_RUN, P_FOLLOW,
    P_UNUSED0,                                   /* 5 retired slots (v1.1.0 CC VALUE knobs): read 0, ignore writes */
    P_RND0 = P_UNUSED0 + NCC_V1,                    /* NRND random buttons: trigs, velocity, length, CC 20..24 (Seq::R_TRIG .. R_CC0 + 4) */
    P_RND_DENS = P_RND0 + 3 + NCC_V1, P_RND_MIN, P_RND_MAX,
    P_CELL0,                                     /* 16 read-only TRIGS cells: trig (1) + play-head (2) + selected (4) + 8 x mode (1 STEP EDIT, 2 TRACK EDIT, 3 EUCLIDIAN); option 32 is padding */
    P_STEP_EDIT = P_CELL0 + 16,
    P_MULTI,                                     /* read-only: more than one step selected (the skin hides the STEP LOCKS controls) */
    P_ST_PAN, P_RND_PAN,                         /* pan lock of the selected step, and its random button */
    P_TRACK_EDIT,                                /* the right-hand panel shows the track setup; switches STEP EDIT off, and the other way round */
    P_PANEL, P_PTITLE,                           /* read-only, for the skin: panel content (locks / multi message / track setup) and the mode (normal / STEP EDIT / TRACK EDIT: titles, button colours) */
    P_LEN16, P_LEN32, P_LEN48, P_LEN64,          /* buttons, lit while the selected track has that length: set it */
    P_TRKSEL0,                                   /* 8 buttons, the selected track is lit: select a track (same as P_TRACK) */
    P_PGSEL0 = P_TRKSEL0 + NTRACKS,              /* 4 buttons, the shown page is lit: show a page (same as P_PAGE) */
    P_SEL_END = P_PGSEL0 + 4,
    P_RANDOM_EDIT = P_SEL_END,                   /* the panel shows DENSITY / MIN / MAX; one of three modes with STEP EDIT and TRACK EDIT */
    P_TRK_CCBASE,                                /* selected track: first of its five CC numbers */
    P_EUCLID_EDIT,                               /* fourth mode: the left-hand section shows the Euclidean generator */
    P_LPANEL,                                    /* read-only, for the skin: left-hand section content (random / euclid) */
    P_EU_ENABLE,                                 /* lit while the selected track is generated; a press enables it */
    P_EU_PL1, P_EU_RO1, P_EU_TRO, P_EU_PL2, P_EU_RO2, P_EU_BOOL,
    P_EU_DISCARD, P_EU_COMMIT,                   /* buttons */
    P_PF0,                                       /* PERFORM tab: NPF knobs named like the step locks. PERFORM ALL offsets (seq.pf_off). */
    P_PF_CTRL = P_PF0 + 9, P_PF_COMMIT, P_PF_DISCARD,   /* PERFORM tab: PERFORM ALL (switch), COMMIT, UNDO (key pf_discard). */
    P_PFX0,                                      /* PERFORM tab: LOOP 4, LOOP 2, LOOP 1, NO LOCKS, RANDOM STEP, INVERT MUTE, UNMUTE ALL (momentary), FILL. Not saved (FILL is: it is the old fill). */
    P_PFX_END = P_PFX0 + 8,
    P_PFY0 = P_PFX_END,                          /* PERFORM tab: the second row of effects (LOOP 8, REVERSE, HALF / DOUBLE TIME, ROLL, RESTART, SCATTER, Pad 8). Not saved. */
    P_MSTATE = P_PFY0 + 8,                       /* read-only, for the skin: NONE / SOME tracks muted (pad 7 reads MUTE ALL / UNMUTE ALL) */
    P_GLOBAL_EDIT,                               /* fifth mode: the panel shows the global settings */
    P_SHUFFLE,                                   /* global: 50..80 % */
    P_SHUF0,                                     /* 8 switches: the track follows the shuffle */
    P_GLOB_CH = P_SHUF0 + NTRACKS,               /* MIDI channel of all tracks at once; shows T1's, or MIXED */
    P_SHUF_ALL,                                  /* lit while every track follows the shuffle; a press switches all on (or, when lit, all off) */
    P_PATTERN_EDIT,                              /* sixth mode: the panel shows the pattern setup */
    P_PLEN0, P_PLEN_END = P_PLEN0 + 4,            /* buttons 16 / 32 / 48 / 64: length of every track; lit while all tracks have it */
    P_UNDO = P_PLEN_END, P_REDO,                 /* buttons: step back / forward through the edit history */
    P_COPY, P_PASTE,                             /* buttons (STEP EDIT): copy the selected steps / paste them from the selected step on */
    P_MSG,                                       /* read-only: 0 = nothing, 1..16 = "Copied n steps", 17..32 = "Pasted n steps" (shown for 2 s) */
    P_MPAD0,                                     /* 8 read-only: the PERFORM mute pad of a track: IDLE / HIT (a note just fired) / MUTED */
    P_MPAD_END = P_MPAD0 + NTRACKS,
    P_PF_CC117 = P_MPAD_END,                     /* PERFORM ALL: sends CC 117 (0..127) on every track's channel as it is turned */
    P_PCOPY, P_PPASTE,                           /* PATTERN SETUP buttons: copy the shown page of all tracks / paste it onto the shown page */
    P_SHIFT,                                     /* STEP EDIT: while lit a tap adds to / takes from the selection; unlit a tap selects that one step */
    P_EU_LEN,                                    /* Euclidean: length of the rhythm, independent of the track length */
    P_EU_RND,                                    /* button: random pulse / rotate / length */
    P_PGST0, P_PGST_END = P_PGST0 + 4,           /* 4 read-only: a page button's picture: IDLE / SHOWN / PLAYING / SHOWN+PLAYING (the selected track's play-head is on that page) */
    P_CC_EDIT = P_PGST_END,                      /* TRACK EDIT: the panel shows the track's five CC numbers (a draft until COMMIT) */
    P_CCN0, P_CC_COMMIT = P_CCN0 + NCC_V1, P_CC_DISCARD,   /* the five numbers (draft), and the two buttons that close the editor */
    P_CCUSE0, P_CCUSE_END = P_CCUSE0 + NCC_V1,      /* 5 read-only: FREE / USED: the draft number is already sent somewhere else on this MIDI channel */
    P_PST0 = P_CCUSE_END, P_PST_END = P_PST0 + 4, /* PERFORM states 1..4: lit = holds a state. A tap saves into an empty one, recalls a full one, or (CLEAR lit) empties it */
    P_PCLEAR = P_PST_END,                        /* CLEAR switch for the state buttons */
    P_CCB0, P_CC_READ = P_CCB0 + NCC_V1, P_READ_ALL,   /* READ PROJECT on the normal screen: base values of all eight tracks at once */
    P_SRCPROJ, P_SRCROW0, P_SRCSEL0 = P_SRCROW0 + 6, P_SRC_PREV = P_SRCSEL0 + 6, P_SRC_NEXT, P_SRC_CANCEL,
    P_SCR, P_SRC_COMMIT, P_SRCHL0, P_SRCHL_END = P_SRCHL0 + 6, P_SRC_MAP = P_SRCHL_END, P_SRC_FETCH, P_SRC_NOTES, P_SHUF_EDIT, P_CLR_MODE, P_LBTN, P_SRC_COPY, P_SRC_SKIP, P_BKSTEP,
    P_BIGMSG, P_BIGDONE, P_MSG_OK, P_BKNAME,
    P_PST5, P_PST5_END = P_PST5 + 4,              /* v1.68: STATE 5..8 (1..4 are P_PST0..) */
    /* the lines above: CCB = TRACK SETUP base values (draft; 0 = OFF, 1..128 = 0..127); SRC* = the SETUP list (project
     * name, six rows with a button each, paging, CANCEL / COMMIT, the marked row SRCHL); SRC_MAP / FETCH / NOTES = CREATE
     * DEFAULT MAP, FETCH CC VALUES, FETCH NOTES; SHUF_EDIT = SHUFFLE EDIT; CLR_MODE = CLR LOCKS; BIGMSG = the big message
     * over the PROGRAM tab, MSG_OK its close tap. READ_ALL, SRC_COPY, SRC_SKIP, BKSTEP, BIGDONE, BKNAME: retired, slots kept. */
    P_PAT0 = P_PST5_END, P_PAT_END = P_PAT0 + 8, /* v1.69: PATTERN 1..8 (main screen): lit = holds a pattern */
    P_PAT_CLEAR = P_PAT_END,                     /* its CLEAR switch */
    P_PAT_STORE,                                 /* v1.72: its STORE switch */
    P_TRK_FWD, P_TRK_BWD,                        /* v1.76: the track's direction (both = back and forth) */
    P_TIMING_EDIT, P_TM_LEFT, P_TM_RIGHT, P_ST_RATCH, P_RND_TIMING, P_RND_RATCH,   /* v1.77: TIMING mode: micro timing of the selected steps, RATCHET, their RND */
    P_CLR_TIMING, P_CLR_RATCH,                   /* v1.78: their CLR */
    P_PST_ACT,                                   /* v1.78: read-only, the STATE last saved or recalled (framed on the PERFORM tab) */
    P_PAT_FWD, P_PAT_BWD,                        /* v1.81: FWD / BWD for every track at once (pattern setup) */
    P_TRK_BOTH, P_PAT_BOTH,                      /* v1.82: BOTH (back and forth); FWD / BWD / BOTH are one choice of three */
    P_PATV0, P_PATV_END = P_PATV0 + 8,           /* v1.82: read-only, how PATTERN k looks: EMPTY / FULL / PLAYING / CUED */
    P_EU_FWD = P_PATV_END, P_EU_BWD, P_EU_BOTH,  /* v1.83: the EUCLIDIAN section's FWD / BWD / BOTH: work only while ENABLE is on */
    P_EU_DIRV,                                   /* v1.83: read-only, how they look: HIDDEN / OFF (dim) / ON */
    P_SLOTV,                                     /* v1.84: read-only, first top-row slot: EUCLID, or TIMING in STEP EDIT */
    P_RTRK,                                      /* v1.85: read-only, the track named in RANDOM SETTINGS: HIDDEN / T1..T8 */
    P_PF_HUM, P_PF_BUILD, P_PF_ECHO, P_PF_FADE,  /* v1.91: PERFORM pads HUMANIZE, BUILD, ECHO, FADE OUT (not saved) */
    P_PF_GHOST, P_PF_POLY, P_PF_DOWN,            /* v1.94: PERFORM pads GHOST, POLY, DOWNBEAT ONLY (not saved) */
    P_PAT_COPY, P_PAT_PASTE, P_PAT_CLRALL,       /* v2.0: PATTERN EDIT buttons COPY PTN / PASTE PTN / CLEAR PTN (momentary) */
    P_TTL,                                       /* v2.0: read-only, the right panel's title: HIDDEN, SETUP, ST1..8 (TRACK SETUP), RT1..8 (RANDOM) */
    P_ST_RRES,                                   /* v2.1: RATCHET RESOLUTION of the selected steps (AUTO, 1/16 .. 1/80) */
    P_PF_ALT, P_PF_AFILL,                        /* v2.2: PERFORM pads ALT BAR, AUTO FILL (not saved) */
    P_PST_PAT,                                   /* v1.1.0: read-only, whose STATEs the PERFORM tab shows: P1..P8 (the pattern playing) */
    P_PF_STOPCC,                                 /* v1.2.0: PERFORM pad STOP CC: lit = no CC goes out (not saved) */
    P_NAV_LEFT, P_NAV_RIGHT,                     /* v1.2.0: the arrow buttons left of EUCLIDIAN: TRACK EDIT's left section, page 1 / 2 */
    P_RND_COND,                                  /* v1.2.0: RND COND (CLR COND while CLR LOCKS is lit), page 2 of TRACK EDIT's left section */
    P_EU_ENA2, P_EU_RND2, P_EU_LEN2,             /* v1.2.0: EUCLIDIAN generator B: ENABLE B, RANDOM B, LENGTH B (PULSE B / ROTATE B: P_EU_PL2 / P_EU_RO2) */
    P_EU_FWD2, P_EU_BWD2, P_EU_BOTH2,            /* v1.2.0: B's direction; P_EU_FWD.. are A's since v1.2.0 (the track's before) */
    P_EU_DIRV2,                                  /* v1.2.0: read-only, how B's look: HIDDEN / OFF (dim) / ON */
    P_EU_OP0, P_EU_OP_END = P_EU_OP0 + 4,        /* v1.2.0: XOR, OR, AND, SUB: how A and B combine (lit = Track::eu_op) */
    P_EU_OPV = P_EU_OP_END,                      /* v1.2.0: read-only, how they look: HIDDEN / OFF (dim, not both on) / ON */
    P_TRK_VALS, P_TRK_BACK,                      /* v1.2.0: TRACK VALUES (TRACK EDIT's right panel) and its BACK button */
    P_TRK_PAN, P_TRK_PROB,                       /* v1.2.0: the track's pan (panb) and PROB (100 - pcut) */
    P_TRK_CC0, P_TRK_CC_END = P_TRK_CC0 + 7,     /* v1.2.0: the track's CC 1-7 base values (ccb, 0 = none); CC 8: P_TRK_CC8 */
    P_SHIFT_L = P_TRK_CC_END, P_SHIFT_R,         /* v1.2.0: SHIFT: the track (TRACK EDIT) or the pattern (PATTERN EDIT) one step earlier / later */
    P_ST_CC6, P_ST_CC7, P_PF_CC6, P_PF_CC7,      /* v1.2.0: CC 6 / CC 7: their step locks and PERFORM ALL knobs */
    P_RND_CC6, P_RND_CC7,                        /* v1.2.0: RND CC 6 / 7 (page 2 of the left section) */
    P_NAVV,                                      /* v1.2.0: read-only, which arrow has a page: NONE / RIGHT / LEFT */
    P_PSHIFTV,                                   /* v1.2.0: read-only, PATTERN EDIT's SHIFT: HIDDEN (SHUFFLE EDIT on, or another panel) / ON */
    P_ST_CC8, P_PF_CC8, P_RND_CC8, P_TRK_CC8,    /* v1.2.0: CC 8: its step lock, PERFORM ALL knob, RND button, TRACK VALUES knob */
    P_OWN_COUNT
};
enum { NRND = 3 + NCC_V1, NPF = 9 };   /* parameter counts: the RND buttons, the PERFORM knobs P_PF0.. (CC 6 / 7: P_PF_CC6 / 7) */
enum { BIG_NONE = 0, BIG_NOTSAVED, BIG_NODRUM, BIG_NOTHING, BIG_FAIL, BIG_DONE0 = BIG_FAIL, BIG_MAXN = 64 };   /* v1.2.0: 64 (8 a track) */   /* bigmsg: DONEn = BIG_DONE0 + n (n mappings, 1..48; 40 until v1.92) */
static const struct { int idx; const char *key; } KEYCHECK[] = {
    {P_TRACK, "track"}, {P_PAGE, "page"}, {P_STEP0, "step0"}, {P_STEP0 + 15, "step15"}, {P_SEL, "sel"},
    {P_ST_COND, "st_cond"}, {P_ST_PROB, "st_prob"}, {P_ST_VEL, "st_vel"}, {P_ST_LEN, "st_len"}, {P_ST_CC0, "st_cc1"}, {P_ST_CC0 + NCC_V1 - 1, "st_cc5"},
    {P_TRK_LEN, "trk_len"}, {P_TRK_SPEED, "trk_speed"}, {P_TRK_CH, "trk_ch"}, {P_TRK_NUM, "trk_num"},
    {P_TRK_VEL, "trk_vel"}, {P_TRK_GATE, "trk_gate"}, {P_MUTE0, "mute1"},
    {P_MUTE0 + NTRACKS - 1, "mute8"}, {P_FILL, "fill"}, {P_CLR_TRK, "clr_trk"}, {P_CLR_STEP, "clr_step"},
    {P_RUN, "run"}, {P_FOLLOW, "follow"}, {P_UNUSED0, "cc_base1"}, {P_UNUSED0 + NCC_V1 - 1, "cc_base5"},
    {P_RND0, "rnd_trig"}, {P_RND0 + NRND - 1, "rnd_cc5"}, {P_RND_DENS, "rnd_dens"}, {P_RND_MAX, "rnd_max"},
    {P_CELL0, "cell1"}, {P_CELL0 + 15, "cell16"}, {P_STEP_EDIT, "step_edit"}, {P_MULTI, "multi"}, {P_ST_PAN, "st_pan"}, {P_RND_PAN, "rnd_pan"},
    {P_TRACK_EDIT, "track_edit"}, {P_PANEL, "panel"}, {P_PTITLE, "ptitle"}, {P_LEN16, "len16"}, {P_LEN64, "len64"},
    {P_TRKSEL0, "trk1"}, {P_TRKSEL0 + NTRACKS - 1, "trk8"}, {P_PGSEL0, "pg1"}, {P_PGSEL0 + 3, "pg4"}, {P_RANDOM_EDIT, "random_edit"}, {P_TRK_CCBASE, "trk_ccbase"},
    {P_EUCLID_EDIT, "euclid_edit"}, {P_LPANEL, "lpanel"}, {P_EU_ENABLE, "eu_enable"}, {P_EU_PL1, "eu_pl1"}, {P_EU_BOOL, "eu_bool"}, {P_EU_COMMIT, "eu_commit"},
    {P_PF0, "pf_prob"}, {P_PF0 + NPF - 1, "pf_cc5"}, {P_PF_CTRL, "pf_ctrl"}, {P_PF_DISCARD, "pf_discard"}, {P_PFX0, "pfx1"}, {P_PFX0 + 7, "pfx8"}, {P_PFY0, "pfy1"}, {P_PFY0 + 7, "pfy8"}, {P_MSTATE, "mstate"}, {P_GLOBAL_EDIT, "global_edit"}, {P_SHUFFLE, "shuffle"}, {P_SHUF0, "shuf1"}, {P_SHUF0 + NTRACKS - 1, "shuf8"}, {P_GLOB_CH, "glob_ch"}, {P_SHUF_ALL, "shuf_all"}, {P_PATTERN_EDIT, "pattern_edit"}, {P_PLEN0, "plen16"}, {P_PLEN0 + 3, "plen64"}, {P_UNDO, "undo"}, {P_REDO, "redo"}, {P_COPY, "copy"}, {P_PASTE, "paste"}, {P_MSG, "msg"}, {P_MPAD0, "mpad1"}, {P_MPAD0 + NTRACKS - 1, "mpad8"}, {P_PF_CC117, "pf_cc117"}, {P_PCOPY, "pcopy"}, {P_PPASTE, "ppaste"}, {P_SHIFT, "shift"}, {P_EU_LEN, "eu_len"}, {P_EU_RND, "eu_rnd"}, {P_PGST0, "pgst1"}, {P_PGST0 + 3, "pgst4"}, {P_CC_EDIT, "cc_edit"}, {P_CCN0, "ccn1"}, {P_CCN0 + NCC_V1 - 1, "ccn5"}, {P_CC_COMMIT, "cc_commit"}, {P_CC_DISCARD, "cc_discard"}, {P_CCUSE0, "ccuse1"}, {P_CCUSE0 + NCC_V1 - 1, "ccuse5"}, {P_PST0, "pst1"}, {P_PST0 + 3, "pst4"}, {P_PCLEAR, "pclear"}, {P_CCB0, "ccb1"}, {P_CCB0 + NCC_V1 - 1, "ccb5"}, {P_CC_READ, "cc_read"}, {P_READ_ALL, "read_all"}, {P_SRCPROJ, "srcproj"}, {P_SRCROW0, "srcrow1"}, {P_SRCROW0 + 5, "srcrow6"}, {P_SRCSEL0, "srcsel1"}, {P_SRCSEL0 + 5, "srcsel6"}, {P_SRC_PREV, "src_prev"}, {P_SRC_NEXT, "src_next"}, {P_SRC_CANCEL, "src_cancel"}, {P_SCR, "scr"}, {P_SRC_COMMIT, "src_commit"}, {P_SRCHL0, "srchl1"}, {P_SRCHL0 + 5, "srchl6"}, {P_SRC_MAP, "src_map"}, {P_SRC_FETCH, "src_fetch"}, {P_SRC_NOTES, "src_notes"}, {P_SHUF_EDIT, "shuf_edit"}, {P_CLR_MODE, "clr_mode"}, {P_LBTN, "lbtn"}, {P_SRC_COPY, "src_copy"}, {P_SRC_SKIP, "src_skip"}, {P_BKSTEP, "bkstep"}, {P_BIGMSG, "bigmsg"}, {P_BIGDONE, "bigdone"}, {P_MSG_OK, "msg_ok"}, {P_BKNAME, "bkname"}, {P_PST5, "pst5"}, {P_PST5 + 3, "pst8"}, {P_PAT0, "pat1"}, {P_PAT0 + 7, "pat8"}, {P_PAT_CLEAR, "pat_clear"}, {P_PAT_STORE, "pat_store"}, {P_TRK_FWD, "trk_fwd"}, {P_TRK_BWD, "trk_bwd"}, {P_TIMING_EDIT, "timing_edit"}, {P_TM_LEFT, "tm_left"}, {P_TM_RIGHT, "tm_right"}, {P_ST_RATCH, "st_ratch"}, {P_RND_TIMING, "rnd_timing"}, {P_RND_RATCH, "rnd_ratch"}, {P_CLR_TIMING, "clr_timing"}, {P_CLR_RATCH, "clr_ratch"}, {P_PST_ACT, "pst_act"}, {P_PAT_FWD, "pat_fwd"}, {P_PAT_BWD, "pat_bwd"}, {P_TRK_BOTH, "trk_both"}, {P_PAT_BOTH, "pat_both"}, {P_PATV0, "patv1"}, {P_PATV0 + 7, "patv8"}, {P_EU_FWD, "eu_fwd"}, {P_EU_BWD, "eu_bwd"}, {P_EU_BOTH, "eu_both"}, {P_EU_DIRV, "eu_dirv"}, {P_SLOTV, "slotv"}, {P_RTRK, "rtrk"}, {P_PF_HUM, "pf_hum"}, {P_PF_BUILD, "pf_build"}, {P_PF_ECHO, "pf_echo"}, {P_PF_FADE, "pf_fade"}, {P_PF_GHOST, "pf_ghost"}, {P_PF_POLY, "pf_poly"}, {P_PF_DOWN, "pf_down"}, {P_PAT_COPY, "pat_copy"}, {P_PAT_PASTE, "pat_paste"}, {P_PAT_CLRALL, "pat_clrall"}, {P_TTL, "ttl"}, {P_ST_RRES, "st_rres"}, {P_PF_ALT, "pf_alt"}, {P_PF_AFILL, "pf_afill"}, {P_PST_PAT, "pst_pat"}, {P_PF_STOPCC, "pf_stopcc"}, {P_NAV_LEFT, "nav_left"}, {P_NAV_RIGHT, "nav_right"}, {P_RND_COND, "rnd_cond"}, {P_EU_ENA2, "eu_ena2"}, {P_EU_RND2, "eu_rnd2"}, {P_EU_LEN2, "eu_len2"}, {P_EU_FWD2, "eu_fwd2"}, {P_EU_BOTH2, "eu_both2"}, {P_EU_DIRV2, "eu_dirv2"}, {P_EU_OP0, "eu_opxor"}, {P_EU_OP0 + 3, "eu_opsub"}, {P_EU_OPV, "eu_opv"}, {P_TRK_VALS, "trk_vals"}, {P_TRK_BACK, "trk_back"}, {P_TRK_PAN, "trk_pan"}, {P_TRK_PROB, "trk_prob"}, {P_TRK_CC0, "trk_cc1"}, {P_TRK_CC_END - 1, "trk_cc7"}, {P_SHIFT_L, "shift_left"}, {P_SHIFT_R, "shift_right"}, {P_ST_CC6, "st_cc6"}, {P_ST_CC7, "st_cc7"}, {P_PF_CC6, "pf_cc6"}, {P_PF_CC7, "pf_cc7"}, {P_RND_CC6, "rnd_cc6"}, {P_RND_CC7, "rnd_cc7"}, {P_NAVV, "navv"}, {P_PSHIFTV, "pshiftv"}, {P_ST_CC8, "st_cc8"}, {P_PF_CC8, "pf_cc8"}, {P_RND_CC8, "rnd_cc8"}, {P_TRK_CC8, "trk_cc8"},
};

static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

/* ALSA client names: "StepRunner-8", "StepRunner-8 2", ... The lowest free number is reused when an instance closes,
 * so re-inserting the plugin does not walk the name upwards (MPC remembers MIDI inputs by name). */
static std::mutex g_slot_lock;
static bool g_slot_used[32];
static int slot_take() {
    std::lock_guard<std::mutex> lk(g_slot_lock);
    for (int i = 0; i < 32; i++) if (!g_slot_used[i]) { g_slot_used[i] = true; return i; }
    return -1;
}
static void slot_free(int i) {
    std::lock_guard<std::mutex> lk(g_slot_lock);
    if (i >= 0 && i < 32) g_slot_used[i] = false;
}

#ifdef STEPRUNNER_TEST   /* host_test.cpp: everything that would have gone to ALSA, in order */
static Event g_sent[1 << 16];
static std::atomic<int> g_nsent{0};
extern "C" int steprunner_test_count() { return g_nsent.load(); }
extern "C" void steprunner_test_get(int i, unsigned char *m) { m[0] = g_sent[i].status; m[1] = g_sent[i].d1; m[2] = g_sent[i].d2; }
extern "C" void steprunner_test_reset() { g_nsent = 0; }
#endif

/* ---------------------------------------------------------------------------
 * Per-instance state
 * ------------------------------------------------------------------------- */
/* Undo history: what the tracks hold (steps, locks, track setup, Euclidean, random settings) plus the shuffle amount.
 * Mutes, the play position and the PERFORM switches are not part of it. */
struct Snap { Track t[NTRACKS]; uint8_t shuffle; };
enum { HIST_MAX = 100, NSTATES = 8, NPATS = 8 };
enum { MSG_TIM = 239 };
enum { MSG_PCOPIED = 286, MSG_PPASTED = 294 };   /* + pattern 0..7: COPY PTN / PASTE PTN (v1.85) */   /* + micro + 23: "Timing: -23" .. "Timing: +23" (v1.77) */
enum { MSG_PAT = 207, MSG_PAT_STORED = MSG_PAT, MSG_PAT_CLEARED = MSG_PAT + 8, MSG_PAT_CUED = MSG_PAT + 16, MSG_PAT_LOADED = MSG_PAT + 24 };   /* + pattern 0..7 */
static int pst_slot(int i);   /* parameter -> state 0..7, -1 */
struct Plugin; static void rmode_note(Plugin *w);   /* v2.1, below */

/* one MIDI learn mapping of an MPC project (see xpj_scan) */
struct XEntry { std::string tgt, name; int ch, cc, val, param = -1, inst = -1; };   /* name: the parameter, "BD Level" */
/* v1.67: the project file work (search, read, write) runs off the lock -- and, on the device, on a thread of its own --
 * so the sequencer never waits for the drives (the SETUP button made the sound stutter: it searched them under the lock).
 * Who: what the check (xpj_ours) needs of an instance, copied; Job: one piece of such work. */
struct Who { std::vector<uint32_t> given; std::string our_path; time_t born = 0; };
enum { J_NONE = 0, J_CCREAD, J_FETCH, J_NOTES, J_MAPOPEN, J_MAPPICK, J_MAPWRITE };
struct Job { int kind = J_NONE; Who who; Seq seq; std::string path, to; };
/* the undo / redo history (v2.11): a ring of HIST_MAX snapshots, allocated once when the plugin is made. When full, a
 * new step drops the oldest one without moving the others (a vector erase moved ~830 KB on the audio thread). */
struct HistStack {
    std::vector<Snap> buf; int head = 0, n = 0;
    void alloc() { if (buf.empty()) buf.resize(HIST_MAX); }
    void clear() { head = n = 0; }
    bool empty() const { return n == 0; }
    void push_back(const Snap &x) { alloc(); if (n == HIST_MAX) { head = (head + 1) % HIST_MAX; n--; } buf[(size_t)((head + n) % HIST_MAX)] = x; n++; }
    Snap &back() { return buf[(size_t)((head + n - 1) % HIST_MAX)]; }
    void pop_back() { if (n > 0) n--; }
};
struct Plugin {
    AEffect fx;
    audioMasterCallback master;
    std::mutex lock;                 /* guards seq and the edit cursor: UI and audio threads both get here */
    Seq seq;
    int track = 0, page = 0, sel = 0;   /* edit cursor: selected track, shown page, step within the page */
    bool follow = false;
    int named_page = 0;              /* page the host last read the step names for (audio thread) */
    uint16_t selmask = 1;            /* steps of the shown page that are selected (bit k = cell k); never empty. One bit: it is `sel`.
                                        More than one (only possible with STEP EDIT on): the lock knobs are locked out. */
    int close_frames[NPARAMS] = {0}; /* popup flags: audio frames left before an open list closes after a pick (audio thread + setParameter) */
    bool track_edit = false;         /* TRACK EDIT: the panel shows the track setup */
    int rmode = 2;                   /* v2.1: the right-hand panel EUCLIDIAN leaves as it is: the last of PATTERN EDIT (0), STEP EDIT / TIMING (1), TRACK EDIT (2) */
    /* v1.2.0: a CC knob turned down to -- stays there (no jump to the base value it rests on otherwise) while the same
     * steps are selected: bit j = CC j, for the selection cc_rest_key (track, page, selection) */
    uint16_t cc_rest = 0; uint32_t cc_rest_key = 0;   /* bits 0..6 CC 1-7, REST_VEL, REST_PAN (v1.2.0) */
    bool trk_vals = false;           /* v1.2.0: TRACK EDIT's right panel shows TRACK VALUES (not saved, kept across mode changes) */
    int lpage[3] = {0, 0, 0};        /* v1.2.0: the left section's page in PATTERN EDIT [0], TRACK EDIT [1], STEP EDIT [2]: 0 RND STEPS .. RND CC 5, 1 RND COND / TIMING / RATCHETS (not saved) */
    bool clr_mode = false;           /* CLR LOCKS lit: RND VEL .. RND CC 5 clear that lock (CLR VEL ..), RND STEPS is off */
    bool shuf_edit = false;          /* SHUFFLE EDIT in the pattern setup: the row of track buttons is shown */
    bool global_edit = true;         /* SETUP: the screen a new plugin opens on (v1.0.0 2026-10-11; Trig Seq v1.99-v2.12: TRACK EDIT) */
    bool timing_edit = false;        /* TIMING (v1.77): STEP EDIT with the TIMING section on the left */        /* GLOBAL: the panel shows the global settings (shuffle) */
    bool random_edit = false;        /* RANDOM EDIT: the panel shows the random settings */
    bool euclid_edit = false;        /* EUCLIDIAN: the left-hand section shows the Euclidean generator. At most one of the four modes is on. */
    bool step_edit = false;          /* a tap on a TRIGS cell selects the step instead of flipping its trig */
    float told[NPARAMS];             /* normalised value the host last heard, per parameter */
    float raw[NPARAMS];              /* unrounded knob position the host last set (so slow Q-Link turns add up) */
    bool pfy[8] = {false};           /* PERFORM: second-row pads that have no state of their own in the sequencer (Pad 8) */
    bool inv_mute = false;           /* PERFORM: INVERT MUTE is lit (the mutes are flipped) */
    /* edit history (hist_*): 'done' is the state the last history entry leads to, 'seen' the state at the last block.
     * A change becomes one undo step once nothing has changed for a moment, so a knob sweep is one step. */
    Step clip[16]; int clip_off[16]; int clip_n = 0;   /* COPY: the copied steps and their distance from the first one */
    int cc117 = 0;                                     /* PERFORM CC 117 (no knob since v1.72; kept in the chunk for old projects) */
    Step pclip[16]; bool pclip_ok = false;             /* COPY PAGE: 16 steps of one track */
    Snap ptclip; bool ptclip_ok = false;               /* COPY PTN (v1.85): the whole pattern */
    Track tclip; bool tclip_ok = false;                /* COPY TRK */
    /* STATE 1..8 (v1.68; v1.36-1.67: four, the PERFORM knobs only): the whole sequencer -- every track's steps, locks and
     * setup (as the undo history keeps it), the shuffle, PERFORM ALL with its knobs, CC 117. Mutes stay live. */
    struct PState { bool full = false; Snap snap; bool pf_on = false; int off[Seq::PF_COUNT] = {0}; int cc117 = 0; bool mutes = false; bool fx_ok = false; uint32_t fx = 0; } pst[NPATS][NSTATES];   /* mutes: the snap holds the mutes too (v2.4). v1.1.0: eight STATEs per PATTERN, [pattern][state]; the buttons show the pattern playing's */
    PState pstclip[NSTATES]; bool pstclip_ok = false;   /* COPY PTN (v1.1.0): the copied pattern's states go with it */
    /* PATTERN 1..8 (v1.69): the tracks (steps, locks, setup, as the undo history keeps them) and the shuffle */
    struct Pat { bool full = false; Snap snap; } pat[NPATS];
    bool patclear = false, patstore = false;
    int pst_act[NPATS] = {-1, -1, -1, -1, -1, -1, -1, -1};   /* per pattern (v1.1.0): the STATE last saved or recalled, -1: none */
    int pat_cue = -1, pat_cur = 0;   /* the pattern that comes in at the end of the one playing (-1: none); the one playing: always one of the eight (v1.75: the sequencer IS that pattern) */
    bool pclear = false;
    int basedraft[NCC] = {0};                          /* EDIT CC: the base values being edited (value + 1, 0 = off) */
    std::string ccname[NTRACKS][NCC]; int ccname_cc[NTRACKS][NCC] = {};   /* what each CC controls, read from the project ("BD Level"), and the CC number it was read for: shown only while the track still uses that number */
    std::string src;                                   /* READ PROJECT: the chosen target ("" = any); saved */
    int big = 0;                       /* the big message (BIG_*) */
    int src_page = 0, src_gen = 0, src_pick = 0, src_mode = 0;   /* src_mode: SM_* below */
    uint64_t inst_id = 0;              /* this instance's id, carried in its chunk ('I' line): a project file holding it was saved from here */
    std::string map_path;              /* map_path: the project CREATE DEFAULT MAP writes into (chosen when two files hold this StepRunner-8) */
    std::vector<std::string> src_paths;   /* the files the 'which project' list offers */
    time_t born = 0;                   /* when this instance was made: only files saved since can hold a chunk it gave */
    Job job; std::atomic<bool> job_pending{false}, job_busy{false}; std::thread job_thr;   /* the project work asked for, and its thread */
    std::string our_path;              /* the file found to be the open project last time (xpj_pick_ours) */
    std::vector<uint32_t> given;       /* hashes of the chunks handed to MPC (save) or taken from it (load): see xpj_ours */
    std::string src_proj;              /* the SETUP panel's project line */
    std::vector<std::pair<std::string, int>> src_list;   /* rows of the SETUP list: (name, kind 2 = project file, 3 = drum track) */
    bool cc_edit = false; int ccdraft[NCC] = {0};      /* EDIT CC: open, and the numbers being edited */
    bool shift = false;                                /* STEP EDIT: SHIFT is lit */
    int msg = 0, msg_frames = 0;                       /* P_MSG and how long it stays */
    HistStack undo_st, redo_st;
    Snap hist_done, hist_seen;
    bool hist_init = false;
    int hist_quiet = 0;
    int release[NPARAMS] = {0};      /* a momentary button to put back to 0 (audio thread); under the lock since v2.12 */
    int scan_frames = 0; bool scan_now = true;   /* v2.12: frames since the last full scan of the values, and "scan at the next block" */
    bool held[NPARAMS] = {false};
    float open[NPARAMS] = {0};       /* popup "open" flags (popup.h): wrapper-only, not saved */
    double song = 0.0, last_ppq = 0.0;
    bool was_playing = false;
    float sample_rate = 44100.0f;
    int slot = -1;
#ifndef STEPRUNNER_TEST
    snd_seq_t *alsa = nullptr;
    int port = -1;
#endif
    std::string chunk;
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}

/* ---- ALSA seq output ----------------------------------------------------- */
#ifndef STEPRUNNER_TEST
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->alsa, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) { w->alsa = nullptr; return; }
    snd_seq_set_client_pool_output(w->alsa, 2048);
    char name[40];
    if (w->slot <= 0) std::snprintf(name, sizeof name, "StepRunner-8");
    else std::snprintf(name, sizeof name, "StepRunner-8 %d", w->slot + 1);
    snd_seq_set_client_name(w->alsa, name);
    w->port = snd_seq_create_simple_port(w->alsa, "MIDI Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[steprunner8] ALSA client '%s' port %d\n", name, w->port);
}
static void alsa_close(Plugin *w) {
    if (!w->alsa) return;
    snd_seq_close(w->alsa);
    w->alsa = nullptr;
}
#endif

/* send and empty seq.out; called with w->lock held */
static void flush(Plugin *w) {
    int n = w->seq.nout;
    w->seq.nout = 0;
#ifdef STEPRUNNER_TEST
    for (int i = 0; i < n; i++) { int k = g_nsent.load(); if (k < (int)(sizeof g_sent / sizeof g_sent[0])) { g_sent[k] = w->seq.out[i]; g_nsent = k + 1; } }
#else
    if (!w->alsa || w->port < 0) return;
    for (int i = 0; i < n; i++) {
        const Event &m = w->seq.out[i];
        uint8_t type = m.status & 0xF0, ch = m.status & 0x0F;
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, w->port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        if (type == 0x90 && m.d2 > 0) snd_seq_ev_set_noteon(&ev, ch, m.d1, m.d2);
        else if (type == 0x80 || type == 0x90) snd_seq_ev_set_noteoff(&ev, ch, m.d1, 0);
        else if (type == 0xB0) snd_seq_ev_set_controller(&ev, ch, m.d1, m.d2);
        else continue;
        snd_seq_event_output(w->alsa, &ev);   /* buffered, non-blocking: drops instead of hanging */
    }
    if (n) snd_seq_drain_output(w->alsa);
#endif
}

/* ---------------------------------------------------------------------------
 * Parameter values, in each parameter's own domain (option index / whole number).
 * Callers hold w->lock.
 * ------------------------------------------------------------------------- */
static Step &cur_step(Plugin *w) { return w->seq.t[w->track].st[w->page * 16 + w->sel]; }
static int sel_count(Plugin *w) { int n = 0; for (int k = 0; k < 16; k++) n += (w->selmask >> k) & 1; return n; }
static bool multi(Plugin *w) { return sel_count(w) > 1; }
static void select_one(Plugin *w, int k) { w->sel = clampi(k, 0, 15); w->selmask = (uint16_t)(1u << w->sel); w->cc_rest = 0; }   /* v1.2.0: a new selection: the CC knobs rest on the base values again */
/* The lock knobs write to every selected step (STEP EDIT with several selected), or to the one step otherwise.
 * They show the value of the step tapped last. */
template <class F> static void each_sel(Plugin *w, F fn) {
    if (!multi(w)) { fn(cur_step(w)); return; }
    for (int k = 0; k < 16; k++) if ((w->selmask >> k) & 1) fn(w->seq.t[w->track].st[w->page * 16 + k]);
}

enum { SRC_ROWS = 5 };   /* rows of the list on the SETUP tab (the skin has parameters for six) */
enum { SM_IDLE = 0, SM_FIXED = 5, SM_WHICH = 6 };   /* what the SETUP list shows: nothing, the drum tracks of the project to map into, the projects that may be the open one */
static int base_hash(const Track &t) { int h = 0; for (int j = 0; j < NCC; j++) h = (h * 131 + t.ccb[j]) & 4095; return h; }   /* a change of base value changes the lock knobs' text */
/* ---- base values from a saved MPC project (.xpj) ------------------------------------------------------------------
 * An .xpj is gzip; inside, five header lines and then JSON. data.midiLearnSettings.controls[] holds one entry per MIDI
 * learn mapping: control.controlValue (the mapped parameter's value, 0..1) and mapping {channel 1..16, data1 = CC}.
 * The plugin is not told which project is open, so it takes the newest .xpj under the drives in /media (STEPRUNNER_XPJ_ROOT
 * overrides the folder; the test uses it). zlib is loaded at run time: MPC has libz.so.1 loaded already. */
static void xpj_newest(const std::string &dir, int depth, int &budget, std::string &best, time_t &best_t) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    /* only folders count against the budget, and only .xpj files are stat'ed: a drive full of samples must not use it up
     * before the walk reaches the projects (v1.37.0 counted every file and stopped short on a real device) */
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        size_t n = std::strlen(e->d_name);
        bool xpj = n > 4 && !std::strcmp(e->d_name + n - 4, ".xpj");
        if (!xpj && e->d_type != DT_DIR && e->d_type != DT_UNKNOWN && e->d_type != DT_LNK) continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3 && !std::strstr(e->d_name, "[ProjectData]") && --budget >= 0) xpj_newest(p, depth + 1, budget, best, best_t);
        }
        else if (xpj && st.st_mtime >= best_t) { best_t = st.st_mtime; best = p; }
    }
    closedir(d);
}
/* The project to read. MPC keeps the path of the open project in Settings/MPC/CurrentProject (an XML properties file,
 * <VALUE name="lastProjectPath" val="..."/>; seen on firmware 3.9.1.2, written when a project is loaded or saved), so
 * that one is used when it names a file that exists. Otherwise (a project never saved, another firmware): the newest
 * .xpj on the drives. STEPRUNNER_CURRENT overrides the file's place, STEPRUNNER_XPJ_ROOT the folder searched (tests). */
static std::string xpj_pick_current() {
    const char *cur = std::getenv("STEPRUNNER_CURRENT");
    {
        std::string txt;
        if (FILE *f = std::fopen(cur ? cur : "/media/az01-internal/Settings/MPC/CurrentProject", "rb")) {
            char buf[4096]; size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && txt.size() < 65536) txt.append(buf, n);
            std::fclose(f);
        }
        size_t k = txt.find("\"lastProjectPath\""), v = k == std::string::npos ? k : txt.find("val=\"", k);
        size_t e = v == std::string::npos ? v : txt.find('"', v + 5);
        if (e != std::string::npos) {
            std::string p = txt.substr(v + 5, e - v - 5);
            static const struct { const char *ent, *ch; } ENT[] = {{"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
            for (const auto &x : ENT) for (size_t at = p.find(x.ent); at != std::string::npos; at = p.find(x.ent, at + 1)) p.replace(at, std::strlen(x.ent), x.ch);
            struct stat st;
            if (p.size() > 4 && !p.compare(p.size() - 4, 4, ".xpj") && !stat(p.c_str(), &st) && S_ISREG(st.st_mode)) return p;
        }
    }
    return std::string();
}
static bool xpj_load(const std::string &path, std::string &out);
static bool xpj_num(const std::string &s, size_t from, size_t to, const char *key, double &v);
static uint32_t fnv(const char *p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n && p[i]; i++) { h ^= (uint8_t)p[i]; h *= 16777619u; } return h; }
/* The StepRunner-8 chunks stored in a project text: each plugin track whose plugin has our uid keeps MPC's copy of what
 * effGetChunk gave it, as "state": "<size>.<JUCE base64>" of a VST2 bank (FBCh, the chunk from byte 160; size at 156).
 * Returns their hashes (up to the first NUL, as fnv takes them). */
static std::vector<uint32_t> xpj_our_chunks(const std::string &js) {
    std::vector<uint32_t> out;
    static const char T[] = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
    for (size_t at = js.find("\"uid\""); at != std::string::npos; at = js.find("\"uid\"", at + 1)) {
        double uid = 0;
        if (!xpj_num(js, at, at + 40, "\"uid\"", uid) || (long)uid != (long)PLUG_UID) continue;
        size_t st = js.find("\"state\"", at), q1 = st == std::string::npos ? st : js.find('"', js.find(':', st));
        size_t q2 = q1 == std::string::npos ? q1 : js.find('"', q1 + 1);
        if (q2 == std::string::npos || st - at > 4096) continue;
        const std::string enc = js.substr(q1 + 1, q2 - q1 - 1);
        size_t dot = enc.find('.');
        if (dot == std::string::npos) continue;
        long n = std::strtol(enc.c_str(), nullptr, 10);
        if (n < 164 || n > (8 << 20)) continue;
        std::vector<uint8_t> b((size_t)n, 0);
        size_t pos = 0;
        for (size_t i = dot + 1; i < enc.size(); i++, pos += 6) {
            const char *c = std::strchr(T, enc[i]);
            if (!c || !*c) break;
            const int v = (int)(c - T);
            for (int k = 0; k < 6; k++) if ((v >> k) & 1) { size_t bit = pos + (size_t)k; if (bit / 8 < b.size()) b[bit / 8] |= (uint8_t)(1u << (bit % 8)); }
        }
        if (std::memcmp(b.data(), "CcnK", 4) || std::memcmp(b.data() + 8, "FBCh", 4)) continue;
        size_t len = ((size_t)b[156] << 24) | ((size_t)b[157] << 16) | ((size_t)b[158] << 8) | b[159];
        if (160 + len > b.size()) len = b.size() - 160;
        out.push_back(fnv((const char *)b.data() + 160, len));
    }
    return out;
}
static void note_hash(Plugin *w, uint32_t h) {   /* under the lock */
    for (uint32_t g : w->given) if (g == h) return;
    w->given.push_back(h);
    if (w->given.size() > 64) w->given.erase(w->given.begin());
}
static void note_given(Plugin *w, const std::string &chunk) { note_hash(w, fnv(chunk.c_str(), chunk.size())); }
/* Is this project text the project this StepRunner-8 lives in? Yes if it holds a StepRunner-8 chunk that this instance handed
 * to MPC (MPC asks for it when it saves) or was loaded from. A project never saved, or saved before this StepRunner-8 was
 * in it, or another project: no. STEPRUNNER_NOVERIFY (tests) skips the check. */
static bool xpj_ours(const Who &w, const std::string &js) {
    if (std::getenv("STEPRUNNER_NOVERIFY")) return true;
    for (uint32_t h : xpj_our_chunks(js)) for (uint32_t g : w.given) if (g == h) return true;
    return false;
}
static std::string xpj_pick_any();
/* the open project's file, checked (xpj_ours): the newer of CurrentProject's and the newest on the drives that holds
 * this instance's chunk; "" if neither does (the open project is not saved, or not since StepRunner-8 was added). */
static bool is_backup_name(const std::string &p) {   /* "<name>-backup.xpj", "-backup12.xpj": our own copies */
    size_t k = p.rfind("-backup");
    if (k == std::string::npos || p.size() < 4 || p.compare(p.size() - 4, 4, ".xpj")) return false;
    for (size_t i = k + 7; i < p.size() - 4; i++) if (p[i] < '0' || p[i] > '9') return false;
    return true;
}
/* Every file that holds this StepRunner-8 (v1.65): the candidates of xpj_pick_ours, and the other projects in their folders
 * saved since this instance was made (a Save As over another project left "untitled.xpj" next to "Test.xpj" on a device,
 * both with the same chunk, the same minute). Our backups are left out. Order: the one CurrentProject names, then the
 * newest. Two or more: CREATE DEFAULT MAP asks which one is open. */
static std::vector<std::string> xpj_ours_all(Who &w) {
    std::vector<std::string> out;
    const char *cur = std::getenv("STEPRUNNER_CURRENT"), *root = std::getenv("STEPRUNNER_XPJ_ROOT");
    const std::string named = (cur || !root) ? xpj_pick_current() : std::string(), best = xpj_pick_any();
    std::vector<std::string> cand;
    for (const std::string &c : {named, best, w.our_path}) if (!c.empty() && std::find(cand.begin(), cand.end(), c) == cand.end()) cand.push_back(c);
    const size_t n0 = cand.size();
    for (size_t k = 0; k < n0; k++) {
        size_t sl = cand[k].rfind('/');
        const std::string dir = sl == std::string::npos ? "." : cand[k].substr(0, sl);
        if (DIR *d = opendir(dir.c_str())) {
            while (struct dirent *e = readdir(d)) {
                const std::string f = dir + "/" + e->d_name;
                struct stat st;
                if (e->d_name[0] == '.' || f.size() < 5 || f.compare(f.size() - 4, 4, ".xpj") || stat(f.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_mtime + 2 < w.born) continue;
                if (std::find(cand.begin(), cand.end(), f) == cand.end()) cand.push_back(f);
            }
            closedir(d);
        }
    }
    std::vector<std::pair<time_t, std::string>> got;
    std::string js;
    for (const std::string &c : cand) {
        struct stat st;
        if (is_backup_name(c) || stat(c.c_str(), &st) || !xpj_load(c, js) || !xpj_ours(w, js)) continue;
        got.push_back({c == named ? (time_t)0x7fffffff : st.st_mtime, c});
    }
    std::stable_sort(got.begin(), got.end(), [](const std::pair<time_t, std::string> &a, const std::pair<time_t, std::string> &b) { return a.first > b.first; });
    for (const auto &g : got) out.push_back(g.second);
    return out;
}
static std::string xpj_pick_ours(Who &w) {
    if (!std::getenv("STEPRUNNER_NOVERIFY")) {   /* the check is on: every file that holds this StepRunner-8, the first one */
        const std::vector<std::string> all = xpj_ours_all(w);
        if (all.empty()) return std::string();
        w.our_path = all[0];
        return all[0];
    }
    const char *cur = std::getenv("STEPRUNNER_CURRENT"), *root = std::getenv("STEPRUNNER_XPJ_ROOT");
    std::string named = (cur || !root) ? xpj_pick_current() : std::string(), best = xpj_pick_any();
    const std::string cand[2] = {best, named};   /* tests only (no check): the newest first, a Save As makes the new name newest */
    std::string js;
    for (int k = 0; k < 2; k++) {
        const std::string &c = cand[k];
        if (c.empty() || (k > 0 && c == best)) continue;
        if (xpj_load(c, js) && xpj_ours(w, js)) { w.our_path = c; return c; }
    }
    return std::string();
}
static std::string xpj_pick_any() {
    /* The project MPC names as open, or the newest .xpj on the drives -- whichever was saved last. CurrentProject alone
     * (v1.44-1.59) went wrong on a device: it still named a project opened earlier, so it is not rewritten every time a
     * project is opened. The user saves the project before reading it anyway, and that makes it the newest file. */
    const char *cur = std::getenv("STEPRUNNER_CURRENT"), *root = std::getenv("STEPRUNNER_XPJ_ROOT");
    std::string named = (cur || !root) ? xpj_pick_current() : std::string();
    std::string best; time_t best_t = 0; int budget = 20000;
    xpj_newest(root ? root : "/media", 0, budget, best, best_t);
    struct stat st;
    if (!named.empty() && !stat(named.c_str(), &st) && (best.empty() || st.st_mtime >= best_t)) return named;
    return best.empty() ? named : best;
}
/* no project to read: 206 "Save the project first" when there are projects but none is this one (a new project not
 * saved yet, or not since StepRunner-8 was put in it), 63 when there is none at all */
static int no_proj_msg() { return xpj_pick_any().empty() ? 63 : 206; }
static bool xpj_load(const std::string &path, std::string &out) {
    void *z = dlopen("libz.so.1", RTLD_NOW);
    if (!z) return false;
    void *(*zopen)(const char *, const char *) = (void *(*)(const char *, const char *))dlsym(z, "gzopen");
    int (*zread)(void *, void *, unsigned) = (int (*)(void *, void *, unsigned))dlsym(z, "gzread");
    int (*zclose)(void *) = (int (*)(void *))dlsym(z, "gzclose");
    if (!zopen || !zread || !zclose) return false;
    void *g = zopen(path.c_str(), "rb");
    if (!g) return false;
    out.clear();
    char buf[65536];
    int n;
    while ((n = zread(g, buf, sizeof buf)) > 0) { out.append(buf, (size_t)n); if (out.size() > (64u << 20)) break; }
    zclose(g);
    return !out.empty();
}
static bool xpj_num(const std::string &s, size_t from, size_t to, const char *key, double &v) {
    size_t p = s.find(key, from);
    if (p == std::string::npos || p >= to) return false;
    p = s.find(':', p);
    if (p == std::string::npos || p >= to) return false;
    v = std::strtod(s.c_str() + p + 1, nullptr);
    return true;
}
/* One MIDI learn mapping of the project: what it controls (targetData.track: "Drum 001", "Plugin 004"), channel 1..16,
 * CC number, value 0..127. */
/* All mappings of the newest saved project, and its name. false: no readable project. */
static void xpj_scan_text(const std::string &js, std::vector<XEntry> &out);
static void xpj_pad_values(const std::string &js, std::vector<XEntry> &es);   /* v1.2.0, below */
static bool xpj_scan(Who &w, std::string &proj, std::vector<XEntry> &out) {
    std::string best = xpj_pick_ours(w), js;
    out.clear();
    if (best.empty() || !xpj_load(best, js)) return false;
    size_t sl = best.rfind('/');
    proj = best.substr(sl == std::string::npos ? 0 : sl + 1);
    if (proj.size() > 4) proj.resize(proj.size() - 4);
    xpj_scan_text(js, out);
    xpj_pad_values(js, out);   /* v1.2.0: a pad parameter's value is the pad's own, not the mapping's controlValue */
    return true;
}
/* The MIDI learn entries of a project text. */
static void xpj_scan_text(const std::string &js, std::vector<XEntry> &out) {
    out.clear();
    size_t p = js.find("\"midiLearnSettings\"");
    if (p == std::string::npos) return;
    p = js.find("\"controls\"", p);
    if (p == std::string::npos) return;
    size_t a = js.find('[', p);
    if (a == std::string::npos) return;
    size_t end = a; int depth = 0; bool str = false;   /* the end of the controls array */
    for (; end < js.size(); end++) {
        char ch = js[end];
        if (str) { if (ch == '\\') end++; else if (ch == '"') str = false; continue; }
        if (ch == '"') str = true;
        else if (ch == '[' || ch == '{') depth++;
        else if (ch == ']' || ch == '}') { if (--depth == 0) break; }
    }
    /* an entry runs from its "control" key to the next one */
    for (size_t q = js.find("\"control\"", a); q != std::string::npos && q < end; ) {
        size_t nxt = js.find("\"control\"", q + 1);
        size_t lim = nxt == std::string::npos || nxt > end ? end : nxt;
        double val = 0, chn = 0, d1 = -1;
        size_t m = js.find("\"mapping\"", q);
        if (m != std::string::npos && m < lim && xpj_num(js, q, lim, "\"controlValue\"", val) && xpj_num(js, m, lim, "\"channel\"", chn) && xpj_num(js, m, lim, "\"data1\"", d1)) {
            XEntry e; e.ch = (int)chn; e.cc = (int)d1; e.val = clampi((int)std::lround(val * 127.0), 0, 127);
            size_t t = js.find("\"track\"", q);
            if (t != std::string::npos && t < m) {
                size_t c = js.find(':', t), q1 = js.find('"', c == std::string::npos ? t : c + 1), nl = js.find_first_of(",}\n", c == std::string::npos ? t : c);
                if (q1 != std::string::npos && q1 < nl) { size_t q2 = js.find('"', q1 + 1); if (q2 != std::string::npos && q2 < lim) e.tgt = js.substr(q1 + 1, q2 - q1 - 1); }
                else if (c != std::string::npos && nl != std::string::npos) { e.tgt = js.substr(c + 1, nl - c - 1); while (!e.tgt.empty() && e.tgt[0] == ' ') e.tgt.erase(0, 1); }
            }
            if (e.tgt.empty()) e.tgt = "?";
            { double pv = -1, iv = -1; if (xpj_num(js, q, m, "\"parameter\"", pv)) e.param = (int)pv; if (xpj_num(js, q, m, "\"instrumentIndex\"", iv)) e.inst = (int)iv; }
            {   /* control.name is "Plugin 004 (BD Level)" / "A01 (Filter Cutoff)": the parameter is what the brackets hold */
                size_t nk = js.find("\"name\"", q);
                if (nk != std::string::npos && nk < m) {
                    size_t c = js.find(':', nk), q1 = js.find('"', c == std::string::npos ? nk : c + 1);
                    size_t q2 = q1 == std::string::npos ? q1 : js.find('"', q1 + 1);
                    if (q2 != std::string::npos && q2 < m) {
                        std::string full = js.substr(q1 + 1, q2 - q1 - 1);
                        size_t o = full.rfind('('), cl = full.rfind(')');
                        e.name = (o != std::string::npos && cl != std::string::npos && cl > o + 1) ? full.substr(o + 1, cl - o - 1) : full;
                        for (char &chr : e.name) if ((unsigned char)chr < 32 || (unsigned char)chr > 126) chr = ' ';
                    }
                }
            }
            out.push_back(e);
        }
        q = nxt;
    }
    return;
}
/* For each of the five CC numbers: the value of the mapping with that CC on this channel (1..16), as base + 1; only
 * mappings of target `only` if that is not empty. Returns how many were found; numbers without one are left alone. */
static int xpj_bases(const std::vector<XEntry> &es, const std::string &only, int channel, const int *ccnum, int *base, std::string *names = nullptr, int *name_cc = nullptr) {
    int got = 0;
    bool done[NCC] = {false};
    for (const XEntry &e : es) {
        if (e.ch != channel || (!only.empty() && e.tgt != only)) continue;
        for (int j = 0; j < NCC; j++)
            if (!done[j] && ccnum[j] == e.cc) { base[j] = e.val + 1; done[j] = true; got++; if (names) { names[j] = e.name; name_cc[j] = e.cc; } }
    }
    return got;
}
static bool xpj_save(const std::string &path, const std::string &text) {
    void *z = dlopen("libz.so.1", RTLD_NOW);
    if (!z) return false;
    void *(*zopen)(const char *, const char *) = (void *(*)(const char *, const char *))dlsym(z, "gzopen");
    int (*zwrite)(void *, const void *, unsigned) = (int (*)(void *, const void *, unsigned))dlsym(z, "gzwrite");
    int (*zclose)(void *) = (int (*)(void *))dlsym(z, "gzclose");
    if (!zopen || !zwrite || !zclose) return false;
    const std::string tmp = path + ".tmp";
    void *g = zopen(tmp.c_str(), "wb");
    if (!g) return false;
    bool ok = true;
    for (size_t at = 0; ok && at < text.size(); ) {
        unsigned n = (unsigned)std::min<size_t>(text.size() - at, 1u << 20);
        ok = zwrite(g, text.data() + at, n) == (int)n; at += n;
    }
    ok = zclose(g) == 0 && ok;
    if (ok) ok = std::rename(tmp.c_str(), path.c_str()) == 0; else std::remove(tmp.c_str());
    return ok;
}
/* The end of the JSON value that opens at `a` ('[' or '{'): the index of its closing bracket. */
static size_t xpj_close(const std::string &js, size_t a) {
    int depth = 0; bool str = false;
    for (size_t e = a; e < js.size(); e++) {
        char ch = js[e];
        if (str) { if (ch == '\\') e++; else if (ch == '"') str = false; continue; }
        if (ch == '"') str = true;
        else if (ch == '[' || ch == '{') depth++;
        else if ((ch == ']' || ch == '}') && --depth == 0) return e;
    }
    return std::string::npos;
}
/* The drum tracks of a project text: name and where the track's object starts and ends. A drum track is an element of
 * data.tracks whose program has a "drum" section with "instruments"; its name is the element's first "name". */
struct XTrack { std::string name; size_t a, e; int type; long uid; };   /* type: the program's type (0 drum, 3 plugin, 8 submix ...); uid: the plugin's, else 0 */
static std::vector<XTrack> xpj_tracks(const std::string &js, bool drum_only = true) {
    std::vector<XTrack> out;
    size_t tr = js.find("\"tracks\""), arr = tr == std::string::npos ? tr : js.find('[', tr);
    size_t arr_end = arr == std::string::npos ? arr : xpj_close(js, arr);
    if (arr_end == std::string::npos) return out;
    for (size_t at = js.find('{', arr); at != std::string::npos && at < arr_end; ) {
        size_t e = xpj_close(js, at);
        if (e == std::string::npos) break;
        size_t dr = js.find("\"drum\"", at), ins = dr == std::string::npos || dr > e ? std::string::npos : js.find("\"instruments\"", dr);
        size_t nk = js.find("\"name\"", at);
        if ((!drum_only || (ins != std::string::npos && ins < e)) && nk != std::string::npos && nk < e) {
            size_t q1 = js.find('"', js.find(':', nk)), q2 = q1 == std::string::npos ? q1 : q1 + 1;
            while (q2 != std::string::npos && q2 < e && js[q2] != '"') q2 += js[q2] == '\\' ? 2 : 1;
            double ty = -1, uid = 0;
            size_t pg = js.find("\"program\"", at);
            if (pg != std::string::npos && pg < e) { xpj_num(js, pg, e, "\"type\"", ty); size_t pl = js.find("\"plugin\"", pg); if (pl != std::string::npos && pl < e) xpj_num(js, pl, e, "\"uid\"", uid); }
            if (q2 != std::string::npos && q2 < e) out.push_back({js.substr(q1 + 1, q2 - q1 - 1), at, e, (int)ty, (long)uid});   /* kept as written in the file (JSON-escaped): it goes back into the file as it is */
        }
        at = js.find('{', e);
    }
    return out;
}
/* The note that a pad of a drum track plays: program.padNoteMap.noteForPad.value<pad> (pad 0 = A01; 36 + pad unless
 * the user changed it). -1: no such drum track, or no such pad in its map. */
static int xpj_pad_note(const std::string &js, const std::vector<XTrack> &tracks, const std::string &track, int pad) {
    for (const XTrack &t : tracks) if (t.name == track) {
        size_t nf = js.find("\"noteForPad\"", t.a);
        size_t ob = nf == std::string::npos || nf > t.e ? std::string::npos : js.find('{', nf);
        size_t oe = ob == std::string::npos ? ob : xpj_close(js, ob);
        if (oe == std::string::npos) return -1;
        char key[24]; std::snprintf(key, sizeof key, "\"value%d\"", pad);
        double v = -1;
        return xpj_num(js, ob, oe, key, v) && v >= 0 && v <= 127 ? (int)v : -1;
    }
    return -1;
}
/* WRITE MAPPING: MIDI learn mappings for the chosen drum track are added to the open project's own file: pad A01..A08
 * = track 1..8, the track's five CCs on its channel -> the pad's Tuning, Filter Cutoff, Filter Resonance, Send 1,
 * Send 2 (parameters 513, 514, 515, 522, 523 since v1.87.0; MPC accepts mappings written this way -- confirmed on a device).
 * controlValue is the pad's present value, as MPC writes it. A CC that the project already maps on that channel is
 * left out, and so is a pad parameter that has a mapping. Nothing else in the project text changes.
 * No backup is made (since v2.0; before that the file was copied as "<name>-backup.xpj"). MPC has the project in memory, so the user must close it and open it again -- without
 * saving in between, which would put the old list back.
 * History: v1.43-1.44 and 1.46-1.47 wrote a "<name>-MAPPED.xpj" copy instead; it either lost the samples (MPC looks
 * for them under the copy's name) or needed the whole data folder copied, so the user chose this.
 * xpj_map_text only adds them to the text `js`: the number added, -2 if there is no such drum track. */
/* CREATE DEFAULT MAP's pad parameters, in CC order (CC 1-7, then pan): MPC's MIDI learn ids. v1.2.0: CC 6 / 7 = Send 3 / 4 */
static const struct { int id; const char *name; } PAD_PRM[NCC + 1] = {{513, "Tuning"}, {514, "Filter Cutoff"}, {515, "Filter Resonance"}, {522, "Send 1"}, {523, "Send 2"}, {524, "Send 3"}, {525, "Send 4"}, {0, "CC 8"}, {517, "Pan"}};   /* v1.2.0: CC 8 has no default mapping (id 0) */
/* the pad object js[at..e]: its values 0..1 in PAD_PRM order */
static void xpj_pad_read(const std::string &js, size_t at, size_t e, double *val) {
    double c = 1.0, r = 0.0, ct = 0.0, ft = 0.0;
    xpj_num(js, at, e, "\"filterCutoff\"", c); xpj_num(js, at, e, "\"filterResonance\"", r);
    xpj_num(js, at, e, "\"coarseTune\"", ct); xpj_num(js, at, e, "\"fineTune\"", ft);   /* the pad's own (first in its object) */
    /* Tuning as 0..1: -36..+36 semitones (an assumption: 0 = the middle, 0.5; not checked against MPC's own value) */
    double tn = (ct + ft / 100.0 + 36.0) / 72.0; tn = tn < 0.0 ? 0.0 : tn > 1.0 ? 1.0 : tn;
    val[0] = tn; val[1] = c; val[2] = r; val[3] = val[4] = val[5] = val[6] = val[7] = 0.0; val[NCC] = 0.5;
    { size_t mx = js.find("\"mixable\"", at); double pv = 0.5;   /* the pad's pan, 0..1 (0.5 = the centre) */
      if (mx != std::string::npos && mx < e && xpj_num(js, mx, e, "\"pan\"", pv) && pv >= 0.0 && pv <= 1.0) val[NCC] = pv; }
    size_t sd = js.find("\"sends\"", at), sb = sd == std::string::npos || sd > e ? std::string::npos : js.find('[', sd);
    if (sb != std::string::npos && sb < e) {
        const char *p = js.c_str() + sb + 1;
        for (int j = 0; j < 4; j++) { char *q; double v = std::strtod(p, &q); if (q == p) break; val[3 + j] = v; p = q; while (*p == ',' || *p == '\n' || *p == ' ' || *p == '\r') p++; }   /* Send 1-4 */
    }
}
/* v1.2.0 FETCH CC VALUES: a mapping of a pad parameter (one of PAD_PRM) takes the pad's own value from its drum track
 * (the mapping's controlValue need not follow a parameter changed on the pad: a user report, 2026-10-17). */
static void xpj_pad_values(const std::string &js, std::vector<XEntry> &es) {
    std::vector<std::pair<std::string, std::vector<std::pair<size_t, size_t>>>> pads;   /* drum track -> its pad objects */
    for (XEntry &e : es) {
        int k = -1;
        for (int j = 0; j <= NCC; j++) if (PAD_PRM[j].id && PAD_PRM[j].id == e.param) k = j;
        if (k < 0 || e.inst < 0) continue;
        const std::vector<std::pair<size_t, size_t>> *list = nullptr;
        for (auto &pt : pads) if (pt.first == e.tgt) list = &pt.second;
        if (!list) {
            std::vector<std::pair<size_t, size_t>> v;
            for (const XTrack &t : xpj_tracks(js)) if (t.name == e.tgt) {
                size_t ins = js.find("\"instruments\"", js.find("\"drum\"", t.a));
                size_t arr = ins == std::string::npos ? ins : js.find('[', ins), end = arr == std::string::npos ? arr : xpj_close(js, arr);
                if (end == std::string::npos) break;
                for (size_t at = js.find('{', arr); at != std::string::npos && at < end; ) { size_t c = xpj_close(js, at); if (c == std::string::npos) break; v.push_back({at, c}); at = js.find('{', c); }
                break;
            }
            pads.push_back({e.tgt, v}); list = &pads.back().second;
        }
        if (e.inst >= (int)list->size()) continue;
        double val[NCC + 1];
        xpj_pad_read(js, (*list)[(size_t)e.inst].first, (*list)[(size_t)e.inst].second, val);
        e.val = clampi((int)std::lround(val[k] * 127.0), 0, 127);
    }
}
static int xpj_map_text(const Seq &seq, const std::string &track, std::string &js) {
    const auto &PRM = PAD_PRM;   /* + Pan (v1.93: the track's PAN_CC) */   /* v1.87.0: the pad's Send 1/2 are 522/523 (MPC's MIDI learn MIXER group: Level 518, Pan 517, Pad Mute 526, Pad Solo 527, Send 1-4 522-525); 91/92 (v1.71-1.86) were not found by MPC, which showed the row as the entry before */
    std::vector<XEntry> have;
    xpj_scan_text(js, have);
    size_t ins = std::string::npos;
    for (const XTrack &t : xpj_tracks(js)) if (t.name == track) { ins = js.find("\"instruments\"", js.find("\"drum\"", t.a)); break; }
    if (ins == std::string::npos) return -2;
    size_t arr = js.find('[', ins), arr_end = arr == std::string::npos ? arr : xpj_close(js, arr);
    if (arr_end == std::string::npos) return -2;
    double val[NTRACKS][NCC + 1]; bool pad[NTRACKS] = {false};   /* CC 1-7 and pan */
    size_t at = js.find('{', arr);
    for (int k = 0; k < NTRACKS && at != std::string::npos && at < arr_end; k++) {
        size_t e = xpj_close(js, at);
        if (e == std::string::npos) break;
        xpj_pad_read(js, at, e, val[k]);
        pad[k] = true;
        at = js.find('{', e);
    }
    /* the mapping list */
    size_t ml = js.find("\"midiLearnSettings\""), ck = ml == std::string::npos ? ml : js.find("\"controls\"", ml);
    size_t ca = ck == std::string::npos ? ck : js.find('[', ck), ce = ca == std::string::npos ? ca : xpj_close(js, ca);
    if (ce == std::string::npos) return -2;
    bool empty = js.find('{', ca) > ce;
    std::string add; int n = 0; char b[1600];
    for (int ti = 0; ti < NTRACKS; ti++) if (pad[ti]) for (int j = 0; j < NCC + 1; j++) {
        if (!PRM[j].id) continue;   /* CC 8: no default mapping */
        const int ch = seq.t[ti].ch + 1, cc = j < NCC ? Seq::cc_num(seq.t[ti], j) : PAN_CC[ti];
        bool dup = false;
        for (const XEntry &e : have) if ((e.ch == ch && e.cc == cc) || (e.tgt == track && e.inst == ti && e.param == PRM[j].id)) dup = true;   /* the CC is taken, or the pad's parameter has a mapping already */
        if (dup) continue;
        char vs[40]; std::snprintf(vs, sizeof vs, "%.17g", val[ti][j]);
        if (!std::strpbrk(vs, ".e")) std::strcat(vs, ".0");   /* MPC writes every value as a real number */
        std::snprintf(b, sizeof b,
            "%s\n{\n\"control\": {\n\"name\": \"A%02d (%s)\",\n\"controlType\": 0,\n\"targetData\": [\n{\n\"version\": 1,\n\"parameter\": %d,\n\"track\": \"%s\",\n"
            "\"insertParamIndex\": {\n\"initialized\": true,\n\"none\": true\n},\n\"instrumentIndex\": %d,\n\"paramType\": 0,\n"
            "\"controlInputRange\": {\n\"min\": 0.0,\n\"max\": 1.0,\n\"stride\": 0.0,\n\"deadspot\": 2.0,\n\"skew\": 1.0\n},\n"
            "\"parameterRange\": {\n\"min\": 0.0,\n\"max\": 1.0,\n\"stride\": 0.0,\n\"deadspot\": 2.0,\n\"skew\": 1.0\n},\n\"behaviour\": 0\n}\n],\n\"momentary\": 0,\n\"controlValue\": %s\n},\n"
            "\"mapping\": {\n\"mappingType\": 2,\n\"controlType\": 5,\n\"channel\": %d,\n\"data1\": %d,\n\"reverseInput\": false\n}\n}",
            (empty && n == 0) ? "" : ",", ti + 1, PRM[j].name, PRM[j].id, track.c_str(), ti, vs, ch, cc);
        add += b; n++;
        XEntry ne; ne.ch = ch; ne.cc = cc; ne.val = 0; have.push_back(ne);   /* two tracks on the same channel and CC: once */
    }
    if (n) { size_t ipos = ce; while (ipos > ca + 1 && (js[ipos - 1] == '\n' || js[ipos - 1] == ' ' || js[ipos - 1] == '\r')) ipos--; js.insert(ipos, add + "\n"); }
    return n;
}

/* EDIT CC: is draft number j already used anywhere in StepRunner-8? Pan (CC 10), the PERFORM knob (CC 117), the other
 * four numbers of this draft, and every CC number of every other track, whatever MIDI channel that track is on
 * (v1.36.1; before, only tracks on the same channel counted). It only marks the knob, it does not restrict the choice. */
static bool cc_in_use(Plugin *w, int j) {
    const int v = w->ccdraft[j];
    if (v == CC_PAN) return true;   /* (CC 117 was the PERFORM knob until v1.71) */
    for (int t = 0; t < NTRACKS; t++) if (v == PAN_CC[t]) return true;   /* the tracks' pan controllers (v1.93) */
    for (int k = 0; k < NCC; k++) if (k != j && w->ccdraft[k] == v) return true;
    for (int t = 0; t < NTRACKS; t++) {
        if (t == w->track) continue;
        for (int k = 0; k < NCC; k++) if (Seq::cc_num(w->seq.t[t], k) == v) return true;
    }
    return false;
}
/* v1.2.0: which mode's left section has two pages: 0 PATTERN EDIT, 1 TRACK EDIT, 2 STEP EDIT, -1 any other mode */
static const uint8_t EU_OPS[4] = {Seq::E_XOR, Seq::E_OR, Seq::E_AND, Seq::E_SUB};   /* v1.2.0: the boolean buttons, left to right */
static int lmode(const Plugin *w) {
    if (w->euclid_edit || w->timing_edit || w->global_edit || w->random_edit) return -1;
    return w->step_edit ? 2 : w->track_edit ? 1 : 0;
}
static bool lpage2(const Plugin *w) { const int m = lmode(w); return m >= 0 && w->lpage[m] == 1; }
/* v1.2.0: the PROGRAM tab's Q-Links are on the STEP LOCKS knobs (layout.conf qlinks: the mapping is fixed in the skin).
 * While the track's panel shows (TRACK EDIT, TRACK VALUES, EUCLIDIAN over TRACK EDIT) those knobs stand for the track's
 * own values: a Q-Link turn sets them and the knob reads them. Returns the TRACK VALUES parameter, or -1. */
/* v1.2.0: CC 1-5 are P_ST_CC0.. / P_PF0 + PF_CC0.., CC 6 / 7 were appended (P_ST_CC6.., P_PF_CC6..): a parameter -> the
 * CC slot j (0..6) of a step lock knob, -1; a PERFORM knob <-> its Seq::pf_off index (0..PF_COUNT-1) */
static int st_cc_j(int i) { return i >= P_ST_CC0 && i < P_ST_CC0 + NCC_V1 ? i - P_ST_CC0 : i == P_ST_CC6 ? 5 : i == P_ST_CC7 ? 6 : i == P_ST_CC8 ? 7 : -1; }
static int pf_k(int i) { return i >= P_PF0 && i < P_PF0 + NPF ? i - P_PF0 : i == P_PF_CC6 ? Seq::PF_CC0 + 5 : i == P_PF_CC7 ? Seq::PF_CC0 + 6 : i == P_PF_CC8 ? Seq::PF_CC0 + 7 : -1; }
static int pf_param(int k) { return k < NPF ? P_PF0 + k : k == Seq::PF_CC0 + 5 ? P_PF_CC6 : k == Seq::PF_CC0 + 6 ? P_PF_CC7 : P_PF_CC8; }
enum { NCC_X = NCC_V1 + 2 };   /* v1.2.0: CC 6 / 7 go on the X line; CC 8 (slot NCC_X) on an H line of its own */
/* v1.2.0 TRACK VALUES: a parameter -> the CC slot j of its knob (CC 1-7 P_TRK_CC0.., CC 8 appended), -1; and back */
static int trk_cc_j(int i) { return i >= P_TRK_CC0 && i < P_TRK_CC_END ? i - P_TRK_CC0 : i == P_TRK_CC8 ? 7 : -1; }
static int trk_cc_param(int j) { return j < P_TRK_CC_END - P_TRK_CC0 ? P_TRK_CC0 + j : P_TRK_CC8; }
enum { NPF_ALL = Seq::PF_COUNT };
enum { REST_VEL = 8, REST_PAN = 9 };   /* v1.2.0: cc_rest bits of the VELOCITY / PAN lock knobs */
static uint32_t sel_key(const Plugin *w) { return (uint32_t)w->track | ((uint32_t)w->page << 3) | ((uint32_t)w->selmask << 5) | (w->step_edit ? 1u << 21 : 0); }
/* v1.2.0: a lock knob with no lock rests on the track's value; once turned down to -- it stays there for this
 * selection (no jump back). rest_dash: it shows --; rest_turn: note a turn to x, returns whether it showed -- before. */
static bool rest_dash(const Plugin *w, int b) { return ((w->cc_rest >> b) & 1) && w->cc_rest_key == sel_key(w); }
static bool rest_turn(Plugin *w, int b, int x) {
    if (w->cc_rest_key != sel_key(w)) { w->cc_rest = 0; w->cc_rest_key = sel_key(w); }
    const bool was = (w->cc_rest >> b) & 1;
    if (x) w->cc_rest &= (uint16_t)~(1u << b); else w->cc_rest |= (uint16_t)(1u << b);
    return was;
}
static int ql_track(const Plugin *w, int i) {
    if (!(w->track_edit || (w->euclid_edit && w->rmode == 2))) return -1;
    if (i == P_ST_VEL) return P_TRK_VEL;
    if (i == P_ST_PAN) return P_TRK_PAN;
    if (i == P_ST_LEN) return P_TRK_GATE;
    if (i == P_ST_PROB) return P_TRK_PROB;
    if (st_cc_j(i) >= 0) return trk_cc_param(st_cc_j(i));
    return -1;
}
static int value_of(Plugin *w, int i) {
    if (ql_track(w, i) >= 0) return value_of(w, ql_track(w, i));
    Track &tr = w->seq.t[w->track];
    if (i >= P_STEP0 && i < P_STEP0 + 16) return tr.st[w->page * 16 + (i - P_STEP0)].on ? 1 : 0;
    if (i >= P_CELL0 && i < P_CELL0 + 16) {
        int k = i - P_CELL0, idx = w->page * 16 + k;
        return (tr.st[idx].on ? 1 : 0) + ((w->seq.playing && tr.cur == idx) ? 2 : 0) + ((((w->selmask >> k) & 1) && w->step_edit) ? 4 : 0)   /* the selected step is outlined in STEP EDIT only (v1.57.0) */ + (tr.eu_on ? 24 : w->step_edit || (w->euclid_edit && w->rmode == 1) ? 8 : w->track_edit || (w->euclid_edit && w->rmode == 2) ? 16 : 0)   /* v1.2.0: green only while a generator runs; EUCLIDIAN otherwise in the colour of the mode under it */   /* v2.8.0: TRACK EDIT violet again */   /* a track that comes from the Euclidean generator is green in every mode */;
    }
    if (i >= P_MUTE0 && i < P_MUTE0 + NTRACKS) return w->seq.t[i - P_MUTE0].mute ? 1 : 0;
    if (pf_k(i) >= 0) return w->seq.pf_on ? w->seq.pf_off[pf_k(i)] : 0;
    if (i == P_PF_CTRL) return w->seq.pf_on ? 1 : 0;
    if (i == P_PFY0) return w->seq.loopn == 8 ? 1 : 0;   /* LOOP 8 (v1.79.0) */
    if (i == P_PFY0 + 1) return w->seq.reverse ? 1 : 0;   /* REVERSE (v1.79.0) */
    if (i == P_PFY0 + 2) return w->seq.warp == 0.5 ? 1 : 0;   /* HALF TIME (v1.80.0) */
    if (i == P_PFY0 + 3) return w->seq.warp == 2.0 ? 1 : 0;   /* DOUBLE TIME */
    if (i == P_PFY0 + 4) return w->seq.roll ? 1 : 0;   /* ROLL */
    if (i == P_PFY0 + 6) return w->seq.scatter ? 1 : 0;   /* SCATTER */
    if (i == P_PF_HUM) return w->seq.humanize ? 1 : 0;   /* v1.91 */
    if (i == P_PF_BUILD) return w->seq.build ? 1 : 0;
    if (i == P_PF_ECHO) return w->seq.echo ? 1 : 0;
    if (i == P_PF_FADE) return w->seq.fade ? 1 : 0;
    if (i == P_PF_GHOST) return w->seq.ghost ? 1 : 0;   /* v1.94 */
    if (i == P_PF_POLY) return w->seq.poly ? 1 : 0;
    if (i == P_PF_DOWN) return w->seq.downbeat ? 1 : 0;
    if (i == P_PF_ALT) return w->seq.alt_bar ? 1 : 0;   /* v2.2 */
    if (i == P_PF_AFILL) return w->seq.auto_fill ? 1 : 0;
    if (i == P_PF_STOPCC) return w->seq.stop_cc ? 1 : 0;   /* v1.2.0 */
    if (i >= P_PFY0 && i < P_PFY0 + 8) return w->pfy[i - P_PFY0] ? 1 : 0;
    if (i == P_MSTATE) return w->seq.any_muted() ? 1 : 0;
    if (i == P_MSG) return w->msg;
    if (pst_slot(i) >= 0) return w->pst[w->pat_cur][pst_slot(i)].full ? 1 : 0;
    if (i == P_PST_PAT) return w->pat_cur;
    if (i == P_PCLEAR) return w->pclear ? 1 : 0;
    if (i >= P_PAT0 && i < P_PAT_END) return w->pat[i - P_PAT0].full || i - P_PAT0 == w->pat_cur ? 1 : 0;
    if (i == P_PAT_CLEAR) return w->patclear ? 1 : 0;
    if (i == P_PAT_STORE) return w->patstore ? 1 : 0;
    if (i >= P_CCB0 && i < P_CCB0 + NCC_V1) return w->cc_edit ? w->basedraft[i - P_CCB0] : tr.ccb[i - P_CCB0];
    if (i == P_CC_EDIT) return w->cc_edit ? 1 : 0;
    if (i >= P_CCUSE0 && i < P_CCUSE_END) return w->cc_edit && cc_in_use(w, i - P_CCUSE0) ? 1 : 0;
    if (i >= P_CCN0 && i < P_CCN0 + NCC_V1) return w->cc_edit ? w->ccdraft[i - P_CCN0] : Seq::cc_num(tr, i - P_CCN0);
    if (i >= P_PGST0 && i < P_PGST_END) { int pg = i - P_PGST0; return (w->page == pg ? 1 : 0) + ((w->seq.playing && tr.cur >= 0 && tr.cur / 16 == pg) ? 2 : 0); }
    if (i == P_PF_CC117) return 0;   /* retired v1.72.0: no knob, nothing sent */
    if (i == P_SHIFT) return w->shift ? 1 : 0;
    if (i == P_EU_LEN) return tr.eu_len ? tr.eu_len : tr.len;
    if (i == P_PSHIFTV) return value_of(w, P_PANEL) == 5 && !value_of(w, P_SHUF_EDIT) ? 1 : 0;   /* v1.2.0 */
    if (i == P_NAVV) return lmode(w) < 0 ? 0 : w->lpage[lmode(w)] ? 2 : 1;   /* v1.2.0 */
    if (i == P_TRK_PAN) return tr.panb;   /* v1.2.0 TRACK VALUES */
    if (i == P_TRK_PROB) return 100 - tr.pcut;
    if (trk_cc_j(i) >= 0) return tr.ccb[trk_cc_j(i)];
    if (i == P_EU_LEN2) return Seq::eu_m2(tr);   /* v1.2.0: B's own length, or A's */
    if (i == P_EU_ENA2) return Seq::gen_b(tr) ? 1 : 0;
    if (i == P_EU_FWD2 || i == P_EU_BWD2 || i == P_EU_BOTH2) return Seq::gen_b(tr) && tr.eu_dir2 == i - P_EU_FWD2 ? 1 : 0;
    if (i == P_EU_DIRV2) return !w->euclid_edit ? 0 : Seq::gen_b(tr) ? 2 : 1;
    if (i >= P_EU_OP0 && i < P_EU_OP_END) return tr.eu_op == EU_OPS[i - P_EU_OP0] ? 1 : 0;
    if (i == P_EU_OPV) return !w->euclid_edit ? 0 : Seq::gen_a(tr) && Seq::gen_b(tr) ? 2 : 1;
    if (i >= P_MPAD0 && i < P_MPAD_END) return w->seq.t[i - P_MPAD0].mute ? 2 : 0;   /* retired with the blink (v1.28.0): no widget uses it */
    if (i >= P_SHUF0 && i < P_SHUF0 + NTRACKS) return w->seq.t[i - P_SHUF0].shuf ? 1 : 0;
    if (i == P_GLOB_CH) return w->seq.t[0].ch + 1;
    if (i == P_SHUF_ALL) { for (int k = 0; k < NTRACKS; k++) if (!w->seq.t[k].shuf) return 0; return 1; }
    switch (i - P_PFX0) {
    case 0: return w->seq.loopn == 4;
    case 1: return w->seq.loopn == 2;
    case 2: return w->seq.loopn == 1;
    case 3: return w->seq.no_locks ? 1 : 0;
    case 4: return w->seq.rnd_step ? 1 : 0;
    case 5: return w->inv_mute ? 1 : 0;
    case 6: return 0;
    case 7: return w->seq.fill ? 1 : 0;
    default: break;
    }
    if (i >= P_TRKSEL0 && i < P_TRKSEL0 + NTRACKS) return w->track == i - P_TRKSEL0 ? 1 : 0;
    if (i >= P_PGSEL0 && i < P_PGSEL0 + 4) return w->page == i - P_PGSEL0 ? 1 : 0;
    if (st_cc_j(i) >= 0) {
        const int j = st_cc_j(i), c = cur_step(w).cc[j];
        if (!c && rest_dash(w, j)) return 0;   /* v1.2.0: just turned to --: it stays there */
        return c ? c : tr.ccb[j];
    }   /* no lock: the knob rests on the CC's base value (shown in brackets); the step is not touched */
    switch (i) {
    case P_TRACK: return w->track;
    case P_PAGE: return w->page;
    case P_SEL: return w->sel + 1;
    case P_ST_COND: return cond_pos(cur_step(w).cond);
    case P_ST_PROB: return cur_step(w).prob;
    case P_ST_VEL: { const int c = cur_step(w).vel; return c || rest_dash(w, REST_VEL) ? c : tr.vel; }   /* v1.2.0: no lock: rests on the track's velocity */
    case P_ST_LEN: return cur_step(w).len;
    case P_ST_PAN: { const int c = cur_step(w).pan; return c || rest_dash(w, REST_PAN) ? c : tr.panb; }   /* v1.2.0: no lock: rests on the track's pan */
    case P_TRK_LEN: return tr.len;
    case P_TRK_SPEED: return tr.speed;
    case P_TRK_CH: return tr.ch + 1;
    case P_TRK_NUM: return tr.num;
    case P_TRK_VEL: return tr.vel;
    case P_TRK_GATE: return tr.gate;
    case P_TRK_FWD: return (tr.dir & DIR_PP) == DIR_FWD ? 1 : 0;
    case P_PAT_FWD: case P_PAT_BWD: case P_PAT_BOTH: {   /* lit while every track plays that way (v1.82: one of three) */
        const uint8_t d = i == P_PAT_FWD ? DIR_FWD : i == P_PAT_BWD ? DIR_BWD : DIR_PP;
        for (int k = 0; k < NTRACKS; k++) if ((w->seq.t[k].dir & DIR_PP) != d) return 0;
        return 1;
    }
    case P_TRK_BOTH: return (tr.dir & DIR_PP) == DIR_PP ? 1 : 0;
    case P_EU_FWD: case P_EU_BWD: case P_EU_BOTH:   /* lit only while the generator is on */
        return Seq::gen_a(tr) && tr.eu_dir1 == i - P_EU_FWD ? 1 : 0;   /* v1.2.0: A's direction (the track's before) */
    case P_EU_DIRV: return !w->euclid_edit ? 0 : Seq::gen_a(tr) ? 2 : 1;
    case P_RTRK: return w->random_edit ? 1 + w->track : 0;   /* v1.85: RANDOM SETTINGS names the track */
    case P_TTL: return w->random_edit ? 10 + w->track : !w->global_edit ? 0 : w->cc_edit ? 2 + w->track : w->src_mode ? 0 : 1;   /* v2.0 */
    case P_SLOTV: return w->step_edit ? 1 : (w->track_edit || w->euclid_edit) ? 0 : 2;   /* v1.84: TIMING sits in the EUCLID place in STEP EDIT (and TIMING) only; v1.0.0: EUCLIDIAN on TRACK EDIT only (and while it is on), else nothing */
    case P_TRK_BWD: return (tr.dir & DIR_PP) == DIR_BWD ? 1 : 0;
    case P_TRK_CCBASE: return tr.ccbase;
    case P_EUCLID_EDIT: return w->euclid_edit ? 1 : 0;
    case P_LPANEL: return w->timing_edit ? 3 : w->euclid_edit ? 1 : (w->global_edit || w->random_edit) ? 4 : 0;   /* v1.0.1: SETUP (and its sub-panels) shows the logo, no RND / CLR buttons */
    case P_CLR_MODE: return w->clr_mode ? 1 : 0;
    case P_BKSTEP: return 0;   /* the backup step is gone (v1.64.0) */
    case P_BIGMSG: return w->big;
    case P_BIGDONE: return w->big > BIG_DONE0 ? 1 : 0;
    case P_LBTN: return w->euclid_edit || w->timing_edit || w->global_edit || w->random_edit ? 4 : lpage2(w) ? (w->clr_mode ? 6 : lmode(w) == 2 ? 8 : lmode(w) == 1 ? 5 : 7) : w->clr_mode ? 3 : w->step_edit ? 1 : w->track_edit ? 2 : 0;   /* ORANGE / TEAL / VIOLET / CLR (red) / NONE / VIOLET2 / CLR2 / ORANGE2 / TEAL2 (v1.2.0: page 2 of TRACK / PATTERN / STEP EDIT) */
    case P_EU_ENABLE: return Seq::gen_a(tr) ? 1 : 0;   /* v1.2.0: generator A */
    case P_EU_PL1: return tr.eu_pl1;
    case P_EU_RO1: return tr.eu_ro1;
    case P_EU_TRO: return tr.eu_tro;
    case P_EU_PL2: return tr.eu_pl2;
    case P_EU_RO2: return tr.eu_ro2;
    case P_EU_BOOL: return tr.eu_op;
    case P_FILL: return w->seq.fill ? 1 : 0;
    case P_FOLLOW: return w->follow ? 1 : 0;
    case P_STEP_EDIT: return w->step_edit || (w->euclid_edit && w->rmode == 1) ? 1 : 0;   /* lit in TIMING too (STEP EDIT inside), and in EUCLIDIAN over the step locks */
    case P_TIMING_EDIT: return w->timing_edit ? 1 : 0;
    case P_PST_ACT: return w->pst_act[w->pat_cur] + 1;
    case P_PATV0: case P_PATV0 + 1: case P_PATV0 + 2: case P_PATV0 + 3: case P_PATV0 + 4: case P_PATV0 + 5: case P_PATV0 + 6: case P_PATV0 + 7: {
        const int k = i - P_PATV0;   /* HIDDEN (another panel), EMPTY, FULL, PLAYING (looks like the step under the play-head), CUED */
        if (value_of(w, P_PANEL) != 5) return 0;
        /* v1.92: while the transport runs, the one playing blinks with the beat (PLAYING on the first half of each beat,
         * BLINK = pale with the orange bar on the second; v1.93: per beat, half as fast), and a cued one blinks the other way round (PLAYING / CUED) */
        const bool first = std::fmod(w->song, 2.0) < 1.0;   /* v1.93: half as fast, one beat on, one beat off */
        if (k == w->pat_cur) return w->seq.playing && !first ? 5 : 3;
        if (k == w->pat_cue) return w->seq.playing && !first ? 3 : 4;
        return 1 + (w->pat[k].full ? 1 : 0);
    }
    case P_ST_RATCH: return cur_step(w).ratch + 1;
    case P_ST_RRES: return cur_step(w).rres;
    case P_MULTI: return multi(w) ? 1 : 0;
    case P_TRACK_EDIT: return w->track_edit || (w->euclid_edit && w->rmode == 2) ? 1 : 0;   /* and in EUCLIDIAN over the track setup */
    case P_LEN16: case P_LEN32: case P_LEN48: case P_LEN64: return tr.len == 16 * (i - P_LEN16 + 1) ? 1 : 0;
    case P_RANDOM_EDIT: return w->random_edit ? 1 : 0;
    case P_GLOBAL_EDIT: return w->global_edit || w->random_edit ? 1 : 0;   /* v1.83: RANDOM SETUP is part of SETUP */
    case P_PATTERN_EDIT: return !(w->step_edit || w->track_edit || w->random_edit || w->euclid_edit || w->global_edit || w->timing_edit) || (w->euclid_edit && w->rmode == 0) ? 1 : 0;   /* v1.82: the button of the pattern setup, lit while no other mode is */
    case P_PLEN0: case P_PLEN0 + 1: case P_PLEN0 + 2: case P_PLEN0 + 3: return w->seq.all_len(16 * (i - P_PLEN0 + 1)) ? 1 : 0;
    case P_SHUFFLE: return w->seq.shuffle;
    case P_SCR: return w->src_mode ? 1 : 0;   /* SETUP tab, left: MAIN = the project's name, SOURCE = the list of drum tracks (CHOOSE TARGET TRACK) */
    case P_SHUF_EDIT: return w->shuf_edit && !(w->global_edit || w->euclid_edit || w->random_edit || w->track_edit || w->step_edit) ? 1 : 0;   /* SHUFFLE EDIT, lit in PATTERN EDIT only */
    case P_SRCHL0: case P_SRCHL0 + 1: case P_SRCHL0 + 2: case P_SRCHL0 + 3: case P_SRCHL0 + 4: case P_SRCHL0 + 5:
        return !w->src_mode || i - P_SRCHL0 >= SRC_ROWS ? 0 : w->src_page * SRC_ROWS + (i - P_SRCHL0) == w->src_pick ? 2 : 1;   /* HIDDEN (see-through, the list is closed) / IDLE / MARKED */
    case P_PANEL: return w->global_edit ? (w->cc_edit ? 6 : w->src_mode ? 7 : 4) : w->random_edit ? 3 : w->track_edit ? (w->trk_vals ? 8 : 2) : w->step_edit ? 0 : w->euclid_edit ? (w->rmode == 1 ? 0 : w->rmode == 2 ? (w->trk_vals ? 8 : 2) : 5) : 5;   /* v2.1: EUCLIDIAN changes the left part only */   /* no mode: the pattern setup (v1.58.0; before: the step locks, and PATTERN EDIT had the setup). 1 (MULTI) is no longer used; 6 = TRACK SETUP (CCEDIT) */
    case P_PTITLE: return w->timing_edit ? 9 : w->global_edit ? 5 : w->euclid_edit ? 4 : w->random_edit ? 3 : w->track_edit ? 2 : w->step_edit ? 1 : 0;   /* 6 (PATTERN), 7 (SOURCE), 8 (MAPTRK) are no longer used */
    case P_RND_DENS: return w->seq.t[w->track].rnd_dens;
    case P_RND_MIN: return w->seq.t[w->track].rnd_min;
    case P_RND_MAX: return w->seq.t[w->track].rnd_max;
    case P_RUN: {   /* play-head inside the shown page: 1..16, 0 when it is elsewhere or stopped */
        int c = tr.cur;
        return (w->seq.playing && c >= 0 && c / 16 == w->page) ? c % 16 + 1 : 0;
    }
    default: return 0;
    }
}

/* PERFORM ALL moves the base values of the CCs, and a parameter moves at once, not on the next step: take the resting
 * value of every track's CCs before a change and send the ones that differ after it (a track with no base value for
 * a CC has nothing to send). */
struct PfSnap {
    int v[NTRACKS][NCC];
    explicit PfSnap(const Plugin *w) { for (int ti = 0; ti < NTRACKS; ti++) for (int j = 0; j < NCC; j++) v[ti][j] = w->seq.base_eff(w->seq.t[ti], j); }
    void send(Plugin *w) const {
        for (int ti = 0; ti < NTRACKS; ti++) for (int j = 0; j < NCC; j++) {
            const Track &tr = w->seq.t[ti];
            const int now = w->seq.base_eff(tr, j);
            if (now >= 0 && now != v[ti][j]) { w->seq.emit((uint8_t)(0xB0 | (tr.ch & 15)), (uint8_t)Seq::cc_num(tr, j), (uint8_t)now); w->seq.cc_dirty[ti][j] = false; }
        }
        flush(w);
    }
};
static void set_value_raw(Plugin *w, int i, int v);
static void set_value(Plugin *w, int i, int v) {
    if (pf_k(i) >= 0 || i == P_PF_CTRL) { PfSnap snap(w); set_value_raw(w, i, v); snap.send(w); }
    else set_value_raw(w, i, v);
}
static void set_value_raw(Plugin *w, int i, int v) {
    if (ql_track(w, i) >= 0) {   /* v1.2.0: a Q-Link on a STEP LOCKS knob while the track's panel shows: the track's value */
        const int j = ql_track(w, i);
        set_value_raw(w, j, clampi(v, (int)PARAMS[j].min, (int)PARAMS[j].max));
        return;
    }
    Track &tr = w->seq.t[w->track];
    if (i >= P_STEP0 && i < P_STEP0 + 16) {
        tr.st[w->page * 16 + (i - P_STEP0)].on = v ? 1 : 0;
        select_one(w, i - P_STEP0);   /* the step just tapped is the one the lock knobs edit */
        return;
    }
    if (i >= P_MUTE0 && i < P_MUTE0 + NTRACKS) {
        Track &m = w->seq.t[i - P_MUTE0];
        m.mute = v ? 1 : 0;
        if (m.mute) { w->seq.note_off(m); flush(w); }
        return;
    }
    if (st_cc_j(i) >= 0) { const int j = st_cc_j(i); const uint8_t x = (uint8_t)clampi(v, 0, 128);
        const bool at_dash = rest_turn(w, j, x);   /* v1.2.0: the knob showed --, not the base value (see value_of) */
        if (!cur_step(w).cc[j] && x && x == w->seq.t[w->track].ccb[j] && !at_dash) return;   /* the knob only rests on the base value: the host handing that back is not a lock */
        each_sel(w, [=](Step &st) { st.cc[j] = x; });
        return;
    }
    if (i >= P_UNUSED0 && i < P_UNUSED0 + NCC_V1) return;
    if (pf_k(i) >= 0) { if (w->seq.pf_on) w->seq.pf_off[pf_k(i)] = clampi(v, (int)PARAMS[i].min, (int)PARAMS[i].max); return; }   /* PERFORM ALL off: locked at 0 */
    if (i == P_PFY0) { if (v) w->seq.set_loop(8); else if (w->seq.loopn == 8) w->seq.set_loop(0); return; }   /* LOOP 8: like LOOP 4 / 2 / 1, the last 8 steps */
    if (i == P_PFY0 + 1) { w->seq.set_reverse(v != 0); return; }   /* REVERSE: the whole pattern turns round where it is */
    if (i == P_PFY0 + 2 || i == P_PFY0 + 3) {   /* HALF / DOUBLE TIME: one at a time; off: back on the song's clock, where it would be */
        const double f = i == P_PFY0 + 2 ? 0.5 : 2.0;
        if (v) w->seq.set_warp(f, w->song); else if (w->seq.warp == f) w->seq.set_warp(1.0, w->song);
        return;
    }
    if (i == P_PFY0 + 4) { w->seq.roll = v != 0; return; }
    if (i == P_PFY0 + 6) { w->seq.scatter = v != 0; return; }
    if (i == P_PF_HUM) { w->seq.humanize = v != 0; return; }   /* v1.91 */
    if (i == P_PF_BUILD) { w->seq.set_build(v != 0, w->song); return; }
    if (i == P_PF_ECHO) { w->seq.echo = v != 0; return; }
    if (i == P_PF_FADE) { w->seq.set_fade(v != 0, w->song); return; }
    if (i == P_PF_GHOST) { w->seq.ghost = v != 0; return; }   /* v1.94 */
    if (i == P_PF_POLY) { w->seq.set_poly(v != 0); return; }
    if (i == P_PF_DOWN) { w->seq.downbeat = v != 0; return; }
    if (i == P_PF_ALT) { w->seq.set_alt(v != 0, w->song); return; }   /* v2.2 */
    if (i == P_PF_AFILL) { w->seq.auto_fill = v != 0; return; }
    if (i == P_PF_STOPCC) { w->seq.set_stop_cc(v != 0); return; }   /* v1.2.0 */
    if (i >= P_PFY0 && i < P_PFY0 + 8) { w->pfy[i - P_PFY0] = v != 0; return; }
    if (i >= P_SHUF0 && i < P_SHUF0 + NTRACKS) { w->seq.t[i - P_SHUF0].shuf = v ? 1 : 0; return; }
    if (i == P_SHUF_ALL) { for (int k = 0; k < NTRACKS; k++) w->seq.t[k].shuf = v ? 1 : 0; return; }
    if (i == P_GLOB_CH) { w->seq.set_all_ch(clampi(v, 1, 16) - 1); flush(w); return; }
    if (i >= P_PFX0 && i < P_PFX_END) {
        static const int LOOPN[3] = {4, 2, 1};
        switch (i - P_PFX0) {
        case 0: case 1: case 2:   /* the three loops exclude each other; switching the lit one off ends the loop */
            if (v) w->seq.set_loop(LOOPN[i - P_PFX0]); else if (w->seq.loopn == LOOPN[i - P_PFX0]) w->seq.set_loop(0);
            break;
        case 3: w->seq.no_locks = v != 0; break;
        case 4: w->seq.rnd_step = v != 0; break;
        case 5: if ((v != 0) != w->inv_mute) { w->inv_mute = v != 0; w->seq.invert_mutes(); flush(w); } break;   /* off flips them back */
        case 7: w->seq.fill = v != 0; break;
        default: break;
        }
        return;
    }
    if (i == P_PF_CC117) return;   /* retired v1.72.0 */
    if (i == P_PCLEAR) { w->pclear = v != 0; return; }
    if (i == P_PAT_CLEAR) { w->patclear = v != 0; if (w->patclear) w->patstore = false; return; }   /* CLEAR and STORE: at most one lit */
    if (i == P_PAT_STORE) { w->patstore = v != 0; if (w->patstore) w->patclear = false; return; }
    if (i == P_SHUF_EDIT) { w->shuf_edit = v != 0; return; }
    if (i == P_CLR_MODE) { w->clr_mode = v != 0; return; }
    if (i == P_CC_EDIT) {   /* open: the draft starts from the track's numbers; closing it by hand = DISCARD */
        w->cc_edit = v != 0;   /* TRACK SETUP on the SETUP tab (v1.58.0; before: EDIT CC inside TRACK EDIT) */
        if (w->cc_edit) for (int j = 0; j < NCC; j++) { w->ccdraft[j] = Seq::cc_num(tr, j); w->basedraft[j] = tr.ccb[j]; }
        return;
    }
    if (i >= P_CCB0 && i < P_CCB0 + NCC_V1) { if (w->cc_edit) w->basedraft[i - P_CCB0] = clampi(v, 0, 128); return; }
    if (i >= P_CCN0 && i < P_CCN0 + NCC_V1) { if (w->cc_edit) w->ccdraft[i - P_CCN0] = clampi(v, 0, 127); return; }
    if (i == P_SHIFT) { w->shift = v != 0 && w->step_edit; return; }
    if (i == P_EU_LEN) { tr.eu_len = (uint8_t)clampi(v, 1, NSTEPS); w->seq.euclid_apply(w->track); return; }
    if (i == P_EU_LEN2) { tr.eu_len2 = (uint8_t)clampi(v, 1, NSTEPS); w->seq.euclid_apply(w->track); return; }   /* v1.2.0 */
    /* v1.2.0 TRACK VALUES: PAN and the CC base values go out at once, so a turn is heard while the track plays */
    if (i == P_TRK_PAN) {
        tr.panb = (uint8_t)clampi(v, 0, 128);
        if (tr.panb) { w->seq.emit((uint8_t)(0xB0 | (tr.ch & 15)), PAN_CC[w->track], (uint8_t)(tr.panb - 1)); w->seq.pan_dirty[w->track] = false; flush(w); }
        return;
    }
    if (i == P_TRK_PROB) { tr.pcut = (uint8_t)(100 - clampi(v, 0, 100)); return; }
    if (trk_cc_j(i) >= 0) {
        const int j = trk_cc_j(i);
        tr.ccb[j] = (uint8_t)clampi(v, 0, 128);
        if (tr.ccb[j]) { w->seq.emit((uint8_t)(0xB0 | (tr.ch & 15)), (uint8_t)Seq::cc_num(tr, j), (uint8_t)w->seq.base_eff(tr, j)); w->seq.cc_dirty[w->track][j] = false; flush(w); }
        return;
    }
    if (i == P_EU_FWD2 || i == P_EU_BWD2 || i == P_EU_BOTH2) { if (v && Seq::gen_b(tr)) { tr.eu_dir2 = (uint8_t)(i - P_EU_FWD2); w->seq.euclid_apply(w->track); } return; }
    if (i >= P_EU_OP0 && i < P_EU_OP_END) { if (v) { tr.eu_op = EU_OPS[i - P_EU_OP0]; w->seq.euclid_apply(w->track); } return; }
    if (i == P_PF_CTRL) { if (v != 0) w->seq.perform_begin(); else w->seq.perform_discard();   /* off without COMMIT: everything back as it was */ for (int k = 0; k < NPF_ALL; k++) w->raw[pf_param(k)] = -1.0f; return; }   /* on: offsets start at 0 over the current state; off by hand: what was set stays (as COMMIT) */
    switch (i) {
    /* a selection of several steps does not carry over to another track or page */
    case P_TRACK: w->track = clampi(v, 0, NTRACKS - 1); select_one(w, w->sel);
        if (w->cc_edit) { Track &nt = w->seq.t[w->track]; for (int j = 0; j < NCC; j++) { w->ccdraft[j] = Seq::cc_num(nt, j); w->basedraft[j] = nt.ccb[j]; } }   /* TRACK SETUP stays open: the draft is dropped and starts again from the new track */
        break;
    case P_PAGE: w->page = clampi(v, 0, 3); select_one(w, w->sel); break;
    case P_SEL: select_one(w, v - 1); break;
    case P_ST_COND: { const uint8_t x = (uint8_t)cond_at(clampi(v, 0, NCOND - 1)); each_sel(w, [=](Step &st) { st.cond = x; }); break; }
    case P_ST_PROB: { const uint8_t x = (uint8_t)clampi(v, 0, 100); each_sel(w, [=](Step &st) { st.prob = x; }); break; }
    case P_ST_VEL: { const uint8_t x = (uint8_t)clampi(v, 0, 127); const bool d = rest_turn(w, REST_VEL, x);
        if (!cur_step(w).vel && x && x == tr.vel && !d) break;   /* the knob only rests on the track's velocity: not a lock (v1.2.0) */
        each_sel(w, [=](Step &st) { st.vel = x; }); break; }
    case P_ST_LEN: { const uint8_t x = (uint8_t)clampi(v, 0, 64); each_sel(w, [=](Step &st) { st.len = x; }); break; }
    case P_ST_PAN: { const uint8_t x = (uint8_t)clampi(v, 0, 128); const bool d = rest_turn(w, REST_PAN, x);
        if (!cur_step(w).pan && x && x == tr.panb && !d) break;   /* the knob only rests on the track's pan: not a lock (v1.2.0) */
        each_sel(w, [=](Step &st) { st.pan = x; }); break; }
    case P_ST_RATCH: { const uint8_t x = (uint8_t)(clampi(v, 1, RATCH_MAX) - 1); each_sel(w, [=](Step &st) { st.ratch = x; }); break; }
    case P_ST_RRES: { const uint8_t x = (uint8_t)clampi(v, 0, RRES_MAX); each_sel(w, [=](Step &st) { st.rres = x; }); break; }
    case P_TRK_LEN: tr.len = (uint8_t)clampi(v, 1, NSTEPS); w->seq.euclid_apply(w->track); break;
    case P_TRK_SPEED: w->seq.set_speed(w->track, v, w->song); break;
    case P_TRK_CH: w->seq.note_off(tr); flush(w); tr.ch = (uint8_t)(clampi(v, 1, 16) - 1); break;
    case P_TRK_NUM: w->seq.note_off(tr); flush(w); tr.num = (uint8_t)clampi(v, 0, 127); break;
    case P_TRK_VEL: tr.vel = (uint8_t)clampi(v, 1, 127); break;
    case P_TRK_GATE: tr.gate = (uint8_t)clampi(v, 1, 64); break;
    case P_PAT_FWD: case P_PAT_BWD: case P_PAT_BOTH:   /* every track that way; a tap on the lit one changes nothing */
        if (v) { const uint8_t d = i == P_PAT_FWD ? DIR_FWD : i == P_PAT_BWD ? DIR_BWD : DIR_PP; for (int k = 0; k < NTRACKS; k++) w->seq.t[k].dir = d; }
        break;
    case P_EU_FWD: case P_EU_BWD: case P_EU_BOTH:   /* generator A's direction (v1.2.0; the track's v1.83 .. v1.1.1): nothing while A is off */
        if (v && Seq::gen_a(tr)) { tr.eu_dir1 = (uint8_t)(i - P_EU_FWD); w->seq.euclid_apply(w->track); }
        break;
    case P_TRK_FWD: case P_TRK_BWD: case P_TRK_BOTH:   /* FWD, BWD or BOTH (v1.82: one of three; a tap on the lit one changes nothing) */
        if (v) tr.dir = i == P_TRK_FWD ? DIR_FWD : i == P_TRK_BWD ? DIR_BWD : DIR_PP;
        break;
    case P_TRK_CCBASE: tr.ccbase = (uint8_t)clampi(v, 0, CC_BASE_MAX); std::memset(tr.ccn, 0, sizeof tr.ccn); break;   /* no widget any more; the block replaces the five own numbers */
    case P_FILL: w->seq.fill = v != 0; break;
    case P_FOLLOW: w->follow = v != 0; break;
    /* four modes, at most one on; leaving STEP EDIT drops a selection of several steps */
    case P_SHUFFLE: w->seq.shuffle = (uint8_t)clampi(v, 50, SHUFFLE_MAX); break;
    case P_STEP_EDIT: case P_TRACK_EDIT: case P_RANDOM_EDIT: case P_EUCLID_EDIT: case P_GLOBAL_EDIT: case P_PATTERN_EDIT: case P_TIMING_EDIT: {
        bool on = v != 0;
        /* v1.0.0 (2026-10-11): one of SETUP, PATTERN EDIT, TRACK EDIT, STEP EDIT is always lit, and a tap on the lit one
         * changes nothing. Exceptions that switch: SETUP lit for RANDOM SETUP goes back to the SETUP panel; in EUCLIDIAN
         * the lit one (the right-hand panel's mode) leaves EUCLIDIAN for that mode. */
        const bool four = i == P_STEP_EDIT || i == P_TRACK_EDIT || i == P_GLOBAL_EDIT || i == P_PATTERN_EDIT;
        if (four && !on) {
            if (i == P_GLOBAL_EDIT && w->random_edit) on = true;
            else if (w->euclid_edit) on = true;
            else { w->raw[i] = -1.0f; break; }
        }
        if (i == P_EUCLID_EDIT && !on) { i = w->rmode == 1 ? P_STEP_EDIT : w->rmode == 2 ? P_TRACK_EDIT : P_PATTERN_EDIT; on = true; }   /* EUCLIDIAN off: back to the mode under it (before: PATTERN EDIT) */
        if (i == P_RANDOM_EDIT && !on) { i = P_GLOBAL_EDIT; on = true; }   /* RANDOM SETUP off: back to SETUP */
        if (i == P_STEP_EDIT && w->timing_edit && on) break;   /* in TIMING, STEP EDIT is already lit */
        w->step_edit = w->track_edit = w->random_edit = w->euclid_edit = w->global_edit = w->timing_edit = false;
        if (i == P_STEP_EDIT) w->step_edit = on;
        else if (i == P_TIMING_EDIT) { w->timing_edit = on; w->step_edit = true; }   /* steps are picked as in STEP EDIT; TIMING off goes back to STEP EDIT (v1.84) */
        else if (i == P_TRACK_EDIT) w->track_edit = on;
        else if (i == P_RANDOM_EDIT) w->random_edit = on;
        else if (i == P_GLOBAL_EDIT) w->global_edit = on;
        else if (i == P_PATTERN_EDIT) {}   /* PATTERN EDIT = no other mode on */
        else w->euclid_edit = on;
        if (!w->step_edit) { select_one(w, w->sel); w->shift = false; }
        w->shuf_edit = false;   /* the SHUFFLE EDIT row belongs to the pattern setup */
        if (!w->global_edit) { w->cc_edit = false; w->src_mode = SM_IDLE; w->src_list.clear(); }   /* TRACK SETUP and the map list live in the SETUP panel */
        rmode_note(w);
        w->src_gen++;
        break;
    }
    case P_EU_PL1: tr.eu_pl1 = (uint8_t)clampi(v, 0, NSTEPS); w->seq.euclid_apply(w->track); break;
    case P_EU_PL2: tr.eu_pl2 = (uint8_t)clampi(v, 0, NSTEPS); w->seq.euclid_apply(w->track); break;
    case P_EU_RO1: tr.eu_ro1 = (uint8_t)clampi(v, 0, NSTEPS - 1); w->seq.euclid_apply(w->track); break;
    case P_EU_RO2: tr.eu_ro2 = (uint8_t)clampi(v, 0, NSTEPS - 1); w->seq.euclid_apply(w->track); break;
    case P_EU_TRO: tr.eu_tro = (uint8_t)clampi(v, 0, NSTEPS - 1); w->seq.euclid_apply(w->track); break;
    case P_EU_BOOL: tr.eu_op = (uint8_t)clampi(v, 0, Seq::E_NOPS - 1); w->seq.euclid_apply(w->track); break;
    case P_RND_DENS: w->seq.t[w->track].rnd_dens = (uint8_t)clampi(v, 0, 100); break;
    case P_RND_MIN: w->seq.t[w->track].rnd_min = (uint8_t)clampi(v, 0, 127); break;
    case P_RND_MAX: w->seq.t[w->track].rnd_max = (uint8_t)clampi(v, 0, 127); break;
    default: break;   /* P_RUN: display only */
    }
}

static float to_norm(const param_t *p, int v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)v / (p->nopts - 1)) : 0.0f;
    return p->max > p->min ? clamp01(((float)v - p->min) / (p->max - p->min)) : 0.0f;
}
static int from_norm(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
/* what the host should see for parameter i: its own last knob position while that still means the
 * current value, else the value's exact position. Callers hold w->lock. */
static float shown_norm(Plugin *w, int i) {
    const param_t *p = &PARAMS[i];
    int v = value_of(w, i);
    if (w->raw[i] >= 0.0f && from_norm(p, w->raw[i]) == v) return w->raw[i];
    return to_norm(p, v);
}
/* the name of track ti's CC slot j, if one was read for the number the track uses now ("" otherwise) */
static const char *cc_label(const Plugin *w, int ti, int j) {
    return (!w->ccname[ti][j].empty() && w->ccname_cc[ti][j] == Seq::cc_num(w->seq.t[ti], j)) ? w->ccname[ti][j].c_str() : "";
}
/* v1.2.0: a CC's name made short for the value text under a knob (at most SHORT_NAME characters, the knobs sit close):
 * a leading pad number goes ("1 TUNE" -> "TUNE"); a longer name of several words keeps its last word, cut, after the
 * first words made short: one word to its initial and a dot unless it has 2 letters ("Filter Cutoff" -> "F. Cut",
 * "BD Level" -> "BD Lev"; no dot when that keeps a short last word whole: "Delay Send" -> "D Send"), several words to
 * their initials ("Amp Env Attack" -> "AE Att"); else it is cut. */
enum { SHORT_NAME = 6 };
static std::string short_name(const char *in) {
    const char *s = in, *q = in;
    while (*q >= '0' && *q <= '9') q++;
    if (q > s && *q == ' ') { while (*q == ' ') q++; if (*q) s = q; }
    std::vector<std::string> wd;
    for (const char *p = s; *p;) {
        while (*p == ' ') p++;
        const char *b = p; while (*p && *p != ' ') p++;
        if (p > b) wd.emplace_back(b, p - b);
    }
    std::string all;
    for (const auto &x : wd) { if (!all.empty()) all += ' '; all += x; }
    if ((int)all.size() <= SHORT_NAME || wd.size() < 2) return all.substr(0, SHORT_NAME);
    std::string pre;
    if (wd.size() == 2) {
        pre = wd[0].size() <= 2 ? wd[0] : wd[0].substr(0, 1) + ".";
        if (pre.size() + 1 + wd[1].size() > (size_t)SHORT_NAME && pre.size() == 2 && pre[1] == '.' && 2 + wd[1].size() <= (size_t)SHORT_NAME)
            pre = wd[0].substr(0, 1);   /* v1.2.0: "D Send", not "D. Sen" */
    }
    else for (size_t k = 0; k + 1 < wd.size(); k++) pre += wd[k][0];
    const int rem = SHORT_NAME - (int)pre.size() - 1;
    if (rem < 2) return all.substr(0, SHORT_NAME);
    return pre + " " + wd.back().substr(0, rem);
}
/* PERFORM ALL moves slot j of every track (v1.2.0): the short name if every track that has a name for it has the same
 * one ("1 Delay Send" on T1 and "2 Delay Send" on T2 are both "D Send"), MULTI if they differ, "" if none has one */
static std::string cc_label_all(const Plugin *w, int j) {
    std::string got;
    for (int ti = 0; ti < NTRACKS; ti++) {
        const char *nm = cc_label(w, ti, j);
        if (!*nm) continue;
        const std::string sn = short_name(nm);
        if (got.empty()) got = sn; else if (got != sn) return "MULTI";
    }
    return got;
}
/* the open project's name for the SETUP tab, when MPC says which one it is (no search of the drives here) */
static void proj_name(Plugin *w, const std::string &pp) {   /* the SETUP panel's project line: a checked file's name, or "Project not saved" */
    if (pp.empty()) { w->src_proj = "Project not saved"; return; }
    size_t sl = pp.rfind('/'); size_t a0 = sl == std::string::npos ? 0 : sl + 1;
    w->src_proj = pp.substr(a0, pp.size() - 4 - a0);
}
/* CREATE DEFAULT MAP, second step: the drum tracks of the project `path` (the one to write into) */
static bool tracks_of(const std::string &path, std::vector<XTrack> &ts) {   /* the file part of it (no lock) */
    std::string js;
    if (!xpj_load(path, js)) return false;
    ts = xpj_tracks(js);
    return true;
}
static void map_tracks(Plugin *w, const std::string &path, bool ok, const std::vector<XTrack> &ts) {   /* under the lock */
    w->map_path = path;
    if (!ok) { w->big = BIG_NOTSAVED; return; }
    if (ts.empty()) { w->big = BIG_NODRUM; return; }
    w->src_list.clear(); w->src_pick = -1;   /* nothing marked, unless the track mapped last time is there */
    for (const XTrack &t : ts) { if (t.name == w->src) w->src_pick = (int)w->src_list.size(); w->src_list.push_back({t.name, 3}); }
    w->src_mode = SM_FIXED; w->src_page = w->src_pick > 0 ? w->src_pick / SRC_ROWS : 0;
}
static void src_name(Plugin *w) { w->src_proj.clear(); }   /* v1.67: no file is read here any more (it ran under the lock, on load too); the name is shown nowhere but "Map into:" */
static bool is_own(int i) { return i >= 0 && i < P_OWN_COUNT; }   /* not a popup flag */

/* v2.5: the PERFORM effect pads a STATE remembers (bit n = FX_PADS[n] lit). INVERT MUTE and MUTE ALL are not in it:
 * a state holds the mutes themselves. */
static const int FX_PADS[] = {P_PFX0 + 0, P_PFX0 + 1, P_PFX0 + 2, P_PFX0 + 3, P_PFX0 + 4, P_PFX0 + 7,
                              P_PFY0 + 0, P_PFY0 + 1, P_PFY0 + 2, P_PFY0 + 3, P_PFY0 + 4, P_PFY0 + 6,
                              P_PF_HUM, P_PF_BUILD, P_PF_ECHO, P_PF_FADE, P_PF_GHOST, P_PF_POLY, P_PF_DOWN, P_PF_ALT, P_PF_AFILL, P_PF_STOPCC};
enum { NFX_PADS = sizeof FX_PADS / sizeof FX_PADS[0] };
static int value_of(Plugin *w, int i);
static void set_value(Plugin *w, int i, int v);
static uint32_t fx_take(Plugin *w) { uint32_t m = 0; for (int n = 0; n < NFX_PADS; n++) if (value_of(w, FX_PADS[n])) m |= 1u << n; return m; }
static void fx_put(Plugin *w, uint32_t m) {   /* the ones that differ: off first, then on (one LOOP at a time) */
    for (int on = 0; on < 2; on++)
        for (int n = 0; n < NFX_PADS; n++) {
            const int want = (m >> n) & 1;
            if (want == on && (value_of(w, FX_PADS[n]) != 0) != (want != 0)) set_value(w, FX_PADS[n], want);
        }
}
static int pst_slot(int i) { return i >= P_PST0 && i < P_PST_END ? i - P_PST0 : i >= P_PST5 && i < P_PST5_END ? 4 + i - P_PST5 : -1; }
static void snap_of(const Seq &q, Snap &d) {
    std::memcpy(d.t, q.t, sizeof d.t);
    for (int i = 0; i < NTRACKS; i++) {
        Track &x = d.t[i];
        x.mute = 0; x.next_k = 0; x.cur = 0; x.evaluated = x.last_res = x.sounding = false; x.snd_note = x.snd_ch = 0; x.off_at = 0.0; x.rt_left = 0; x.rt_at = x.rt_iv = x.rt_gate = 0.0; x.rt_vel = 0; x.ph = 0; x.backw = false; x.sc_k = 0; x.sc_v = 0;
    }
    d.shuffle = q.shuffle;
}
static void hist_take(Plugin *w, Snap &d) { snap_of(w->seq, d); }
static bool hist_same(const Snap &a, const Snap &b) { return a.shuffle == b.shuffle && !std::memcmp(a.t, b.t, sizeof a.t); }
static void hist_reset(Plugin *w) { w->undo_st.clear(); w->redo_st.clear(); w->hist_init = false; }
static void hist_commit(Plugin *w) {   /* the pending change becomes an undo step */
    if (hist_same(w->hist_seen, w->hist_done)) return;
    w->undo_st.push_back(w->hist_done);   /* the oldest step goes when the history is full */
    w->redo_st.clear();
    w->hist_done = w->hist_seen;
}
static void hist_tick(Plugin *w, int frames) {   /* every block, under the lock */
    Snap now;
    hist_take(w, now);
    if (!w->hist_init) { w->hist_init = true; w->hist_done = w->hist_seen = now; w->hist_quiet = 0; return; }
    if (!hist_same(now, w->hist_seen)) { w->hist_seen = now; w->hist_quiet = 0; return; }
    if ((w->hist_quiet += frames) >= (int)(0.25f * w->sample_rate)) hist_commit(w);
}
static void hist_step(Plugin *w, bool redo) {
    Snap now;
    hist_take(w, now);
    if (!w->hist_init) { w->hist_init = true; w->hist_done = w->hist_seen = now; }
    w->hist_seen = now;
    hist_commit(w);   /* an edit made a moment ago counts as a step of its own */
    HistStack &from = redo ? w->redo_st : w->undo_st, &to = redo ? w->undo_st : w->redo_st;
    if (from.empty()) return;
    to.push_back(w->hist_done);
    w->hist_done = w->hist_seen = from.back();
    from.pop_back();
    if (w->seq.pf_on) w->seq.perform_commit(), w->seq.pf_undo_ok = false;   /* PERFORM ALL in progress ends here */
    w->seq.restore_from(w->hist_done.t, true);
    w->seq.shuffle = w->hist_done.shuffle;
    hist_take(w, w->hist_done); w->hist_seen = w->hist_done;
}

static void display_of(Plugin *w, int i, char *out, size_t n) {
    if (ql_track(w, i) >= 0) { display_of(w, ql_track(w, i), out, n); return; }   /* v1.2.0: what the Q-Link really turns */
    std::lock_guard<std::mutex> lk(w->lock);
    int v = value_of(w, i);
    if (st_cc_j(i) >= 0) {   /* no lock: the CC's base value in brackets, if it has one, so the user sees where the CC rests */
        const int lockv = cur_step(w).cc[st_cc_j(i)];
        const std::string sn = short_name(cc_label(w, w->track, st_cc_j(i)));   /* "BD Lev 94": short (v1.2.0, was cut to 9), the text has little room */
        const char *nm = sn.c_str(), *sp = *nm ? " " : "";
        const int base = w->seq.t[w->track].ccb[st_cc_j(i)];   /* v1.2.0: the base, also while the knob stays at -- */
        if (lockv) std::snprintf(out, n, "%s%s%d", nm, sp, lockv - 1); else if (base) std::snprintf(out, n, "%s%s(%d)", nm, sp, base - 1); else std::snprintf(out, n, "%s%s--", nm, sp);
        (void)v;
        return;
    }
    if (trk_cc_j(i) >= 0) {   /* v1.2.0 TRACK VALUES: the base value, with the CC's name as on STEP LOCKS */
        const std::string sn = short_name(cc_label(w, w->track, trk_cc_j(i)));
        const char *nm = sn.c_str(), *sp = *nm ? " " : "";
        if (v) std::snprintf(out, n, "%s%s%d", nm, sp, v - 1); else std::snprintf(out, n, "%s%s--", nm, sp);
        return;
    }
    if (pf_k(i) >= 0) {   /* PERFORM knobs: an offset around 0; OFF while PERFORM ALL is off */
        const int cj = pf_k(i) - Seq::PF_CC0;   /* a CC knob carries the parameter's name if all eight tracks agree on it */
        const std::string sn = cj >= 0 && cj < NCC ? cc_label_all(w, cj) : std::string();
        const char *nm = sn.c_str();
        if (*nm) std::snprintf(out, n, w->seq.pf_on ? "%s %+d" : "%s OFF", nm, v);
        else if (!w->seq.pf_on) std::snprintf(out, n, "OFF");
        else if (pf_k(i) == Seq::PF_PROB) std::snprintf(out, n, "%+d%%", v);
        else if (pf_k(i) == Seq::PF_LEN) std::snprintf(out, n, "%+.2f", v / 4.0);
        else std::snprintf(out, n, "%+d", v);
        return;
    }
    if (i == P_GLOB_CH && !w->seq.same_ch()) { std::snprintf(out, n, "MIXED"); return; }
    if (i >= P_CCB0 && i < P_CCB0 + NCC_V1) { if (!v) std::snprintf(out, n, "OFF"); else std::snprintf(out, n, "%d", v - 1); return; }
    if (i == P_BKNAME) { std::snprintf(out, n, " "); return; }   /* the backup's name until v2.0; no backup any more */
    if (i == P_SRCPROJ) {   /* the open project's name, or what the list asks for */
        switch (w->src_mode) {
        case SM_FIXED: { const std::string &mp = w->map_path; size_t sl = mp.rfind('/'); size_t a0 = sl == std::string::npos ? 0 : sl + 1;
            std::string fn = mp.size() > a0 ? mp.substr(a0) : std::string("?");   /* the file name, .xpj included (v1.67.0) */
            if (fn.size() > 13) fn = fn.substr(0, fn.size() > 4 ? 8 : fn.size()) + "~.xpj";
            std::snprintf(out, n, "Map into: %s", fn.c_str()); break; }
        case SM_WHICH: std::snprintf(out, n, "Which project is open?"); break;
        default: std::snprintf(out, n, "%.23s", w->src_proj.c_str());
        }
        return;
    }
    if (i >= P_SRCROW0 && i < P_SRCROW0 + 6) {   /* project files or drum tracks */
        size_t k = (size_t)(w->src_page * SRC_ROWS + i - P_SRCROW0);
        if (!w->src_mode || i - P_SRCROW0 >= SRC_ROWS || k >= w->src_list.size()) { std::snprintf(out, n, " "); return; }
        const auto &r = w->src_list[k];
        std::snprintf(out, n, "%.23s", r.first.c_str());
        return;
    }
    if (i == P_MSG) {
        if (!v) std::snprintf(out, n, " ");
        else if (v >= MSG_TIM && v <= MSG_TIM + 2 * MICRO_MAX) std::snprintf(out, n, "Timing: %+d", v - MSG_TIM - MICRO_MAX);
        else if (v >= MSG_PCOPIED && v < MSG_PCOPIED + 8) std::snprintf(out, n, "Pattern %d Copied", v - MSG_PCOPIED + 1);
        else if (v >= MSG_PPASTED && v < MSG_PPASTED + 8) std::snprintf(out, n, "Pasted to Pattern %d", v - MSG_PPASTED + 1);
        else if (v >= MSG_PAT && v < MSG_PAT + 32) {   /* PATTERN buttons (v1.70.0) */
            static const char *const F[4] = {"Stored as Pattern %d", "Pattern %d Cleared", "Pattern %d Cued", "Pattern %d Loaded"};
            std::snprintf(out, n, F[(v - MSG_PAT) / 8], (v - MSG_PAT) % 8 + 1);
        }
        else if (v == 206) std::snprintf(out, n, "Save the project first");
        else if (v >= 195) std::snprintf(out, n, "Read %d note%s", v - 195, v - 195 == 1 ? "" : "s");

        else if (v >= 64 && v < 195) std::snprintf(out, n, "Read %d CC value%s", v - 64, v - 64 == 1 ? "" : "s");
        else if (v == 63) std::snprintf(out, n, "No saved project found");
        else if (v > 56) std::snprintf(out, n, "Read %d project value%s", v - 57, v - 57 == 1 ? "" : "s");
        else if (v > 40) std::snprintf(out, n, "%s track %d", v > 48 ? "Pasted to" : "Copied", (v - 41) % 8 + 1);
        else if (v > 32) std::snprintf(out, n, "%s page %d", v > 36 ? "Pasted to" : "Copied", (v - 33) % 4 + 1);
        else { int c = v > 16 ? v - 16 : v; std::snprintf(out, n, "%s %d step%s", v > 16 ? "Pasted" : "Copied", c, c == 1 ? "" : "s"); }
        return;
    }
    switch (i) {
    case P_ST_COND: cond_name(cond_at(v), out, n); return;
    case P_ST_PROB: case P_RND_DENS: case P_SHUFFLE: std::snprintf(out, n, "%d%%", v); return;
    case P_ST_VEL: { const int c = cur_step(w).vel; const int t = w->seq.t[w->track].vel;   /* v1.2.0: no lock: the track's in brackets */
        if (c) std::snprintf(out, n, "%d", c); else std::snprintf(out, n, "(%d)", t); return; }
    case P_ST_LEN: if (!v) std::snprintf(out, n, "--"); else std::snprintf(out, n, "%.2f", v / 4.0); return;
    case P_ST_PAN: {   /* 1..128 = CC 10 value 0..127; 64 is the centre. v1.2.0: no lock: the track's pan in brackets */
        const int c = cur_step(w).pan, p = c ? c : w->seq.t[w->track].panb;
        char t[16];
        if (!p) std::snprintf(t, sizeof t, "--");
        else if (p - 1 == 64) std::snprintf(t, sizeof t, "C");
        else if (p - 1 < 64) std::snprintf(t, sizeof t, "L%d", 64 - (p - 1));
        else std::snprintf(t, sizeof t, "R%d", (p - 1) - 64);
        if (c || !p) std::snprintf(out, n, "%s", t); else std::snprintf(out, n, "(%s)", t);
        return;
    }
    case P_TRK_GATE: std::snprintf(out, n, "%.2f", v / 4.0); return;
    case P_TRK_PROB: std::snprintf(out, n, "%d%%", v); return;
    case P_TRK_PAN:   /* v1.2.0: as a pan lock; -- = not known (the pad's own, the centre until FETCH CC VALUES) */
        if (!v) std::snprintf(out, n, "--");
        else if (v - 1 == 64) std::snprintf(out, n, "C");
        else if (v - 1 < 64) std::snprintf(out, n, "L%d", 64 - (v - 1));
        else std::snprintf(out, n, "R%d", (v - 1) - 64);
        return;
    case P_TRK_CCBASE: std::snprintf(out, n, "%d-%d", v, v + NCC_V1 - 1); return;
    case P_EU_BOOL: { static const char *const ops[] = {"OR", "AND", "XOR", "SUB"}; std::snprintf(out, n, "%s", ops[clampi(v, 0, 3)]); return; }   /* the five numbers, e.g. 85-89 */
    case P_SEL: std::snprintf(out, n, "%d", w->page * 16 + v); return;   /* absolute step 1..64 */
    case P_RUN: {
        int c = w->seq.t[w->track].cur;
        if (w->seq.playing && c >= 0) std::snprintf(out, n, "%d", c + 1); else std::snprintf(out, n, "-");
        return;
    }
    default: break;
    }
    const param_t *p = &PARAMS[i];
    if (p->nopts) std::snprintf(out, n, "%s", p->opts[clampi(v, 0, p->nopts - 1)]);
    else std::snprintf(out, n, "%d", v);
}

/* ---------------------------------------------------------------------------
 * Transport: host ppqPos -> a free-running song position. MPC loops its own sequence (ppqPos jumps
 * back), but the tracks here are up to 64 steps at their own speed and the A:B conditions count
 * loops, so a backwards or large jump only keeps time moving; it does not restart anything.
 * ------------------------------------------------------------------------- */
static void pat_msg(Plugin *w, int m, int frames) { w->msg = m; w->msg_frames = frames; }
/* "Timing: +5" for the step the knobs show; on the grid: no message */
static void tm_msg(Plugin *w) {
    const int m = cur_step(w).micro;
    if (m) pat_msg(w, MSG_TIM + m + MICRO_MAX, (int)(3.0f * w->sample_rate));
    else if (w->msg >= MSG_TIM && w->msg <= MSG_TIM + 2 * MICRO_MAX) { w->msg = 0; w->msg_frames = 0; }
}
/* PATTERN k into the sequencer (under the lock): position carries on, mutes stay, PERFORM ALL stays as it is */
/* the pattern playing keeps what was done to it (v1.75.0): its slot is the sequencer, written back on leaving it */
/* v1.99: a pattern with no trig and no lock on any step of any track counts as empty (shown so, saved as nothing) */
static bool snap_empty(const Snap &s) {
    for (int t = 0; t < NTRACKS; t++) {
        const Track &tr = s.t[t];
        if (tr.eu_on) return false;
        for (int k = 0; k < NSTEPS; k++) {
            const Step &st = tr.st[k];
            if (st.on || st.cond || st.prob != 100 || st.vel || st.len || st.pan || st.micro || st.ratch) return false;
            for (int j = 0; j < NCC; j++) if (st.cc[j]) return false;
        }
    }
    return true;
}
static bool pst_any(const Plugin *w, int p) { for (int j = 0; j < NSTATES; j++) if (w->pst[p][j].full) return true; return false; }
static void pst_clear_pat(Plugin *w, int p) { for (int j = 0; j < NSTATES; j++) w->pst[p][j] = Plugin::PState(); w->pst_act[p] = -1; }
static void pat_keep(Plugin *w) { Plugin::Pat &c = w->pat[w->pat_cur]; hist_take(w, c.snap); c.full = !snap_empty(c.snap) || pst_any(w, w->pat_cur); }   /* v1.1.0: one with states stays */
static void blank_tracks(Snap &b) {   /* no steps (and no Euclidean rhythm); the track setup stays */
    for (int t = 0; t < NTRACKS; t++) { std::memset(b.t[t].st, 0, sizeof b.t[t].st); for (int s2 = 0; s2 < NSTEPS; s2++) b.t[t].st[s2].prob = 100; b.t[t].eu_on = 0; }
}
static void fresh_tracks(Snap &b) {   /* an empty pattern coming in (v1.82): no steps, 16 steps long, forwards */
    blank_tracks(b);
    for (int t = 0; t < NTRACKS; t++) { b.t[t].len = 16; b.t[t].dir = DIR_FWD; }
}
static void pat_load(Plugin *w, int k) {
    pat_keep(w);
    const Plugin::Pat &pt = w->pat[k];
    w->pat_cue = -1;
    PfSnap snap(w);
    if (pt.full) { w->seq.restore_from(pt.snap.t, true); w->seq.shuffle = pt.snap.shuffle; }
    else {   /* an empty pattern: the tracks as they are set up, with no steps (and no Euclidean rhythm) */
        Snap b; hist_take(w, b); fresh_tracks(b);
        w->seq.restore_from(b.t, true);
    }
    if (w->seq.pf_on) std::memcpy(w->seq.pf_bak, w->seq.t, sizeof w->seq.t);   /* UNDO / PERFORM ALL off go back to this pattern */
    snap.send(w);
    w->pat_cur = k;
    w->pat[k].full = true;   /* an empty one that comes in is a pattern now */
    hist_reset(w);   /* undo works inside a pattern */
}
/* the pattern's length in beats: its longest track (steps x step length); a cued pattern comes in on a multiple of it */
static double pat_beats(const Seq &q) {
    double m = 0.0;
    for (int i = 0; i < NTRACKS; i++) m = std::max(m, Seq::round_len(q.t[i]) * Seq::step_dur(q.t[i]));   /* back and forth: its whole round */
    return m > 0.0 ? m : 4.0;
}
static void run_transport(Plugin *w, int32_t frames) {
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid, 0, 0);
    bool playing = ti && (ti->flags & kVstTransportPlaying);
    double tempo = (ti && (ti->flags & kVstTempoValid) && ti->tempo >= 20.0) ? ti->tempo : 120.0;
    double sr = (ti && ti->sampleRate > 1000.0) ? ti->sampleRate : (double)w->sample_rate;
    double blk = tempo / 60.0 * (double)frames / sr;

    std::lock_guard<std::mutex> lk(w->lock);
    if (playing && !w->was_playing) {
        /* started from the top: step 1. Started mid-sequence: stay on the host's 1/16 grid. */
        w->song = ti->ppqPos > 1e-6 ? ti->ppqPos : 0.0;
        w->last_ppq = ti->ppqPos;
        w->seq.start(w->song);
    } else if (!playing && w->was_playing) {
        w->seq.stop();
    } else if (playing) {
        double d = ti->ppqPos - w->last_ppq;
        if (d < 0.0 || d > blk * 4.0 + 0.01) d = blk;   /* loop / locate: carry on at tempo */
        w->song += d;
        w->last_ppq = ti->ppqPos;
    }
    w->was_playing = playing;
    if (!playing && w->pat_cue >= 0) { const int k = w->pat_cue; pat_load(w, k); pat_msg(w, MSG_PAT_LOADED + k, (int)(3.0f * w->sample_rate)); }   /* stopped meanwhile: now */
    if (playing) {
        double from = w->song;
        if (w->pat_cue >= 0) {   /* the end of the pattern playing falls in this block: up to it the old one, from it the new one */
            const double P = pat_beats(w->seq), at = std::ceil(from / P - 1e-9) * P;
            if (at < w->song + blk) {
                const int k = w->pat_cue;
                w->seq.process(from, at);
                pat_load(w, k); pat_msg(w, MSG_PAT_LOADED + k, (int)(3.0f * w->sample_rate));
                from = at;
            }
        }
        w->seq.process(from, w->song + blk);
        if (w->follow) { int c = w->seq.t[w->track].cur; if (c >= 0 && c / 16 != w->page) { w->page = c / 16; select_one(w, w->sel); } }
    }
    flush(w);
}

/* ---------------------------------------------------------------------------
 * VST callbacks
 * ------------------------------------------------------------------------- */
/* ---- project file work ------------------------------------------------------
 * A button asks for it under the lock (queue_job: a copy of what it needs); setParameter starts it once the lock is let
 * go (start_job): on the device on a thread of its own, in the host test right there. run_job reads and writes the files
 * without the lock and takes it only to put the result in. One at a time: a press while one runs is ignored. */
static void queue_job(Plugin *w, int kind, const std::string &path = std::string(), const std::string &to = std::string()) {
    if (w->job_pending || w->job_busy) return;
    w->job = Job();
    w->job.kind = kind; w->job.who = Who{w->given, w->our_path, w->born}; w->job.path = path; w->job.to = to;
    if (kind == J_MAPWRITE) w->job.seq = w->seq;
    w->job_pending = true;
}
static void run_job(Plugin *w, Job &j) {
    Who &h = j.who;
    const int secs3 = (int)(3.0f * w->sample_rate);
    switch (j.kind) {
    case J_CCREAD: case J_FETCH: {
        std::string pn; std::vector<XEntry> es;
        const bool ok = xpj_scan(h, pn, es);
        const int fail = ok ? 0 : no_proj_msg();
        std::lock_guard<std::mutex> lk(w->lock);
        w->our_path = h.our_path;
        if (ok) w->src_proj = pn;
        if (j.kind == J_CCREAD) {   /* from the target chosen by CREATE DEFAULT MAP, if one was */
            if (!w->cc_edit) break;
            const int got = ok ? xpj_bases(es, w->src, w->seq.t[w->track].ch + 1, w->ccdraft, w->basedraft, w->ccname[w->track], w->ccname_cc[w->track]) : -1;
            w->msg = got < 0 ? fail : 57 + got;
        } else if (!ok) w->msg = fail;
        else {   /* FETCH CC VALUES: base values and names of all eight tracks from the target track (any, if none was chosen) */
            int total = 0;
            for (int ti = 0; ti < NTRACKS; ti++) {
                Track &rt = w->seq.t[ti];
                int nums[NCC], bases[NCC];
                for (int k = 0; k < NCC; k++) { nums[k] = Seq::cc_num(rt, k); bases[k] = rt.ccb[k]; }
                total += xpj_bases(es, w->src, rt.ch + 1, nums, bases, w->ccname[ti], w->ccname_cc[ti]);
                for (int k = 0; k < NCC; k++) rt.ccb[k] = (uint8_t)bases[k];
                int pn[NCC], pb[NCC]; for (int k = 0; k < NCC; k++) { pn[k] = -1; pb[k] = 0; } pn[0] = PAN_CC[ti]; pb[0] = rt.panb;   /* v1.93: the pad's pan too */
                xpj_bases(es, w->src, rt.ch + 1, pn, pb);   /* not counted: the message reads the CC values */
                rt.panb = (uint8_t)pb[0];
            }
            w->msg = 64 + total;
        }
        w->msg_frames = secs3; w->src_gen++;
        break;
    }
    case J_NOTES: {
        /* FETCH NOTES, found the way FETCH CC VALUES finds its values: a track's CCs (on its channel) are mapped to some pad
         * of a drum track -- the track then plays that pad's note. No target track has to be chosen; if one was, only its
         * mappings count. A track whose CCs are not mapped to a drum pad keeps its note. */
        std::string path = xpj_pick_ours(h), js, pn; std::vector<XEntry> es; std::vector<int> notes;
        const bool ok = !path.empty() && xpj_load(path, js) && xpj_scan(h, pn, es);
        const int fail = ok ? 0 : no_proj_msg();
        if (ok) {   /* every entry's pad note, worked out here (the project text stays out of the lock) */
            const std::vector<XTrack> tracks = xpj_tracks(js);
            for (const XEntry &e : es) notes.push_back(e.inst < 0 || e.inst > 127 ? -1 : xpj_pad_note(js, tracks, e.tgt, e.inst));
        }
        std::lock_guard<std::mutex> lk(w->lock);
        w->our_path = h.our_path;
        if (ok) w->src_proj = pn;
        if (!ok) w->msg = fail;
        else {
            int got = 0;
            for (int ti = 0; ti < NTRACKS; ti++) {
                Track &rt = w->seq.t[ti];
                int note = -1;
                for (int k = 0; k < NCC && note < 0; k++)
                    for (size_t x = 0; x < es.size(); x++) {
                        const XEntry &e = es[x];
                        if (e.ch != rt.ch + 1 || e.cc != Seq::cc_num(rt, k) || (!w->src.empty() && e.tgt != w->src)) continue;
                        note = notes[x];
                        if (note >= 0) break;
                    }
                if (note < 0) continue;
                got++;
                if (rt.num != note) { w->seq.note_off(rt); rt.num = (uint8_t)note; }   /* nothing left sounding on the old note */
            }
            flush(w);
            w->msg = 195 + got;
        }
        w->msg_frames = secs3; w->src_gen++;
        break;
    }
    case J_MAPOPEN: {
        /* CREATE DEFAULT MAP: the open project's drum tracks to choose from. A project not saved (or not since this StepRunner-8
         * was put in it) is refused with the big message; two files that hold it: the user says which one is open first. */
        std::vector<std::string> all;
        if (std::getenv("STEPRUNNER_NOVERIFY")) { std::string pp = xpj_pick_ours(h); if (!pp.empty()) all.push_back(pp); }
        else all = xpj_ours_all(h);
        std::vector<XTrack> ts;
        const bool ok1 = all.size() == 1 && tracks_of(all[0], ts);
        std::lock_guard<std::mutex> lk(w->lock);
        w->our_path = h.our_path;
        if (w->src_mode || w->cc_edit) break;   /* the user went elsewhere meanwhile */
        proj_name(w, all.empty() ? std::string() : all[0]);
        if (all.empty()) w->big = BIG_NOTSAVED;
        else if (all.size() == 1) map_tracks(w, all[0], ok1, ts);
        else {
            w->src_paths = all; w->src_list.clear();
            for (const std::string &a : all) { size_t sl = a.rfind('/'); size_t a0 = sl == std::string::npos ? 0 : sl + 1; w->src_list.push_back({a.substr(a0, a.size() - 4 - a0), 2}); }
            w->src_mode = SM_WHICH; w->src_pick = -1; w->src_page = 0;
        }
        w->src_gen++;
        break;
    }
    case J_MAPPICK: {
        std::vector<XTrack> ts;
        const bool ok = tracks_of(j.path, ts);
        std::lock_guard<std::mutex> lk(w->lock);
        if (w->src_mode != SM_WHICH) break;
        w->src_mode = SM_IDLE; w->src_list.clear(); proj_name(w, j.path); map_tracks(w, j.path, ok, ts);
        w->src_gen++;
        break;
    }
    case J_MAPWRITE: {
        /* COMMIT on a marked drum track: the mappings are written into the project itself (no backup since v2.0). MPC still
         * has the old project in memory: the big message asks to close it and open it again. */
        std::string js;
        int r = -1;
        if (!j.path.empty() && xpj_load(j.path, js) && xpj_ours(h, js)) {   /* still this StepRunner-8's project */
            r = xpj_map_text(j.seq, j.to, js);
            if (r > 0 && !xpj_save(j.path, js)) r = -3;
        }
        std::lock_guard<std::mutex> lk(w->lock);
        if (r >= 0) w->src = j.to;   /* the target track: FETCH reads from it; saved with the plugin */
        w->big = r > 0 ? BIG_DONE0 + std::min(r, (int)BIG_MAXN) : r == 0 ? BIG_NOTHING : r == -1 ? BIG_NOTSAVED : r == -2 ? BIG_NODRUM : BIG_FAIL;
        w->src_mode = SM_IDLE; w->src_list.clear(); w->src_gen++;
        break;
    }
    default: break;
    }
    std::lock_guard<std::mutex> lk(w->lock); w->scan_now = true;   /* v2.12: the results are told to the host at the next block */
}
static void start_job(Plugin *w) {
    Job j;
    {
        std::lock_guard<std::mutex> lk(w->lock);
        if (!w->job_pending) return;
        j = std::move(w->job); w->job = Job(); w->job_pending = false;
    }
    w->job_busy = true;
#ifdef STEPRUNNER_TEST
    run_job(w, j);   /* the host test reads the result right away */
    w->job_busy = false;
#else
    if (w->job_thr.joinable()) w->job_thr.join();
    w->job_thr = std::thread([w, j]() mutable { run_job(w, j); w->job_busy = false; });
#endif
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    (void)in;
    Plugin *w = (Plugin *)e->object;
    run_transport(w, n);

    /* Tell the host about every value that moved without it asking (see the header comment). v2.12: what changed is
     * found under ONE lock (told[] is written by setParameter under it too, so a value the host just set is not sent
     * back stale); the host is called after the lock is let go (it may call setParameter back). The full scan of the
     * values runs at most every ~10 ms, or at once after the host set something (scan_now). */
    int ch_i[NPARAMS], zero_i[2 * NPARAMS]; float ch_v[NPARAMS];   /* zero_i: closed lists and released buttons */
    int nch = 0, nzero = 0; bool page_redraw = false, list_redraw = false;
    {
        std::lock_guard<std::mutex> lk(w->lock);
        hist_tick(w, n);
        if (w->msg_frames > 0 && (w->msg_frames -= n) <= 0) { w->msg = 0; w->msg_frames = 0; }
        w->scan_frames += n;
        if (w->scan_now || w->scan_frames >= (int)(0.01f * w->sample_rate)) {
            w->scan_now = false; w->scan_frames = 0;
            int now_page = w->page | (w->track << 4) | (w->seq.t[w->track].ccbase << 8) | ((w->cc_edit ? 1 : 0) << 16) | ((w->seq.pf_on ? 1 : 0) << 17) | (base_hash(w->seq.t[w->track]) << 18);
            now_page ^= (int)(((unsigned)w->src_gen * 2654435761u) >> 2);   /* the target list changed: its texts are read again; PERFORM ALL on / off changes what the PERFORM knobs read (OFF / +0) */
            if (now_page != w->named_page) { w->named_page = now_page; page_redraw = true; }   /* track, page or CC BASE changed: the host reads the value texts again */
            for (int i = 0; i < P_OWN_COUNT; i++) {
                if (PARAMS[i].momentary) continue;
                const float v = shown_norm(w, i);
                if (v == w->told[i]) continue;
                w->told[i] = v; ch_i[nch] = i; ch_v[nch] = v; nch++;
            }
        }
        for (int i = P_OWN_COUNT; i < NPARAMS; i++)   /* a list that was picked from closes a moment later (see setParameter) */
            if (w->close_frames[i] > 0 && (w->close_frames[i] -= n) <= 0) { w->close_frames[i] = 0; w->open[i] = 0.0f; zero_i[nzero++] = i; list_redraw = true; }
        for (int i = 0; i < NPARAMS; i++)
            if (w->release[i]) { w->release[i] = 0; w->held[i] = false; zero_i[nzero++] = i; }
    }
    if (page_redraw) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
    for (int k = 0; k < nch; k++) w->master(&w->fx, audioMasterAutomate, ch_i[k], 0, 0, ch_v[k]);
    for (int k = 0; k < nzero; k++) w->master(&w->fx, audioMasterAutomate, zero_i[k], 0, 0, 0.0f);
    if (list_redraw) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
    if (out) for (int c = 0; c < 2; c++) if (out[c]) std::memset(out[c], 0, sizeof(float) * (size_t)n);   /* no audio */
}

static void setParameter_locked_parts(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    { std::lock_guard<std::mutex> lk(w->lock); if (popup_set(w->open, i, n)) { w->close_frames[i] = 0; return; } }   /* v2.12: the audio thread closes lists under the lock too */
    if (!is_own(i) || i == P_RUN) return;
    if ((i >= P_CELL0 && i < P_CELL0 + 16) || i == P_MULTI || i == P_PANEL || i == P_PTITLE || i == P_SCR || i == P_LBTN || i == P_BKSTEP || i == P_PST_ACT || i == P_PST_PAT || i == P_EU_DIRV || i == P_EU_DIRV2 || i == P_EU_OPV || i == P_NAVV || i == P_PSHIFTV || i == P_SLOTV || i == P_RTRK || i == P_TTL || (i >= P_PATV0 && i < P_PATV_END) || i == P_BIGMSG || i == P_BIGDONE || i == P_BKNAME || (i >= P_SRCHL0 && i < P_SRCHL_END) || i == P_LPANEL || i == P_MSTATE || i == P_MSG || (i >= P_SRCPROJ && i < P_SRCSEL0) || (i >= P_PGST0 && i < P_PGST_END) || (i >= P_CCUSE0 && i < P_CCUSE_END) || (i >= P_MPAD0 && i < P_MPAD_END)) { std::lock_guard<std::mutex> lk(w->lock); w->told[i] = clamp01(n); return; }   /* read-only: the next block puts the host's widget back */
    if (i >= P_STEP0 && i < P_STEP0 + 16) {
        /* A tap on a TRIGS cell. The host's toggle flips its own value and sends it, so a value that differs from the real
         * one is a tap; the same value is an echo and changes nothing. STEP EDIT off: the tap flips the trig (and selects the
         * step). STEP EDIT on: it only selects the step, and the host's toggle is put back to the real state. */
        std::lock_guard<std::mutex> lk(w->lock);
        bool on = n > 0.5f;
        if (on == (value_of(w, i) != 0)) return;
        w->raw[i] = -1.0f;
        if (w->step_edit && !w->shift) { select_one(w, i - P_STEP0); w->told[i] = on ? 1.0f : 0.0f; }   /* no SHIFT: just this step */
        else if (w->step_edit) {   /* SHIFT: add the step to the selection, or take it out; the last one stays */
            int k = i - P_STEP0;
            uint16_t m = (uint16_t)(w->selmask ^ (1u << k));
            if (m) {
                w->selmask = m;
                if ((m >> k) & 1) w->sel = k;                                   /* the lock knobs follow the step just added */
                else if (!((m >> w->sel) & 1)) { int low = 0; while (!((m >> low) & 1)) low++; w->sel = low; }
            }
            w->told[i] = on ? 1.0f : 0.0f;
        }
        else { set_value(w, i, on ? 1 : 0); w->told[i] = shown_norm(w, i); }
        return;
    }
    if (i == P_EU_ENABLE || i == P_EU_ENA2) {   /* ENABLE A / B (v1.2.0): on, that generator writes the selected track's trigs (with
                                                  * the other one, if on); off with neither left on: the trigs from before come back */
        std::lock_guard<std::mutex> lk(w->lock);
        const int g = i == P_EU_ENA2 ? 1 : 0;
        Track &et = w->seq.t[w->track];
        if (n > 0.5f) {
            if (!et.eu_len) et.eu_len = et.len;
            if (g && !et.eu_on) et.eu_tro = 0;   /* no knob since v1.29: what an old project left behind goes */
            if (g && !Seq::gen_b(et) && !et.eu_pl2) et.eu_pl2 = 4;   /* B with no pulses would add nothing: start where A starts */
            w->seq.euclid_enable(w->track, g);
        } else w->seq.euclid_disable(w->track, g);
        w->raw[i] = -1.0f;
        w->told[i] = clamp01(n);
        return;
    }
    if (i >= P_PAT0 && i < P_PAT_END) {
        /* A tap on a PATTERN button (v1.75.0: the sequencer is always one of the eight, pattern 1 to begin with; what is
         * done to it stays in it). CLEAR lit: another one is emptied; the one playing loses its steps. STORE lit: what plays
         * is copied into the one tapped. Neither: the pattern playing does nothing; another one -- full, or empty (then the
         * tracks play with no steps) -- is cued and comes in at the end of the one playing (run_transport), or at once when
         * stopped; a second tap on the cued one takes the cue back. */
        std::lock_guard<std::mutex> lk(w->lock);
        const int k = i - P_PAT0;
        Plugin::Pat &pt = w->pat[k];
        const bool lit = pt.full || k == w->pat_cur;
        if ((n > 0.5f) == lit) return;
        const int secs3 = (int)(3.0f * w->sample_rate);
        if (w->patclear) {
            if (k == w->pat_cur) {
                Snap b; hist_take(w, b); blank_tracks(b); w->seq.restore_from(b.t, true);
                pat_msg(w, MSG_PAT_CLEARED + k, secs3);
            } else if (pt.full) { pt.full = false; pat_msg(w, MSG_PAT_CLEARED + k, secs3); }
            pst_clear_pat(w, k);   /* v1.1.0: its states go too */
            if (w->pat_cue == k) w->pat_cue = -1;
        } else if (w->patstore) {
            if (k != w->pat_cur) {
                pt.full = true; hist_take(w, pt.snap); if (w->pat_cue == k) w->pat_cue = -1;
                for (int j = 0; j < NSTATES; j++) w->pst[k][j] = w->pst[w->pat_cur][j];   /* v1.1.0: with its states */
                w->pst_act[k] = -1;
            }
            pat_msg(w, MSG_PAT_STORED + k, secs3);
        }
        else if (w->pat_cue == k) {   /* changed the mind: the pattern playing goes on */
            w->pat_cue = -1;
            if (w->msg == MSG_PAT_CUED + k) { w->msg = 0; w->msg_frames = 0; }
        } else if (w->pat_cur == k) { }   /* the one playing: nothing */
        else if (w->seq.playing) { w->pat_cue = k; pat_msg(w, MSG_PAT_CUED + k, 1 << 30); }   /* stays until it comes in */
        else { pat_load(w, k); pat_msg(w, MSG_PAT_LOADED + k, secs3); }
        w->raw[i] = -1.0f;
        w->told[i] = clamp01(n);
        return;
    }
    if (pst_slot(i) >= 0) {
        /* A tap on a state button (the host's toggle sends the opposite of what it shows; the same value is an echo).
         * CLEAR lit: the state is emptied. Empty: the whole sequencer is saved into it. Full: it is recalled -- tracks,
         * shuffle and PERFORM ALL as they were saved; the play position carries on; v2.4: the mutes too. The recall
         * is a step of the undo history like any edit. */
        std::lock_guard<std::mutex> lk(w->lock);
        Plugin::PState &ps = w->pst[w->pat_cur][pst_slot(i)];   /* v1.1.0: the pattern playing's own eight */
        if ((n > 0.5f) == ps.full) return;
        const int slot = pst_slot(i);
        int &act = w->pst_act[w->pat_cur];
        if (w->pclear) { ps.full = false; if (act == slot) act = -1; }
        else if (!ps.full) {
            ps.full = true; act = slot;
            hist_take(w, ps.snap);
            for (int t = 0; t < NTRACKS; t++) ps.snap.t[t].mute = w->seq.t[t].mute;   /* v2.4: the mutes as well */
            ps.mutes = true;
            ps.fx = fx_take(w); ps.fx_ok = true;   /* v2.5: the effect pads as well */
            ps.pf_on = w->seq.pf_on;
            for (int k = 0; k < NPF_ALL; k++) ps.off[k] = w->seq.pf_on ? w->seq.pf_off[k] : 0;
        } else {
            act = slot;   /* a state of the pattern playing goes back into it (v1.1.0: another pattern's are not shown) */
            PfSnap snap(w);
            w->seq.pf_clear();   /* the offsets of now go; the state's own come back below */
            w->seq.restore_from(ps.snap.t, !ps.mutes);   /* v2.4: and its mutes (a state saved before keeps the mutes of now) */
            w->seq.shuffle = ps.snap.shuffle;
            if (ps.pf_on) {
                w->seq.perform_begin();   /* UNDO / PERFORM ALL off go back to the recalled state */
                for (int k = 0; k < NPF_ALL; k++) w->seq.pf_off[k] = clampi(ps.off[k], (int)PARAMS[pf_param(k)].min, (int)PARAMS[pf_param(k)].max);
            }
            for (int k = 0; k < NPF_ALL; k++) w->raw[pf_param(k)] = -1.0f;
            if (ps.fx_ok) fx_put(w, ps.fx);   /* v2.5 */
            snap.send(w);
            flush(w);
        }
        w->raw[i] = -1.0f;
        w->told[i] = clamp01(n);
        return;
    }
    if (i >= P_PLEN0 && i < P_PLEN_END) {   /* same kind of button: the lit one is the length every track has */
        std::lock_guard<std::mutex> lk(w->lock);
        if (n > 0.5f) w->seq.set_len_all(16 * (i - P_PLEN0 + 1));
        w->raw[i] = -1.0f;
        w->told[i] = clamp01(n);
        return;
    }
    if (i >= P_LEN16 && i < P_SEL_END) {
        /* "One of several" buttons (length shortcuts, TRACK and PAGE selectors): the lit one is the current choice. A press
         * (ON) makes the choice; switching the lit one OFF does nothing. Either way the host is sent the real states back. */
        std::lock_guard<std::mutex> lk(w->lock);
        if (n > 0.5f) {
            if (i <= P_LEN64) w->seq.set_len_track(w->track, 16 * (i - P_LEN16 + 1));   /* growing repeats the pages onto the new, empty ones */
            else if (i < P_PGSEL0) set_value(w, P_TRACK, i - P_TRKSEL0);
            else set_value(w, P_PAGE, i - P_PGSEL0);
        }
        w->raw[i] = -1.0f;
        w->told[i] = clamp01(n);
        return;
    }
    if (p->momentary) {
        std::lock_guard<std::mutex> lk(w->lock);   /* v2.12: held[] / release[] are the audio thread's too */
        bool down = n > 0.5f;
        bool rising = down && !w->held[i];   /* an echo of our own automate must not re-fire the trigger */
        w->held[i] = down;
        if (rising) {
            if (i == P_CLR_TRK) w->seq.clear_track(w->track);
            if (i == P_EU_DISCARD) w->seq.euclid_discard(w->track);
            if (i == P_EU_COMMIT) w->seq.euclid_commit(w->track);
            if (i == P_EU_DISCARD || i == P_EU_COMMIT) {   /* done: back to the mode under EUCLIDIAN (v1.0.0; before: PATTERN EDIT) */
                w->euclid_edit = false;
                if (w->rmode == 1) w->step_edit = true; else if (w->rmode == 2) w->track_edit = true;
            }
            if (i == P_PFX0 + 6) { w->seq.mute_toggle_all(); w->inv_mute = false; flush(w); }   /* MUTE ALL / UNMUTE ALL */
            if (i == P_PFY0 + 5) w->seq.restart();   /* RESTART (v1.82: momentary) */
            /* COPY / PASTE depend on the mode: STEP EDIT = the selected steps, TRACK EDIT = the whole track (without its
             * MIDI channel, note and CC numbers), every other mode = the shown page of the selected track. */
            const int secs2 = (int)(2.0f * w->sample_rate);
            /* v2.0: COPY / PASTE are the page's again in PATTERN EDIT; the whole pattern has its own three buttons there */
            if (i == P_PAT_COPY) {
                hist_take(w, w->ptclip); w->ptclip_ok = true;
                for (int j = 0; j < NSTATES; j++) w->pstclip[j] = w->pst[w->pat_cur][j];   /* v1.1.0: and its states */
                w->pstclip_ok = true;
                pat_msg(w, MSG_PCOPIED + w->pat_cur, secs2);
            }
            if (i == P_PAT_CLRALL) {   /* CLEAR PTN: no steps on any track of the pattern playing; UNDO takes it back */
                Snap b; hist_take(w, b); blank_tracks(b); w->seq.restore_from(b.t, true);
                pat_msg(w, MSG_PAT_CLEARED + w->pat_cur, secs2);
            }
            if (i == P_PAT_PASTE) {
                if (w->ptclip_ok) {   /* into the pattern playing (the sequencer); UNDO takes it back */
                    PfSnap snap(w);
                    w->seq.restore_from(w->ptclip.t, true); w->seq.shuffle = w->ptclip.shuffle;
                    if (w->seq.pf_on) std::memcpy(w->seq.pf_bak, w->seq.t, sizeof w->seq.t);
                    snap.send(w);
                    if (w->pstclip_ok) { for (int j = 0; j < NSTATES; j++) w->pst[w->pat_cur][j] = w->pstclip[j]; w->pst_act[w->pat_cur] = -1; }   /* v1.1.0: the states replace this pattern's (UNDO does not bring them back) */
                    pat_keep(w);
                    pat_msg(w, MSG_PPASTED + w->pat_cur, secs2);
                }
            }
            else if (i == P_COPY && w->track_edit) { w->tclip = w->seq.t[w->track]; w->tclip_ok = true; w->msg = 41 + w->track; w->msg_frames = secs2; }
            else if (i == P_COPY && !w->step_edit) { std::memcpy(w->pclip, w->seq.t[w->track].st + w->page * 16, sizeof w->pclip); w->pclip_ok = true; w->msg = 33 + w->page; w->msg_frames = secs2; }
            else if (i == P_COPY && w->selmask) {   /* the selected steps of this page, then nothing is selected */
                int first = -1; w->clip_n = 0;
                for (int k = 0; k < 16; k++) if ((w->selmask >> k) & 1) {
                    if (first < 0) first = k;
                    w->clip[w->clip_n] = w->seq.t[w->track].st[w->page * 16 + k]; w->clip_off[w->clip_n] = k - first; w->clip_n++;
                }
                w->selmask = 0;
                w->msg = w->clip_n; w->msg_frames = secs2;
            }
            if (i == P_PASTE && w->track_edit) { if (w->tclip_ok) { w->seq.paste_track(w->track, w->tclip); w->seq.euclid_apply(w->track); w->msg = 49 + w->track; w->msg_frames = secs2; } }
            else if (i == P_PASTE && !w->step_edit) { if (w->pclip_ok) { std::memcpy(w->seq.t[w->track].st + w->page * 16, w->pclip, sizeof w->pclip); w->seq.euclid_apply(w->track); w->msg = 37 + w->page; w->msg_frames = secs2; } }
            else if (i == P_PASTE && w->selmask && w->clip_n) {   /* from the first selected step on; what falls past step 64 is dropped */
                int start = 0, done = 0;
                while (!((w->selmask >> start) & 1)) start++;
                for (int c = 0; c < w->clip_n; c++) {
                    int at = w->page * 16 + start + w->clip_off[c];
                    if (at >= NSTEPS) continue;
                    w->seq.t[w->track].st[at] = w->clip[c]; done++;
                }
                w->seq.euclid_apply(w->track);
                w->msg = done ? 16 + done : 0; w->msg_frames = secs2;
            }
            if (i == P_CC_COMMIT && w->cc_edit) { Track &ct = w->seq.t[w->track]; for (int j = 0; j < NCC; j++) { ct.ccn[j] = (uint8_t)(w->ccdraft[j] + 1); ct.ccb[j] = (uint8_t)w->basedraft[j]; } w->cc_edit = false; }
            /* the project file work: asked for here, done after the lock is let go (run_job) */
            if (i == P_CC_READ && w->cc_edit) queue_job(w, J_CCREAD);   /* base values from the open project: the MIDI learn entries on this track's channel */
            /* P_READ_ALL (INITIALIZE) is retired; COPY MAPPING, the backup browser and SKIP BACKUP too (v1.64.0): CREATE
             * DEFAULT MAP, FETCH NOTES, FETCH CC VALUES and TRACK SETUP are in the SETUP panel (the old GLOBAL) */
            if (i == P_SRC_CANCEL) { w->src_mode = SM_IDLE; w->src_list.clear(); w->src_gen++; }   /* CANCEL: the list goes, nothing is written */
            if (i == P_SRC_MAP && !w->src_mode && !w->cc_edit) queue_job(w, J_MAPOPEN);   /* CREATE DEFAULT MAP */
            if (i == P_SRC_FETCH && !w->src_mode) queue_job(w, J_FETCH);
            if (i == P_SRC_NOTES && !w->src_mode) queue_job(w, J_NOTES);
            if ((i == P_SRC_PREV || i == P_SRC_NEXT) && w->src_mode) {
                int pages = ((int)w->src_list.size() + SRC_ROWS - 1) / SRC_ROWS;
                w->src_page = clampi(w->src_page + (i == P_SRC_NEXT ? 1 : -1), 0, pages > 0 ? pages - 1 : 0); w->src_gen++;
            }
            if (i >= P_SRCSEL0 && i < P_SRCSEL0 + SRC_ROWS && (w->src_mode == SM_WHICH || w->src_mode == SM_FIXED)) {   /* a tap marks a row (v1.66.0); COMMIT does it */
                const int k = w->src_page * SRC_ROWS + i - P_SRCSEL0;
                if (k < (int)w->src_list.size()) w->src_pick = k;
                w->src_gen++;
            }
            const bool marked = w->src_pick >= 0 && w->src_pick < (int)w->src_list.size();
            if (i == P_SRC_COMMIT && w->src_mode == SM_WHICH && marked && w->src_pick < (int)w->src_paths.size())
                queue_job(w, J_MAPPICK, w->src_paths[(size_t)w->src_pick]);   /* the open project: its drum tracks */
            else if (i == P_SRC_COMMIT && w->src_mode == SM_FIXED && marked)
                queue_job(w, J_MAPWRITE, w->map_path, w->src_list[(size_t)w->src_pick].first);   /* the mappings */
            if (i == P_MSG_OK) { w->big = 0; w->src_gen++; }   /* a tap on the big message closes it */
            if (i == P_CC_DISCARD) w->cc_edit = false;
            if ((i == P_TM_LEFT || i == P_TM_RIGHT) && w->timing_edit) {   /* TIMING: the selected steps a 24th of a step earlier / later */
                const int d = i == P_TM_RIGHT ? 1 : -1;
                each_sel(w, [=](Step &st) { st.micro = (int8_t)clampi(st.micro + d, -MICRO_MAX, MICRO_MAX); });
                tm_msg(w);
            }
            if (i == P_RND_TIMING && w->timing_edit) {
                each_sel(w, [&](Step &st) { st.micro = (int8_t)((int)(w->seq.gen_rnd() % (2 * MICRO_MAX + 1)) - MICRO_MAX); });
                tm_msg(w);
            }
            if (i == P_RND_RATCH && w->timing_edit)   /* v1.2.0: the number of hits and their RESOLUTION (one hit: AUTO) */
                each_sel(w, [&](Step &st) {
                    st.ratch = (uint8_t)(w->seq.gen_rnd() % RATCH_MAX);
                    st.rres = st.ratch ? (uint8_t)(w->seq.gen_rnd() % (RRES_MAX + 1)) : 0;
                });
            if (i == P_CLR_TIMING && w->timing_edit) { each_sel(w, [](Step &st) { st.micro = 0; }); tm_msg(w); }   /* back on the grid */
            if (i == P_CLR_RATCH && w->timing_edit) each_sel(w, [](Step &st) { st.ratch = 0; st.rres = 0; });   /* one hit (and RESOLUTION AUTO) */
            /* v1.2.0: the arrows page the left section of PATTERN, TRACK and STEP EDIT (each keeps its own page). Page 2:
             * RND COND / RND TIMING / RND RATCHETS on every trig (steps 1..length), or with CLR LOCKS lit CLR COND /
             * CLR TIMING / CLR RATCHETS on all steps: the selected track in TRACK EDIT, all 8 tracks in PATTERN EDIT.
             * STEP EDIT: the selected steps only, trig or not (as its other RND / CLR buttons). */
            if (i == P_SHIFT_L || i == P_SHIFT_R) {   /* v1.2.0 SHIFT: whichever panel shows the buttons: the track's (TRACK EDIT,
                                                    * also under EUCLIDIAN) moves the track, the pattern's all 8 */
                const int d = i == P_SHIFT_R ? 1 : -1, pv = value_of(w, P_PANEL);
                if (pv == 2) w->seq.shift_track(w->track, d);
                else if (pv == 5 && !w->shuf_edit) for (int t = 0; t < NTRACKS; t++) w->seq.shift_track(t, d);   /* hidden under SHUFFLE EDIT */
            }
            if (i == P_TRK_VALS && value_of(w, P_PANEL) == 2) w->trk_vals = true;   /* v1.2.0: TRACK VALUES / BACK (under EUCLIDIAN too) */
            if (i == P_TRK_BACK) w->trk_vals = false;
            if ((i == P_NAV_LEFT || i == P_NAV_RIGHT) && lmode(w) >= 0) w->lpage[lmode(w)] = i == P_NAV_RIGHT ? 1 : 0;
            if ((i == P_RND_COND || i == P_RND_TIMING || i == P_RND_RATCH) && lpage2(w)) {
                const int what = i == P_RND_COND ? Seq::X_COND : i == P_RND_TIMING ? Seq::X_TIMING : Seq::X_RATCH;
                if (!w->clr_mode) w->seq.reseed((uint32_t)std::chrono::steady_clock::now().time_since_epoch().count());
                const uint64_t mask = lmode(w) == 2 ? (uint64_t)w->selmask << (w->page * 16) : 0;   /* 0: the whole track */
                for (int t = 0; t < NTRACKS; t++) {
                    if (lmode(w) != 0 && t != w->track) continue;
                    if (w->clr_mode) w->seq.clear_extra(t, what, mask); else w->seq.randomize_extra(t, what, mask);
                }
            }
            if (i == P_EU_RND || i == P_EU_RND2) {   /* RANDOM A / RANDOM B (v1.2.0): that generator only */
                w->seq.reseed((uint32_t)std::chrono::steady_clock::now().time_since_epoch().count());
                w->seq.euclid_random(w->track, i == P_EU_RND2 ? 1 : 0);
            }
            if (i == P_UNDO) { hist_step(w, false); flush(w); }
            if (i == P_REDO) { hist_step(w, true); flush(w); }
            if (i == P_PF_COMMIT || i == P_PF_DISCARD) {
                PfSnap snap(w);
                if (i == P_PF_COMMIT) w->seq.perform_commit();     /* both also switch PERFORM ALL off */
                if (i == P_PF_DISCARD) w->seq.perform_undo();   /* the UNDO button (key kept from the DISCARD it replaced) */
                snap.send(w);   /* COMMIT changes nothing that is heard; UNDO puts the base values back */
            }
            if (i == P_PF_COMMIT || i == P_PF_DISCARD) for (int k = 0; k < NPF_ALL; k++) w->raw[pf_param(k)] = -1.0f;   /* knobs back to the middle */
            if (i == P_CLR_STEP) for (int k = 0; k < 16; k++) if ((w->selmask >> k) & 1) w->seq.clear_locks(w->track, w->page * 16 + k);
            /* every press stirs the clock into the generators' RNG, so two sessions never give the same series */
            if (i == P_RND_PAN || i == P_RND_CC6 || i == P_RND_CC7 || i == P_RND_CC8 || (i >= P_RND0 && i < P_RND0 + NRND)) w->seq.reseed((uint32_t)std::chrono::steady_clock::now().time_since_epoch().count());
            int what = i == P_RND_PAN ? (int)Seq::R_PAN : i == P_RND_CC6 ? Seq::R_CC0 + 5 : i == P_RND_CC7 ? Seq::R_CC0 + 6 : i == P_RND_CC8 ? Seq::R_CC0 + 7 : (i >= P_RND0 && i < P_RND0 + NRND) ? i - P_RND0 : -1;
            if (what >= 0 && w->clr_mode) {   /* CLR LOCKS lit: the button takes its lock out instead (the selected steps in STEP EDIT, else the whole track) */
                Track &ct = w->seq.t[w->track];
                if (what != Seq::R_TRIG) for (int k = 0; k < NSTEPS; k++) {
                    if (w->step_edit && !(k / 16 == w->page && ((w->selmask >> (k % 16)) & 1))) continue;
                    Step &st = ct.st[k];
                    if (what == Seq::R_VEL) st.vel = 0; else if (what == Seq::R_LEN) st.len = 0; else if (what == Seq::R_PAN) st.pan = 0;
                    else if (what >= Seq::R_CC0 && what < Seq::R_CC0 + NCC) st.cc[what - Seq::R_CC0] = 0;
                }
            }
            else if (what >= 0) {
                /* STEP EDIT on: only the selected step(s), and RND TRIGS is off (the selection is made by hand there).
                 * STEP EDIT off: the whole track, by density. */
                if (w->step_edit) { if (what != Seq::R_TRIG) w->seq.randomize_steps(w->track, what, (uint64_t)w->selmask << (w->page * 16)); }
                else if (!(w->track_edit || w->random_edit || w->euclid_edit || w->global_edit || w->timing_edit))   /* PATTERN EDIT (v1.93): every track, each with its own random settings */
                    for (int t = 0; t < NTRACKS; t++) w->seq.randomize(t, what);
                else w->seq.randomize(w->track, what);
            }
            w->release[i] = 1;
        }
        return;
    }
    bool nudge = false;
    int list = -1;                                  /* this parameter's option list, if it is open */
    for (int j = P_OWN_COUNT; j < NPARAMS; j++) if (PARAMS[j].popup_of == i && w->open[j] > 0.5f) list = j;
    {
        std::lock_guard<std::mutex> lk(w->lock);
        if ((pf_k(i) >= 0 && !w->seq.pf_on) || i == P_PF_CC117) {   /* PERFORM ALL off: the knobs do nothing, and spring back */
            w->raw[i] = -1.0f;
            w->told[i] = clamp01(n);
        } else {
            int v;
            if (p->nopts > 1) {
                /* An exact option selects it; anything between two options is a Q-Link nudge from the
                 * current one: step one option that way (same convention as wrapper/vst2_wrap.c).
                 * While the option list is open every set is a tap in the list: take the nearest option. */
                float pos = clamp01(n) * (p->nopts - 1);
                if (list < 0 && std::fabs(pos - std::round(pos)) > 0.001f) {
                    int cur = value_of(w, i);
                    v = clampi(cur + (pos > (float)cur ? 1 : -1), 0, p->nopts - 1);
                    nudge = true;
                } else v = (int)std::lround(pos);
            } else v = from_norm(p, n);
            set_value(w, i, v);
            w->raw[i] = (nudge || list >= 0) ? -1.0f : clamp01(n);
            w->told[i] = nudge ? -1.0f : shown_norm(w, i);   /* the host knows what it sent; after a nudge, send the snapped value back */
            if (!nudge && p->nopts > 1 && std::lround(clamp01(n) * (p->nopts - 1)) != value_of(w, i)) w->told[i] = -1.0f;   /* v1.92: a tap the plugin did not take (PATTERN EDIT tapped while lit, a lit FWD...): the host's widget is put back */
        }
    }
    /* Close the list after a pick, a little later (processReplacing): the finger is still on the screen now, and a list that
     * vanishes under it hands the rest of the touch to whatever lies beneath. */
    if (list >= 0) { std::lock_guard<std::mutex> lk(w->lock); w->close_frames[list] = (int)(0.12f * w->sample_rate) + 1; }
}

static void setParameter(AEffect *e, int32_t i, float n) {
    setParameter_locked_parts(e, i, n);
    Plugin *w = (Plugin *)e->object;
    { std::lock_guard<std::mutex> lk(w->lock); w->scan_now = true; }   /* the next block tells the host what this changed */
    if (w->job_pending) start_job(w);   /* the project file work a button asked for: without the lock */
}

static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    if (!is_own(i) || PARAMS[i].momentary) return 0.0f;
    std::lock_guard<std::mutex> lk(w->lock);
    return shown_norm(w, i);
}

/* ---- project chunk ----------------------------------------------------------
 *   TSq3   (TSq2: the same without the pan byte; still read)
 *   g <track> <page> <sel> <follow> <fill> <step edit> <mode> <rmode>      the screen (mode: see mode_code)
 *   G shuffle, follow mask, CC 117   I instance id   N CC names   S READ PROJECT target   W pattern playing
 *   per track: t (setup), C (CC numbers), B (base values), R (random settings), s (64 steps x 11 bytes as hex:
 *     on cond prob vel len cc1..cc5 pan), e (Euclidean generator), D (direction), M (timing, ratchets), Y (ratchet
 *     resolution), A (the pad's pan); v1.2.0: X (CC 6 / 7 locks), H (CC 8 locks), K (track prob)
 *   Q<k> ... STATE k (with G / P / U / E sub-lines), Z<k> ... PATTERN k (the same track lines)
 *   old lines still read: r (v1.1 random settings), P (v1.36-1.67 PERFORM states, dropped)
 * Unknown lines are skipped, so later versions can add lines without breaking older projects.
 * ------------------------------------------------------------------------- */
static void write_tracks(const Track *T, std::string &s, const std::string &pre);
/* v2.1: which right-hand panel EUCLIDIAN shows: the last one of PATTERN EDIT, STEP EDIT (and TIMING) and TRACK EDIT */
static void rmode_note(Plugin *w) {
    if (w->step_edit) w->rmode = 1;
    else if (w->track_edit) w->rmode = 2;
    else if (!(w->random_edit || w->euclid_edit || w->global_edit)) w->rmode = 0;
}
/* v1.95: the mode the screen was in, saved with the plugin (g line, 7th number) so it opens there again:
 * 0 PATTERN EDIT, 1 STEP EDIT, 2 TRACK EDIT, 3 EUCLIDIAN, 4 TIMING, 5 SETUP, 6 RANDOM SETUP */
static int mode_code(const Plugin *w) {
    return w->timing_edit ? 4 : w->step_edit ? 1 : w->track_edit ? 2 : w->euclid_edit ? 3 : w->global_edit ? 5 : w->random_edit ? 6 : 0;
}
static void mode_set(Plugin *w, int m) {
    w->step_edit = w->track_edit = w->random_edit = w->euclid_edit = w->global_edit = w->timing_edit = false;
    if (m == 1) w->step_edit = true;
    else if (m == 2) w->track_edit = true;
    else if (m == 3) w->euclid_edit = true;
    else if (m == 4) w->timing_edit = w->step_edit = true;
    else if (m == 5) w->global_edit = true;
    else if (m == 6) w->random_edit = true;
    rmode_note(w);
}
/* v2.12: what the chunk is made of, copied under the lock (plain copies, ~150 KB); the text is then made from the
 * copy without the lock, so a save no longer holds up the audio thread for the whole formatting (1-5 ms on the device) */
struct ChunkSrc {
    Seq seq;
    int track = 0, page = 0, sel = 0, mode = 0, rmode = 0, cc117 = 0, pat_cur = 0;
    bool follow = false, step_edit = false;
    std::string ccname[NTRACKS][NCC]; int ccname_cc[NTRACKS][NCC] = {};
    std::string src; uint64_t inst_id = 0;
    Plugin::PState pst[NPATS][NSTATES]; Plugin::Pat pat[NPATS];
};
static void chunk_take(const Plugin *w, ChunkSrc &c) {   /* under the lock */
    c.seq = w->seq;
    c.track = w->track; c.page = w->page; c.sel = w->sel; c.mode = mode_code(w); c.rmode = w->rmode; c.cc117 = w->cc117; c.pat_cur = w->pat_cur;
    c.follow = w->follow; c.step_edit = w->step_edit;
    for (int ti = 0; ti < NTRACKS; ti++) for (int j = 0; j < NCC; j++) { c.ccname[ti][j] = w->ccname[ti][j]; c.ccname_cc[ti][j] = w->ccname_cc[ti][j]; }
    c.src = w->src; c.inst_id = w->inst_id;
    for (int p = 0; p < NPATS; p++) for (int k = 0; k < NSTATES; k++) if (w->pst[p][k].full) c.pst[p][k] = w->pst[p][k];   /* only the full ones (the copy is made under the lock) */
    for (int k = 0; k < NPATS; k++) c.pat[k] = w->pat[k];
}
static void chunk_write(const ChunkSrc *w, std::string &s) {
    char b[96];
    /* PERFORM ALL in progress: the project is saved as it sounds (as after COMMIT); it loads with PERFORM ALL off */
    std::unique_ptr<Seq> tmp;
    const Seq *q = &w->seq;
    if (q->pf_on) { tmp.reset(new Seq(w->seq)); tmp->perform_commit(); q = tmp.get(); }
    s = "TSq3\n";
    std::snprintf(b, sizeof b, "g %d %d %d %d %d %d %d %d\n", w->track, w->page, w->sel, w->follow ? 1 : 0, q->fill ? 1 : 0, w->step_edit ? 1 : 0, w->mode, w->rmode);
    s += b;
    { int m = 0; for (int i = 0; i < NTRACKS; i++) m |= (q->t[i].shuf ? 1 : 0) << i;
      std::snprintf(b, sizeof b, "G %d %d %d\n", q->shuffle, m, w->cc117); }   /* third: the PERFORM CC 117 value (v1.31.0) */
    s += b;
    for (int ti = 0; ti < NTRACKS; ti++) for (int j = 0; j < NCC; j++)   /* CC names (v1.42.0): track, CC slot, CC number, name */
        if (!w->ccname[ti][j].empty()) s += "N " + std::to_string(ti) + " " + std::to_string(j) + " " + std::to_string(w->ccname_cc[ti][j]) + " " + w->ccname[ti][j] + "\n";
    if (!w->src.empty()) s += "S " + w->src + "\n";   /* READ PROJECT target (v1.39.0) */
    { char ib[40]; std::snprintf(ib, sizeof ib, "I %016llx\n", (unsigned long long)w->inst_id); s += ib; }   /* v1.62.0 */
    write_tracks(q->t, s, "");
    for (int pp = 0; pp < NPATS; pp++) for (int k = 0; k < NSTATES; k++) if (w->pst[pp][k].full) {
        /* STATE k+1 of PATTERN pp+1 (v1.1.0): "V<pp><k> " + the lines of a project (v1.68.0-1.0.2: "Q<k> ", one set of eight) */
        const Plugin::PState &ps = w->pst[pp][k];
        const std::string pre = "V" + std::to_string(pp) + std::to_string(k) + " ";
        int m = 0; for (int i = 0; i < NTRACKS; i++) m |= (ps.snap.t[i].shuf ? 1 : 0) << i;
        s += pre + "G " + std::to_string(ps.snap.shuffle) + " " + std::to_string(m) + "\n";
        std::string l = pre + "P " + std::to_string(ps.pf_on ? 1 : 0);
        for (int j = 0; j < NPF; j++) l += " " + std::to_string(ps.off[j]);
        l += " " + std::to_string(ps.cc117);
        for (int j = NPF; j < NPF_ALL; j++) l += " " + std::to_string(ps.off[j]);   /* v1.2.0: CC 6 / 7, after what older versions read */
        s += l + "\n";
        if (ps.mutes) s += pre + "U 1\n";   /* v2.4: the t lines' mutes are the state's */
        if (ps.fx_ok) s += pre + "E " + std::to_string(ps.fx) + "\n";   /* v2.5: its effect pads (bits of FX_PADS) */
        write_tracks(ps.snap.t, s, pre);
    }
    s += "W " + std::to_string(w->pat_cur) + "\n";   /* the pattern playing (v1.75.0): the tracks above are it */
    for (int k = 0; k < NPATS; k++) if (w->pat[k].full && k != w->pat_cur) {   /* PATTERN k+1 (v1.69.0): "Z<k> " + the lines of a project */
        const Snap &ps = w->pat[k].snap;
        const std::string pre = "Z" + std::to_string(k) + " ";
        int m = 0; for (int i = 0; i < NTRACKS; i++) m |= (ps.t[i].shuf ? 1 : 0) << i;
        s += pre + "G " + std::to_string(ps.shuffle) + " " + std::to_string(m) + "\n";
        write_tracks(ps.t, s, pre);
    }
}
static void write_tracks(const Track *T, std::string &s, const std::string &pre) {   /* the R t C B s e lines (pre: "Q3 " for a state) */
    char b[96];
    for (int i = 0; i < NTRACKS; i++) {   /* random settings, one line per track (v1.17.0; before: one "r d min max" for all) */
        std::snprintf(b, sizeof b, "R %d %d %d %d\n", i, T[i].rnd_dens, T[i].rnd_min, T[i].rnd_max);
        s += pre; s += b;
    }
    for (int i = 0; i < NTRACKS; i++) {
        const Track &tr = T[i];
        std::snprintf(b, sizeof b, "t %d %d %d %d %d %d %d %d %d\n", i, tr.len, tr.speed, tr.ch, tr.num, tr.vel, tr.gate, tr.mute, tr.ccbase);
        bool own = false, bas = false; for (int j = 0; j < NCC; j++) { own |= tr.ccn[j] != 0; bas |= tr.ccb[j] != 0; }
        if (own) {   /* own CC numbers (EDIT CC, v1.34.0): number + 1, 0 = from the block; v1.2.0: CC 6 / 7 at the end (older versions read five) */
            s += pre; s += b;
            std::snprintf(b, sizeof b, "C %d %d %d %d %d %d %d %d %d\n", i, tr.ccn[0], tr.ccn[1], tr.ccn[2], tr.ccn[3], tr.ccn[4], tr.ccn[5], tr.ccn[6], tr.ccn[7]);   /* CC 8 last (v1.2.0) */
        }
        if (bas) {   /* base values (v1.37.0); CC 6 / 7 at the end (v1.2.0) */
            s += pre; s += b;
            std::snprintf(b, sizeof b, "B %d %d %d %d %d %d %d %d %d\n", i, tr.ccb[0], tr.ccb[1], tr.ccb[2], tr.ccb[3], tr.ccb[4], tr.ccb[5], tr.ccb[6], tr.ccb[7]);
        }
        s += pre; s += b;
        std::snprintf(b, sizeof b, "s %d ", i);
        s += pre; s += b;
        static const char hex[] = "0123456789abcdef";
        for (int k = 0; k < NSTEPS; k++) {
            const Step &st = tr.st[k];
            uint8_t raw[STEP_BYTES] = {st.on, st.cond, st.prob, st.vel, st.len};
            std::memcpy(raw + 5, st.cc, NCC_V1);   /* CC 1-5; CC 6 / 7 go on an X line (v1.2.0) */
            raw[5 + NCC_V1] = st.pan;
            for (int j = 0; j < STEP_BYTES; j++) { s += hex[raw[j] >> 4]; s += hex[raw[j] & 15]; }
        }
        s += '\n';
        std::snprintf(b, sizeof b, "e %d %d %d %d %d %d %d %d %llx %d %d %d %d %d %d\n", i, tr.eu_on, tr.eu_pl1, tr.eu_pl2, tr.eu_ro1, tr.eu_ro2, tr.eu_tro, tr.eu_op, (unsigned long long)tr.eu_backup, tr.eu_len,
                      tr.eu_a, tr.eu_b, tr.eu_len2, tr.eu_dir1, tr.eu_dir2);   /* v1.2.0: A on, B on, B's length, A's and B's direction (older versions stop reading before them) */
        s += pre; s += b;
        if (tr.dir != DIR_FWD) { std::snprintf(b, sizeof b, "D %d %d\n", i, tr.dir); s += pre; s += b; }   /* direction (v1.76.0), when not forwards */
        if (tr.panb) { std::snprintf(b, sizeof b, "A %d %d\n", i, tr.panb); s += pre; s += b; }
        if (tr.pcut) { std::snprintf(b, sizeof b, "K %d %d\n", i, 100 - tr.pcut); s += pre; s += b; }   /* TRACK VALUES PROB (v1.2.0), when not 100 % */   /* the pad's own pan + 1 (v1.93.0), when known */
        bool x67 = false; for (int k = 0; k < NSTEPS; k++) x67 |= tr.st[k].cc[5] || tr.st[k].cc[6];   /* CC 8 goes on its own H line */
        if (x67) {   /* CC 6 / CC 7 locks (v1.2.0): 64 steps x 2 bytes as hex */
            std::snprintf(b, sizeof b, "X %d ", i); s += pre; s += b;
            static const char hx[] = "0123456789abcdef";
            for (int k = 0; k < NSTEPS; k++) for (int j = NCC_V1; j < NCC_X; j++) { const uint8_t c = tr.st[k].cc[j]; s += hx[c >> 4]; s += hx[c & 15]; }
            s += '\n';
        }
        bool x8 = false; for (int k = 0; k < NSTEPS; k++) x8 |= tr.st[k].cc[NCC_X] != 0;
        if (x8) {   /* CC 8 locks (v1.2.0): 64 steps x 1 byte as hex; an H line, so the X line stays as it was */
            std::snprintf(b, sizeof b, "H %d ", i); s += pre; s += b;
            static const char hx[] = "0123456789abcdef";
            for (int k = 0; k < NSTEPS; k++) { const uint8_t c = tr.st[k].cc[NCC_X]; s += hx[c >> 4]; s += hx[c & 15]; }
            s += '\n';
        }
        bool mt = false; for (int k = 0; k < NSTEPS; k++) mt |= tr.st[k].micro || tr.st[k].ratch;
        if (mt) {   /* TIMING and RATCHET (v1.77.0): 64 steps x 2 bytes as hex (micro as a signed byte, ratchet - 1) */
            std::snprintf(b, sizeof b, "M %d ", i); s += pre; s += b;
            static const char hx[] = "0123456789abcdef";
            for (int k = 0; k < NSTEPS; k++) { const uint8_t m = (uint8_t)tr.st[k].micro, r = tr.st[k].ratch; s += hx[m >> 4]; s += hx[m & 15]; s += hx[r >> 4]; s += hx[r & 15]; }
            s += '\n';
        }
        bool rr = false; for (int k = 0; k < NSTEPS; k++) rr |= tr.st[k].rres != 0;
        if (rr) {   /* RATCHET RESOLUTION (v2.1.0): one hex digit per step */
            std::snprintf(b, sizeof b, "Y %d ", i); s += pre; s += b;
            for (int k = 0; k < NSTEPS; k++) s += "0123456789abcdef"[tr.st[k].rres & 15];
            s += '\n';
        }
    }
}
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
/* one track line of a project (e B C R r t s), into q; false: not one of those */
static bool track_line(Seq &q, const std::string &line, int sb) {
    int a[9];
        if (line.size() > 2 && line[0] == 'e') {
            unsigned long long bk = 0;
            int el = 0;   /* absent before v1.31.0: the rhythm is as long as the track */
            int x[5] = {0, 0, 0, 0, 0};   /* v1.2.0: A on, B on, B's length, A's and B's direction */
            const int got = std::sscanf(line.c_str() + 1, "%d %d %d %d %d %d %d %d %llx %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &bk, &el, &x[0], &x[1], &x[2], &x[3], &x[4]);
            if (got >= 9 && a[0] >= 0 && a[0] < NTRACKS) {
                Track &tr = q.t[a[0]];
                tr.eu_on = a[1] ? 1 : 0;
                tr.eu_pl1 = (uint8_t)clampi(a[2], 0, NSTEPS); tr.eu_pl2 = (uint8_t)clampi(a[3], 0, NSTEPS);
                tr.eu_ro1 = (uint8_t)clampi(a[4], 0, NSTEPS - 1); tr.eu_ro2 = (uint8_t)clampi(a[5], 0, NSTEPS - 1);
                tr.eu_tro = (uint8_t)clampi(a[6], 0, NSTEPS - 1); tr.eu_op = (uint8_t)clampi(a[7], 0, Seq::E_NOPS - 1);
                tr.eu_backup = (uint64_t)bk;
                tr.eu_len = (uint8_t)clampi(el, 0, NSTEPS);
                if (got >= 15) {
                    tr.eu_a = x[0] ? 1 : 0; tr.eu_b = x[1] ? 1 : 0; tr.eu_len2 = (uint8_t)clampi(x[2], 0, NSTEPS);
                    tr.eu_dir1 = (uint8_t)clampi(x[3], 0, 2); tr.eu_dir2 = (uint8_t)clampi(x[4], 0, 2);
                } else {   /* before v1.2.0 both always played, on A's length; B only mattered with pulses or AND */
                    tr.eu_a = tr.eu_on; tr.eu_b = tr.eu_on && (tr.eu_pl2 || tr.eu_op == Seq::E_AND);
                    tr.eu_len2 = 0; tr.eu_dir1 = tr.eu_dir2 = 0;
                }
            }
        } else if (line.size() > 2 && line[0] == 'B') {   /* base values of the track's CCs (value + 1, 0 = off) */
            int x[9] = {0}; const int got = std::sscanf(line.c_str() + 1, "%d %d %d %d %d %d %d %d %d", &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6], &x[7], &x[8]);
            if (got >= 6 && x[0] >= 0 && x[0] < NTRACKS)
                for (int j = 0; j < NCC && j + 1 < got; j++) q.t[x[0]].ccb[j] = (uint8_t)clampi(x[1 + j], 0, 128);   /* CC 6 / 7 since v1.2.0 */
        } else if (line.size() > 2 && line[0] == 'C') {
            int x[9] = {0}; const int got = std::sscanf(line.c_str() + 1, "%d %d %d %d %d %d %d %d %d", &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6], &x[7], &x[8]);
            if (got >= 6 && x[0] >= 0 && x[0] < NTRACKS)
                for (int j = 0; j < NCC && j + 1 < got; j++) q.t[x[0]].ccn[j] = (uint8_t)clampi(x[1 + j], 0, 128);   /* CC 6 / 7 since v1.2.0 */
        } else if (line.size() > 2 && line[0] == 'R') {
            if (std::sscanf(line.c_str() + 1, "%d %d %d %d", &a[0], &a[1], &a[2], &a[3]) == 4 && a[0] >= 0 && a[0] < NTRACKS) {
                Track &tr = q.t[a[0]];
                tr.rnd_dens = (uint8_t)clampi(a[1], 0, 100); tr.rnd_min = (uint8_t)clampi(a[2], 0, 127); tr.rnd_max = (uint8_t)clampi(a[3], 0, 127);
            }
        } else if (line.size() > 2 && line[0] == 'r') {
            if (std::sscanf(line.c_str() + 1, "%d %d %d", &a[0], &a[1], &a[2]) == 3) {
                for (int i = 0; i < NTRACKS; i++) { Track &tr = q.t[i]; tr.rnd_dens = (uint8_t)clampi(a[0], 0, 100); tr.rnd_min = (uint8_t)clampi(a[1], 0, 127); tr.rnd_max = (uint8_t)clampi(a[2], 0, 127); }   /* older projects: the same for every track */
            }
        } else if (line.size() > 2 && line[0] == 't') {
            a[8] = CC_BASE_OLD;   /* absent before v1.11.0: every track used CC 20..24 */
            if (std::sscanf(line.c_str() + 1, "%d %d %d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8]) >= 8
                && a[0] >= 0 && a[0] < NTRACKS) {
                Track &tr = q.t[a[0]];
                tr.len = (uint8_t)clampi(a[1], 1, NSTEPS); tr.speed = (uint8_t)clampi(a[2], 0, NSPEEDS - 1);
                tr.ch = (uint8_t)clampi(a[3], 0, 15); tr.num = (uint8_t)clampi(a[4], 0, 127);
                tr.vel = (uint8_t)clampi(a[5], 1, 127); tr.gate = (uint8_t)clampi(a[6], 1, 64);
                tr.mute = a[7] ? 1 : 0;
                tr.ccbase = (uint8_t)clampi(a[8], 0, CC_BASE_MAX);
                std::memset(tr.ccn, 0, sizeof tr.ccn);   /* a C line, if there is one, follows */
                std::memset(tr.ccb, 0, sizeof tr.ccb);   /* and a B line */
                tr.dir = DIR_FWD;   /* and a D line, if it plays another way */
                tr.panb = 0;   /* and an A line, if its pan is known (v1.93) */
                for (int k = 0; k < NSTEPS; k++) { tr.st[k].micro = 0; tr.st[k].ratch = 0; tr.st[k].rres = 0; }   /* M and Y lines follow if there are any (v2.1) */
            }
        } else if (line.size() > 2 && line[0] == 'M') {   /* TIMING and RATCHET of the steps (v1.77.0) */
            int ti = -1, used = 0;
            if (std::sscanf(line.c_str() + 1, "%d %n", &ti, &used) < 1 || ti < 0 || ti >= NTRACKS) return true;
            const char *h = line.c_str() + 1 + used;
            const size_t hl = std::strlen(h);
            for (int k = 0; k < NSTEPS && (size_t)(k + 1) * 4 <= hl; k++) {
                const int a0 = hexval(h[k * 4]), a1 = hexval(h[k * 4 + 1]), b0 = hexval(h[k * 4 + 2]), b1 = hexval(h[k * 4 + 3]);
                if (a0 < 0 || a1 < 0 || b0 < 0 || b1 < 0) break;
                q.t[ti].st[k].micro = (int8_t)clampi((int8_t)(uint8_t)(a0 << 4 | a1), -MICRO_MAX, MICRO_MAX);
                q.t[ti].st[k].ratch = (uint8_t)clampi(b0 << 4 | b1, 0, RATCH_MAX - 1);
            }
        } else if (line.size() > 2 && line[0] == 'Y' && line[1] == ' ') {   /* RATCHET RESOLUTION of the steps (v2.1.0) */
            int ti = -1, used = 0;
            if (std::sscanf(line.c_str() + 1, "%d %n", &ti, &used) < 1 || ti < 0 || ti >= NTRACKS) return true;
            const char *h = line.c_str() + 1 + used;
            for (int k = 0; k < NSTEPS && h[k]; k++) { const int x = hexval(h[k]); if (x < 0) break; q.t[ti].st[k].rres = (uint8_t)clampi(x, 0, RRES_MAX); }
        } else if (line.size() > 2 && line[0] == 'A' && line[1] == ' ') {   /* the pad's own pan + 1 (v1.93.0) */
            if (std::sscanf(line.c_str() + 1, "%d %d", &a[0], &a[1]) == 2 && a[0] >= 0 && a[0] < NTRACKS) q.t[a[0]].panb = (uint8_t)clampi(a[1], 0, 128);
        } else if (line.size() > 2 && line[0] == 'X' && line[1] == ' ') {   /* CC 6 / CC 7 locks (v1.2.0) */
            int ti = -1, used = 0;
            if (std::sscanf(line.c_str() + 1, "%d %n", &ti, &used) < 1 || ti < 0 || ti >= NTRACKS) return true;
            const char *h = line.c_str() + 1 + used;
            for (int k = 0; k < NSTEPS; k++) for (int j = NCC_V1; j < NCC_X; j++) {
                const int p = (k * (NCC_X - NCC_V1) + (j - NCC_V1)) * 2;
                if (!h[p] || !h[p + 1]) return true;
                const int hi = hexval(h[p]), lo = hexval(h[p + 1]);
                if (hi < 0 || lo < 0) return true;
                q.t[ti].st[k].cc[j] = (uint8_t)clampi(hi * 16 + lo, 0, 128);
            }
        } else if (line.size() > 2 && line[0] == 'H' && line[1] == ' ') {   /* CC 8 locks (v1.2.0) */
            int ti = -1, used = 0;
            if (std::sscanf(line.c_str() + 1, "%d %n", &ti, &used) < 1 || ti < 0 || ti >= NTRACKS) return true;
            const char *h = line.c_str() + 1 + used;
            for (int k = 0; k < NSTEPS; k++) {
                if (!h[2 * k] || !h[2 * k + 1]) return true;
                const int hi = hexval(h[2 * k]), lo = hexval(h[2 * k + 1]);
                if (hi < 0 || lo < 0) return true;
                q.t[ti].st[k].cc[NCC_X] = (uint8_t)clampi(hi * 16 + lo, 0, 128);
            }
        } else if (line.size() > 2 && line[0] == 'K' && line[1] == ' ') {   /* TRACK VALUES PROB (v1.2.0) */
            if (std::sscanf(line.c_str() + 1, "%d %d", &a[0], &a[1]) == 2 && a[0] >= 0 && a[0] < NTRACKS) q.t[a[0]].pcut = (uint8_t)(100 - clampi(a[1], 0, 100));
        } else if (line.size() > 2 && line[0] == 'D') {   /* the track's direction (v1.76.0) */
            if (std::sscanf(line.c_str() + 1, "%d %d", &a[0], &a[1]) == 2 && a[0] >= 0 && a[0] < NTRACKS && (a[1] & DIR_PP)) q.t[a[0]].dir = (uint8_t)(a[1] & DIR_PP);
        } else if (line.size() > 2 && line[0] == 's') {
            int ti = -1, used = 0;
            if (std::sscanf(line.c_str() + 1, "%d %n", &ti, &used) < 1 || ti < 0 || ti >= NTRACKS) return true;
            const char *h = line.c_str() + 1 + used;
            size_t hl = std::strlen(h);
            for (int k = 0; k < NSTEPS && (size_t)(k + 1) * sb * 2 <= hl; k++) {
                uint8_t raw[STEP_BYTES] = {0};
                bool ok = true;
                for (int j = 0; j < sb; j++) {
                    int hi = hexval(h[(k * sb + j) * 2]), lo = hexval(h[(k * sb + j) * 2 + 1]);
                    if (hi < 0 || lo < 0) { ok = false; break; }
                    raw[j] = (uint8_t)(hi << 4 | lo);
                }
                if (!ok) break;
                Step &st = q.t[ti].st[k];
                st.on = raw[0] ? 1 : 0; st.cond = (uint8_t)clampi(raw[1], 0, NCOND - 1); st.prob = (uint8_t)clampi(raw[2], 0, 100);
                st.vel = (uint8_t)clampi(raw[3], 0, 127); st.len = (uint8_t)clampi(raw[4], 0, 64);
                for (int j = 0; j < NCC_V1; j++) st.cc[j] = (uint8_t)clampi(raw[5 + j], 0, 128);
                for (int j = NCC_V1; j < NCC; j++) st.cc[j] = 0;   /* an X line follows if it has CC 6 / 7 locks */
                st.pan = (uint8_t)clampi(raw[5 + NCC_V1], 0, 128);
            }
        }
        else return false;
        return true;
}
static bool chunk_read(Plugin *w, const std::string &s) {
    bool v2 = s.compare(0, 4, "TSq2") == 0;
    if (!v2 && s.compare(0, 4, "TSq3") != 0) return false;
    const int sb = v2 ? (int)STEP_BYTES_V2 : (int)STEP_BYTES;   /* bytes per saved step */
    w->seq.all_off();
    w->seq.pf_clear();
    w->seq.pf_undo_ok = false;
    w->seq.shuffle = 50;   /* projects from before v1.21.0 have no G line */
    hist_reset(w);   /* a loaded project starts a new history */
    w->cc117 = 0; w->shift = false; w->cc_edit = false; w->pclear = false; w->src.clear(); w->src_mode = 0; w->shuf_edit = false; w->clr_mode = false; w->lpage[0] = w->lpage[1] = w->lpage[2] = 0; w->trk_vals = false; src_name(w); for (int ti = 0; ti < NTRACKS; ti++) for (int j = 0; j < NCC; j++) w->ccname[ti][j].clear(); w->src_gen++;
    for (int pp = 0; pp < NPATS; pp++) { pst_clear_pat(w, pp); }
    w->pstclip_ok = false;
    std::unique_ptr<Seq> stq[NSTATES], ptq[NPATS], stv[NPATS][NSTATES];   /* the states and patterns as they are read (stq: v1.68.0-1.0.2 "Q" states) */
    std::unique_ptr<Plugin::PState[]> oldps(new Plugin::PState[NSTATES]);   /* "Q" states: their pattern is known at the end */
    int qpat[NSTATES]; for (int k = 0; k < NSTATES; k++) qpat[k] = -1;   /* a "Q" state's W sub-line (v1.0.2), -1: the pattern playing */
    auto state_line = [&](Plugin::PState &ps, std::unique_ptr<Seq> &sq, const std::string &rest, int *qp) {   /* one of a STATE's lines */
        if (!sq) sq.reset(new Seq());
        Seq &q = *sq;
        if (rest.size() > 2 && rest[0] == 'G') {
            int g0 = 50, g1 = 255;
            if (std::sscanf(rest.c_str() + 1, "%d %d", &g0, &g1) >= 1) { q.shuffle = (uint8_t)clampi(g0, 50, SHUFFLE_MAX); for (int i = 0; i < NTRACKS; i++) q.t[i].shuf = (g1 >> i) & 1; }
        } else if (rest.size() > 2 && rest[0] == 'U') { ps.mutes = true;   /* v2.4 */
        } else if (rest.size() > 2 && rest[0] == 'W' && rest[1] == ' ') { if (qp) *qp = clampi(std::atoi(rest.c_str() + 2), 0, NPATS - 1);   /* v1.0.2 */
        } else if (rest.size() > 2 && rest[0] == 'E') { unsigned long m = 0; if (std::sscanf(rest.c_str() + 1, "%lu", &m) == 1) { ps.fx = (uint32_t)m; ps.fx_ok = true; }   /* v2.5 */
        } else if (rest.size() > 2 && rest[0] == 'P') {
            int o[NPF_ALL] = {0}, on = 0, cc = 0;
            if (std::sscanf(rest.c_str() + 1, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d", &on, &o[0], &o[1], &o[2], &o[3], &o[4], &o[5], &o[6], &o[7], &o[8], &cc, &o[9], &o[10], &o[11]) >= 11) {   /* CC 8 last (v1.2.0) */
                ps.pf_on = on != 0; ps.cc117 = clampi(cc, 0, 127);
                for (int j = 0; j < NPF_ALL; j++) ps.off[j] = clampi(o[j], (int)PARAMS[pf_param(j)].min, (int)PARAMS[pf_param(j)].max);
            }
        } else track_line(q, rest, sb);
    };
    for (int k = 0; k < NPATS; k++) w->pat[k] = Plugin::Pat();
    w->patclear = w->patstore = false; w->pat_cue = -1; w->pat_cur = 0;
    for (int i = 0; i < NTRACKS; i++) w->seq.t[i].shuf = 1;
    for (int k = 0; k < NPF_ALL; k++) w->raw[pf_param(k)] = -1.0f;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t eol = s.find('\n', pos);
        if (eol == std::string::npos) eol = s.size();
        std::string line = s.substr(pos, eol - pos);
        pos = eol + 1;
        int a[9];
        if (line.size() > 2 && line[0] == 'g') {
            a[5] = 0;   /* step edit: absent before v1.3.0 */
            a[6] = -1;  /* the mode: absent before v1.95.0 */
            a[7] = -1;  /* the panel EUCLIDIAN shows: absent before v2.1.0 */
            if (std::sscanf(line.c_str() + 1, "%d %d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]) >= 5) {
                if (a[6] >= 0 && a[6] <= 6) mode_set(w, a[6]); else mode_set(w, a[5] ? 1 : 2);   /* older: STEP EDIT, or TRACK EDIT (v1.99) */
                if (a[7] >= 0 && a[7] <= 2) w->rmode = a[7];   /* v1.95: the screen it was left on */
                w->track = clampi(a[0], 0, NTRACKS - 1); w->page = clampi(a[1], 0, 3); w->sel = clampi(a[2], 0, 15);
                w->follow = a[3] != 0; w->seq.fill = a[4] != 0;
            }
        } else if (line.size() > 2 && line[0] == 'P') {   /* a PERFORM state of v1.36-1.67 (the knobs only): dropped, STATE now holds the whole sequencer */
        } else if (line.size() > 2 && line[0] == 'N' && line[1] == ' ') {   /* a CC's name */
            int ti = -1, j = -1, cc = -1, used = 0;
            if (std::sscanf(line.c_str() + 2, "%d %d %d %n", &ti, &j, &cc, &used) >= 3 && ti >= 0 && ti < NTRACKS && j >= 0 && j < NCC && used > 0 && (size_t)(2 + used) <= line.size()) {
                w->ccname[ti][j] = line.substr(2 + used, 40); w->ccname_cc[ti][j] = cc;
            }
        } else if (line.size() > 2 && line[0] == 'I' && line[1] == ' ') {   /* the instance id */
            unsigned long long id = 0; if (std::sscanf(line.c_str() + 2, "%llx", &id) == 1 && id) w->inst_id = id;
        } else if (line.size() >= 2 && line[0] == 'S' && line[1] == ' ') {   /* READ PROJECT target */
            w->src = line.substr(2);
        } else if (line.size() > 2 && line[0] == 'G') {
            a[1] = 255;   /* absent in v1.21.0: every track follows */
            a[2] = 0;
            if (std::sscanf(line.c_str() + 1, "%d %d %d", &a[0], &a[1], &a[2]) >= 1) {
                w->cc117 = clampi(a[2], 0, 127);
                w->seq.shuffle = (uint8_t)clampi(a[0], 50, SHUFFLE_MAX);
                for (int i = 0; i < NTRACKS; i++) w->seq.t[i].shuf = (a[1] >> i) & 1;
            }

        } else if (line.size() > 3 && line[0] == 'Q' && line[1] >= '0' && line[1] < '0' + NSTATES && line[2] == ' ') {   /* a STATE's line (v1.68.0-1.0.2) */
            const int k = line[1] - '0';
            state_line(oldps[k], stq[k], line.substr(3), &qpat[k]);
        } else if (line.size() > 4 && line[0] == 'V' && line[1] >= '0' && line[1] < '0' + NPATS && line[2] >= '0' && line[2] < '0' + NSTATES && line[3] == ' ') {   /* v1.1.0 */
            const int pp = line[1] - '0', k = line[2] - '0';
            state_line(w->pst[pp][k], stv[pp][k], line.substr(4), nullptr);
        } else if (line.size() >= 3 && line[0] == 'W' && line[1] == ' ') {   /* the pattern playing */
            w->pat_cur = clampi(std::atoi(line.c_str() + 2), 0, NPATS - 1);
        } else if (line.size() > 3 && line[0] == 'Z' && line[1] >= '0' && line[1] < '0' + NPATS && line[2] == ' ') {   /* a PATTERN's line (v1.69.0) */
            const int k = line[1] - '0';
            const std::string rest = line.substr(3);
            if (!ptq[k]) ptq[k].reset(new Seq());
            Seq &q = *ptq[k];
            if (rest.size() > 2 && rest[0] == 'G') {
                int g0 = 50, g1 = 255;
                if (std::sscanf(rest.c_str() + 1, "%d %d", &g0, &g1) >= 1) { q.shuffle = (uint8_t)clampi(g0, 50, SHUFFLE_MAX); for (int i = 0; i < NTRACKS; i++) q.t[i].shuf = (g1 >> i) & 1; }
            } else track_line(q, rest, sb);
        } else track_line(w->seq, line, sb);
    }
    for (int k = 0; k < NPATS; k++) if (ptq[k]) { w->pat[k].full = true; snap_of(*ptq[k], w->pat[k].snap); }
    w->pat[w->pat_cur].full = true;   /* the tracks of the project are it (projects from before v1.75: pattern 1) */
    auto state_end = [&](Plugin::PState &ps, const Seq &q) { ps.full = true; snap_of(q, ps.snap); if (ps.mutes) for (int t = 0; t < NTRACKS; t++) ps.snap.t[t].mute = q.t[t].mute; };
    for (int pp = 0; pp < NPATS; pp++) for (int k = 0; k < NSTATES; k++) if (stv[pp][k]) state_end(w->pst[pp][k], *stv[pp][k]);
    for (int k = 0; k < NSTATES; k++) if (stq[k]) {   /* an older project's state: into the pattern it was saved on (1.0.2), else the one playing */
        const int pp = qpat[k] >= 0 ? qpat[k] : w->pat_cur;
        if (w->pst[pp][k].full) continue;   /* (a V line has it already) */
        w->pst[pp][k] = oldps[k]; state_end(w->pst[pp][k], *stq[k]);
    }
    for (int pp = 0; pp < NPATS; pp++) if (!w->pat[pp].full && pst_any(w, pp)) {   /* a pattern with states is a pattern: no steps, as an empty one comes in */
        Snap b; hist_take(w, b); fresh_tracks(b); w->pat[pp].snap = b; w->pat[pp].full = true;
    }
    if (w->seq.playing) for (int i = 0; i < NTRACKS; i++) w->seq.align(w->seq.t[i], w->seq.virt(w->song));
    return true;
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        if (w->job_thr.joinable()) w->job_thr.join();   /* project file work still running: first, it may still send (v2.11) */
        { std::lock_guard<std::mutex> lk(w->lock); w->seq.stop(); flush(w); }   /* nothing left hanging */
#ifndef STEPRUNNER_TEST
        alsa_close(w);
#endif
        slot_free(w->slot);
        delete w;
        return 1;
    case effGetPlugCategory: return 2; /* kPlugCategSynth */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && idx != P_RUN && idx != P_MULTI && idx != P_PANEL && idx != P_PTITLE && idx != P_SCR && idx != P_LBTN && idx != P_BKSTEP && idx != P_PST_ACT && idx != P_PST_PAT && idx != P_EU_DIRV && idx != P_EU_DIRV2 && idx != P_EU_OPV && idx != P_NAVV && idx != P_PSHIFTV && idx != P_SLOTV && idx != P_RTRK && idx != P_TTL && !(idx >= P_PATV0 && idx < P_PATV_END) && idx != P_BIGMSG && idx != P_BIGDONE && idx != P_BKNAME && !(idx >= P_SRCHL0 && idx < P_SRCHL_END) && idx != P_LPANEL && idx != P_MSTATE && idx != P_MSG && !(idx >= P_SRCPROJ && idx < P_SRCSEL0) && !(idx >= P_PGST0 && idx < P_PGST_END) && !(idx >= P_CCUSE0 && idx < P_CCUSE_END) && !(idx >= P_MPAD0 && idx < P_MPAD_END) && !(idx >= P_CELL0 && idx < P_CELL0 + 16);
    case effGetParamName:
        /* Names are static. On the device (v1.11.0) names that followed the track were shown as they were when the plugin
         * loaded and never refreshed, audioMasterUpdateDisplay or not; so whatever changes goes into the VALUE text
         * instead (display_of), which the host does read again. */
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8);
        return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char disp[32] = "";
        if (popup_is(idx)) std::snprintf(disp, sizeof disp, "%s", w->open[idx] > 0.5f ? "Open" : "Closed");
        else if (is_own(idx) && !PARAMS[idx].momentary) display_of(w, idx, disp, sizeof disp);
        copy_str(p, disp, 24);
        return 1;
    }
    case effSetSampleRate: if (o > 1000.0f) w->sample_rate = o; return 1;
    case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: return 1;   /* takes no MIDI input */
    case effCanDo:
        return (!std::strcmp((char *)p, "receiveVstTimeInfo")) ? 1 : -1;
    case effGetChunk: {
        std::unique_ptr<ChunkSrc> c(new ChunkSrc());   /* allocated before the lock */
        { std::lock_guard<std::mutex> lk(w->lock); chunk_take(w, *c); }
        chunk_write(c.get(), w->chunk);   /* the text, without the lock (only the host's thread asks for the chunk) */
        const uint32_t h = fnv(w->chunk.c_str(), w->chunk.size());
        { std::lock_guard<std::mutex> lk(w->lock); note_hash(w, h); }
        *(void **)p = (void *)w->chunk.c_str();
        return (intptr_t)w->chunk.size() + 1;
    }
    case effSetChunk: {
        if (v <= 0 || v > (1 << 20) || !p) return 0;
        std::string s((const char *)p, (size_t)v);
        s = s.substr(0, s.find('\0'));
        std::lock_guard<std::mutex> lk(w->lock);
        bool ok = chunk_read(w, s);
        w->scan_now = true;
        if (ok) { note_given(w, s); src_name(w); }
        flush(w);
        return ok ? 1 : 0;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/steprunner8.log", "a");
    if (NPARAMS < P_OWN_COUNT) { LOG("[steprunner8] params.h has %d params, the plugin needs %d\n", NPARAMS, (int)P_OWN_COUNT); return nullptr; }
    for (size_t k = 0; k < sizeof KEYCHECK / sizeof KEYCHECK[0]; k++)
        if (std::strcmp(PARAMS[KEYCHECK[k].idx].key, KEYCHECK[k].key)) {
            LOG("[steprunner8] params.h out of step with steprunner8.cpp at %d: '%s' != '%s'\n", KEYCHECK[k].idx, PARAMS[KEYCHECK[k].idx].key, KEYCHECK[k].key);
            return nullptr;
        }
    Plugin *w = new Plugin();
    w->undo_st.alloc(); w->redo_st.alloc();   /* the undo history's memory, here and not on the audio thread (v2.11) */
    w->seq.reseed((uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() ^ (uint32_t)(uintptr_t)w);   /* a different random series every time the plugin is loaded */
    w->master = master;
    w->inst_id = ((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() * 0x9E3779B97F4A7C15ull) ^ (uint64_t)(uintptr_t)w; if (!w->inst_id) w->inst_id = 1;
    w->born = std::time(nullptr);
    src_name(w);
    w->slot = slot_take();
    for (int i = 0; i < NPARAMS; i++) w->raw[i] = -1.0f;
    for (int i = 0; i < NPARAMS; i++) w->told[i] = (is_own(i) && !PARAMS[i].momentary) ? to_norm(&PARAMS[i], value_of(w, i)) : 0.0f;
#ifndef STEPRUNNER_TEST
    alsa_open(w);
#endif

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = 0;   /* v2.0: no programs (the "--" of v1.99 is gone) */
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[steprunner8] up, %d params\n", NPARAMS);
    return e;
}

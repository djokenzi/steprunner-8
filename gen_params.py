#!/usr/bin/env python3
"""Writes params.json (the VST parameter list, in index order). APPEND ONLY: saved projects and the skin
bind to the index, and steprunner8.cpp's enum P_* must list the same keys in the same order (checked at load)."""
import json

# list order (seqcore.hpp cond_at): the codes saved in projects are separate, so this order can change freely
conds = ["--", "1ST", "NO 1ST"]
conds += ["%d:%d" % (a, b) for b in range(2, 9) for a in range(1, b + 1)]
conds += ["FILL", "NO FILL", "PRE", "NO PRE", "NEI", "NO NEI"]
onoff = ["OFF", "ON"]
P = []
def num(key, name, lo, hi, d): P.append({"key": key, "name": name, "min": lo, "max": hi, "default": d, "display": "int"})
def opt(key, name, options, d=0): P.append({"key": key, "name": name, "options": options, "default": d})
def trig(key, name): P.append({"key": key, "name": name, "momentary": True})

opt("track", "Track", ["T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8"])
opt("page", "Page", ["1-16", "17-32", "33-48", "49-64"])
for i in range(16): opt("step%d" % i, "Step %d" % (i + 1), onoff)
num("sel", "Edit Step", 1, 16, 1)
opt("st_cond", "Condition", conds)
num("st_prob", "Probability", 0, 100, 100)
num("st_vel", "Velocity", 0, 127, 0)
num("st_len", "Length", 0, 64, 0)
for i in range(5): num("st_cc%d" % (i + 1), "CC %d" % (i + 1), 0, 128, 0)   # the plugin reports the real number as the name (CC 85 ...)
num("trk_len", "Track Length", 1, 64, 16)
opt("trk_speed", "Track Speed", ["1/8x", "1/4x", "1/2x", "3/4x", "1x", "3/2x", "2x"], 4)
num("trk_ch", "MIDI CH", 1, 16, 1)
num("trk_num", "Note", 0, 127, 37)
num("trk_vel", "Track Velocity", 1, 127, 100)
num("trk_gate", "Gate", 1, 64, 4)
for i in range(8): opt("mute%d" % (i + 1), "Mute T%d" % (i + 1), onoff)
opt("fill", "Fill", onoff)
trig("clr_trk", "Clear Track")
trig("clr_step", "Clear Locks")
opt("run", "Playhead", ["-"] + [str(i) for i in range(1, 17)])
opt("follow", "Follow", onoff)
# appended 2026-10-03 (v1.1.0): were the CC VALUE knobs, removed in v1.2.0. The slots stay so later indices do not move.
for i in range(5): num("cc_base%d" % (i + 1), "Unused %d" % (i + 1), 0, 127, 0)
# appended v1.2.0: random generators
trig("rnd_trig", "Random Steps")
trig("rnd_vel", "Random Velocity")
trig("rnd_len", "Random Length")
for i in range(5): trig("rnd_cc%d" % (i + 1), "Random CC %d" % (i + 1))
num("rnd_dens", "Random Density", 0, 100, 50)
num("rnd_min", "Random Min", 0, 127, 0)
num("rnd_max", "Random Max", 0, 127, 127)

# appended v1.3.0: TRIGS cells (read-only; the plugin reports them) and the step-edit switch
# state = trig (1) + play-head (2) + selected (4) + 8 x mode (0 normal, 1 STEP EDIT, 2 TRACK EDIT, 3 EUCLIDIAN: the colour); the last option is padding
cell_states = ["%s%s%s%s" % ("-ETU"[s >> 3], "S" if s & 4 else "-", "P" if s & 2 else "-", "T" if s & 1 else "-") for s in range(32)] + ["(unused)"]
for i in range(16): opt("cell%d" % (i + 1), "Cell %d" % (i + 1), cell_states)
opt("step_edit", "Step Edit", onoff)
# appended v1.5.0: read-only, ON while more than one step is selected (the skin hides the STEP LOCKS controls)
opt("multi", "Multi Select", onoff)
# appended v1.6.0: pan lock per step (sent on the track's PAN_CC since v1.93) and its random button
num("st_pan", "Pan", 0, 128, 0)
trig("rnd_pan", "Random Pan")
# appended v1.7.0: TRACK EDIT (the right-hand panel shows the track's setup instead of the step locks) and two read-only
# parameters the skin's when= lines follow: which controls the panel shows, and which title it carries
opt("track_edit", "Track Edit", onoff)
opt("panel", "Panel", ["LOCKS", "MULTI", "TRACK", "RANDOM", "GLOBAL", "PATTERN", "CCEDIT", "SOURCE"])       # RANDOM added in v1.10.0 (read-only, so its range may change)
opt("ptitle", "Panel Title", ["NORMAL", "EDIT", "TRACK", "RANDOM", "EUCLID", "GLOBAL", "PATTERN", "SOURCE", "MAPTRK", "TIMING"])   # the mode: titles and colours
# appended v1.8.0: track length shortcuts. ON while the selected track has exactly that length; setting one ON sets it.
for n in (16, 32, 48, 64): opt("len%d" % n, "Length %d" % n, onoff)
# appended v1.9.0: TRACK and PAGE selector buttons (the same idea: ON = the current one). "track" and "page" stay for Q-Links.
for i in range(8): opt("trk%d" % (i + 1), "Select T%d" % (i + 1), onoff)
for i in range(4): opt("pg%d" % (i + 1), "Page %d" % (i + 1), onoff)
# appended v1.10.0: RANDOM EDIT (the panel shows the random generators' settings)
opt("random_edit", "Random Edit", onoff)
# appended v1.11.0: the first of the five CC numbers the selected track sends its CC locks on
num("trk_ccbase", "CC Base", 0, 123, 20)
# appended v1.12.0: Euclidean generator. EUCLIDIAN is the fourth mode; the eu_* controls belong to the selected track.
opt("euclid_edit", "Euclidian", onoff)
opt("lpanel", "Left Panel", ["RANDOM", "EUCLID", "HIDDEN", "TIMING", "LOGO"])          # read-only, for the skin: what the left-hand section shows
opt("eu_enable", "Euclid Enable", onoff)                    # ON while the selected track's trigs come from the generator
num("eu_pl1", "Pulse", 0, 64, 4)
num("eu_ro1", "Rotate", 0, 63, 0)
num("eu_tro", "TRO", 0, 63, 0)
num("eu_pl2", "PL2", 0, 64, 0)
num("eu_ro2", "RO2", 0, 63, 0)
num("eu_bool", "BOOL", 0, 3, 0)                             # shown as OR / AND / XOR / SUB
trig("eu_discard", "Euclid Discard")
trig("eu_commit", "Euclid Commit")
# appended v1.13.0: PERFORM tab. Nine knobs named like the step locks, and PERFORM ALL / COMMIT / DISCARD (DISCARD works as UNDO).
# PERFORM ALL offsets, 0 in the middle
num("pf_prob", "Probability", -100, 0, 0)
num("pf_vel", "Velocity", -127, 127, 0)
num("pf_len", "Length", -64, 64, 0)
num("pf_pan", "Pan", -64, 63, 0)   # back in v1.72.0 (the pan of trigs with no pan lock, from the centre)
for i in range(5): num("pf_cc%d" % (i + 1), "CC %d" % (i + 1), -127, 127, 0)
opt("pf_ctrl", "Perform All", onoff)
trig("pf_commit", "Perform Commit")
trig("pf_discard", "Perform Undo")
# PERFORM tab pads
opt("pfx1", "Loop 4", onoff); opt("pfx2", "Loop 2", onoff); opt("pfx3", "Loop 1", onoff)
opt("pfx4", "No Locks", onoff); opt("pfx5", "Random Step", onoff); opt("pfx6", "Invert Mute", onoff)
trig("pfx7", "Mute All"); opt("pfx8", "Fill", onoff)
for i, nm in enumerate(["Loop 8", "Reverse", "Half Time", "Double Time", "Roll", "Restart", "Scatter", "Pad 8"]):   # second pad row (v1.79-1.80)
    if nm == "Restart": trig("pfy%d" % (i + 1), nm)   # v1.82.0: momentary
    else: opt("pfy%d" % (i + 1), nm, onoff)
opt("mstate", "Mute State", ["NONE", "SOME"])   # read-only, for the skin
opt("global_edit", "Global", onoff)
num("shuffle", "Shuffle", 50, 80, 50)
for i in range(1, 9): opt("shuf%d" % i, "Shuffle T%d" % i, onoff, 1)
num("glob_ch", "Global MIDI CH", 1, 16, 1)
opt("shuf_all", "Shuffle All", onoff, 1)
opt("pattern_edit", "Pattern Edit", onoff)
for n in (16, 32, 48, 64): opt("plen%d" % n, "Pattern Length %d" % n, onoff)
trig("undo", "Undo"); trig("redo", "Redo")
trig("copy", "Copy"); trig("paste", "Paste")
num("msg", "Message", 0, 301, 0)   # read-only: its value text is the message
for i in range(1, 9): opt("mpad%d" % i, "Mute Pad %d" % i, ["IDLE", "HIT", "MUTED"])   # read-only, for the skin

num("pf_cc117", "CC 117", 0, 127, 0)   # retired v1.72.0 (no knob, nothing sent)
trig("pcopy", "Copy Page"); trig("ppaste", "Paste Page")
opt("shift", "Shift", onoff)
num("eu_len", "Length", 1, 64, 16)
trig("eu_rnd", "Euclid Random")

for i in range(1, 5): opt("pgst%d" % i, "Page %d State" % i, ["IDLE", "SHOWN", "PLAYING", "SHOWN PLAYING", "HIDDEN"])   # read-only, for the skin

opt("cc_edit", "Edit CC", onoff)
for i in range(1, 6): num("ccn%d" % i, "CC %d Number" % i, 0, 127, 0)
trig("cc_commit", "CC Commit"); trig("cc_discard", "CC Discard")

for i in range(1, 6): opt("ccuse%d" % i, "CC %d In Use" % i, ["FREE", "USED"])   # read-only, for the skin

for i in range(1, 5): opt("pst%d" % i, "Perform State %d" % i, onoff)
opt("pclear", "Clear State", onoff)

for i in range(1, 6): num("ccb%d" % i, "CC %d Base" % i, 0, 128, 0)
trig("cc_read", "Read Project")
trig("read_all", "Read Project All")
num("srcproj", "Project", 0, 1, 0)   # read-only: its value text is the project's name
for i in range(1, 7): num("srcrow%d" % i, "Target %d" % i, 0, 1, 0)   # read-only texts: "Plugin 004 (38)"
for i in range(1, 7): trig("srcsel%d" % i, "Read Target %d" % i)
trig("src_prev", "Targets Prev"); trig("src_next", "Targets Next"); trig("src_cancel", "Read Cancel")
opt("scr", "Screen", ["MAIN", "SOURCE"])   # read-only: SOURCE = the READ PROJECT list has the whole PROGRAM tab
trig("src_commit", "Read Commit")
for i in range(1, 7): opt("srchl%d" % i, "Target %d Row" % i, ["HIDDEN", "IDLE", "MARKED"])
trig("src_map", "Create Default Map")
trig("src_fetch", "Fetch CC Values")
trig("src_notes", "Fetch Notes")
opt("shuf_edit", "Shuffle Edit", onoff)
opt("clr_mode", "Clear Locks Mode", onoff)
opt("lbtn", "Left Buttons", ["ORANGE", "TEAL", "VIOLET", "CLR", "NONE"])   # read-only, for the skin: the look of the RND buttons
trig("src_copy", "Copy Mapping")
trig("src_skip", "Skip Backup")
opt("bkstep", "Backup Step", onoff)   # retired (v1.64, no backup step); slot kept
opt("bigmsg", "Big Message", ["NONE", "NOTSAVED", "NODRUM", "NOTHING", "FAIL"] + ["DONE%d" % n for n in range(1, 49)])   # 41..48 v1.93.0 (pan: 6 per track)   # read-only: the big message over the PROGRAM tab (v1.64.0)
opt("bigdone", "Big Message Done", onoff)   # read-only: the DONE message is up (shows the backup's name)
trig("msg_ok", "Close Message")
num("bkname", "Backup Name", 0, 1, 0)   # read-only: its value text is the backup's file name
for i in range(5, 9): opt("pst%d" % i, "State %d" % i, onoff)   # v1.68.0: STATE 5..8 (1..4 above)
for i in range(1, 9): opt("pat%d" % i, "Pattern %d" % i, onoff)   # v1.69.0: PATTERN 1..8 on the main screen
opt("pat_clear", "Pattern Clear", onoff)
opt("pat_store", "Pattern Store", onoff)   # v1.72.0
opt("trk_fwd", "Forward", onoff, 1)   # v1.76.0: the track plays forwards; with trk_bwd too: back and forth
opt("trk_bwd", "Backward", onoff)
opt("timing_edit", "Timing", onoff)   # v1.77.0: TIMING mode
trig("tm_left", "Timing Earlier"); trig("tm_right", "Timing Later")
num("st_ratch", "Ratchet", 1, 8, 1)
trig("rnd_timing", "Random Timing"); trig("rnd_ratch", "Random Ratchets")
trig("clr_timing", "Clear Timing"); trig("clr_ratch", "Clear Ratchets")   # v1.78.0
opt("pst_act", "Active State", ["NONE"] + ["S%d" % k for k in range(1, 9)])   # read-only: the STATE last saved or recalled
opt("pat_fwd", "Pattern Forward", onoff, 1); opt("pat_bwd", "Pattern Backward", onoff)   # v1.81.0: every track at once
opt("trk_both", "Back and Forth", onoff); opt("pat_both", "Pattern Back and Forth", onoff)   # v1.82.0
for i in range(1, 9): opt("patv%d" % i, "Pattern %d Look" % i, ["HIDDEN", "EMPTY", "FULL", "PLAYING", "CUED", "BLINK"])   # read-only, v1.82.0; BLINK v1.92.0 (HIDDEN: not the pattern setup)
opt("eu_fwd", "Euclid Forward", onoff); opt("eu_bwd", "Euclid Backward", onoff); opt("eu_both", "Euclid Back and Forth", onoff)   # v1.83.0
opt("eu_dirv", "Euclid Direction Look", ["HIDDEN", "OFF", "ON"])   # read-only
opt("slotv", "Mode Slot Look", ["EUCLID", "TIMING", "HIDDEN"])   # read-only, v1.84.0; HIDDEN: StepRunner-8 v1.0.0 (EUCLIDIAN on TRACK EDIT only)
opt("rtrk", "Random Track Look", ["HIDDEN"] + ["T%d" % k for k in range(1, 9)])   # read-only, v1.85.0
opt("pf_hum", "Humanize", onoff); opt("pf_build", "Build", onoff); opt("pf_echo", "Echo", onoff); opt("pf_fade", "Fade Out", onoff)   # v1.91.0 PERFORM pads
opt("pf_ghost", "Ghost", onoff); opt("pf_poly", "Poly", onoff); opt("pf_down", "Downbeat Only", onoff)   # v1.94.0 PERFORM pads
trig("pat_copy", "Copy Pattern"); trig("pat_paste", "Paste Pattern"); trig("pat_clrall", "Clear Pattern")   # v2.0.0 PATTERN EDIT
opt("ttl", "Panel Title Look", ["HIDDEN", "SETUP"] + ["ST%d" % k for k in range(1, 9)] + ["RT%d" % k for k in range(1, 9)])   # read-only, v2.0.0
opt("st_rres", "Ratchet Resolution", ["AUTO", "1/16", "1/32", "1/48", "1/64", "1/80"])   # v2.1.0: hits per step of the RATCHET
opt("pf_alt", "Alt Bar", onoff); opt("pf_afill", "Auto Fill", onoff)   # v2.2.0 PERFORM pads

json.dump({"name": "StepRunner-8", "params": P}, open("params.json", "w"), indent=1)
print(len(P), "params")

# StepRunner-8 — project notes

8-track, 64-step sequencer for MPC Live as a VST2 MIDI generator (mpc-vst-plugins). A new plugin, independent of
Step Seq. Developed in `mpc-vst-plugins/poc/steprunner8/` (nothing outside it is changed) and published as its own GitHub
repo; the build needs an mpc-vst-plugins checkout (`MPC_VST`, default `../..`).

Status: v1.2.0 (in development, local only, not published): STOP CC pad, RND RATCHETS draws the resolution, PATTERN / TRACK / STEP EDIT page 2 (◀ ▶), EUCLIDIAN generators A and B, TRACK VALUES, SHIFT. v1.1.1 (2026-10-13), tested on MPC Live 3.9.1.2: low-RAM skin (opening the window took 25-30% of the MPC's RAM); not yet tried on the device. v1.1.0: every PATTERN has its own eight STATEs.
v1.0.0 (2026-10-10) = Trig Seq v2.12.0 renamed: released on GitHub (djokenzi/steprunner-8), tested on the device, in the catalog.

## What it does

- 8 independent tracks × 64 steps, T1–T8, notes 36–43 = pads A01–A08 of the MPC drum program (since v1.63.0; previously 37, 36, 42, 82, 40, 38, 46, 44).
- Per track: length 1–64, speed (1/8x, 1/4x, 1/2x, 3/4x, 1x, 3/2x, 2x), MIDI channel, note, default
  velocity, default gate, mute.
- Per step: trig, condition, probability 0–100 %, and locks:
  - velocity and length (`--` = the track's value),
  - pan lock: CC 10 (`--` = not sent; display L64 … C … R63). Sent after the CC locks, before the note.
  - five CC locks (`--` = nothing is sent; `0` is a valid lock). Each track sends them on its own block of five
    consecutive CC numbers, CC BASE … CC BASE + 4 (knob CC BASE in TRACK SETUP, display e.g. `85-89`), so all tracks
    can be on the same MIDI channel. Defaults: T1 20–24, T2 25–29, T3 85–89, T4 102–106, T5 107–111, T6 112–116,
    T7 14–18, T8 80–84. The knobs and RND buttons are called "CC 1–5" and show only the locked value; which CC numbers the track uses is shown
    on the CC BASE knob.
  - Locked CCs are sent immediately before the note, on the track's MIDI channel, only when the step actually fires
    (condition and probability pass, the track is not muted).
  - A step without a lock sends nothing, and nothing is sent to "reset" a lock (same as in Step Seq): the receiver stays
    at the last sent value until a later step sends a different one.
- Random generators (buttons on the STEPS tab), one-shot, write into the selected track, steps 1..track length:
  - RND TRIGS: each step gets a trig with probability DENSITY.
  - RND VEL / RND LEN / RND CC20–24: among the steps that have a trig, DENSITY % get a new lock in the range MIN..MAX
    (velocity at least 1; length: 0..127 is mapped to 0.25–16 steps). Other steps keep what they had.
  - DENSITY / MIN / MAX are on the TRACK tab (shared by all generators and tracks; defaults 50 %, 0, 127).
- Conditions (44), in list order: `--`, 1ST, NO 1ST, A:B for B = 2..8, then FILL, NO FILL, PRE, NO PRE, NEI, NO NEI.
  - A:B and 1ST count the loops of **that track** (its length), not of the MPC sequence.
  - PRE / NEI look at the last conditional trig (condition or probability < 100) on the same / previous track.
    T1 has no neighbour, so NEI on T1 is always false.
  - Condition and probability must both pass.
- MIDI output: ALSA seq port `StepRunner-8` / `MIDI Out` (a second instance is `StepRunner-8 2`; the number is freed when the instance
  is closed). In the MPC: Preferences → MIDI → enable Track on that port, then select it as the track's MIDI input.

## How to use it (screen)

Parameters are a window onto the pattern. STEPS tab, top to bottom:

- **STEP LOCKS** (top right): CONDITION, PROB, VELOCITY, LENGTH, PAN, then the row CC 20–24. Everything applies to the selected
  step of the selected track. (Track length is currently only on the TRACK tab.)
- **RANDOM / CLEAR** (top left): RND TRIGS, RND VEL, RND LEN, RND PAN, RND CC20–24, CLR LOCKS (resets condition/prob/locks
  of the selected step), CLR TRACK (clears the track).
- **MUTE** row below the TRACK buttons: one button per track (red = muted); same parameters as MUTE on the
  TRACK tab.
- **TRACK** selects the track, **PAGE** selects which 16 steps are shown (1–16 … 49–64).
- **TRIGS** (bottom, no frame or caption): 16 cells in a single row, STEP EDIT switch above them on the left. A cell shows: trig (orange), playhead
  (light cell with a white bar on top; also visible over a trig) and the selected step (red border). The playhead is the playhead
  of the selected track and is visible only on the page (PAGE) on which the track is currently playing.
  - **STEP EDIT** (button above the steps, looks like a TRACK segment) off: touching a cell toggles the trig and selects
    that step. On: touching adds the step to the selection or removes it from it (the trig is not changed); the last selected
    step cannot be removed. The selection applies to the displayed page: changing TRACK / PAGE or turning STEP EDIT off resets
    the selection to a single step.
  - While STEP EDIT is on, everything orange on the STEPS tab except the knobs turns teal (`19b3c6`): trigs in
    cells, RND / CLR buttons and section titles.
  - Multiple selected steps: all controls in STEP LOCKS (and TRK LENGTH) disappear, and in their place it says
    "MULTIPLE STEPS SELECTED". The parameters still do not accept changes (e.g. from a Q-Link). CLR LOCKS clears all selected steps.
  - Random buttons with STEP EDIT on work only on the selected steps: each selected step gets a new value
    (regardless of DENSITY and of whether it has a trig). RND TRIGS is disabled in this mode (grey button, does nothing).
    With STEP EDIT off, they all work on the whole track as before.
  - The numbers 1–16 below the cells are drawn in the background (position on the page) and do not change with PAGE.
- The CONDITION list closes by itself about 0.12 s after a selection. While it is open, every touch in it takes the nearest option.
- LENGTH and GATE are in quarters of a step (display `1.00` = one step).
- **TRACK EDIT** (button next to STEP EDIT): instead of STEP LOCKS the right section shows TRACK SETUP of the selected track
  (SPEED, LENGTH, MIDI CH, NOTE, VELOCITY, GATE). TRACK EDIT and STEP EDIT turn each other off; with TRACK EDIT, touching
  a cell toggles the trig as in normal mode.
  TRACK SETUP also has four buttons for the track length: 16, 32, 48, 64. Only the one matching the current
  length is coloured (e.g. with 19 none is). Knob order: LENGTH, VELOCITY, GATE, MIDI CH, NOTE.
- Colours per mode (trigs, RND buttons, titles): normal orange `f0a020`, STEP EDIT teal `19b3c6`, TRACK EDIT
  purple `a064e8`. CLR LOCKS and CLR TRACK are grey in all modes. Knobs are always orange.
- Two rows of buttons above the steps, 44 px high: top T1–T8 (left) and STEP EDIT / TRACK EDIT / RANDOM EDIT (right);
  bottom MUTE (left) and PAGE (right).
- **RANDOM EDIT**: the right section shows RANDOM SETTINGS (DENSITY, MIN, MAX). Colours stay orange. The three modes
  (STEP / TRACK / RANDOM EDIT) turn each other off.
- Two tabs: STEPS and PERFORM.
- **PERFORM tab**: at the bottom eight MUTE buttons (140×84, same height as the step cells) labelled T1–T8 (same parameters
  as the MUTE row on the STEPS tab); in a frame without a title (own image `pf_panel.png`, because the stock frame always draws
  a title line) PERFORM ALL and COMMIT on the left, eight knobs on the right (PROB, VELOCITY, LENGTH, CC 1–5). While PERFORM ALL
  is off, the knobs are hidden and a message is shown (`when=pf_ctrl:OFF/ON`, like MULTI on the STEPS tab).
- **PERFORM ALL** (v1.14.0, reworked in v1.15.0): when turned on, the entire state of all tracks is saved
  (`seq.perform_begin` → `pf_bak[]`: steps, locks, track settings, mute, Euclidian, + random settings).
  While it is on, each knob is an **offset** (display `+20`, `-30%`, `+1.00`) that is added during playback to ALL
  steps of ALL 8 tracks, relatively: PROB (only −100..0) to the step probability; VELOCITY to the velocity lock or, where there
  is none, to the track velocity; LENGTH to the length lock or the track gate; CC 1–5 only to steps that have that lock (CC 1
  shifts CC 20, 25, 85, 102, 107, 112, 14, 80 – each track its own number). Steps without a CC lock send nothing.
  **Turning PERFORM ALL off = discard**: EVERYTHING is restored as it was when it was turned on, including changes made in
  the meantime on the STEPS tab (the play position is not reset; the track/page/step selection is not restored).
  **COMMIT** writes the offsets into the pattern (`perform_commit`), also keeps the changes from the STEPS tab and turns PERFORM ALL off.
  A project saved while PERFORM ALL is on is stored as after a COMMIT (it loads with PERFORM ALL off).
  **UNDO** (v1.16.0, third button on the left; the parameter still has the key `pf_discard`): restores the state from the last
  time PERFORM ALL was turned on, even after a COMMIT, together with everything changed after the commit (`perform_undo`,
  `pf_undo_ok`). One level, one-shot; valid until PERFORM ALL is turned on again or a project is loaded (the backup is
  not saved in the project). While PERFORM ALL is on, UNDO does the same as turning it off.
  The parameter `pf_pan` is retired (stays in the list because of indices; always 0).
- **Performance pads** (v1.17.0/v1.18.0; a row of 8 pads 140×84 between the PERFORM ALL section, which is at the very top,
  and the MUTE row; parameters `pfx1..8`; purple – the TRACK EDIT colour – while on; not saved in the project):
  1 **LOOP 4**, 2 **LOOP 2**, 3 **LOOP 1** – each track repeats the n steps it played last before
  being turned on (`seq.set_loop`, `loopn`, `lp_k0[]`); mutually exclusive, changing the size keeps the same end
  point; the song position keeps running underneath, so when turned off the track returns to where it would otherwise be.
  4 **NO LOCKS** – trigs play "bare": without conditions, probability and locks (`seq.no_locks`).
  5 **RANDOM STEP** – each track plays a random step of its own instead of the next one (`seq.rnd_step`).
  6 **INVERT MUTE** – inverts the mute of all tracks; turning it off inverts them back. 7 **UNMUTE ALL** – momentary button, removes
  all mutes (and turns INVERT MUTE off). 8 **FILL** – the same switch as the `fill` parameter (FILL / NO FILL conditions).
- **v1.19.0 layout**: STEPS tab – the mode buttons (EUCLIDIAN / STEP EDIT / TRACK EDIT / RANDOM EDIT) are at the top right
  (cy=110), frames start at y=140, the T1–T8 / MUTE / page rows are 44 px lower, the cells 14 px lower (y=584).
  PERFORM tab – pads are 140×64; above the LOOP row there is a new row of 8 empty pads (`pfy1..8`, no function, not
  saved). Pad 7 is **MUTE ALL / UNMUTE ALL**: if no track is muted it mutes all, otherwise it removes all mutes
  (`seq.mute_toggle_all`); the caption changes via the read-only parameter `mstate` (NONE / SOME, `when=`).
- **v1.20.0**: the STEPS tab is now called **PROGRAM** (in older notes "STEPS tab" = PROGRAM). The EUCLIDIAN button is
  at the very top left (cx=96), the other three modes at the top right. RND TRIGS → **RND STEPS** (key `rnd_trig` unchanged).
  **Multiple selected steps no longer lock STEP LOCKS**: the knobs (condition, prob, velocity, length, pan, CC 1–5)
  write to ALL selected steps (`each_sel`), and show the value of the last touched one. The MULTI message and the option
  `panel:MULTI` are no longer used (the parameter `multi` still exists).
- **GLOBAL** (v1.21.0; fifth mode, button at the very top right – STEP EDIT / TRACK EDIT / RANDOM EDIT moved to the left;
  `global_edit`, `panel:GLOBAL`, `ptitle:GLOBAL`, colours as in normal mode): the right section shows GLOBAL SETTINGS.
  For now one knob: **SHUFFLE** 50–80 % (`seq.shuffle`, `step_time()`): every second step (odd index) is delayed by
  (shuffle−50)/50 of a step length; 50 = straight, 66 ≈ triplet. Each track shuffles on its own grid (its own speed).
  Chunk: line `G shuffle mask`; projects without it get 50 and all tracks enabled.
  v1.22.0: switches **SHUFFLE ON** T1–T8 (`shuf1..8`, `Track.shuf`, all on by default; a disabled track
  plays straight) and knob **MIDI CH (ALL)** (`glob_ch`): sets the channel of all 8 tracks at once; shows the channel of T1 or
  `MIXED` when the tracks differ (individual channel still in TRACK EDIT).
- **PATTERN EDIT** (v1.24.0; sixth mode, button between TRACK EDIT and RANDOM EDIT; `pattern_edit`, `panel:PATTERN`,
  `ptitle:PATTERN`): right section PATTERN SETUP with buttons **LENGTH (ALL TRACKS)** 16/32/48/64 (`plen16..64`,
  the one valid for all tracks is lit; `seq.set_len_all`). When a track grows, the existing pages are repeated onto the new ones
  (steps with all locks: 16→64 = page 1 three more times; 32→64 = pages 1–2 once more), but ONLY onto pages
  that have no trig – a page with trigs is not touched (64→16→64 loses nothing). The buttons at the top right are now:
  STEP EDIT 561, TRACK EDIT 713, PATTERN EDIT 865, RANDOM EDIT 1017, GLOBAL 1169 (cx).
- **UNDO / REDO** (v1.25.0; PROGRAM tab, two buttons one below the other at cx=750, in the T1–T8 and MUTE rows;
  parameters `undo`, `redo`): history of up to 100 steps (`Snap` = all tracks without mute and play position + shuffle;
  `hist_tick` in every block compares the state with the previous one). A change becomes one step when nothing changes for
  0.25 s, so turning a knob is one step; UNDO right after a change reverts only that change. A new change deletes the REDO
  steps. The history also includes RND, CLR, Euclidian, PATTERN/TRACK lengths, PERFORM commit/undo. Not included: mute, the
  track/page/step selection, performance pads. UNDO/REDO while PERFORM ALL is on first closes it (commit).
  The history is not saved in the project and is cleared when a project is loaded.
- **COPY / PASTE** (v1.26.0; only in STEP EDIT, `when=ptitle:EDIT`; UNDO/REDO are narrowed to 104 px, cx=691, and COPY /
  PASTE are to their right, cx=809). COPY stores the selected steps of the current page (the whole `Step`: trig, condition, prob,
  locks) with their spacing from the first one, then deselects all (`selmask = 0`). PASTE writes them from the first selected step
  onwards, with the same spacing, and across the page boundary; anything beyond step 64 is discarded. The clipboard is valid for other
  tracks/pages too, and is not saved in the project. The message "Copied n step(s)" / "Pasted n step(s)" stays for 2 s in the `readout` field
  above the page buttons (read-only parameter `msg`: 0, 1–16 copied, 17–32 pasted; the text is the VALUE text).
- **v1.27.0**: the PERFORM tab no longer has an UNDO button (the parameter `pf_discard` and `perform_undo` still exist, without a
  button; it is covered by UNDO on the PROGRAM tab); PERFORM ALL / COMMIT are at cy=206 / 290. **MUTE pads blink**: each is a
  `picture` (read-only `mpad1..8`: IDLE / HIT / MUTED → `pf_mute_off/hit/on.png`) under a transparent button
  (`hit_pad.png`) bound to `muteN`; `seq.hits[]` counts fired notes, the wrapper turns HIT on for 90 ms. The green buttons
  (ENABLE on, both COMMITs) are now `BTN_GREEN` (30,150,110); EUCLIDIAN mode (title, cells) stays 58c24f.
  PROGRAM tab: EUCLIDIAN is immediately left of STEP EDIT (cx=409); the mode row ends at x=1241, like 49–64.
- **v1.28.0**: MUTE pad blinking removed (the user did not like it; `mpad1..8` stay in the list, without a widget).
  EUCLIDIAN: DISCARD button removed; **ENABLE can now be turned off** – turning it off restores the trigs from before it was turned on
  (`euclid_discard`), you stay in EUCLIDIAN mode; COMMIT as before. The CONDITION list also closes on a touch outside it:
  the last widget of the PROGRAM tab is a transparent full-screen button (`hit_full.png`, `key=st_cond__open`,
  `when=st_cond__open:Open`) under the list – NOT CHECKED ON THE DEVICE (if it swallows touches on the list options, remove it).
  Random: the generator RNG had a fixed seed, so every load produced the same sequence; now the seed is mixed with the clock
  when the plugin loads and on every press of an RND button (`seq.reseed`).
- **v1.29.0**: EUCLIDIAN has only one generator: knobs **PULSE** (`eu_pl1`) and **ROTATE** (`eu_ro1`), r=34.
  PL2 / RO2 / TRO / BOOL have no knobs (parameters and core remain; ENABLE zeroes them when turned on). **Colours swapped**:
  normal mode is green (58c24f: titles, RND buttons, cells, PERFORM ALL, PLEN), EUCLIDIAN is orange (f0a020).
  In `gen_cell_imgs.py` this is a single swap `TRIG, EUC = EUC, TRIG` (the constant names stayed the old ones!). `theme_accent`
  (knob values, list arrow) is still orange.
  **v1.30.0: the colour swap was REVERTED** (normal mode orange f0a020 again, EUCLIDIAN green 58c24f). ENABLE is
  now the size of COMMIT (132×39) and sits at the bottom left of the section (cx=150, cy=446), left of COMMIT. GLOBAL: the ALL button
  is moved to the right (cx=1202).
- **v1.31.0** (new parameters at the end: `pf_cc117`, `pcopy`, `ppaste`, `shift`, `eu_len`, `eu_rnd`):
  - PERFORM ALL: knob **CC 117** left of PROB. Not an offset: an absolute value 0–127 that is sent immediately while turning
    as CC 117 on the MIDI channel of each track (once per channel; `send_cc117`), for MIDI learn. Works only while
    PERFORM ALL is on; turning PERFORM ALL off restores and re-sends the value from before it was turned on; COMMIT keeps it.
    Saved in the chunk (third field of line `G`). Not in the UNDO history.
  - PATTERN SETUP: **PAGE (ALL TRACKS)** – page selector (the same `pg1..4` buttons) + **COPY PAGE / PASTE PAGE**
    (`seq.copy_page/paste_page`): copies 16 steps with locks of ALL 8 tracks from the displayed page to another.
    Message "Copied page n" / "Pasted to page n" (`msg` 33–40) in the readout field, now also in PATTERN mode.
  - STEP EDIT: button **SHIFT** (`shift`, left of the message). Without SHIFT a touch selects only that one step; with SHIFT
    a touch adds/removes the step from the selection (old behaviour). SHIFT turns off when leaving STEP EDIT.
  - EUCLIDIAN: knob **LENGTH** (`eu_len`, 1–64) – rhythm length independent of the track length; the rhythm repeats across
    the track (`k % eu_len`). `eu_len = 0` means "track length" (old projects; ENABLE then sets it to the track
    length). Button **RANDOM** (`eu_rnd`, `seq.euclid_random`): length 3–16, pulses 1..length−1, any rotation;
    turns the generator on if it is not on. Buttons at the bottom: ENABLE, RANDOM, COMMIT. Chunk: `eu_len` is the 10th field of line `e`.
- **v1.32.0** (PROGRAM tab layout): the MUTE row is removed from the PROGRAM tab (mute remains on the PERFORM tab). T1–T8 are
  in the bottom row (cy=551, where MUTE was). Above them (cy=504), in ALL modes: SHIFT, COPY, PASTE, UNDO, REDO
  (cx 87/208/330/451/573, 104 px). COPY / PASTE work in all modes (outside STEP EDIT on the one selected step);
  SHIFT has an effect only in STEP EDIT (elsewhere it reverts to off). The message (`msg` readout) is always on the right, above
  the page buttons. PATTERN SETUP: the page selector and COPY PAGE / PASTE PAGE are removed (parameters `pcopy`,
  `ppaste` and `seq.copy_page/paste_page` remain, without buttons). **A Euclid LENGTH greater than the track length overrides it**
  while the generator is on (`Seq::eff_len`, used by `fire`, `euclid_apply`, `randomize`); `tr.len` does not change
  until COMMIT is pressed, which then writes the rhythm length as the track length.
- **v1.34.0** (new parameters: `cc_edit`, `ccn1..5`, `cc_commit`, `cc_discard`):
  - **COPY / PASTE depend on the mode** (the same two parameters `copy` / `paste`; caption via `when=ptitle:` and images
    `copy_step/page/trk.png`): STEP EDIT = selected steps (as before); TRACK EDIT = **COPY TRK / PASTE TRK** – the whole
    track (steps, locks, length, speed, velocity, gate, random and Euclid settings, shuffle switch) WITHOUT MIDI
    channel, note, CC numbers and mute (`seq.paste_track`); all other modes = **COPY PAGE / PASTE PAGE** – the displayed
    page of the SELECTED track. Three separate clipboards (`clip`, `pclip`, `tclip`). Messages: `msg` 33–40 page, 41–56 track.
  - **EDIT CC** (TRACK EDIT, button in place of the CC BASE knob; `panel:CCEDIT`): five knobs CC 1–5 select the controller
    number (0–127) of each CC lock separately. It is a draft (`ccdraft[]`) until COMMIT is pressed; DISCARD, turning off
    EDIT CC, changing the track or leaving TRACK EDIT discard it. In the core: `Track.ccn[5]` (number+1, 0 = old block
    `ccbase + j`), `Seq::cc_num()`. Chunk: line `C i n1..n5` after the `t` line, only if the track has its own numbers.
    The `trk_ccbase` parameter exists without a widget (setting it clears `ccn`).
- **RANDOM settings per track** (v1.17.0): DENSITY / MIN / MAX are in `Track` (`rnd_dens/min/max`), the knobs
  show the values of the selected track. Chunk: line `R i dens min max` per track; the old `r dens min max` is still
  read and applies to all tracks.
- **EUCLIDIAN** (fourth mode button, left of STEP EDIT; green `58c24f`): instead of RANDOM / CLEAR the left section
  shows the Euclidian generator of the selected track.
  - ENABLE (in the title row): saves the track's current trigs, and from that moment the generator writes them (steps
    1..track length; step locks and conditions are not touched). While it is on, touching it again does nothing.
  - PL1 / PL2: number of pulses of the two generators (0–64, at most the track length); RO1 / RO2: rotation of each; BOOL: how they
    are combined (OR, AND, XOR, SUB = first without second); TRO: rotation of the result. Changing anything, including the track length,
    immediately rewrites the trigs.
  - DISCARD: restores the trigs from before ENABLE and turns the generator off. COMMIT: keeps the generated trigs and turns the generator off.
    Both immediately return the screen to normal mode (RANDOM / CLEAR section, orange trigs).
  - The state is per track and is saved in the project (including the saved original trigs), so DISCARD works even after
    reloading. The track stays "generated" even when you leave EUCLIDIAN mode; manually changing trigs then applies until
    the next change of the Euclidian settings.
- PLAYHEAD, FOLLOW and FILL no longer have an on-screen control. The parameters exist (append-only); FILL is on a Q-Link
  (STEPS and TRACK page), so the FILL / NO FILL conditions can still be used.

## Files

| File | What it is |
|---|---|
| `seqcore.hpp` | sequencer (no VST/ALSA): tracks, steps, conditions, locks, timing |
| `steprunner8.cpp` | VST2 wrapper, ALSA port, parameters-as-window, chunk |
| `gen_params.py` → `params.json` | parameter list (VST index = order) |
| `vst.json`, `layout.conf` | input for `tools/gen_vst.py` (params.h + skin) |
| `gen_cell_imgs.py` → `images/` | all skin images (cells, buttons, pads, large messages); generates exactly the ones layout.conf mentions (including commented-out lines) |
| `host_test.cpp`, `test.sh` | offline test (core + plugin); not part of the plugin |
| `testdata*/`, `test_mkxpj.py` | sample MPC projects (.xpj) and the tool the tests use for mapping / reading a project |
| `skin_edit.sh` | hides/restores layout.conf lines for Skin Studio |
| `PROJECT_HISTORY.md` | Trig Seq change history (v1.x – v2.12.0) |
| `build.sh` | ARM build (Docker), same recipe as Maze Sequencer |
| `release.sh` | packages the build into `dist/StepRunner-8-<version>-mpc-armv7.zip` with `install.sh` (after `./build.sh`) |
| `README.md`, `LICENSE`, `tested.json`, `.github/workflows/release.yml`, `docs/screenshots/` | own GitHub repo: user docs, license, tested devices, CI release (builds with sd88me/mpc-vst-plugins' reusable workflow), catalog screenshot |

## Rules

- Offline first: `./test.sh` must print `PASSED`.
- Parameters are **append-only**: new ones go at the end of `gen_params.py` and at the end of `enum P_*` in `steprunner8.cpp`
  (before `P_OWN_COUNT`). On load, the plugin checks that the keys match.
- Condition codes (`C_*`) and speeds (`SPEEDS`) are also append-only — they are saved in the project. The order of conditions in the list is
  separate (`cond_at()` in `seqcore.hpp` + `conds` in `gen_params.py`, must match; the test checks this) and can
  be changed freely.
- Chunk `TSq3` (since v1.6.0; `TSq2` = the same without the pan byte, still loaded): text, lines `g`, `t <i>`, `s <i> <hex>` (11 bytes per step: on, cond, prob, vel, len, cc20..cc24, pan), `r` (random density, min, max). Line `c` from v1.1.0 is ignored. Unknown lines are skipped, so new versions can
  add lines.

## History

StepRunner-8 continues Trig Seq (`poc/trigseq/`, VST id `TrSq`, last version v2.12.0). The whole Trig Seq history
(v1.x and v2.x) is in `PROJECT_HISTORY.md`; here only StepRunner-8 versions.

### v1.2.0 (in development)
- RND RATCHETS also draws RATCHET RESOLUTION (AUTO, 1/16 .. 1/80) for each selected step; a step that gets one hit gets
  AUTO.
- PERFORM: STOP CC pad (`pf_stopcc`, appended) in the place of HUMANIZE. While lit, `Seq::emit` drops every CC
  (status 0xBn): locks, base values, pan, PERFORM ALL. Notes play on. Switched off (`set_stop_cc`): every CC with a base
  value and every pan rest is marked dirty, so they go out on each track's next step. Not saved; a STATE remembers it
  like the other pads (appended to `FX_PADS`, older versions ignore the bit). `pf_hum` and the HUMANIZE code stay, with
  no pad.
- PROGRAM tab: two arrow buttons `nav_left` / `nav_right` (momentary, appended; 72x44, the EUCLIDIAN style, a triangle
  for text) on the left edge of the top row, in every mode.
- The left section of PATTERN, TRACK and STEP EDIT has two pages (`Plugin::lpage[3]`, one per mode (`lmode`), not
  saved, kept across mode changes; ◀ / ▶ act only in those three modes, not in TIMING). Page 2: RND COND (`rnd_cond`, appended), RND TIMING and RND RATCHETS (the TIMING mode's `rnd_timing` /
  `rnd_ratch` keys, other pictures) on every trig of the track's length, no density (`Seq::randomize_extra`); with
  CLR LOCKS lit CLR COND / CLR TIMING / CLR RATCHETS on all 64 steps (`Seq::clear_extra`). In PATTERN EDIT the same three buttons
  (orange) act on all 8 tracks, CLR ... too. In STEP EDIT (teal) both act on the selected steps only, trig or not (the
  mask argument). `lbtn` got four options, VIOLET2, CLR2, ORANGE2 and TEAL2, so the page-1 buttons hide and CLR STEPS / CLR LOCKS (`lpanel:RANDOM`) stay. Undo covers it (Snap).
- EUCLIDIAN has two generators again, A and B, each with ENABLE, RANDOM, PULSE, ROTATE, LENGTH and its own direction;
  XOR / OR / AND / SUB combine them when both are on (`Track::eu_op`, buttons `eu_opxor .. eu_opsub`). New `Track`
  fields `eu_a`, `eu_b` (which is on; `eu_on` = at least one), `eu_len2` (0 = A's length), `eu_dir1`, `eu_dir2`
  (`Seq::EU_FWD / EU_BWD / EU_BOTH`, back and forth = 2m - 2 steps). `eff_len` takes the longer of the running rhythms.
  `eu_tro` has no knob (kept for old projects). New params (appended): `eu_ena2`, `eu_rnd2`, `eu_len2`, `eu_fwd2`,
  `eu_bwd2`, `eu_both2`, `eu_dirv2` (read-only), the four boolean buttons, `eu_opv` (read-only). Renamed (key and slot
  unchanged): Pulse A, Rotate A, Length A, Pulse B, Rotate B.
  - Behaviour change: `eu_fwd / eu_bwd / eu_both` now set A's rhythm direction; until v1.1.1 they set the track's
    direction (TRACK EDIT has its own FWD / BWD / BOTH).
  - Saved: the `e` line gets five more numbers (A on, B on, B's length, A's and B's direction); older versions stop
    reading before them. A project without them loads with A = eu_on and B on only if it had pulses on B or AND.
  - Layout: one row per generator (a knob takes 130 px, three a row): ENABLE / RANDOM 80x26 and three 26x26 arrows on
    the left, the knobs at r=26; boolean row and COMMIT at the bottom.
- TRACK EDIT: the VELOCITY and GATE knobs moved to TRACK VALUES (`trk_vals` button, `Plugin::trk_vals`, panel
  option TRKVAL, not saved), laid out as STEP LOCKS with BACK (`trk_back`) where CONDITION is. VELOCITY / LENGTH are
  `trk_vel` / `trk_gate`; PAN (`trk_pan`) is `Track::panb`; PROB (`trk_prob`) is the new `Track::pcut` (100 - %, so a
  zeroed track means 100 %), rolled after the step's own condition and probability, not under NO LOCKS, saved as a
  `K t %` line when not 100; CC 1-5 (`trk_cc1..5`) are `Track::ccb` (the same base values TRACK SETUP edits). PAN and the
  CCs are sent when turned (STOP CC still holds them back).
- SHIFT ◀ / ▶ (`shift_left` / `shift_right`) in TRACK EDIT (the track) and PATTERN EDIT (all 8): `Seq::shift_track`
  rotates steps 1..eff_len by one with everything on them, and the backup bits; a generated track also moves `eu_tro`
  by one (exact while the rhythm's length divides 64, e.g. 4, 8, 16, 32). It follows the panel that shows the
  buttons (`panel` TRACK or PATTERN), so it works under EUCLIDIAN too; nothing in STEP EDIT.
- vst.json 10200, release.sh 1.2.0.

### v1.1.1 (2026-10-13)
- Low-RAM skin, after a user report: opening the plugin window added about 25-30% RAM on the MPC. MPC decodes every
  picture of the skin when the window opens. The 1.1.0 skin had 1244 pictures, 110 of them full-screen, about 385 MB
  decoded (RGBA). Most were the 48 "N VALUES MAPPED" messages: each a full-screen button, and the skin tool writes a
  button's on and off picture separately even when they are the same file.
  - The DONE message is now one full-screen button `big_done_bg` (`when=bigdone:ON`, a title band with no text) plus a
    small button per count (`big_done_t1..48`, 880x86, cut from the full picture so it lies exactly over the band,
    `when=bigmsg:DONEn`, also `key=msg_ok`). gen_cell_imgs.py makes them; `images/big_done1..48.png` are gone.
  - `skin_dedupe.py` (run by build.sh after gen_vst.py) keeps one file per picture content, points the skin JSON at it
    (plain text replacement of the quoted names) and deletes the copies: 707 copies removed.
  - Result: 539 pictures, 10 full-screen, about 61 MB decoded. The plugin binary and its behaviour are unchanged.
- vst.json 10101, release.sh 1.1.1.

### v1.1.0 (2026-10-12)
- STATE per PATTERN: `Plugin::pst[NPATS][NSTATES]` (was eight for the whole sequencer), `pst_act[NPATS]`. The STATE
  buttons, `pst_act` and a recall all use the pattern playing (`pat_cur`), so a state can only go back into its own
  pattern and the 1.0.2 cross-pattern switch is gone (and with it the cue drop: a recall leaves a cued pattern cued).
  When a pattern comes in (tap or cue) the buttons show its states (getP follows `pat_cur`).
- New read-only param `pst_pat` (P1..P8, appended; in the told[] list and the effCanBeAutomated exclusions): the
  PERFORM tab shows "STATES OF / PATTERN n" under CLEAR STATE (layout.conf, text when=pst_pat:Pn).
- COPY PTN copies the pattern's states into `pstclip`, PASTE PTN replaces the target's states (UNDO does not bring
  states back). STORE + pattern copies them too. CLEAR + pattern (the playing one or another) deletes them. CLEAR PTN
  (the PATTERN EDIT button that blanks the steps, undoable) keeps the states.
- `pat_keep`: a pattern with states stays full even with no steps (`pst_any`).
- Chunk: `V<pattern><state> ` + the state's lines (was `Q<state> `). Loading still reads `Q` lines: into the pattern
  of their `W` sub-line (1.0.2) or else the pattern playing; a pattern that only has states gets fresh empty tracks.
  ChunkSrc copies only the full states under the lock. Memory: 64 states x ~8 KB = ~530 KB more per instance, no CPU
  cost (states are touched only on a tap). Tests rewritten. vst.json 10100, release.sh 1.1.0.

### v1.0.2 (2026-10-12, built, never published)
- STATE and PATTERN: a state remembers the pattern it was saved on (`PState::pat`, chunk sub-line `Q<k> W <pattern>`;
  older versions skip it as an unknown track line). Before, a recall wrote the snapshot into whatever pattern was
  playing, so recalling STATE 1 (saved on pattern 2) while on pattern 3 overwrote pattern 3. Now a recall on another
  pattern keeps the playing one (`pat_load`), switches to the state's pattern at once (also while playing; no cue) and
  loads the snapshot there; a cued pattern is dropped (also on the state's own pattern). `pat_load` resets the undo
  history, so the recall re-seeds `hist_done` from the loaded pattern: one UNDO restores that pattern as it was before
  the recall. States from before 1.0.2 (`pat = -1`) still go into the pattern playing. Message: "Pattern N Loaded".
  Tests added. vst.json 10002, release.sh 1.0.2.
- test.sh works with newer mpc-vst-plugins tools too: their `gen_vst.gen_params` reads the vst.json folder from
  `sys.argv[1]` (CC 20-35 / NRPN map), so test.sh sets it. Checked against the pinned c16b824 and upstream main (36f3fba).

### v1.0.1 (2026-10-12)
- SETUP, TRACK SETUP, RANDOM SETUP and the map list: the left section shows the StepRunner-8 logo (`art/logo.png`,
  476x335 = the frame; gen_cell_imgs.py crops 1 px all round into `images/logo.png`, drawn as a frame picture
  `when=lpanel:LOGO`) instead of the RND / CLR buttons and the "RANDOM PATTERN / CLEAR PATTERN" title. `lpanel`
  (read-only) got a fifth option LOGO; `lbtn` is NONE there. Test added. vst.json 10001, release.sh 1.0.1.
- Manual: SETUP no longer has random buttons; version 1.0.1.
- Logo replaced (2026-10-12) with the user's second version (subtler bottom line); manual screenshots not retaken.

### v1.0.0 (2026-10-10) – Trig Seq v2.12.0 renamed
- New plugin identity: name "StepRunner-8", VST id `SR-8` (0x53522D38; Trig Seq was `TrSq`), version 1.0.0 (vst.json
  10000), plugin folder `/sdcard/Synths/djokenzi - VST - StepRunner-8/`, binary `steprunner8.so`, ALSA client
  "StepRunner-8" ("StepRunner-8 2", ...), log `/tmp/steprunner8.log`. New repo folder `poc/steprunner8/`;
  `poc/trigseq/` stays as it was (archive).
- Because the id is new, StepRunner-8 and Trig Seq can be installed side by side; projects saved with Trig Seq keep
  opening with Trig Seq. CREATE DEFAULT MAP / FETCH find the project by the plugin id, so they work with StepRunner-8
  projects only. MIDI Learn mappings (channel + CC) are unaffected. In a project the tracks' MIDI input must be set to
  the port "StepRunner-8".
- Unchanged: every parameter and its index, the chunk format (internal tag `TSq3`), CC numbers, behaviour, skin
  (images and layout byte-identical; only the widget hash names differ).
- Code: `trigseq.cpp` → `steprunner8.cpp`; `TRIGSEQ_*` (test build flag, test env vars) → `STEPRUNNER_*`,
  `trigseq_test_*` → `steprunner_test_*`. testdata9/9w9.xpj and test_mkxpj.py carry the new uid (and the bank's fxID
  `SR-8`). build.sh no longer removes an old Presets folder. New test: uid, product, vendor, version.
- Mode buttons (2026-10-11, still v1.0.0, not released yet): a new plugin opens on SETUP (Trig Seq: TRACK EDIT; a saved
  project still opens where it was left). One of SETUP, PATTERN EDIT, TRACK EDIT, STEP EDIT is always lit and a tap on
  the lit one changes nothing (before, STEP / TRACK EDIT and SETUP switched off to PATTERN EDIT). STEP EDIT stays lit in
  TIMING; in EUCLIDIAN the button of the right-hand panel's mode (rmode) stays lit, a tap on it leaves EUCLIDIAN for that
  mode, and EUCLIDIAN off / COMMIT / DISCARD go back to that mode (before: PATTERN EDIT). SETUP lit for RANDOM SETUP: a
  tap goes back to the SETUP panel (as before); RANDOM SETUP off goes back to SETUP too. Tests adjusted, new test for
  the four buttons.
- EUCLIDIAN button (2026-10-11, still v1.0.0): shown on TRACK EDIT only (and while EUCLIDIAN is on, to switch it off);
  STEP EDIT shows TIMING in its place as before, PATTERN EDIT and SETUP show nothing there. `slotv` (read-only) got a
  third option HIDDEN. Tests for it.
- User manual (2026-10-11): `docs/MANUAL.md` with screenshots from the device in `docs/manual/` (taken through MPC
  Remote, full 1280x800 frames). README links to it.
- Name check (2026-10-10): no music product named StepRunner / StepRunner-8 found (a 2D game "StepRunner" on itch.io and
  an unrelated GitHub user exist); no trademark database was searched.

## MPC project format (.xpj) – researched 2026-10-06, firmware 3.9.1.2

From comparing two projects (`cctest1` / `cctest2`, difference: cutoff and resonance of pad A01, pan of pad A02, two MIDI learn
mappings). Research only, none of this is in the code yet.
- `.xpj` is gzip; inside, 5 header lines (`ACVS`, version, `SerialisableProjectData`, `json`, `Linux`) then JSON (~4 MB).
- MIDI learn: `data.midiLearnSettings.controls[]`, each entry:
  `control.name` („A01 (Filter Cutoff)"), `control.targetData[0]` = {`parameter` (514 = cutoff, 515 = resonance),
  `track` („Drum 001"), `instrumentIndex` (0 = pad A01), `paramType`, ranges 0..1}, `control.controlValue` (current
  parameter value 0..1) and `mapping` = {`mappingType` 2, `controlType` 5 (Abs CC), `channel` 1..16, `data1` = CC number}.
- Pad values: `data.tracks[i].program.drum.instruments[pad]`: `synthSection.filterData.value0.filterCutoff` /
  `filterResonance` (0..1; 37 → 0.2913 = 37/127), `mixable.pan` (0..1, 0.5 = center; +20 → 0.70).
- Consequence: initial CC value = `round(controlValue * 127)` for the entry whose `mapping.data1` = CC number (and channel).
  Not verified whether `controlValue` follows the parameter if it is changed AFTER mapping (in the test they were equal).
- The plugin does not know the path of the open project; idea: the newest `.xpj` by modification time on `/media/*`.

## How it works inside (important for later)

- Time: host `ppqPos` is converted into a free-running position `song` that only increases. When the MPC returns the sequence to the start
  (loop), the tracks continue — that is why 64 steps and A:B work even in a 1-bar MPC sequence. Start from the beginning = step 1.
- Resolution: events are sent at the start of the audio block (128 samples ≈ 3 ms), as in the other plugins in the repo.
- Since the same knob means a different value after a track/page/step change, `processReplacing` compares all parameters with
  what the host knows (`told[]`) and sends `audioMasterAutomate` for the changed ones. Never from `setParameter`.

## To check on the device

0. v1.3.0: whether the cells follow the playhead without stuttering (2 notifications to the host per step), whether the transparent toggle receives touch,
   whether the toggle returns when STEP EDIT is on, whether the MPC shows the step number below the cell and refreshes it on
   page change (`audioMasterUpdateDisplay`), and the look of the new (browser) skin.

1. Whether the buttons and knobs refresh when TRACK / PAGE / EDIT STEP changes (the `told[]` mechanism).
2. Whether the MPC responds to our `audioMasterAutomate` with `setParameter` (echo). If so, fast track switching could
   write the old value into the new track — then a safeguard needs to be added.
3. Whether the PLAYHEAD readout and FOLLOW work live.
4. Automation recording: window knobs (st_*, trk_*, step*) make no sense as automation.
5. CONDITION popup (44 options, 8 columns) — readability on screen.

## Ideas for later

- Configurable CC numbers (currently fixed 20–24 in `CC_NUM`), a sixth CC (25), random settings per generator/track and seed (as in Step Seq), playhead display on trigs, copy/paste of tracks and pages, swing, micro-timing, retrig,
  more patterns, colors/images for the playhead (requires `"art": "html"`).

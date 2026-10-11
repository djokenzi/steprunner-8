# Trig Seq — change history (v1.x – v2.12.0)

Trig Seq was renamed StepRunner-8 after v2.12.0 (= StepRunner-8 v1.0.0). Up to v1.99.0 this was moved out of
PROJECT_NOTES.md in v2.10.0; v2.0.0 – v2.12.0 follow at the end.

- 2026-10-03: removed CC tracks C1–C4 and the single CC lock with a per-track number; instead every step has five CC
  locks (CC 20–24). STEPS tab rearranged: locks on top, trigs at the bottom in one row. Parameters were
  renumbered in the process and the chunk became `TSq2` — allowed only because the plugin was not yet on the device. From the
  first build on the device, append-only applies.

- 2026-10-03, v1.1.0: CC lock no longer stays "stuck" — it returns to CC VALUES (new parameters `cc_base1..5`,
  appended at the end; chunk line `c`). Condition list reordered (FILL/PRE/NEI at the end) without changing the saved codes.
  STEP LOCKS on the right, PLAY on the left.

- 2026-10-03, v1.2.0: CC reverted to the v1.0.0 behaviour (sends only the locked step, no return); CC VALUES
  knobs removed, their 5 parameters (`cc_base1..5`) remain as empty slots so the indices do not shift.
  Added random generators (parameters `rnd_*` at the end). PLAYHEAD, FOLLOW, FILL removed from the top-left section.

- 2026-10-03, v1.3.0: TRIGS are cells modelled on Step Seq: a `picture` (read-only parameter `cellN`, 9 options:
  trig + 2·playhead + 4·selected, option 8 is padding so that no state is 1.0) with a transparent `toggle` on top of it
  bound to `stepN`. Therefore the skin now requires the browser renderer (`"art": "html"` in `vst.json`; `build.sh` uses
  the `mpc-vst-html-art` image like Maze Sequencer). Added `step_edit`. New parameters are at the end of the list.

- 2026-10-03, v1.3.1: touching a cell often did not respond. Cause: `tools/shadow_skin.py` gives every `toggle` with
  an image a touch area `max(120, w+10)` wide and `h+26` high, and the cells were at 66 px, so the right
  neighbour's area covered the right part of each cell and swallowed the touch. Cells are now 120×56 in two rows (pitch 132 px, rows
  86 px), with no overlap. **Layout rule: a toggle with an image must not be closer to its neighbour than `max(120, w+10)`
  horizontally and `h+26` vertically.** EDIT STEP knob replaced with TRK LENGTH; CONDITION moved to the left.

- 2026-10-03, v1.3.2: back to a single row of 16. Cells are now a `button` (not a `toggle`) over a `picture`: a `button` with
  an image gets a touch area exactly the size of the image (68×84, pitch 74 px), so there is no overlap, and it behaves the same
  (same "Toggle Switch" action). Cost: a `button` has no live name below it, so the numbers are static. The TRIGS frame and
  label removed, STEP EDIT is in place of the label. **Rule: for densely packed cells use `button`, not `toggle`.**

- 2026-10-03, v1.4.0: multiple selection (`selmask`, 16 bits, per page), random over the selection (`randomize_steps`),
  STEP EDIT as a `button` with its own images (`images/step_edit_on/off.png`, drawn by `gen_cell_imgs.py` with a font from
  `tools/html_art/fonts`). Closing the list: the user reports that the list did not close even though the code did the same as
  `wrapper/popup.h`; cause not determined. Now: closing is delayed by 0.12 s (`close_frames`), `audioMasterAutomate` is accompanied by
  `audioMasterUpdateDisplay`, and while the list is open the value is rounded to the nearest option instead of being treated
  as a Q-Link offset. If even this does not close the list, the problem is on the host/skin side.

- 2026-10-03, v1.5.0: colour per mode via `when=step_edit:OFF/ON` (two copies of a button / title, one visible).
  Buttons are therefore now their own images with the text baked in: two plain `button`s with the same key and label would get
  the same image name in `shadow_skin.py` and overwrite each other; with `img=` the name also contains the appearance tag. Frame titles
  are separate `text` lines (the frame has `title=""`). Cells: 17 states (bit 8 = STEP EDIT colour). New read-only
  parameter `multi` (at the end of the list) hides the STEP LOCKS controls via `when=multi:OFF`. Skin: 485 files, 3.4 MB
  (was 333 / 2.5 MB).

- 2026-10-03, v1.6.0: per-step pan lock (CC 10, `st_pan`) and `rnd_pan`, both at the end of the parameter list. The chunk is
  now `TSq3` (11 bytes per step); `TSq2` projects load without the pan lock. A project saved with v1.6.0 cannot be opened by an
  older build. TRK LENGTH removed from STEP LOCKS (remains on the TRACK tab). Order: PROB, VELOCITY,
  LENGTH, PAN, CC 20–24.

- 2026-10-03, v1.7.0: `track_edit` + two read-only parameters for the skin: `panel` (LOCKS / MULTI / TRACK: what the right
  section shows) and `ptitle` (NORMAL / EDIT / TRACK: its title and colour). Two are needed because one `when=` line
  follows only one option of one parameter. `multi` still exists, but the skin no longer uses it. The TRACK tab was
  renamed to RANDOM.

- 2026-10-03, v1.8.0: length buttons (`len16..len64`, at the end of the list), colour for TRACK EDIT (cells now have 25
  states: 8 × mode; RND buttons in three copies via `when=ptitle:`), grey CLR buttons, lock parameter names without "Lock"
  (`Velocity`, `Length`, `Pan`, `CC 20`…; `trk_vel` is now `Track Velocity`). Skin: 4.4 MB.

- 2026-10-03, v1.9.0: T1–T8 and PAGE are no longer `enum_h` (its height is fixed in the tool, 33 px) but one
  `button` per option, bound to the new parameters `trk1..8` / `pg1..4` (at the end of the list). The plugin keeps lit
  the one currently selected and after every touch reports the true states back to the host (same mechanism as the cells). `len16..64`
  work the same way (they are no longer momentary buttons): ON = the track has that length. The `track` and `page` parameters remain for Q-Link.

- 2026-10-03, v1.10.0: `random_edit` (at the end of the list); `panel` and `ptitle` have a fourth option RANDOM. Second tab
  removed. The PAGE row is below the mode buttons.
- Investigated 2026-10-03, done in v1.11.0 (see below): a separate set of CCs per track on the same MIDI channel. The MIDI
  standard has exactly 40 undefined CC numbers (3, 9, 14, 15, 20–31, 85–90, 102–119), exactly as many as needed for 8 × 5, but they are not contiguous. Proposal:
  per-track CC BASE (as in Step Seq) with default blocks 20, 25, 85, 102, 107, 112 + two blocks outside
  the undefined ones; knob and RND button names must then be generic (CC 1–5) or dynamic. On mpc-forums one
  user reports that the MPC X goes haywire when sent CC 7, and CC 7 and 10 are not offered in MIDI automation —
  check how our PAN lock (CC 10) behaves.

- 2026-10-03, v1.11.0: per-track `ccbase` (`trk_ccbase`, at the end of the parameter list; ninth number in the `t` line
  of the chunk). Projects saved earlier do not have that number, so their tracks load with CC 20–24 as before; a new plugin and
  a new instance start with the default blocks. After upgrading, MIDI learn for T2–T8 must be redone in new
  projects. `effGetParamName` for `st_cc1..5` returns the actual number ("CC 85"), and the host is asked to re-read the names when
  the track or CC BASE changes.

- 2026-10-03, v1.11.1: **The MPC reads a parameter name only once, when the plugin loads.** In v1.11.0 the names of the CC
  knobs depended on the track (`effGetParamName`), but on the device they stayed "CC 20–24" forever, despite
  `audioMasterUpdateDisplay`. Names are now static, and the controller number is in the VALUE text. When the track,
  page or CC BASE changes, the plugin re-reports all five CC knobs (`audioMasterAutomate`), because the value text is
  re-read only when the host receives a report, and the value on two tracks can be the same. **Rule: everything that changes goes into
  the value text, never into the name.**

- 2026-10-04, v1.12.0: Euclidian generator (`euclid_*` in `seqcore.hpp`, parameters `euclid_edit`, `lpanel`, `eu_*`
  at the end of the list, chunk line `e`). `ptitle` has a fifth option EUCLID, cells 33 states. Pulses: `(j * p) % n < p`
  (Bresenham form of the Euclidian rhythm, first pulse on step 1); the layout for 5/8 is a rotation of the usual `x.xx.xx.`.
  The names PL1/PL2/RO1/RO2/TRO/BOOL and the operators are the usual Euclidian generator names. Knobs in that section are r=24 so that two rows fit in the frame. Skin: 5.4 MB.

- 2026-10-04, v1.13.0: COMMIT / DISCARD exit EUCLIDIAN mode. CC knobs again show only the value
  (controller number from v1.11.1 removed on request). PERFORM tab with parameters `pf_*` (at the end of the list), without function.
  v1.13.1: knobs on the PERFORM tab are the same size as on STEPS (r=34), and the top of the tab (y 96–226) is left empty
  for a section still to come.
  v1.14.0: PERFORM ALL (offsets for all 8 tracks, COMMIT / DISCARD, line `p` in the chunk); `pf_*` parameters are now
  bipolar (same indices); button named PERFORM ALL; MUTE buttons 140×84; frame without title and line.
  v1.15.0: PERFORM PROB −100..0; PAN knob and DISCARD button removed; a message instead of the knobs while PERFORM ALL is
  off; turning PERFORM ALL off restores the whole sequencer state (backup of all tracks); line `p` in the chunk dropped.
  v1.16.0: UNDO button in place of DISCARD (restores the state even after COMMIT).
  v1.17.0: PERFORM ALL section all the way at the top; row of performance buttons (`pfx1..8`, 1 = LOOP 4); random settings
  per track (line `R` in the chunk).
  v1.18.0: performance pads got functions (LOOP 4/2/1, NO LOCKS, RANDOM STEP, INVERT MUTE, UNMUTE ALL, FILL)
  and the TRACK EDIT colour.
  v1.19.0: pad 7 = MUTE ALL / UNMUTE ALL; new row of empty pads `pfy1..8`; pads 140×64; mode buttons on the STEPS
  tab at the top right.
  v1.20.0: PROGRAM tab (formerly STEPS); EUCLIDIAN at the top left; RND STEPS; lock knobs also work with multiple selected steps.
  v1.21.0: GLOBAL mode with a SHUFFLE knob (line `G` in the chunk).
  v1.22.0: GLOBAL: per-track shuffle switches and MIDI CH (ALL).
  v1.24.0: PATTERN EDIT mode (length of all tracks + page repetition).
  v1.36.2 (2026-10-06): FIRST PASS ON THE DEVICE (MPC Live, via mpc-addin-remote in standalone mode: screenshot and
  touch through `http://localhost:8090` from the Claude app's built-in browser; device coordinates = layout (cx, cy+24)).
  Confirmed working: STEP EDIT + SHIFT + COPY/PASTE STEP with message, TRACK EDIT, EDIT CC with IN USE (25 on T1 = T2's
  number), Euclid ENABLE / RANDOM (green cells in all modes), PATTERN 64, GLOBAL, closing the CONDITION list by touching
  outside it, PERFORM: STATE 1 saving, MUTE ALL → UNMUTE ALL label, LOOP 4. BUG FOUND: after turning PERFORM
  ALL on, the knobs still showed "OFF" (the value does not change 0 → 0, so the MPC does not re-read the text) – fixed: the
  `pf_on` state is part of `named_page`, so `audioMasterUpdateDisplay` is sent.
  SECOND PASS ON THE DEVICE (v1.36.2, 2026-10-06, sequencer playing; MIDI output monitored via `aconnect 130:0 131:1` and
  the add-in's MCP tool `midi_listen`, called with `fetch('/mcp')` from the browser): the "OFF" fix confirmed (+0 / +0% / +0.00).
  Notes at 468 ms at 128 BPM; CC lock goes out before the note (CC 20 = 32, velocity lock 33); the playhead line on the page
  buttons works; PERFORM: CC 117 is sent while turning and returns to 0 when PERFORM ALL is turned off, the VELOCITY offset changes both
  the track velocity and the lock (33 − 46 → 1), LOOP 4 repeats and when turned off returns to the song position; UNDO / REDO work.
  THIRD PASS (same day, all on the MIDI output): LOOP 4 / 2 / 1 repeat exactly the last 4 / 2 / 1 steps (checked
  against the sequence of velocity locks after RND VEL), NO LOCKS gives everything at 100 without CC, RANDOM STEP gives a random order; SHUFFLE
  shifts the even steps (145 / 90 ms instead of 117), the T1 switch turns it off; PASTE PAGE on T2 and PASTE TRK on T3 transfer steps
  and locks, T3 keeps its own note; Euclid ENABLE on T4 plays 4 pulses over 16; STATE 1 / 2 are saved and loaded.
  SAVING / LOADING AN MPC PROJECT confirmed: trigs and velocity locks, Euclid ENABLE on T4 (4 / 0 / 16),
  STATE 1 and 2 (loading a state with PERFORM ALL off turns it on). Not tested: FILL; restoring the custom CC
  numbers (EDIT CC) and shuffle values after loading (they were not changed in that project).
  RANDOM CC GENERATORS confirmed on the device (MIDI output): RND CC 1–5 each write to their own number (T1: CC 20–24) and do not
  touch the others; with DENSITY 100 / MIN 55 / MAX 98 all 16 steps get a value in 55–98; a second press gives a different sequence;
  T2 uses its own numbers (CC 25) and its own random settings (50 %, 0–127); in STEP EDIT, RND affects only the selected step.
  After an MPC restart `aconnect 130:0 131:1` must be run again.
  ALL 8 TRACKS (RND VEL / LEN / PAN / CC 1–5 on each, RND STEPS on T5–T8): each track plays its own note (37, 36, 42,
  82, 40, 38, 46, 44) and sends CC only within its own block (20–24, 25–29, 85–89, 102–106, 107–111, 112–116, 14–18, 80–84), pan
  on CC 10; no number outside those blocks.
  MIDI Start / MMC Play from the add-in do NOT start the MPC (the "MPC Remote" port is not enabled in the MPC) – the user presses PLAY.
  v1.36.1: IN USE in EDIT CC looks at the whole Trig Seq: the CC numbers of ALL other tracks regardless of MIDI channel (previously only
  tracks on the same channel, which with tracks on different channels looked as if it saw only its own track), + CC 10, CC 117
  and a duplicate in the draft. The PERFORM CC 1–5 knobs are not counted (they send the same numbers as the tracks).
  v1.36.0: **PERFORM states** – a row STATE 1–4 + CLEAR at the top of the PERFORM ALL section (`pst1..4`, `pclear`; knobs are
  at cy=176 / 304, state row cy=116). State = PERFORM knob settings (9 offsets + CC 117), not a pattern. Touching an empty
  state saves it (only while PERFORM ALL is on) and the button turns orange; touching a full one loads it (and turns on
  PERFORM ALL if it is not on); while CLEAR is lit, a touch empties the state. They are stored in the chunk (lines `P slot o0..o8 cc117`).
  v1.35.1: swapped places: the message (`msg`) is on the left above T1–T8 (cx=330, w=590), and COPY / PASTE / UNDO / REDO on the right
  above the page buttons (90 px, same columns: 920/1012/1104/1196). SHIFT only in STEP EDIT (cx=828).
  v1.35.0: EDIT CC marks a number that is already being sent on the same MIDI channel: a red IN USE tag below the knob
  (`picture`, read-only `ccuse1..5`, `cc_in_use()`): CC 10 (pan), CC 117 (PERFORM), the same number twice in the draft, or
  the number of any CC lock of another track on the same channel. Only a marker, the selection is not restricted. The value text itself cannot
  change colour (colour is part of the skin). `picture` does not accept `when=`, so the image for FREE is transparent.
  v1.34.2: Euclid RANDOM: half of the presses give a length of 3–16, half 17–64.
  v1.34.1: Euclid RANDOM picks a length of 3–64 (was 3–16); pulses always 1..length−1.
  v1.34.0: COPY/PASTE per mode (STEP / PAGE / TRK); EDIT CC (individual CC numbers per track, COMMIT / DISCARD).
  v1.33.0: page buttons follow the playhead of the selected track: each one is a `picture` (read-only `pgst1..4`: IDLE / SHOWN /
  PLAYING / SHOWN+PLAYING → `pgN_off/on[_play].png`, thin line near the bottom) under a transparent button (`hit_pg.png`) on `pgN`.
  v1.32.0: PROGRAM without the MUTE row, the SHIFT/COPY/PASTE/UNDO/REDO row always visible; Euclid LENGTH overrides the track length.
  v1.31.0: CC 117 knob, COPY/PASTE PAGE, SHIFT in STEP EDIT, Euclid LENGTH + RANDOM.
  v1.30.3: PERFORM COMMIT is green again (only Euclid COMMIT is gray); PERFORM ALL / COMMIT spaced apart (cy=186 / 310).
  v1.30.2: cells of a track coming from the Euclid generator (`eu_on`) are green in ALL modes (also STEP/TRACK EDIT);
  COMMIT buttons (Euclid and PERFORM) are gray, green only while pressed.
  v1.30.1: ENABLE / COMMIT moved up (cy=414); in EUCLIDIAN mode cells are orange while the track is not ENABLE,
  green when it is (cell mode 3 only with `eu_on`).
  v1.30.0: colors restored; ENABLE bigger and lower; ALL moved away.
  v1.29.0: Euclid only PULSE / ROTATE; colors of normal and EUCLIDIAN mode swapped.
  v1.28.0: no blinking; ENABLE turns off (no DISCARD); the condition list closes on a touch outside it; random seed from the clock.
  v1.27.0: PERFORM without the UNDO button, MUTE pads blink, new green for buttons, EUCLIDIAN with STEP EDIT.
  v1.26.0: COPY / PASTE in STEP EDIT with a 2 s message.
  v1.25.0: UNDO / REDO on the PROGRAM tab (history up to 100 steps).
  v1.24.1: buttons 16/32/48/64 in TRACK EDIT also repeat pages, only for the selected track (`seq.set_len_track`);
  the LENGTH knob (1–64) still only changes the length, without copying.
  v1.23.0: the SHUFFLE ON row also has **ALL** (`shuf_all`: lit when all tracks follow shuffle; a press turns all on, and
  when lit turns all off); the label is above the row. The CONDITION list is now in 5 columns (`cols=5`, 9 rows for 44 conditions).

  v1.37.0 (2026-10-06): CC BASE VALUES (BASE). Each of a track's five CCs can have a base value
  (`Track::ccb[5]`, 0 = OFF, otherwise value+1). When a step sends a CC lock and the next step of that CC has no lock of its own,
  the base value is sent ONCE on that next step (`Seq::cc_dirty`), regardless of whether that step has a trig.
  Two locks in a row: return only after the second. BASE = OFF is the old behavior (the value stays where the lock left it).
  EDIT CC panel: at the top in the title row READ PROJECT / DISCARD / COMMIT, row CC 1–5 (numbers), row BASE 1–5.
  Parameters at the end: `ccb1..5`, `cc_read` (208 in total). Chunk: new line `B i b1..b5`.
  READ PROJECT: reads the newest saved `.xpj` under `/media` (depth ≤3, skips `[ProjectData]`; for testing
  `TRIGSEQ_XPJ_ROOT`), unpacks via `libz.so.1` (dlopen, hence `-ldl`), and for each CC from the draft that has a mapping in
  `midiLearnSettings.controls` on the track's MIDI channel writes BASE = round(controlValue*127). A CC that
  is not found stays as it was. Messages: "Read n project value(s)" / "No saved project found" (message text max 23
  characters!). Limitations: the plugin does not know which project is open (it takes the newest file, so the project must first be SAVED);
  the channel must match exactly; pan has no BASE; not verified whether `controlValue` follows later movement of the
  parameter on the pad itself. Fixture: `testdata/cctest2.xpj` (CC20 ch.1 = 37, CC19 ch.16 = 21).

  v1.37.1 (2026-10-06): READ PROJECT on the device reported "Read 0 project values" although `9w9.xpj` (38 plugin
  mappings on channel 1, CC 20–23 among them) gives 4 offline. Assumed cause (NOT confirmed on the device): the search budget
  of 20000 items counted every file, so on a disk full of samples it was used up before the search reached
  the projects, and some older .xpj without mappings was taken. Now only folders consume the budget, and `stat` is done only for
  .xpj files; the build got `-D_FILE_OFFSET_BITS=64`. Fixture `testdata9/9w9.xpj` + a test with 21000 files.
  If it still reads 0: the next step is for the message to show the name of the found file.

  v1.37.1 confirmed on the device (the user: "now it works") – so the cause was the search budget.
  v1.38.0 (2026-10-06): (1) READ PROJECT on the home screen (top left, cx=105 cy=110, only `ptitle:NORMAL`; parameter
  `read_all` at the end, 209 in total): for all 8 tracks reads BASE for their five CC numbers on the track's channel and writes
  IMMEDIATELY into the tracks (no COMMIT, not in the UNDO history). Message = total count (msg 64+n, msg range now 0..104).
  (2) Lock knobs CC 1–5: when a step has no lock and the CC has a BASE, the text is the BASE in parentheses, e.g. "(94)"; without BASE "--".
  The parameter value stays 0 (no lock, nothing is sent); `base_hash` in `now_page` forces the host to re-read the text.

  v1.39.0 (2026-10-06): READ PROJECT on the home screen no longer reads immediately, but opens a TARGET LIST (panel = SOURCE,
  ptitle = SOURCE – both options added at the end; the left panel is duplicated in the layout for ptitle:SOURCE). At the top the project name
  (`srcproj`), 6 rows (`srcrow1..6`, text "Plugin 004 (38)", the selected one has "> " in front; row 1 = "All targets") with
  a READ button (`srcsel1..6`), PREV/NEXT (`src_prev/next`, 6 per page), CANCEL (`src_cancel`). Target =
  `targetData[0].track` from the MIDI learn entry. READ: BASE for all 8 tracks only from that target, immediately into the tracks, the list
  closes. The selection is ONE for the whole Trig Seq (`Plugin::src`, chunk line `S name`); READ PROJECT in EDIT CC respects it.
  Any mode button closes the list. 225 parameters. Reader reworked: `xpj_scan` (all entries + project name) and
  `xpj_bases` (filter by target and channel). Texts are still max 23 characters (the target name is cut to 15).

  v1.40.0 (2026-10-06): (1) The READ PROJECT list occupies the WHOLE PROGRAM tab. New read-only parameter `scr` (MAIN/SOURCE);
  all widgets without a condition got `when=scr:MAIN` (EXCEPT two frames – a conditional frame broke the background under the title
  in other modes, so the frames stay permanent, and the list is covered by the opaque image `images/src_panel.png` as
  `frame img= when=scr:SOURCE`); panel and ptitle = SOURCE, lpanel got the option HIDDEN, cells go to state 32
  (`cell_32.png` is now transparent), pgst got the option HIDDEN (`blank.png`). Duplicates of the left panel for ptitle:SOURCE
  removed. Rows have SELECT (marks the row, "> "), at the bottom DISCARD (`src_cancel`) and COMMIT (`src_commit`, new) –
  reading happens only on COMMIT. 227 parameters.
  (2) Lock knobs CC 1–5: without a lock the knob SITS at the BASE value (`value_of` returns BASE; text still in parentheses).
  The step does not change; if the host returns the same value back, no lock is created (guard in `set_value`). Turning creates a lock;
  all the way left (0) clears the lock and the knob returns to BASE. Consequence: a lock exactly equal to the BASE value cannot be
  set (it would make no sense anyway).

  v1.41.0 (2026-10-06): READ PROJECT list – no more SELECT button: the whole row is a touch area (transparent
  `button key=srcselN` with `images/src_hit.png` over the readout), and the marked row has an orange fill and a thick frame.
  New read-only `srchl1..6` (HIDDEN/IDLE/MARKED), 233 parameters. IMPORTANT for the skin: the row state is NOT a `picture` but two
  `frame img= when=srchlN:IDLE/MARKED` – a `picture` with a transparent state over a part of the screen where other modes have
  baked-in parts (titles, message frames) erases those parts on the home screen. Bug found along the way: `P_OWN_COUNT` must be
  `= P_..._END` when the enum ends with a range (otherwise it is 1 larger and list popups do not close – the test catches it).
  NOT done (awaiting a decision): CC parameter names from the project on the knobs. Limitations: the label under the knob on the
  device is the parameter NAME that the MPC caches (cannot be changed); `readout` has a fixed 26 px font and a frame. Options:
  name in the value text ("BD Level 94", shortened to ~9 characters) or a separate row of readout fields (~8 characters).

  v1.42.0 (2026-10-06): CC PARAMETER NAMES from the project (chosen variant 1: in the value text). `XEntry.name` =
  contents of the parentheses from `control.name` ("Plugin 004 (BD Level)" → "BD Level"). Stored per track and CC slot together with
  the CC number it was read for (`Plugin::ccname/ccname_cc`, chunk line `N track slot cc name`); shown
  only while the track still uses that CC number (`cc_label`). Written on COMMIT in the list and on READ PROJECT in EDIT CC.
  Lock knobs: "BD Level 94" / "BD Level (94)" / "BD Level --", name cut to 9 characters. PERFORM CC 1–5: name
  only if all 8 tracks have the same name for that slot ("Cutoff +0" / "Cutoff OFF"). No new parameters and no skin changes.

  v1.43.0 (2026-10-06): CREATE -MAPPED (button at the bottom left in the READ PROJECT list, `src_map`, 234 parameters). `xpj_map`:
  takes the newest saved project, finds the drum track "Drum 001" (the first `"name": "Drum 001"` after `"tracks"`, then
  `"instruments"`), and in a COPY `<name>-MAPPED.xpj` in the same folder adds MIDI learn entries: pad A01..A08 = track T1..T8,
  the track's five CCs on its channel → pad parameters 514 Filter Cutoff, 515 Filter Resonance, 522/523/524 Send 1/2/3
  (IDs from the user's list `drum-parametri.txt`, firmware 3.9.1.2; 91–93 are mixer sends, NOT used).
  `controlValue` = current pad value from the file (that is how the MPC writes it), always as a real number. A CC that is
  already mapped on that channel and a pad parameter that already has a mapping are skipped. The original is not changed; writing goes via `.tmp` +
  rename. A project already named …-MAPPED is rejected. Messages: "-MAPPED: n mappings" (msg 105+n), "No Drum 001 drum
  track" (146), "Cannot write project" (147), "Already a -MAPPED file" (148). Verified offline: the output is valid
  JSON, everything outside the mapping list is identical to the original, entry keys are the same as in the MPC's.
  NOT VERIFIED ON THE DEVICE: whether the MPC accepts manually added entries; whether the copy finds the samples (the project stores
  samples by name, the folder is `<name>_[ProjectData]` – the copy may look for `<name>-MAPPED_[ProjectData]`).

  v1.43.1 (2026-10-06): test of `cctest2-MAPPED.xpj` on the device: the MPC opens a "SAMPLE NOT FOUND" dialog (the copy does not
  find the samples – confirmed that it looks for them under the new project name). The user: samples are not needed in the -MAPPED copy anyway.
  `xpj_strip_samples`: all `"samples"` lists (project and programs) → `[]`, `sampleName`/`sampleFile` in layers → "".
  Still NOT confirmed that the MPC accepts the added mappings (the dialog interrupted the test).

  v1.44.0 (2026-10-06): WHICH PROJECT IS READ. On the device (fw 3.9.1.2) there is
  `/media/az01-internal/Settings/MPC/CurrentProject` (XML: `<VALUE name="lastProjectPath" val="…xpj"/>`); the user
  confirmed that it contains exactly the open project ("Analog Kitchen v3"). `xpj_pick()`: first that entry (if the file exists),
  otherwise the newest .xpj as before. Both READ PROJECT and CREATE -MAPPED go through it. NOT verified when exactly the MPC
  refreshes the entry (load / save / Save As); the project still has to be saved for the file to have fresh data.
  v1.43.1 on the device: -MAPPED was not created because the newest file was `9w9.xpj` without a Drum 001 track (not a write error).
  `.xmm` (MPC Documents/Midi Learn; `MidiLearnPreviousMapping` in MPC.settings): the global MIDI learn map from
  Preferences – fixed 119/120 items `Target_control=N` + `Mapping_*`, without track/pad/parameter. It cannot carry
  project parameter mappings; a dead end for CREATE -MAPPED.

  v1.45.0 (2026-10-06): the user confirmed: (a) `CurrentProject` works (v1.44.0), (b) the MPC ACCEPTS mappings written by
  Trig Seq. CREATE -MAPPED is replaced by WRITE MAPPING (same parameter `src_map`): the READ PROJECT list switches to track
  mode (`src_mode` 1, ptitle = MAPTRK – new option at the end, title "MAP TO TRACK") and shows the DRUM tracks of the open
  project (`xpj_tracks`: an element of `data.tracks` that has `"drum"` → `"instruments"`; name = the first `"name"`). A touch on
  a row selects the track, COMMIT writes the mappings INTO THE PROJECT FILE ITSELF (not into a copy), DISCARD cancels. Before writing, the file is
  copied byte-for-byte to `<name>.xpj.bak`. Samples are no longer touched (`xpj_strip_samples` removed). If there is nothing to
  add, the file and the .bak stay untouched ("Nothing new to map"). Messages: "n mapped. Reload proj." (105+n), "No drum track
  found" (146), "Cannot write project" (147). IMPORTANT for the user: the MPC keeps the project in memory – after writing, the project
  must be reloaded WITHOUT saving; saving before that overwrites the file and the mappings disappear.

  v1.46.0 (2026-10-06): the user wants a -MAPPED copy after all, not a write into the open project. The button is CREATE
  -MAPPED again; drum track selection from the list (v1.45.0) STAYS. COMMIT writes `<name>-MAPPED.xpj` next to the open project
  (`CurrentProject`), without samples (`xpj_strip_samples` restored), the original is not touched, no .bak. A project that already
  ends with -MAPPED is rejected ("Already a -MAPPED file", 148). Success: "-MAPPED: n mappings"; 0 new → "Nothing new
  to map" and no copy is made.

  v1.46.1 (2026-10-06): CREATE -MAPPED from a project that is already `…-MAPPED` or `…-MAPPED<n>` is no longer rejected: the copy
  goes to `…-MAPPED1`, `…-MAPPED2`… (first free number; existing files are not overwritten). From a regular project still
  `…-MAPPED.xpj` (overwrites the existing one). Message 148 removed. Still: 0 new mappings → no copy.

  v1.47.0 (2026-10-06): BUG in 1.43.1–1.46.1 per the user: the -MAPPED copy had no samples on ALL drum
  programs (that was done by `xpj_strip_samples` – a requirement understood earlier; now: "all tracks should stay untouched").
  Removed: the project text is changed ONLY in the mapping list. For the copy to find the samples, the folder
  `<name>_[ProjectData]` is copied to `<copy>_[ProjectData]` (`xpj_tree`, recursively, skips a file of the same size)
  in a BACKGROUND THREAD (`Plugin::copy_thr`; does not hold the lock, does not block audio), with a free-space check (statvfs).
  Messages: "Copying samples..." (148, while in progress) → "-MAPPED: n mappings"; "Sample copy failed" (149), "Not enough
  disk space" (150). A new COMMIT while copying is in progress is rejected (148). effClose aborts the copy (`copy_stop`) and waits for
  the thread. Not measured how long it takes for large projects on the device.

  v1.48.0 (2026-10-06): FINAL DECISION by the user (after 1.47.0 with sample copying): write INTO THE OPEN PROJECT with
  a backup copy, no -MAPPED copy and no copying of the data folder. The button is WRITE MAPPING; drum track selection
  stays. COMMIT: the project file is copied byte for byte to `<ime>.xpj.bak` (if it exists: `.bak2`, `.bak3`… – a backup
  copy is never overwritten), then the extended mapping list is written into the project; nothing else is changed.
  Messages in sequence (`Plugin::msg_next`): "n mappings written" 3 s, then "Close & reopen project" 15 s (148).
  0 new → "Nothing new to map", no write and no backup. Removed: background thread, `xpj_tree`, messages
  149/150. NOTE: saving the project in the MPC before reopening it overwrites the file with the in-memory state (the
  mappings are lost; the .bak remains).

  v1.49.0 (2026-10-06): INITIALIZE (renamed READ PROJECT on the start screen; `read_all`). It is NO LONGER full
  screen: a section in the LEFT frame instead of RANDOM / CLEAR (ptitle = SOURCE, lpanel = HIDDEN; the rest of the screen stays).
  The INITIALIZE button is lit while the section is open and a second touch closes it; any mode button also closes it.
  In the section: CHOOSE TARGET TRACK (`src_map`) and FETCH CC VALUES (`src_fetch`, new parameter, 235 total), below them
  the name of the open project. The list is NOT shown immediately.
  - CHOOSE TARGET TRACK → ptitle = MAPTRK: 3 rows of drum tracks (`SRC_ROWS`; srcrow/srcsel/srchl 4–6 are no longer used),
    PREV/NEXT, DISCARD (returns to the two buttons), COMMIT (writes the mappings into the project + .bak as in 1.48; the
    selected track is remembered in `Plugin::src`, chunk `S name`).
  - FETCH CC VALUES: immediately reads BASE and names for all 8 tracks from the target track (`src`; if none is chosen –
    from all mappings) and closes the section. The old "targets" list (All targets / Plugin 004) no longer exists.
  Skin: removed the `scr:MAIN` conditions and the opaque panel; `scr` is always 0, cells and pgst are no longer hidden. The
  right title STEP LOCKS, COPY/PASTE and INITIALIZE are duplicated for ptitle SOURCE/MAPTRK. LESSON: a duplicate `text` line
  must sit right next to the original – at the end of the file its background crop covers the baked parts of later widgets
  (the CONDITION label disappeared).

  v1.50.0 (2026-10-07): FETCH CC VALUES now also fetches the NOTE: track Tk gets the note of pad A0k of the target track
  (`xpj_pad_notes`: `program.padNoteMap.noteForPad.value0..7` in that track's element; in a drum program the default is
  36..43). Only when a target track is chosen (`src` is not empty) and exists as a drum track in the project; otherwise
  the notes stay. Before a note change a note-off is sent for the old one. Not in the UNDO history.

  v1.51.0 (2026-10-07): button FETCH CC VALUES → FETCH NOTES AND CC VALUES. Message: "Read 8 notes, n CCs" (msg 151+n)
  when the notes were read, "Read 0 notes, n CCs" (64+n) when they were not (no target track / not in the project). The two
  menu buttons are now one below the other, full width (440); the project name is below them and is visible only while the
  list is not open. msg range 0..191.

  v1.52.0 (2026-10-07): FETCH is split into two buttons in the second row of the INITIALIZE menu: FETCH NOTES (`src_notes`,
  new parameter, 236 total) and FETCH CC VALUES (`src_fetch`, now only BASE + names). Messages: "Read 8 notes" (192),
  "Choose a target track" (193, no target track chosen), "Target track not found" (194); CC: "Read n CC values"
  (64+n). The range 151..191 is no longer used. msg range 0..194.

  v1.53.0 (2026-10-07): PERFORM ALL (the user provided the description: the same offset on each
  track's value, no scaling, p-locks are NOT changed). Before: the offset was added to step locks. Now:
  - CC 1–5: offset on the BASE value of that CC on each track (`Seq::base_eff`); a lock on a step is sent unchanged, and
    the return after the lock goes to BASE+offset. A track without BASE (OFF) has nothing to offset.
  - Turning the knob SENDS CC IMMEDIATELY for all tracks that have BASE (`PfSnap` in the wrapper: state before/after, sends
    the differences); same on switching off (restores BASE), recalling STATE and UNDO. COMMIT sends nothing.
  - VELOCITY / LENGTH: offset only on the track velocity / gate; steps with a lock stay.
  - PROB: stays on all steps (there is no track-level probability). PAN still disabled.
  - COMMIT writes the offset into `ccb`, `tr.vel`, `tr.gate` (+ prob into steps); locks are not touched.
  The text "(94)" on the lock knob during PERFORM ALL shows the stored BASE, not the offset one.

  v1.54.0 (2026-10-07): PERFORM PROB (the user provided the description): the offset applies only to "plain" steps –
  100 % and no condition (`Seq::prob_eff`). A step with its own probability (<100 %) or with a condition (FILL, PRE, NEI,
  1:2…) is "locked" and does not change. We have no track-level probability, so the "base" is always 100 %. COMMIT writes
  only to enabled plain steps (afterwards they are <100 %, hence locked for the next PERFORM ALL).

  v1.55.0 (2026-10-07): FETCH NOTES no longer requires a target track. The note is found the same way as CC values: for
  track Tk it looks for a MIDI learn entry of one of its CCs (on its channel); the entry gives the drum track (`track`) and
  pad (`instrumentIndex`), and the note is `noteForPad.value<pad>` of that track (`xpj_pad_note`). So it is no longer tied
  to A01–A08 in order, but to the pad the track is actually mapped to. If a target track is chosen, only its mappings apply.
  A track whose CCs are not mapped to a drum pad (or are mapped to a plugin, instrumentIndex 256) keeps its note.
  Message "Read n notes" (msg 195+n, n = 0..8); messages 192–194 removed. msg range 0..203.

  v1.56.0 (2026-10-07): the frame of the selected step is RED only in STEP EDIT; in other modes GREY (`SEL_IDLE`
  150,150,150 in gen_cell_imgs.py). Cell images: mode 1 (STEP EDIT) and 3 (Euclid green) draw red, 0 and 2 grey. Since
  a Euclid track is green in every mode (same mode code), the plugin sets the "selected" bit on it ONLY in STEP EDIT –
  outside it a Euclid track has no selected-step frame.

  v1.57.0 (2026-10-07): the selected-step frame exists ONLY in STEP EDIT (the user: the grey frame from 1.56.0 is not
  needed after all). The plugin sets the "selected" bit in the cell state only while STEP EDIT is on (also applies to
  Euclid tracks); cell images again draw only the red frame. The selected step still exists in the other modes (the lock
  knobs edit it), it is just not marked.

  v1.58.0 (2026-10-07): SCREEN REDISTRIBUTION (the user's request).
  - New, third tab SETUP (the last one). Left "PROJECT": CHOOSE TARGET TRACK, FETCH NOTES, FETCH CC VALUES, TRACK SETUP
    (this is the `cc_edit` parameter), project name (`scr:MAIN`) or the drum track list (`scr:SOURCE`, again 6 rows,
    PREV/NEXT, DISCARD/COMMIT), and the message field (`msg`, same as on PROGRAM). Right "TRACK SETUP", visible while
    TRACK SETUP is on (`when=cc_edit:ON`): T1–T8, MIDI CH, NOTE, READ PROJECT/DISCARD/COMMIT, CC 1–5, IN USE, BASE 1–5.
  - INITIALIZE removed from the PROGRAM tab (`read_all` stays as a parameter, does nothing); the section is always available.
  - TRACK EDIT (PROGRAM) no longer has EDIT CC, MIDI CH and NOTE. The CC editor no longer depends on TRACK EDIT; changing
    the track while it is open does not close it, instead the draft starts over from the new track. COMMIT/DISCARD still
    close it.
  - PROGRAM with no mode: the right panel is PATTERN SETUP (panel = PATTERN) instead of STEP LOCKS: SHUFFLE knob, SHUFFLE EDIT
    (`shuf_edit`, new parameter, 237 total; shows the row T1–T8 + ALL `when=shuf_edit:ON`; applies only with no mode,
    any mode turns it off), label PATTERN LENGTH + 16/32/48/64. The explanation about page repetition removed.
    STEP LOCKS are visible only in STEP EDIT and EUCLIDIAN. PATTERN EDIT button and mode removed (`pattern_edit` always 0);
    mode buttons moved to the right. ptitle 6/7/8 and panel 1/6 are no longer used; `scr` now means the list on the SETUP tab.
  - `xpj_pick_current()` (CurrentProject only, no disk search) → the project name is written when the plugin loads.
  NOT verified on the device: the same parameter on two tabs / two widgets (trk1–8, shuffle, shuf1–8, msg).

  v1.59.0 (2026-10-07): PROGRAM tab polish.
  - PATTERN LENGTH label + 16/32/48/64 at the bottom right (cx 908..1196, cy=449; label cx=866 cy=420) both in PATTERN SETUP
    and in TRACK EDIT (there the label was "LENGTH"; LENGTH/VELOCITY/GATE knobs raised to cy=315).
  - The `eu_len` parameter is now called "Length" (was "Euclid Length"). The MPC caches parameter names – a rescan may be needed.
  - Message field shortened to the right edge of the RANDOM / CLEAR frame (36..500). UNDO above REDO, right of T8 (cx=682,
    cy 504/551). SHIFT (STEP EDIT only) wider (120) with a grey frame, cx=805. COPY/PASTE in the old place.
  - GLOBAL: removed the SHUFFLE knob and the track row (they remain in PATTERN SETUP); MIDI CH (ALL) stays.
  - CLR STEPS (formerly CLR TRACK, `clr_trk`, same function) and CLR LOCKS swapped places. CLR LOCKS is now a TOGGLE
    (`clr_mode`, new; the old trig `clr_step` is no longer on screen): while it is on, the RND buttons are red and read CLR VEL /
    LEN / PAN / CC 1–5 and clear exactly that lock – on the whole track, and in STEP EDIT only on the selected steps; RND STEPS is
    grey and does nothing. The look of the RND buttons is now driven by the new read-only `lbtn` (ORANGE/TEAL/VIOLET/CLR/NONE)
    instead of ptitle (4 copies per button). 239 parameters.

  v1.59.1 (2026-10-07): SHIFT is above T8, right edges aligned (cx=565, w=120 → right edge 625). The PATTERN
  LENGTH label is right-aligned (cx=1238, align=right), above the 64 button, on both panels.

  v1.59.2 (2026-10-07): the user: CHOOSE TARGET TRACK shows tracks from the project opened EARLIER, not the active one.
  So the MPC does not refresh `CurrentProject` on every project open (exactly when – still unknown; it could not be
  checked, remote was not available). `xpj_pick()` now picks the NEWER by modification time between the project from
  `CurrentProject` and the newest .xpj on the disks. Rule for the user: save the project before reading – that makes it
  the newest. UNDO/REDO: left edge in line with step 9 (cx=694).

  v1.60.0 (2026-10-08): COPY MAPPING and a new backup copy (the user's request; no sample copying).
  - COPY MAPPING (`src_copy`, new, 240 params): browser (`br_fill`/`xpj_dir`: "..", folders "ime/", projects; not above
    /media; folders `[ProjectData]` and lost+found are not shown) from the open project's folder → project → its
    track with mappings ("Drum 001 (2)") → a track of the open project of the SAME type (program type, for a plugin also the
    same uid) → folder for the backup copy → COMMIT. `xpj_copy_text` copies every MIDI learn entry of that track as is, only
    changing the track name (targetData.track and the start of control.name); it skips a CC already used on that channel and a
    parameter that already has a mapping. controlValue stays from the source project (unknown whether the MPC sets the
    parameter to it on open).
  - CHOOSE TARGET TRACK (our 5 parameters) goes through the same final step (folder for the backup copy).
  - Backup copy: instead of `.xpj.bak` next to the project, now `<ime>-backup.xpj` (then -backup1, -backup2 …) in the folder
    the user chooses (`xpj_backup`, byte for byte). Such a copy shows up as a project in the MPC, but does not find
    the samples under its own name – it is for restoring under the old name. Then the write into the open project; message
    "Close & reopen project".
  - The list has 5 rows (SRC_ROWS), the row above it (`srcproj`) always visible: the project name or what is being chosen
    ("Project in:", "Copy from which track?", "Map which track?", "Backup to:"). New messages 204 "No mappings in project",
    205 "No track of that kind". `xpj_scan_text`, `xpj_controls` extracted; `XTrack` has type and uid.
  LESSON: a script that opens layout.conf for writing and then crashes leaves an EMPTY file – always build the text first,
  and only then open the file (restored from the last zip).

  v1.61.0 (2026-10-08): backup copy step: the browser now also shows projects. Touching a project marks it and the row above
  the list shows the name of the copy that will be created in that folder (`xpj_backup_name`: "<ime>-backup" / -backupN, end
  of the name if longer than 23 characters). COMMIT: copy of the MARKED project (if none is marked – the open one) into the
  current folder, then the mappings are written into the open project. New SKIP BACKUP (`src_skip`, visible only in that step
  via the read-only `bkstep`): write without a backup copy. 242 parameters. `sm0`: one press = one step (the COMMIT that
  opens the backup step must not also write immediately).
  v1.62.0 (2026-10-08): check that the file really is the open project. Problem: a new, unsaved project has no file, so
  "newest .xpj" and CurrentProject pointed to a DIFFERENT project (the last saved one) – mapping/backup would go to the wrong
  project. Now: each instance has a random id (line `I <hex>` in the chunk) and remembers hashes (FNV-1a, up to 64) of all
  chunks it gave to the MPC (effGetChunk – the MPC requests it on save) or received from it (effSetChunk – project open). A
  candidate file (newest, then CurrentProject, then the one that passed the check last time) is valid only if one of its
  plugin tracks with the Trig Seq uid (0x54725371) carries one of those chunks in its "state" (JUCE base64 VST2 bank, chunk
  from byte 160). Otherwise nothing is read or written: message 206 "Save the project first" (63 remains for when there are
  no projects at all); SETUP shows "Project not saved" instead of the name. The name in SETUP is also checked on project open
  (effSetChunk). TRIGSEQ_NOVERIFY disables the check (tests with foreign fixtures); verify_tests + test_mkxpj.py create a
  project with the instance's chunk. msg max 206.
  v1.63.0 (2026-10-08): the default track notes are 36–43, i.e. pads A01–A08 of an MPC drum program (the MPC default
  `noteForPad`), so that they match CHOOSE TARGET TRACK (track N → pad A0N) even without FETCH NOTES. Previously: 37 36 42 82
  40 38 46 44 (GM). Projects saved earlier keep their notes (in the chunk). Tests switched to the new notes; the test "FETCH
  CC VALUES does not change the note" now first sets T8 to 50 (43 is now both the default and the note of pad A08).
  v1.64.0 (2026-10-08): simplified mapping, SETUP tab deleted. GLOBAL is called SETUP (button and title) and contains
  MIDI CH (ALL), CREATE DEFAULT MAP (`src_map`, orange), TRACK SETUP (`cc_edit`), FETCH NOTES, FETCH CC VALUES and the
  PROJECT row (name of the verified open project or "Project not saved"). `panel` in SETUP: GLOBAL = buttons, CCEDIT = TRACK
  SETUP (MIDI CH / NOTE on the left, CC 1–5 and BASE 1–5, READ PROJECT / DISCARD / COMMIT in the title row; track = T1–T8
  at the bottom), SOURCE = drum track list ("SETUP / CREATE DEFAULT MAP", PREV / NEXT, CANCEL). Any other mode button closes
  TRACK SETUP and the list. CREATE DEFAULT MAP: project not verified (xpj_pick_ours) → big message PROJECT NOT SAVED;
  otherwise the list; a TOUCH on a track immediately: copy `<ime>-backup.xpj` (then -backup1, -backup2…, an existing one is
  not overwritten) in the project folder, then the mappings are written into the project itself. COPY MAPPING, the backup
  browser and SKIP BACKUP removed (the parameters stay, append-only; `xpj_copy_text`/`br_fill` remain in the code, unused).
  New BIG MESSAGE over the whole PROGRAM tab: read-only `bigmsg` (NONE, NOTSAVED, NODRUM, NOTHING, FAIL, DONE1..DONE40),
  for each an image `images/big_<ime>.png` 1280×628 (dimmed screen, box with a coloured strip; palette PNG ~17 KB) as the
  `msg_ok` button — a touch anywhere closes it. DONEn: "n VALUES MAPPED", "Please close the project and open it again.",
  "Do not save it first, or the map is lost.", and the readout `bkname` (backup name, `bigdone` ON). The small message (msg)
  stays for FETCH / READ PROJECT. Q-Links for the TRACK SETUP knobs (were on the SETUP tab) no longer exist. 246 parameters.

  v1.65.0 (2026-10-08): on the device: new project, Save As over the existing Test.xpj → mapping and backup went to
  untitled.xpj / untitled-backup.xpj (same folder, same minute, same Trig Seq chunk in both; the check from v1.62 only
  distinguishes "this" project from "someone else's", not two copies of the same one). Now `xpj_ours_all`: candidates
  (CurrentProject, newest, previous hit) + all .xpj in their folders saved since the instance was created (`born`,
  mtime ≥ born − 2 s), excluding our `*-backup[N].xpj`; only those with our chunk remain, in order: the one CurrentProject
  names, then the newest. CREATE DEFAULT MAP: one → track list; two or more → new step SM_WHICH "Which project is open?"
  (list of names), then the track list. The track list now shows "Map into: <name>" (`map_path`, the write goes exactly
  there and the chunk is checked once more). FETCH / READ PROJECT read the first one in the list. TRIGSEQ_NOVERIFY: old
  behaviour (no folder search).
  v1.66.0 (2026-10-08): SETUP panel without the PROJECT row (text and readout removed; the name remains only as "Map
  into: <name>" above the track list). Layout: MIDI CH (ALL) knob top left, TRACK SETUP below it; on the right, stacked
  CREATE DEFAULT MAP, FETCH NOTES, FETCH CC VALUES. All four buttons grey. CREATE DEFAULT MAP lists (project choice
  SM_WHICH and track list SM_FIXED): a tap only selects a row, the write/step is done by the new COMMIT button
  (`src_commit`, right of CANCEL); COMMIT with no row selected does nothing. Initially only the track mapped last time is
  selected (if any).
  v1.67.0 (2026-10-08): on the device: tapping SETUP briefly stalls the audio. Cause: since v1.64 entering SETUP looked
  for the open project (xpj_pick_ours: search of /media up to 20000 folders + unpacking .xpj) in setParameter, UNDER
  `w->lock`, and processReplacing waits on the same lock → the audio thread stalls while the disks are searched. CREATE
  DEFAULT MAP, COMMIT, FETCH, READ PROJECT, no_proj_msg, and src_name on chunk load (effSetChunk, under the lock) did the
  same (for shorter). Now: entering SETUP reads nothing; src_name no longer reads files; all work with project files is a
  "job" (J_CCREAD, J_FETCH, J_NOTES, J_MAPOPEN, J_MAPPICK, J_MAPWRITE): under the lock a button only records the request
  (queue_job: copy of `given`/`our_path`/`born` as `Who`, for writes also a copy of the Seq), setParameter starts it after
  releasing the lock (start_job) — on the device on a separate thread (`job_thr`, join in effClose), in the host test
  immediately; run_job does I/O without the lock and takes the lock only to write the result. One job at a time (a press
  while one is running is ignored). "Map into:" now with ".xpj" (long name: first 8 characters + "~.xpj").
  v1.68.0 (2026-10-08): STATE buttons store the whole sequencer state and there are 8 of them (new parameters
  `pst5`..`pst8` at the end, P_PST5; `pst_slot` maps both ranges to 0..7; P_OWN_COUNT = P_PST5_END). State = Snap as in the
  undo history (all tracks: steps, locks, track settings, Euclid, random, CC numbers/base; shuffle) + PERFORM ALL (on or
  off, 9 offsets) + CC 117. Mutes stay live (not part of the state). Empty: tap stores (also when PERFORM ALL is off).
  Full: tap restores — restore_from(keep_mute) like undo (position keeps playing), PERFORM ALL is restored as it was (on:
  perform_begin then the offsets, then UNDO/turning off returns to the restored state); restoring is an undo history
  step. CLEAR as before. Chunk: per state the lines "Q<k> G shuffle mask", "Q<k> P on 9 offsets cc117" and "Q<k> " + the
  same R/t/C/B/s/e lines as the project (`write_tracks`, reading `track_line` — split out of chunk_write/chunk_read). Old
  "P" lines (states of PERFORM knobs only, v1.36–1.67) are ignored. PERFORM tab: STATE 1–8 full width (140 wide, cy 116),
  CLEAR below STATE 8.
  v1.69.0 (2026-10-08): PATTERN 1–8 on the main screen (PATTERN SETUP): parameters `pat1`..`pat8` + `pat_clear` (end of
  the list, P_PAT0, P_PAT_CLEAR). The buttons look like step cells (68×84, cell() + number; empty dark, full orange).
  Pattern = Snap (all tracks, as in the undo history) + shuffle; without PERFORM ALL and CC 117 (that is STATE). Empty: tap
  stores; full: tap restores (restore_from keep_mute, position keeps playing; if PERFORM ALL is on, pf_bak = the restored
  pattern, offsets stay); undo history step. CLEAR (132×39, like SHUFFLE EDIT; red when on): while lit, a tap empties the
  pattern. Chunk: "Z<k> G shuffle mask" + "Z<k> " R/t/C/B/s/e lines. PATTERN panel layout: PATTERN LENGTH (text and
  16/32/48/64) moved to the top right (row with SHUFFLE, cy 224), the SHUFFLE EDIT track row stays at cy 345, patterns at
  the bottom (cy 422, cx 568 + 72k), CLEAR to their right (cx 1180).
  v1.70.0 (2026-10-08): PATTERN changes only at the end of the playing pattern. Tap on a full pattern while the transport
  is playing: `pat_cue` = k, message "Pattern N Cued" (stays until it comes in); tapping the same one again = cancel (the
  cue disappears, the message is cleared if it is still that one). Tapping another full pattern moves the cue.
  run_transport: end of pattern = next multiple of `pat_beats` (longest track: steps × step duration) from the absolute
  song position; the block is split — up to that point the old pattern (seq.process(from, at)), then pat_load, then the
  rest of the block with the new one. Stop while cued: comes in immediately. Transport stopped: a tap loads immediately.
  Messages (msg 207..238): "Stored as Pattern N", "Pattern N Cleared", "Pattern N Cued", "Pattern N Loaded" (MSG_PAT*).
  CLEAR on a cued pattern also cancels the cue. Polymetric tracks (length that does not divide the longest) do not start
  from step 1 on a change (the position continues).
  v1.71.0 (2026-10-08): CREATE DEFAULT MAP maps CC 1–5 to pad parameters (per Marko's list): CC1 = 513 Tuning,
  CC2 = 514 Filter Cutoff, CC3 = 515 Filter Resonance, CC4 = 91 Send 1, CC5 = 92 Send 2 (previously 514, 515, 522, 523, 524).
  controlValue: cutoff / resonance as before, Send 1/2 = mixable.sends[0]/[1], Tuning = (coarseTune + fineTune/100 + 36) / 72
  (ASSUMPTION: range ±36 semitones, 0 = 0.5; not verified how the MPC itself writes Tuning). Track list text updated.
  v1.72.0 (2026-10-08):
  - PATTERN STORE (`pat_store`, toggle, above CLEAR; green when lit; CLEAR and STORE are mutually exclusive). STORE
    on: a tap on ANY pattern stores what is playing (also over a full one). Off: tap on the playing pattern = nothing;
    on another (full or EMPTY) = cue, comes in at the end of the pattern (stopped: immediately); tapping the cue again =
    cancel. Empty pattern on load: tracks with their settings, no steps and no Euclid (the slot stays empty until it is
    stored).
  - STEP EDIT knobs: CONDITION, VELOCITY, PAN, LENGTH, PROB (and the Q-Links in that order).
  - PERFORM ALL: CC 117 removed (knob, sending, ccuse reservation, states; the parameter remains, does nothing). Top row:
    VELOCITY, PAN, LENGTH, PROB. PAN works again (`pf_pan`, -64..+63): trigs without a pan lock send CC 10 = 64 + offset
    (seqcore `pan_dirty`: when the offset returns to 0, 64 is sent once); the lock stays untouched; COMMIT writes the
    heard pan as a lock on trigs without a lock.
  v1.73.0 (2026-10-08): TEMPORARY DIAGNOSTICS — whether the plugin can see the hardware SHIFT. The first instance opens a
  second ALSA client "TrigSeq Diag" (input only, separate thread), logs all MIDI clients/ports, listens on all readable
  ports except its own (reading does not take the port away from the MPC) and writes events to /tmp/trigseq.log ("[diag]",
  at most 4000 lines; clock and active sensing are not logged). The dispatcher logs each opcode the first time (59/60 =
  key, always), effProcessEvents logs the MIDI the MPC gives the plugin, effCanDo logs the queries and for this version
  answers YES to receiveVstEvents/receiveVstMidiEvent. Nothing else changes. Remove after analysing the log (diag_run,
  DIAG, dispatcher block).
  v1.74.0 (2026-10-08): diagnostics from v1.73 removed (trigseq.cpp reverted to its state before them). Result from the
  device (MPC Live, 2026-10-08): the control surface = ALSA client 16 "MPC Live Controller", ports 0 "MPC Public", 1 "MPC
  Private", 2/3 "MPC MIDI Port A/B"; the hardware buttons (Shift, Tap, +, −) go through "MPC Private", and subscribing to
  it returns EAGAIN ("Resource temporarily unavailable": the MPC keeps it for itself). Nothing arrives on Shift from the
  other ports; the dispatcher never gets 59/60 (key). So the plugin does not see the hardware Shift; SHIFT stays on
  screen. Track pads reach the plugin as MIDI (effProcessEvents, note + aftertouch) — the user did not want a pad as
  SHIFT. Other clients: 128 "JUCE" (virtual MIDI), 129 "MPC".
  v1.75.0 (2026-10-08): the sequencer is ALWAYS one of the 8 patterns — from the start pattern 1 (`pat_cur` = 0; the button
  is lit even when the slot is still "empty"). All edits go into the playing pattern: when switching to another (pat_load)
  the current one is written into its slot (`pat_keep`), and only then is the new one loaded; an empty slot that comes in
  becomes the pattern. The undo history is cleared on pattern change (undo works within a pattern). STORE: copies what is
  playing into the tapped slot (the same pattern keeps playing). CLEAR: another slot is emptied; the current one loses
  its steps (stays active). Tapping the playing pattern still does nothing. Chunk: main lines = the playing pattern, new
  line "W <k>" (which one it is), "Z<k>" only for the others. Old projects: pattern 1 = their tracks.
  v1.76.0 (2026-10-09): track direction: FWD / BWD (parameters `trk_fwd`, `trk_bwd`, end of the list; Track::dir, DIR_FWD 1,
  DIR_BWD 2, both = 3 back and forth). Seq::dir_step: FWD 1..len, BWD len..1, both 1..len..2 (ends once, a cycle is
  2×len−2 steps; conditions 1:2… count cycles). Also applies to Euclid (same length eff_len). At least one must be lit
  (turning off the only one does nothing). Buttons: TRACK SETUP panel (label DIRECTION, purple when lit) and EUCLIDIAN
  section on the left (row above the knobs, green). End of pattern for cue counts the full back-and-forth cycle of a track
  (round_len). Chunk: "D <i> <dir>" after the "e" line only when not FWD (also in states/patterns); the "t" line resets to
  FWD.
  v1.77.0 (2026-10-09): TIMING mode (button left of EUCLIDIAN, `timing_edit`): internally STEP EDIT (step selection, SHIFT,
  COPY/PASTE STEP, STEP LOCKS on the right), ptitle TIMING (9), lpanel TIMING (3), TIMING section on the left: ◀ / ▶
  (`tm_left`, `tm_right`: selected steps 1/24 step earlier/later, ±23, 0 = on grid), RATCHET knob (`st_ratch` 1..8), RND
  TIMING / RND RATCHETS (selected steps; −23..+23, 1..8). Message "Timing: +5" (msg 239..285; no message on the grid). Step
  got `micro` (int8) and `ratch` (0..7 = 1..8 hits). seqcore: fire_time = grid position (with shuffle) + micro × step/24
  (NO LOCKS and RANDOM STEP: on the grid); step_of split out of fire. Ratchet: hits evenly spaced within the step, each as
  long as the gate but at most its share of the step, same velocity; note off between hits; a new step cuts off the rest.
  CLR LOCKS also clears timing and ratchet (up to v1.77; from v1.78 not). Chunk: "M <i> <64 × 2 bytes hex>" only when the
  track has timing/ratchet (also in states/patterns).
  v1.78.0 (2026-10-09): CLR LOCKS (clear_locks) no longer clears TIMING or RATCHET. In the TIMING section, below RND TIMING /
  RND RATCHETS, grey CLR TIMING / CLR RATCHETS (`clr_timing`, `clr_ratch`; selected steps: to grid / 1 hit). PERFORM:
  the active STATE (last stored or restored; read-only `pst_act` NONE/S1..S8) framed with a white frame (frame
  images/pst_frame.png 148×40 behind the button, when=pst_act:Sk); CLEAR of the active one → no frame; project load → none.
  v1.79.0 (2026-10-09): PERFORM, second row of pads: pfy1 = LOOP 8 (set_loop(8), like LOOP 4/2/1, mutually
  exclusive), pfy2 = REVERSE (Seq::reverse: dir_step reverses the direction of every track — FWD↔BWD, back-and-forth starts
  from the other end; mirrored position, the song position runs underneath as with LOOP, so turning it off returns the
  tracks to where they would have been). Not stored (like the other perform pads). pfy3..8 still free.
  v1.80.0 (2026-10-09): PERFORM second row: pfy3 HALF TIME, pfy4 DOUBLE TIME (mutually exclusive; Seq::warp / virt(): tracks
  run on their own clock = w_v0 + (song − w_t0) × warp; process works in that time, set_speed and align after load
  convert the position; turning off: warp 1 and every track aligned to the song position → where it would have been), pfy5
  ROLL (every trig at least 4 hits like RATCHET 4), pfy6 RESTART (tap, does not stay lit: every track plays step 1 next,
  with REVERSE the end), pfy7 SCATTER (every trig randomly ±8/24 step on top of its TIMING, drawn once per step: sc_k/sc_v).
  REVERSE now goes backwards starting exactly from the step the track is on (and back the same way): the track path has a
  phase `ph` and a direction `backw` (path_pos / path_step; LOOP is applied first), set_reverse sets them so the next step
  is the adjacent one; start with REVERSE on: mirrored. RESTART and REVERSE shift the phase, so the end of pattern for cue
  no longer falls exactly on step 1 of those tracks. pfy8 free.
  v1.81.0 (2026-10-09): PATTERN SETUP: FWD / BWD for the whole pattern (`pat_fwd`, `pat_bwd`; label DIRECTION below PATTERN
  LENGTH, orange when lit). Lit while all tracks have it (tracks differ: not lit). Turning on adds the direction to all
  tracks, turning off removes it; a track that would be left without a direction gets the other one (FWD off → backwards).
  v1.82.0 (2026-10-09):
  - PATTERN buttons: look per state via read-only `patv1..8` (HIDDEN outside the PATTERN panel, EMPTY, FULL, PLAYING, CUED);
    a separate button line for each state (when=patvK:X) with image images/patcK_S.png: empty dark, full orange,
    PLAYING like a step under the playhead (light + white bar), CUED grey with a bar. (picture does not accept when=, hence
    buttons.)
  - An empty pattern that comes in: length 16 and FWD on all tracks (fresh_tracks).
  - Direction: FWD / BWD / BOTH are one choice of three (new `trk_both`, `pat_both`; tapping the lit one changes nothing);
    also in EUCLIDIAN. Buttons narrower (78 in TRACK EDIT / PATTERN, 100 in EUCLIDIAN).
  - TRACK EDIT: title "TRACK EDIT"; PATTERN LENGTH at the top as on the pattern screen (cy 224), DIRECTION below, knobs
    LENGTH/VELOCITY/GATE cy 270 (587/700/813), SPEED all the way at the bottom (cy 445).
  - RESTART (pfy6) momentary (trig; fires in the momentary block).
  - TIMING: arrows grey, 100 wide, below "SHIFT LEFT" / "SHIFT RIGHT".
  - PATTERN EDIT button (the old `pattern_edit`) between EUCLIDIAN and STEP EDIT; lit when no other mode is on (main
    screen); cannot be turned off (some top-row button is always lit); turning off another mode returns to it. Mode
    order: TIMING 257, EUCLIDIAN 409, PATTERN EDIT 561, STEP 713, TRACK 865, RANDOM 1017, SETUP 1169.

### v1.83.0 (2026-10-09)
- EUCLID: FWD/BWD/BOTH (new parameters eu_fwd/eu_bwd/eu_both, appearance eu_dirv HIDDEN/OFF/ON) are active only when
  Euclid ENABLE is on; otherwise they are dimmed and touches are ignored.
- Pattern buttons: the playing pattern is in the standard (orange) color with a line at the BOTTOM; a non-empty pattern
  is dim without a line; CUED is dim with a grey line at the bottom; an empty one stays as before.
- RANDOM EDIT removed from the top row (row rearranged). In the SETUP panel, below TRACK SETUP, the RANDOM SETUP button
  opens the RANDOM panel; while RANDOM is open, SETUP stays lit, and touching SETUP returns to the SETUP panel.

### v1.84.0 (2026-10-09)
- The TIMING button is visible only in STEP EDIT (and TIMING) mode, in place of the EUCLIDIAN button (cx 561); in other
  modes EUCLIDIAN is there. The appearance is selected by the read-only parameter slotv (EUCLID / TIMING). Old slot at
  cx 409 removed.
- Turning TIMING off returns to STEP EDIT (previously: PATTERN EDIT), because the button is part of STEP EDIT mode.

### v1.85.0 (2026-10-09)
- RANDOM SETTINGS: the name of the selected track (TRACK 1-8) on the right of the title, via the read-only parameter
  rtrk (HIDDEN / T1..T8).
- PATTERN EDIT: COPY / PASTE are now COPY PTN / PASTE PTN. COPY PTN stores the entire playing pattern (all tracks +
  shuffle); PASTE PTN writes it into the playing pattern (UNDO restores it). Messages "Pattern N Copied" / "Pasted to
  Pattern N" (msg 286-301, msg max 301). In RANDOM / EUCLID / SETUP mode COPY/PASTE still work on the page (COPY PAGE).

### v1.86.0 (2026-10-09)
- PATTERN EDIT: DIRECTION and FWD/BWD/BOTH hidden (commented out in layout.conf, prefix "# v1.86: hidden"); parameters
  pat_fwd/pat_bwd/pat_both remain and work, so they can be restored just by removing the comments.
- Send 1/2 mapping (CC4 → 91, CC5 → 92, paramType 0, instrumentIndex = pad): the user reports it does not work properly.
  Offline there is no sample that the MPC ITSELF wrote for a pad send, so it is not confirmed whether the same paramType
  and instrumentIndex apply to 91/92 as to 514/515, nor what mixable.sends looks like in the .xpj. The user's list also
  has 522/523 (pad Send 1/2). Waiting for a description of the symptoms and a project with Send 1 mapped manually (MIDI
  learn) to compare the record.

### v1.87.0 (2026-10-09)
- Send 1/2 fixed: CREATE DEFAULT MAP now maps CC4 → 522 (Send 1), CC5 → 523 (Send 2) instead of 91/92.
  Verified on the device (MPC Remote, MIDI learn, pad A08): rows with 91/92 had the correct name in the list, but the
  MPC showed the previous parameter (Filter Resonance) as TARGET on the left – it did not find 91/92. In MIDI learn the
  MPC has two TARGET groups: Track (513 Tuning, 514, 515, 516, envelope…) and Mixer (Level, Pan, Pad Mute, Pad Solo,
  Send 1-4), which matches 518, 517, 526, 527, 522-525 from the user's list. paramType stays 0 (assumption; the record
  the MPC itself writes for the Mixer group has not been checked – verify on the device whether TARGET now shows
  Send 1 / Send 2).
- Existing wrong rows (CC83/84 etc. on 91/92) are NOT corrected automatically: CREATE DEFAULT MAP skips a CC that is
  already mapped. They must be deleted in the MPC (DELETE or CLEAR ALL), saved, then CREATE DEFAULT MAP run again.

### v1.88.0 (2026-10-09) – TRIAL: factory presets
- Investigated on the device (SSH): every factory plugin in /usr/share/Akai/Content/Synths/<folder>/ has only
  version.xml, Plugin Skins/ and (106 of 114) Presets/ with files "NNN-Category-Name.xpl" (e.g. "001-Synth-Static
  Supersaw.xpl", "000-Init.xpl"). There is no thumbnail/artwork file in any folder: the circle with an arrow in Track
  view is the MPC's own icon.
- .xpl format (TubeSynth "000-Rich Swell.xpl", 1108 B): `<pluginstate><version file="1" date="..."/><PLUGIN .../>
  <state>VELIČINA.JUCE-base64</state></pluginstate>`; PLUGIN = the same row as in pluginList (name, format, file,
  uid...). For our VST2, state = a VST2 bank as in the .xpj (CcnK/FBCh, fxID TrSq, fxVersion = version, chunk from byte
  160, size big-endian at 156).
- New: mkpresets.cpp (x86, -DTRIGSEQ_TEST: opens the plugin, sets steps via parameters, effGetChunk →
  build/presets/*.chunk), gen_presets.py (chunk → .xpl, WITHOUT the 'I' line so the instance keeps its own id),
  gen_presets.sh (both steps → presets/*.xpl, in the repo). build.sh copies presets/*.xpl into "build/skin/djokenzi -
  VST - Trig Seq/Presets/"; release.py copies the whole skin folder, including Presets. Two presets: 000-Init (new
  plugin), 001-Demo-Four On The Floor (T1 1/5/9/13, T2 5/13, T3 eighths, T4 4/12). host_test: both .xpl files decode
  and Demo/Init load via effSetChunk.
- Verify on the device: whether the MPC shows these two in the PRESET field (Track view), whether they load, and whether
  an MPC restart is needed for it to see them. A preset changes the ENTIRE state (all tracks, patterns, channel/note
  settings).

### v1.89.0 (2026-10-09) – TRIAL 2: presets as VST2 programs
- Result of v1.88 on the device: Presets/*.xpl are installed, but for Trig Seq tapping PRESET in Track view only
  outlines the field (no list). TubeSynth (MPC Remote) shows a PRESET list with categories from the file names
  ("Synth", "Lead"…, "Rich Swell"/"Init" without category) – that works for format="MPC" plugins. Conclusion
  (assumption): for VST the MPC (JUCE) lists the plugin's programs, and Trig Seq had none (numPrograms 0).
- New: numPrograms = 1 + NFACTORY; program 0 = "--" (does nothing: JUCE selects program 0 on open, so it must not
  overwrite the state restored from the project), 1.. = factory presets from factory_presets.h (generated by
  gen_presets.sh from mkpresets.cpp, same state as the .xpl, without the 'I' line). effSetProgram loads the chunk
  (chunk_read), effGetProgram, effGetProgramName, effGetProgramNameIndexed (name ≤ 23 chars: "Init", "Four On The
  Floor"). The .xpl files remain.
- Verify on the device: whether PRESET now opens a list, whether selecting loads the state, and whether reopening the
  project still restores the saved state (not Init).
- CONFIRMED on the device: the presets (VST2 programs) appear in the PRESET list. The user can already save presets
  themselves (the MPC writes them to the user folder). Plan for sharing the plugin: the user saves several presets on
  the MPC, transfers them to the computer and provides them to be built into factory_presets.h (the list that ships with
  the installation). The tool for this (v1.90, from .xpj) was withdrawn on request; when the files arrive, convert them
  (state from .xpl, without the 'I' line) into chunks for gen_presets.

### v1.90.0 (2026-10-09)
- PERFORM tab, new pad layout (layout.conf only, parameters unchanged). Bottom row (cy 526): LOOP 8 (pfy1), LOOP 4/2/1
  (pfx1-3), ROLL (pfy5), RANDOM STEP (pfx5), INVERT MUTE (pfx6), MUTE ALL (pfx7). Top row (cy 450): NO LOCKS (pfx4,
  not mentioned by the user – placed on the left), four empty slots, REVERSE (pfy2), RESTART (pfy6), FILL (pfx8).
- HALF TIME (pfy3), DOUBLE TIME (pfy4) and SCATTER (pfy7) have no pad (code remains, parameters remain); pfy8 is free.
  For the four empty slots the user asks for suggestions of new effects.

### v1.91.0 (2026-10-09)
- PERFORM top row: HUMANIZE, BUILD, ECHO, FADE OUT, NO LOCKS, REVERSE, RESTART, FILL. New parameters (at the end):
  pf_hum, pf_build, pf_echo, pf_fade (onoff, not saved). seqcore:
  - HUMANIZE: each trig randomly up to ±HUMAN_T (4) 24ths of a step off the grid (like SCATTER, drawn once per step)
    and velocity ±HUMAN_V (16). Also works with NO LOCKS / RANDOM STEP.
  - BUILD: from activation (build_t0, track clock) each trig gets one more hit every BUILD_BEAT (1 beat), up to
    RATCH_MAX (8), like RATCHET; velocity rises from 60 % to full over the same time. With ROLL/RATCHET the larger hit
    count applies.
  - ECHO: each trig repeats ECHO_N (3) times at ECHO_DT (1/8 = 0.5 beat), each at 60 % of the previous; repeats in
    queue eq[] (note on/off, track clock), sent at the start of process() (off first then on, before track notes);
    all_off/start empties it (sends the remaining note offs).
  - FADE OUT: from activation (fade_t0) velocity falls linearly to zero over FADE_BEATS (4 beats), then silence (CC
    locks still go out). Transport start resets build_t0/fade_t0.
- Tests: ECHO 4 notes with decreasing velocity, FADE OUT 4 notes then silence, BUILD 1+2+3 hits, HUMANIZE off the grid
  but close.

### v1.92.0 (2026-10-09)
- Pattern buttons blink: patv got the option BLINK (5; images patcK_4 = dim with an orange line at the bottom). While
  the transport runs, the playing pattern is PLAYING in the first half of each beat, BLINK in the second (w->song); a
  cued pattern the opposite (CUED / PLAYING), so it blinks right after cueing. Transport stopped: the active one stays
  PLAYING.
- Touching an already lit PATTERN EDIT no longer turns the button off: general rule in setParameter – when the plugin
  does not accept an option choice (value after set_value ≠ value sent), told = -1, so the next block returns the real
  value to the host (also applies to lit FWD/BWD/BOTH etc.).
- Top row: EUCLIDIAN/TIMING at cx 409 (one width to the left), cx 561 empty; STEP EDIT 713, TRACK EDIT 865,
  PATTERN EDIT 1017, SETUP 1169.

### v1.93.0 (2026-10-09)
- Pattern button blinking twice as slow: one beat PLAYING, one beat BLINK (fmod(song, 2) < 1).
- PAN fix: until now pan went to CC 10, which the MPC interprets as pan of the WHOLE track (all pads at once) – that is
  why RND PAN changed pan on all tracks. Now each track sends pan on its own CC (seqcore PAN_CC = {30, 31, 90, 117, 118,
  119, 3, 9}, all MIDI "undefined"), and CREATE DEFAULT MAP adds per track a mapping of that CC to pad Pan (517, Mixer
  group; controlValue = pad mixable.pan). Mappings: 6 per track, 48 total (bigmsg DONE41..48 added, BIG_MAXN 48).
  Existing projects: running CREATE DEFAULT MAP again adds only the 8 pan mappings (the others are already there).
- Track.panb (pad pan + 1, 0 = unknown → 64), chunk line "A <track> <panb>", saved with the track (like ccb). FETCH CC
  VALUES also reads pad pan (mapping PAN_CC → controlValue), not included in the count in the message. Pan lock: after
  the lock it returns to panb if known; PERFORM pan = panb (or 64) + offset. EDIT CC marks PAN_CC numbers as taken.
- PATTERN EDIT: RND buttons (RND TRIGS, VEL, LEN, PAN, CC 1-5) work on all 8 tracks, each with its own random settings.
  CLR buttons still affect only the selected track.

### v1.94.0 (2026-10-09)
- PERFORM top row: GHOST, BUILD, ECHO, FADE OUT, NO LOCKS, POLY, DOWNBEAT ONLY, FILL. HUMANIZE, REVERSE and RESTART
  have no pad (code remains). New parameters: pf_ghost, pf_poly, pf_down (onoff, not saved). seqcore:
  - GHOST: an empty step (no trig) on an unmuted track plays a quiet hit, GHOST_PCT (25) % of track velocity, half a step
    long.
  - POLY: track n plays only the first 16 - n steps (T1 16 … T8 9, never longer than its own length), forward, from the
    step it was on (set_poly stores pl_k0/pl_s0); step_of takes it into account (also TIMING); off: back to where it
    would have been. Transport start with POLY on: from step 1.
  - DOWNBEAT ONLY: only trigs on steps 1, 5, 9, 13 … play (step % 4 == 0).

### v1.95.0 (2026-10-09)
- PERFORM layout: top row (empty), ECHO, GHOST, FADE OUT, ROLL, (empty), NO LOCKS, FILL; bottom row LOOP 8, LOOP 4,
  LOOP 2, LOOP 1, BUILD, RANDOM STEP, INVERT MUTE, MUTE ALL. Without a pad (code remains): HALF/DOUBLE TIME, SCATTER,
  HUMANIZE, REVERSE, RESTART, POLY, DOWNBEAT ONLY.
- Main screen = last selected mode: the g line in the chunk got a 7th number (mode_code: 0 PATTERN EDIT, 1 STEP EDIT,
  2 TRACK EDIT, 3 EUCLIDIAN, 4 TIMING, 5 SETUP, 6 RANDOM SETUP), mode_set restores it on load; old projects without
  it: STEP EDIT or PATTERN EDIT as before. Loading a preset (effSetProgram) does not change the screen.

### v1.96.0 (2026-10-09)
- PERFORM: ROLL and RANDOM STEP swapped places (RANDOM STEP top at cx 716, ROLL bottom at cx 868). layout.conf only.

### v1.97.0 (2026-10-10)
- skin_edit.sh hide|show|status: for work in Skin Studio, temporarily comments out (prefix "#EDIT-HIDDEN# ") the
  full-screen st_cond__open catcher and the RND/CLR buttons of the left section (when=lbtn:ORANGE/TEAL/VIOLET/CLR),
  37 lines; show restores them exactly as they were. build.sh refuses to build while any line is hidden. Plugin and skin
  unchanged.

### v1.98.0 (2026-10-10)
- layout.conf: the user's new layout from Skin Studio (both tabs; left-section titles per mode "RANDOM PATTERN / CLEAR
  PATTERN" etc., PATTERN EDIT title, top row of modes, COPY/PASTE/UNDO/REDO, PERFORM panel and pads moved).
  Added on top of that: EUCLID row FWD/BWD/BOTH (132x39, and dimmed OFF variants in the same place) and the
  ENABLE/RANDOM/COMMIT row swapped places (direction cy 414, ENABLE row cy 211); STATE 2 frame x 186 → 185 (all x =
  cx - 74, y = cy - 34, 148x68).
- Images at the new size (gen_cell_imgs.py): STATE 1-8 140x60, CLEAR (perform) 140x48, pst_frame 148x68, dir_*_m/_me/_md
  132x39, TIMING arrows 80x33, pf_panel 1065x274. Check: all button/frame images match w/h in layout.conf.

### v1.99.0 (2026-10-10)
- A new plugin opens on TRACK EDIT (previously PATTERN EDIT); old projects without a saved mode: STEP EDIT if it was
  active, otherwise TRACK EDIT. Saved mode (v1.95) still applies.
- An empty pattern stays empty: pat_keep, when leaving a pattern, sets full = !snap_empty (no trig, no lock / condition
  / prob / timing / ratchet on any track, Euclid off). Such a pattern is shown as EMPTY and is not saved (Z line), and
  when selected again it comes up as new (16 steps, FWD). STORE still saves whatever is playing.
- Factory presets removed: deleted mkpresets.cpp, gen_presets.py, gen_presets.sh, presets/, factory_presets.h;
  build.sh deletes the old Presets folder from the skin. The plugin has one VST program "--" that does nothing (kept so
  that the PRESET field and the MPC's user presets work as before; verify on the device).

## Trig Seq v2.0.0 – v2.12.0

### v2.0.0 (2026-10-10) – after v1.99.0 (vst.json 20000; "1.100" would collide with 2.0.0)
- No VST programs (numPrograms 0; "--" from v1.99 removed, as well as the program opcodes).
- PATTERN EDIT: COPY / PASTE work on the page again (COPY PAGE / PASTE PAGE). Below PATTERN LENGTH, three new buttons
  (111x39, cy 270): COPY PTN (pat_copy), PASTE PTN (pat_paste, into the playing pattern), CLEAR PTN (pat_clrall: no steps
  on all 8 tracks, blank_tracks; message "Pattern N Cleared"). All three go into UNDO.
- SETUP: COPY / PASTE buttons removed (when=ptitle:GLOBAL lines).
- CREATE DEFAULT MAP no longer makes a project backup (only writes into the project); bkname readout removed, list text
  updated ("…SEND 1, 2, PAN.", "MARK A TRACK, THEN COMMIT.").
- Right panel title: new read-only parameter ttl (HIDDEN, SETUP, ST1..8, RT1..8): SETUP (orange) on the SETUP
  buttons, "SETUP TRACK N" in TRACK SETUP and "RANDOM TRACK N" in RANDOM SETUP, purple (a064e8, like TRACK EDIT).
  Old titles "RANDOM SETTINGS" and ": TRACK N" removed.

### v2.1.0 (2026-10-10)
- EUCLIDIAN changes only the left part of the screen: the right panel stays the last of PATTERN EDIT / STEP EDIT (and TIMING) / TRACK EDIT
  (Plugin::rmode, rmode_note after every mode change; SETUP and RANDOM SETUP do not change it; saved as the 8th number of the g line).
  Right titles (PATTERN EDIT, STEP LOCKS, TRACK EDIT) now follow the panel, not ptitle; green "STEP LOCKS" removed.
- RATCHET RESOLUTION (st_rres, knob in TIMING to the left of RATCHET): AUTO (old: hits evenly within the step) or 1/16,
  1/32, 1/48, 1/64, 1/80 = 1..5 hits per step; when there are more than fit, they continue over the following steps
  (the ratchet is no longer cut off at every new step, but only at the next one that plays). Step.rres, chunk line
  "Y <track> <64 hex>"; CLR RATCHETS also resets to AUTO. On load, the 't' line now clears the micro/ratch/rres tracks (previously
  they could remain from the previous state when there was no M line).

### v2.2.0 (2026-10-10)
- PERFORM top row: ALT BAR (cx 108), ECHO, GHOST, FADE OUT, RANDOM STEP, AUTO FILL (cx 868), NO LOCKS, FILL. New
  parameters pf_alt, pf_afill (onoff, not saved). seqcore:
  - ALT BAR: every other bar (4 beats, track bar) silent, counted from the bar in which it was switched on (that one plays); CC locks still go.
  - AUTO FILL: on empty steps among the last 4 steps of the track length, randomly (AFILL_PCT 50 %) an extra hit at
    AFILL_VEL 80 % of the track velocity, half a step long.
- TRACK EDIT: step cells in the PATTERN EDIT color (yellow/orange), no longer purple (offset 16 is not used).

### v2.3.0 (2026-10-10)
- TRACK EDIT: knob GATE (trk_gate) is called LENGTH (label in layout.conf only; on the left, LENGTH = track length as before).
- PERFORM top row: ALT BAR, ECHO, GHOST, FADE OUT, RANDOM STEP, NO LOCKS (cx 868), AUTO FILL (cx 1020), FILL.

### v2.3.1 (2026-10-10)
- TRACK EDIT: knob trk_gate reads GATE again (label change from v2.3.0 reverted).

### v2.4.0 (2026-10-10)
- TRACK EDIT: knob trk_gate reads GATE again (reverted from v2.3).
- STATE 1-8 also store the track mute states: on save, the mute is written into the snap (PState::mutes = true), on recall
  restore_from(..., keep_mute = false) restores them too. In the chunk, "Q<k> U 1" means the mute values of the state's t lines are
  its own; states saved before v2.4 (without U) leave the mute as it currently is when recalled.

### v2.5.0 (2026-10-10)
- STATE 1-8 also remember the PERFORM tab effects: FX_PADS (LOOP 8/4/2/1, NO LOCKS, RANDOM STEP, FILL, REVERSE, HALF/DOUBLE TIME,
  ROLL, SCATTER, HUMANIZE, BUILD, ECHO, FADE OUT, GHOST, POLY, DOWNBEAT ONLY, ALT BAR, AUTO FILL; without INVERT MUTE and MUTE ALL,
  because the state stores the mutes themselves). On save fx_take (bitmask), on recall fx_put changes only the ones that differ
  (first turns off, then turns on). Chunk: "Q<k> E <maska>". States without an E line (before v2.5) do not touch the effects.

### v2.6.0 (2026-10-10)
- layout.conf: the user's new layout (RANDOM SETUP knobs cy 302, COPY/PASTE/CLEAR PTN cy 281, SETUP buttons
  MAP/NOTES/FETCH moved, TRACK SETUP knobs cy 238/383, PREV/NEXT lists, CREATE DEFAULT MAP text, TIMING arrows and
  RATCHETS/RESOLUTION knobs r=30, RND/CLR RATCHETS cx 385, SHIFT in TIMING cx 573, CLEAR STATE cy 200).
- PERFORM: first pad of the top row HUMANIZE (pf_hum) instead of ALT BAR (ALT BAR without a pad, the code stays; states still remember it).
- Labels: CLEAR (perform) → CLEAR STATE; first RND button in PATTERN mode (orange) → RND PTN.

### v2.7.0 (2026-10-10)
- Large mapping messages: DONE messages no longer have the "Backup saved as:" line (no backup is made since v2.0); text centered.
- PROJECT NOT SAVED: below "Save Project before creating a map." added "It is recommended to save this as a new project
  rather than overwrite your existing one." (on two lines). PROJECT NOT WRITTEN no longer mentions a backup.
- vst.json 20700, release.sh 2.7.0 (they had lagged behind at 2.3.1).

### v2.8.0 (2026-10-10)
- TRACK EDIT: step cells purple again (offset 16, images cell_16..23 exist), reverted from v2.2.0. The Euclid track is
  still green in every mode.

### v2.9.0 (2026-10-10)
- PATTERN EDIT: CLEAR PTN button removed from layout.conf (line commented out, parameter pat_clrall stays);
  COPY PTN and PASTE PTN moved to the right (cx 1063 / 1183, to the positions of PASTE PTN / CLEAR PTN).

### v2.10.0 (2026-10-10) – project cleanup, plugin and skin the same as in v2.9.0
- Deleted: `preview/` (old demo screenshots). `test_mkxpj.py` stays: the test uses it (creates a .xpj with a chunk).
- `images/`: 89 images that layout.conf no longer mentions deleted (505 → 416); `gen_cell_imgs.py` no longer generates them
  (old copy/paste, pat1..8_on/off, pfy2/3/4/6/7, pf_poly/pf_down/pf_alt, dir_*_off/t/e/p, mute_*, ccuse_*,
  src_panel, src_row_0, random_edit, read_all, eu/cc_discard, pf_undo, pcopy/ppaste, src_skip/src_copy…).
  Images of hidden buttons (pattern direction, CLEAR PTN) stay. Skin checked: byte-identical to v2.9.0.
- Tests and testdata stay (not part of the plugin). Parameters stay (append-only), including those without a button.
- PROJECT_NOTES.md: history up to v1.99.0 moved to `PROJECT_HISTORY.md`.

### v2.10.1 (2026-10-10)
- Build warning `xpj_backup defined but not used` removed: deleted `xpj_backup()` and `xpj_backup_name()` (no backup
  is made since v2.0) and the dead display of the SM_BACKUP line (that mode is never set; the enum value stays). Build
  now without warnings; behavior unchanged.

### v2.10.2 (2026-10-10)
- Removed all mentions of another sequencer manufacturer and its names (comments in trigseq.cpp, seqcore.hpp,
  gen_params.py, host_test.cpp; PROJECT_NOTES/HISTORY; description in release.sh). Code and skin unchanged.
- release.sh `--about`: also fixed an outdated description ("4 CC tracks" no longer exist since 2026-10-03): now "Trig sequencer: 8
  note tracks, 64 steps each, trig conditions, parameter locks and per-step CC locks."

### v2.10.3 (2026-10-10)
- PROJECT_NOTES.md and PROJECT_HISTORY.md translated from Serbian into English; from now on the notes are written in
  English. Code and skin unchanged.

### v2.11.0 (2026-10-10) – code review: dead code out, four fixes
- Removed dead code (no behaviour change): the old project-folder browser and COPY MAPPING (`br_fill`, `xpj_dir`,
  `xpj_copy_text`, `xpj_controls`), `send_cc117`, the SM_PROJ / SM_FROM / SM_TO / SM_BACKUP list modes and their texts,
  the browser row kinds, fields never read or always constant (`src_open`, `msg_next`, `pattern_edit`, `bkname`,
  `br_dir`, `cp_*`, `src_es`, `cc117_bak`), unreachable P_MSG texts (mapping counts, 146-148, 204/205), the backup
  leftovers in J_MAPWRITE (`bkname` now always " "), the VST program opcodes, two unused includes; in seqcore.hpp
  `dir_step()`, `copy_page()` / `paste_page()`. `hist_take` = `snap_of`; the test-only NOVERIFY path simplified.
  Stale comments fixed (chunk line list, enum notes, pan CC, base values, Euclid generator). gen_cell_imgs.py draws
  PATTERN EDIT once; gen_params.py comments. Skin images and params.json unchanged.
- Fix: PASTE TRK of a track copied mid-ratchet fired the copy's leftover ratchet hits at once (up to 7), and a stale
  REVERSE / RESTART / SCATTER position came with it. `paste_track` now keeps the track's playback state like
  `restore_from` (shared `keep_runtime`). Test added (fails on v2.10.3).
- Fix: the undo history is a ring of 100 snapshots allocated when the plugin is made (`HistStack`); before, once full,
  every new step moved ~830 KB on the audio thread, and the first block allocated ~1.7 MB. Test: 105 edits, 100 undos.
- Fix: the MIDI event outbox is 1024 events (was 256; when full, events, note-offs too, were dropped).
- Fix: on close, the project-file thread is joined before the ALSA port is closed (it could still send).
- Left for after the device test: chunk formatting off the audio thread, the told[] race, throttling the parameter scan.

### v2.12.0 (2026-10-10) – audio thread: shorter lock holds (v2.11.0 tested OK on the device)
- Save (effGetChunk): the data is copied under the lock (`ChunkSrc`, `chunk_take`, ~150 KB of plain copies) and the
  text is made from the copy without the lock; the hash for `given` is noted under the lock afterwards. Before, the
  whole formatting (live pattern + 8 patterns + 8 states as hex) ran under the lock and could hold up a block (1-5 ms).
  The chunk text is unchanged. Load (effSetChunk) still parses under the lock: it happens while a project loads.
- processReplacing: one lock per block instead of three. The told[] comparison is done under it (setParameter writes
  told[] under the lock), so a value the host has just set is not sent back stale; the changed values are collected and
  the host is called after the lock is let go. close_frames[] / open[] (option lists), held[] / release[] (momentary
  buttons) and the read-only parameters' told[] are now written under the lock on both threads.
- The full scan of the ~290 values runs at most every 10 ms, or at the next block after the host set something
  (`scan_now`: setParameter, a loaded chunk, a finished project-file job). Saves ~70 % of that work at small blocks; the
  play-head can show up to 10 ms later.

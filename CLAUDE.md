# StepRunner-8: guide for an AI assistant (or a new developer)

StepRunner-8 is an 8-track, 64-step MIDI sequencer for Akai MPC standalone (MPC OS 3.x, 32-bit ARM). It is a VST2
plugin with a native MPC screen skin, built with the tools of
[sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins). Read `PROJECT_NOTES.md` first: it has the design,
the decisions and the version history. `PROJECT_HISTORY.md` holds the history of its predecessor, Trig Seq.

## The owner

- The owner, Marko (djokenzi), writes in Serbian. Answer in Serbian, concisely.
- He is new to git and GitHub. Give copy-paste commands, one at a time, and say where to run them.
- His working copy is `~/projekti/steprunner-8`, with the tools in `~/projekti/mpc-vst-plugins`.
- He commits and pushes himself. Hand him changed files as a zip of the repo folder (`steprunner-8/` at the top, no
  `build/` or `dist/`), which he unzips with `unzip -o` in `~/projekti`.

## Setup and test

```sh
git clone https://github.com/djokenzi/steprunner-8.git
git clone https://github.com/sd88me/mpc-vst-plugins.git
git -C mpc-vst-plugins checkout c16b824ca1500188b17ec73c18c3d1ec59f18103   # the commit the release workflow pins
cd steprunner-8 && MPC_VST=../mpc-vst-plugins ./test.sh                    # offline x86 test (ASan); must print PASSED
```

- `test.sh` also works with newer mpc-vst-plugins tools.
- `./build.sh` (the ARM `.so` and the skin) needs Docker.
- Releases are built by GitHub Actions, not locally:
  1. Run Actions → Release with the version. This makes a draft release.
  2. The owner installs that zip on his MPC and tests it.
  3. He publishes it. Publishing creates the tag.
- While a release is a draft, Release can be re-run with the same version. A published release and its tag are never
  changed; fixes go into the next version.
- The catalog (sd88me.github.io/mpc-vst-plugins) picks up new published releases on its own, nightly.

## Rules

- **Parameters are append-only.**
  - A new parameter goes at the end of `gen_params.py`, at the end of the `P_...` enum (before `P_OWN_COUNT`) and into
    `KEYCHECK`.
  - A read-only parameter also goes into the `told[]` list in `setParameter` and the `effCanBeAutomated` exclusions.
  - Adding options to a read-only option parameter is fine. Never reorder or remove parameters.
- **Version bump**, all of these:
  - `vst.json` `"version"` (10100 = 1.1.0);
  - `release.sh` default version (both places);
  - `PROJECT_NOTES.md`: the "Status:" line, and a new entry under "## History";
  - `docs/MANUAL.md`: the version line.
- **Saved projects must keep loading.** The chunk format is text lines, and new lines must be ignorable by older
  versions. Add a host test for every new behaviour in `host_test.cpp`.
- **Skin preview without a device.**
  1. Run `python3 ../mpc-vst-plugins/tools/gen_vst.py vst.json`.
  2. Run the `preview` command of `tools/studio.py` on `build/skin/.../Plugin Skins`.
- **Skin RAM.** MPC decodes every skin picture when the window opens. Avoid many full-screen pictures (one per
  `when=` option adds up fast); share a background and put small pictures on top. `build.sh` runs `skin_dedupe.py`.
- **Docs.** All docs and code comments are in English. The manual is `docs/MANUAL.md`, with screenshots in
  `docs/manual/`.
- Do not mention or compare with other companies' sequencers in docs or UI.

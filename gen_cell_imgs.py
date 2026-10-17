#!/usr/bin/env python3
"""Draws the skin's own images: the TRIGS cells (images/cell_<state>.png), the see-through touch target (images/hit.png),
the STEP EDIT button and the RND / CLR buttons in both colours (normal: the accent orange; STEP EDIT on: EDIT, teal).
Cell state = trig (1) + play-head here (2) + selected (4) + 8 x mode (0 normal, 1 STEP EDIT, 2 TRACK EDIT, 3 EUCLIDIAN);
cell_32 is see-through padding, which keeps every real state below the parameter's top value. One set of images serves all 16 cells.
Needs Pillow. Run after changing sizes or colours; the PNGs are part of the skin and are committed."""
import os
from PIL import Image, ImageDraw

W, H = 68, 84            # must match layout.conf (picture w/h, button w/h)
SS = 4                   # supersampling
OFF, OFF_EDGE = (38, 38, 38), (78, 78, 78)
TRIG = (240, 160, 32)    # the skin's accent (theme_accent)
PLAY, PLAY_TRIG = (150, 150, 150), (255, 236, 170)
SEL = (232, 50, 50)
CLR_RED = (200, 48, 48)   # the RND buttons while CLR LOCKS is lit, and CLR LOCKS itself then
EDIT = (25, 179, 198)     # everything orange turns this colour while STEP EDIT is on (layout.conf uses the same value)
EDIT_PLAY_TRIG = (190, 240, 246)
TRACKC = (160, 100, 232)  # the same while TRACK EDIT is on: violet (layout.conf: a064e8)
TRACK_PLAY_TRIG = (226, 208, 250)
GREY = (70, 70, 70)       # CLR buttons: no mode colour
EUC = (88, 194, 79)       # EUCLIDIAN mode: green (layout.conf: 58c24f)
BTN_GREEN = (30, 150, 110)  # ENABLE (lit) and the COMMIT buttons: a darker, bluer green than the EUCLIDIAN one
EUC_PLAY_TRIG = (214, 244, 210)

def cell(state):
    trig, play, sel, mode = state & 1, state & 2, state & 4, state >> 3   # mode 0 normal, 1 STEP EDIT, 2 TRACK EDIT, 3 EUCLIDIAN
    on_col, hot_col = ((TRIG, PLAY_TRIG), (EDIT, EDIT_PLAY_TRIG), (TRACKC, TRACK_PLAY_TRIG), (EUC, EUC_PLAY_TRIG))[mode]
    im = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    fill = (hot_col if trig else PLAY) if play else (on_col if trig else OFF)
    d.rounded_rectangle([2 * SS, 2 * SS, (W - 2) * SS - 1, (H - 2) * SS - 1], radius=8 * SS, fill=fill + (255,),
                        outline=OFF_EDGE + (255,), width=1 * SS)
    if play:   # a bar on top: the play-head reads at a glance, with or without a trig
        d.rounded_rectangle([9 * SS, 8 * SS, (W - 9) * SS - 1, 18 * SS - 1], radius=4 * SS, fill=(255, 255, 255, 255))
    if sel:    # the step the lock knobs edit; the plugin sets this bit in STEP EDIT only, the other modes show no outline
        d.rounded_rectangle([2 * SS, 2 * SS, (W - 2) * SS - 1, (H - 2) * SS - 1], radius=8 * SS, outline=SEL + (255,), width=5 * SS)
    return im.resize((W, H), Image.LANCZOS)

os.makedirs("images", exist_ok=True)
for s in range(32):
    cell(s).save("images/cell_%d.png" % s)
Image.new("RGBA", (W, H), (0, 0, 0, 0)).save("images/cell_32.png")   # state 32: nothing (the READ PROJECT list covers the tab)
Image.new("RGBA", (90, 44), (0, 0, 0, 0)).save("images/blank.png")
Image.new("RGBA", (W, H), (255, 255, 255, 1)).save("images/hit.png")   # alpha 1/255: invisible, but a real image for the button

# STEP EDIT: a button drawn like the TRACK / PAGE option segments of the browser renderer (dark with a sheen when off,
# light when on), two segments wide. Text baked in with the renderer's own font (OFL, in mpc-vst-plugins tools/html_art/fonts).
import sys
from PIL import ImageFont
MPC_VST = os.environ.get("MPC_VST", "../..")
FONT = os.path.join(MPC_VST, "tools", "html_art", "fonts", "TitilliumWeb-SemiBold.ttf")
BW, BH = 144, 44          # mode buttons; the TRACK / PAGE / MUTE rows share the height
def seg(on, text="STEP EDIT", BW=144):
    im = Image.new("RGBA", (BW * SS, BH * SS), ((243, 242, 239) if on else (5, 4, 3)) + (255,))
    if not on:   # white sheen from the top, gone at 55 % of the height (default.css --sheen: 0.10)
        sheen = Image.new("RGBA", im.size, (0, 0, 0, 0))
        px = sheen.load()
        for y in range(int(BH * SS * 0.55)):
            a = int(255 * 0.10 * (1 - y / (BH * SS * 0.55)))
            for x in range(BW * SS): px[x, y] = (255, 255, 255, a)
        im = Image.alpha_composite(im, sheen)
    d = ImageDraw.Draw(im)
    font = ImageFont.truetype(FONT, 14 * SS)
    track = 0.05 * 14 * SS
    wid = sum(d.textlength(ch, font=font) for ch in text) + track * (len(text) - 1)
    x = (BW * SS - wid) / 2
    for ch in text:
        d.text((x, BH * SS / 2), ch, font=font, fill=((28, 26, 23) if on else (143, 136, 120)) + (255,), anchor="lm")
        x += d.textlength(ch, font=font) + track
    return im.resize((BW, BH), Image.LANCZOS)
if os.path.exists(FONT):
    seg(False).save("images/step_edit_off.png"); seg(True).save("images/step_edit_on.png")
    seg(False, "TRACK EDIT").save("images/track_edit_off.png"); seg(True, "TRACK EDIT").save("images/track_edit_on.png")
    seg(False, "PATTERN EDIT").save("images/pattern_edit_off.png"); seg(True, "PATTERN EDIT").save("images/pattern_edit_on.png")
    seg(False, "SETUP").save("images/global_edit_off.png"); seg(True, "SETUP").save("images/global_edit_on.png")   # GLOBAL until v1.63
    seg(False, "EUCLIDIAN").save("images/euclid_edit_off.png"); seg(True, "EUCLIDIAN").save("images/euclid_edit_on.png")
    for on in (False, True):   # v1.2.0: the two arrow buttons left of EUCLIDIAN: its style, half its width, a triangle for text
        for nm, left in (("nav_left", True), ("nav_right", False)):
            im = seg(on, "", 72).resize((72 * SS, BH * SS), Image.LANCZOS); d = ImageDraw.Draw(im)
            cx, cy, hw, hh = 36 * SS, BH * SS / 2, 6 * SS, 8 * SS
            pts = [(cx - hw, cy), (cx + hw, cy - hh), (cx + hw, cy + hh)] if left else [(cx + hw, cy), (cx - hw, cy - hh), (cx - hw, cy + hh)]
            d.polygon(pts, fill=((28, 26, 23) if on else (143, 136, 120)) + (255,))
            im.resize((72, BH), Image.LANCZOS).save("images/%s_%s.png" % (nm, "on" if on else "off"))
else:
    print("warning: %s not found, images/step_edit_*.png left as they are (set MPC_VST)" % FONT, file=sys.stderr)
# RND / CLR buttons, 132 x 39, text baked in: one pair (released, pressed) per colour.
BTN_W, BTN_H = 132, 39
BUTTONS = [("rnd_trig", "RND STEPS"), ("rnd_vel", "RND VEL"), ("rnd_len", "RND LEN"), ("rnd_pan", "RND PAN"), ("rnd_cc1", "RND CC 1"), ("rnd_cc2", "RND CC 2"),
           ("rnd_cc3", "RND CC 3"), ("rnd_cc4", "RND CC 4"), ("rnd_cc5", "RND CC 5"), ("clr_step", "CLR LOCKS"), ("clr_trk", "CLR STEPS")]
def btn(text, col, pressed, ink=(253, 243, 234), BTN_W=BTN_W, BTN_H=BTN_H):
    if pressed: col = tuple(min(255, int(c * 1.3) + 20) for c in col)
    im = Image.new("RGBA", (BTN_W * SS, BTN_H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, BTN_W * SS - 1, BTN_H * SS - 1], radius=6 * SS, fill=col + (255,))
    sheen = Image.new("RGBA", im.size, (0, 0, 0, 0))
    px = sheen.load()
    for y in range(int(BTN_H * SS * 0.55)):
        a = int(255 * 0.16 * (1 - y / (BTN_H * SS * 0.55)))
        for x in range(BTN_W * SS): px[x, y] = (255, 255, 255, a)
    mask = im.split()[3]
    im = Image.composite(Image.alpha_composite(im, sheen), im, mask)
    d = ImageDraw.Draw(im)
    font = ImageFont.truetype(FONT.replace("SemiBold", "Bold"), 15 * SS)
    track = 0.08 * 15 * SS
    wid = sum(d.textlength(ch, font=font) for ch in text) + track * (len(text) - 1)
    x = (BTN_W * SS - wid) / 2
    for ch in text:
        d.text((x, BTN_H * SS / 2), ch, font=font, fill=ink + (255,), anchor="lm")
        x += d.textlength(ch, font=font) + track
    return im.resize((BTN_W, BTN_H), Image.LANCZOS)
if os.path.exists(FONT):
    for key, text in BUTTONS:
        if key.startswith("clr_"):   # CLR LOCKS / CLR TRACK: the same grey in every mode
            btn(text, GREY, False).save("images/btn_%s_g.png" % key)
            if key != "clr_step": btn(text, GREY, True).save("images/btn_%s_g_on.png" % key)
            if key == "clr_step": btn(text, CLR_RED, False).save("images/btn_clr_mode_on.png")   # the lit state of the CLR LOCKS switch
            continue
        for tag, col in (("n", TRIG), ("e", EDIT), ("t", TRACKC)):
            if key == "rnd_trig" and tag == "e":   # RND TRIGS does nothing while STEP EDIT is on: drawn greyed out, no pressed state
                dim = btn(text, (52, 52, 52), False, ink=(120, 120, 120))
                dim.save("images/btn_%s_%s.png" % (key, tag)); dim.save("images/btn_%s_%s_on.png" % (key, tag))
                continue
            t2 = "RND PTN" if key == "rnd_trig" and tag == "n" else text   # v2.6.0: the pattern's (orange) one reads RND PTN
            btn(t2, col, False).save("images/btn_%s_%s.png" % (key, tag)); btn(t2, col, True).save("images/btn_%s_%s_on.png" % (key, tag))
        # CLR LOCKS lit: the same buttons clear their lock (red, "CLR ..."); RND STEPS has nothing to clear: greyed out
        if key == "rnd_trig":
            dim = btn(text, (52, 52, 52), False, ink=(120, 120, 120)); dim.save("images/btn_%s_c.png" % key); dim.save("images/btn_%s_c_on.png" % key)
        else:
            btn(text.replace("RND", "CLR"), CLR_RED, False).save("images/btn_%s_c.png" % key); btn(text.replace("RND", "CLR"), CLR_RED, True).save("images/btn_%s_c_on.png" % key)
    for key, text in (("rnd_cond", "RND COND"), ("rnd_tm2", "RND TIMING"), ("rnd_rt2", "RND RATCHETS")):   # v1.2.0: page 2 of TRACK EDIT (violet; red CLR ... while CLR LOCKS is lit)
        btn(text, TRACKC, False).save("images/btn_%s_t.png" % key); btn(text, TRACKC, True).save("images/btn_%s_t_on.png" % key)
        btn(text, TRIG, False).save("images/btn_%s_n.png" % key); btn(text, TRIG, True).save("images/btn_%s_n_on.png" % key)   # PATTERN EDIT: orange
        btn(text.replace("RND", "CLR"), CLR_RED, False).save("images/btn_%s_c.png" % key); btn(text.replace("RND", "CLR"), CLR_RED, True).save("images/btn_%s_c_on.png" % key)
    for n in (16, 32, 48, 64):   # track length shortcuts in TRACK SETUP: grey, in the TRACK EDIT colour only while the track has that length
        btn(str(n), GREY, False, BTN_W=84).save("images/btn_len%d.png" % n); btn(str(n), TRACKC, False, BTN_W=84).save("images/btn_len%d_on.png" % n); btn(str(n), TRIG, False, BTN_W=84).save("images/btn_plen%d_on.png" % n)
    # Euclidean section: ENABLE sits in the title row (grey, green while the track is generated), COMMIT at the bottom
    btn("ENABLE", GREY, False).save("images/eu_enable_off.png"); btn("ENABLE", BTN_GREEN, False).save("images/eu_enable_on.png")   # the size of COMMIT
    btn("COMMIT", GREY, False).save("images/eu_commit.png"); btn("COMMIT", BTN_GREEN, False).save("images/eu_commit_on.png")   # grey; green only while pressed
    # PERFORM tab: MUTE pads (red when muted), performance pads (violet when on)
    PH = 64   # pad height on the PERFORM tab
    def pad(text, fill_on, on, name, size=22):   # "\n" in text: two lines
        im = Image.new("RGBA", (140 * SS, PH * SS), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.rounded_rectangle([0, 0, 140 * SS - 1, PH * SS - 1], radius=10 * SS, fill=(fill_on if on else (38, 38, 38)) + (255,), outline=(78, 78, 78, 255), width=1 * SS)
        font = ImageFont.truetype(FONT.replace("SemiBold", "Bold"), size * SS)
        track = 0.08 * size * SS
        lines = text.split("\n")
        for li, line in enumerate(lines):
            wid = sum(d.textlength(ch, font=font) for ch in line) + track * max(len(line) - 1, 0)
            x = (140 * SS - wid) / 2
            y = PH / 2 * SS + (li - (len(lines) - 1) / 2) * size * 1.15 * SS
            for ch in line:
                d.text((x, y), ch, font=font, fill=((255, 255, 255) if on else (150, 144, 132)) + (255,), anchor="lm"); x += d.textlength(ch, font=font) + track
        im.resize((140, PH), Image.LANCZOS).save("images/%s.png" % name)
    PADS = ["LOOP 4", "LOOP 2", "LOOP 1", "NO\nLOCKS", "RANDOM\nSTEP", "INVERT\nMUTE", "UNMUTE\nALL", "FILL"]
    for on in (False, True):
        pad("MUTE", (200, 48, 48), on, "pf_mute_%s" % ("on" if on else "off"))
        for n, text in enumerate(PADS):   # performance pads: the TRACK EDIT violet while on
            pad(text, TRACKC, on, "pfx%d_%s" % (n + 1, "on" if on else "off"), 22 if "\n" not in text else 19)
        pad("MUTE\nALL", TRACKC, on, "pfx7m_%s" % ("on" if on else "off"), 19)   # pad 7 while nothing is muted
        pad("LOOP 8", TRACKC, on, "pfy1_%s" % ("on" if on else "off")); pad("ROLL", TRACKC, on, "pfy5_%s" % ("on" if on else "off"))   # v1.79.0 / v1.80.0 (the other pfy pads have no pad any more)
        for key, text in (("pf_stopcc", "STOP CC"), ("pf_hum", "HUMANIZE"), ("pf_build", "BUILD"), ("pf_echo", "ECHO"), ("pf_fade", "FADE\nOUT"),
                          ("pf_ghost", "GHOST"), ("pf_afill", "AUTO\nFILL")):   # v1.91.0; v1.94.0
            pad(text, TRACKC, on, "%s_%s" % (key, "on" if on else "off"), 22 if "\n" not in text else 19)
    btn("PERFORM ALL", GREY, False, BTN_W=190, BTN_H=64).save("images/pf_ctrl_off.png"); btn("PERFORM ALL", TRIG, False, BTN_W=190, BTN_H=64).save("images/pf_ctrl_on.png")
    btn("COMMIT", BTN_GREEN, False, BTN_W=190, BTN_H=64).save("images/pf_commit.png"); btn("COMMIT", BTN_GREEN, True, BTN_W=190, BTN_H=64).save("images/pf_commit_on.png")
    # TRACK and PAGE selectors: one button per option (the plugin lights the current one), as tall as the mode buttons
    for i in range(8):
        seg(False, "T%d" % (i + 1), 72).save("images/trk%d_off.png" % (i + 1)); seg(True, "T%d" % (i + 1), 72).save("images/trk%d_on.png" % (i + 1))
    for on in (False, True):   # SHIFT: wider than its neighbours and with a grey outline (v1.59.0)
        sh = seg(on, "SHIFT", 120); ImageDraw.Draw(sh).rectangle([0, 0, sh.size[0] - 1, sh.size[1] - 1], outline=(150, 150, 150, 255), width=2)
        sh.save("images/shift_%s.png" % ("on" if on else "off"))
    btn("RANDOM", GREY, False).save("images/eu_rnd.png"); btn("RANDOM", GREY, True).save("images/eu_rnd_on.png")
    for i in range(1, 9):   # STATE 1..8 (v1.68.0: eight, 140 wide): grey while empty, orange while they hold a state
        btn("STATE %d" % i, GREY, False, BTN_W=140, BTN_H=60).save("images/pst%d_off.png" % i); btn("STATE %d" % i, TRIG, False, BTN_W=140, BTN_H=60).save("images/pst%d_on.png" % i)   # v1.98.0: 140x60
    btn("CLEAR STATE", GREY, False, BTN_W=140, BTN_H=48).save("images/pclear_off.png"); btn("CLEAR STATE", (200, 48, 48), False, BTN_W=140, BTN_H=48).save("images/pclear_on.png")   # v2.6.0: CLEAR STATE   # v1.98.0: 140x48
    for key, nm in (("pat_copy", "COPY PTN"), ("pat_paste", "PASTE PTN"), ("pat_clrall", "CLEAR PTN")):   # v2.0.0: PATTERN EDIT, under PATTERN LENGTH
        btn(nm, GREY, False, BTN_W=111).save("images/%s.png" % key); btn(nm, GREY, True, BTN_W=111).save("images/%s_on.png" % key)
    for kind in ("STEP", "PAGE", "TRK"):   # COPY / PASTE read differently per mode (PTN: PATTERN EDIT, v1.85)
        for verb in ("COPY", "PASTE"):
            seg(False, "%s %s" % (verb, kind), 90).save("images/%s_%s.png" % (verb.lower(), kind.lower())); seg(True, "%s %s" % (verb, kind), 90).save("images/%s_%s_on.png" % (verb.lower(), kind.lower()))
    for nm, key, col in (("READ PROJECT", "cc_read", GREY), ("DISCARD", "cc_discard_s", GREY), ("COMMIT", "cc_commit_s", BTN_GREEN)):   # small, for the title row of the CC editor
        btn(nm, col, False, BTN_W=128, BTN_H=28).save("images/%s.png" % key); btn(nm, col, True, BTN_W=128, BTN_H=28).save("images/%s_on.png" % key)
    for nm, key, col in (("PREV", "src_prev", GREY), ("NEXT", "src_next", GREY)):   # the READ PROJECT target list
        btn(nm, col, False, BTN_W=96, BTN_H=28).save("images/%s.png" % key); btn(nm, col, True, BTN_W=96, BTN_H=28).save("images/%s_on.png" % key)
    # SETUP panel (v1.64.0): 250 x 44, all grey (v1.66.0)
    btn("CREATE DEFAULT MAP", GREY, False, BTN_W=250, BTN_H=44).save("images/src_map.png"); btn("CREATE DEFAULT MAP", GREY, True, BTN_W=250, BTN_H=44).save("images/src_map_on.png")
    btn("CANCEL", GREY, False).save("images/src_cancel.png"); btn("CANCEL", GREY, True).save("images/src_cancel_on.png")
    btn("FETCH CC VALUES", GREY, False, BTN_W=250, BTN_H=44).save("images/src_fetch.png"); btn("FETCH CC VALUES", GREY, True, BTN_W=250, BTN_H=44).save("images/src_fetch_on.png")
    btn("FETCH NOTES", GREY, False, BTN_W=250, BTN_H=44).save("images/src_notes.png"); btn("FETCH NOTES", GREY, True, BTN_W=250, BTN_H=44).save("images/src_notes_on.png")
    btn("TRACK SETUP", GREY, False, BTN_W=250, BTN_H=44).save("images/cc_edit_off.png"); btn("TRACK SETUP", GREY, True, BTN_W=250, BTN_H=44).save("images/cc_edit_on.png")   # SETUP tab (before v1.58.0: EDIT CC in TRACK EDIT)
    btn("SHUFFLE EDIT", GREY, False).save("images/shuf_edit_off.png"); btn("SHUFFLE EDIT", TRIG, False).save("images/shuf_edit_on.png")
    btn("COMMIT", BTN_GREEN, False).save("images/cc_commit.png"); btn("COMMIT", BTN_GREEN, True).save("images/cc_commit_on.png")
    for nm in ("UNDO", "REDO"):
        seg(False, nm, 90).save("images/%s.png" % nm.lower()); seg(True, nm, 90).save("images/%s_on.png" % nm.lower())
    seg(False, "ALL", 72).save("images/shuf_all_off.png"); seg(True, "ALL", 72).save("images/shuf_all_on.png")
    for i, t in enumerate(("1-16", "17-32", "33-48", "49-64")):
        seg(False, t, 90).save("images/pg%d_off.png" % (i + 1)); seg(True, t, 90).save("images/pg%d_on.png" % (i + 1))
        for on in (False, True):   # the same with a thin bar along the bottom: the play-head is on this page
            pim = seg(on, t, 90).convert("RGBA")
            ImageDraw.Draw(pim).rectangle([10, pim.height - 6, pim.width - 11, pim.height - 4], fill=((90, 86, 78) if on else (150, 144, 132)) + (255,))
            pim.save("images/pg%d_%s_play.png" % (i + 1, "on" if on else "off"))
    Image.new("RGBA", (90, 44), (255, 255, 255, 1)).save("images/hit_pg.png")   # see-through touch target over a page picture
print("images: cells %dx%d, mode buttons %dx%d, %d RND / CLR buttons" % (W, H, BW, BH, len(BUTTONS)))
# PERFORM tab panel: a plain bordered box (the stock frame always draws a title rule)
pim = Image.new("RGBA", (1065, 274), (0, 0, 0, 0))   # v1.98.0: as big as its frame in layout.conf
ImageDraw.Draw(pim).rounded_rectangle([0, 0, 1064, 273], radius=3, fill=(22, 22, 22, 255), outline=(42, 42, 42, 255), width=1)
pim.save("images/pf_panel.png")
# a row of the list: nothing, or the marked row (orange fill and a thick border behind its text); and the row's touch target
for n, fill, line, wd in ((1, (28, 24, 20, 255), (58, 58, 58, 255), 1), (2, (92, 62, 16, 255), (240, 160, 32, 255), 4)):   # opaque: a see-through one would show the frames under the list
    rim = Image.new("RGBA", (440, 38), (22, 22, 22, 255))
    ImageDraw.Draw(rim).rounded_rectangle([0, 0, 439, 37], radius=6, fill=fill, outline=line, width=wd)
    rim.save("images/src_row_%d.png" % n)
Image.new("RGBA", (440, 38), (255, 255, 255, 1)).save("images/src_hit.png")
Image.new("RGBA", (1280, 628), (255, 255, 255, 1)).save("images/hit_full.png")   # see-through, whole screen: a tap outside the CONDITION list closes it
# The big message (v1.64.0): over the whole PROGRAM tab (1280 x 628, the tab's area), the tab dimmed around a box with a
# coloured title band. One picture per message; the DONE ones per count (1..48). v2.7.0: no backup line (no backup since v2.0).
DONE_TITLE = (200, 70, 1080, 156)   # the DONE title's box in the big picture: x0, y0, x1, y1 (layout: y + 86)
if os.path.exists(FONT):
    BOLD = FONT.replace("SemiBold", "Bold")
    def big(name, head, lines, col, done=False):
        S = 2
        im = Image.new("RGBA", (1280 * S, 628 * S), (0, 0, 0, 185))
        d = ImageDraw.Draw(im)
        bx0, by0, bx1, by1 = 110 * S, 50 * S, 1170 * S, 590 * S
        d.rounded_rectangle([bx0, by0, bx1, by1], radius=18 * S, fill=(24, 22, 20, 255), outline=col + (255,), width=6 * S)
        d.rounded_rectangle([bx0, by0, bx1, by0 + 120 * S], radius=18 * S, fill=col + (255,))
        d.rectangle([bx0, by0 + 100 * S, bx1, by0 + 120 * S], fill=col + (255,))
        d.text((640 * S, by0 + 62 * S), head, font=ImageFont.truetype(BOLD, 64 * S), fill=(255, 255, 255, 255), anchor="mm")
        f = ImageFont.truetype(FONT, 38 * S)
        for k, t in enumerate(lines):
            y0 = 330 - 29 * (len(lines) - 1)   # in the middle of the box (v2.7.0: no backup line any more, DONE too)
            d.text((640 * S, (y0 + 58 * k) * S), t, font=f, fill=(245, 240, 232, 255), anchor="mm")
        d.text((640 * S, 548 * S), "TAP ANYWHERE TO CLOSE", font=ImageFont.truetype(BOLD, 22 * S), fill=(140, 132, 120, 255), anchor="mm")
        im.resize((1280, 628), Image.LANCZOS).quantize(colors=96, method=Image.Quantize.FASTOCTREE).save("images/big_%s.png" % name, optimize=True)   # palette PNG: 17 KB instead of 80
    RED, AMBER = (200, 48, 48), (214, 132, 20)
    big("notsaved", "PROJECT NOT SAVED", ["Save Project before creating a map.", "", "It is recommended to save this as a new project", "rather than overwrite your existing one."], RED)   # v2.7.0
    big("nodrum", "NO DRUM TRACK", ["The project has no drum track to map.", "Add a drum program, save it and try again."], RED)
    big("nothing", "NOTHING NEW TO MAP", ["Every CC or pad parameter is mapped already.", "The project was not changed."], AMBER)
    big("fail", "PROJECT NOT WRITTEN", ["The project could not be written.", "The project was not changed."], RED)
    # v1.1.1 (low RAM): MPC decodes every skin picture when the window opens, and 48 full-screen DONE pictures (x2: on and
    # off) were most of it. Now one shared DONE picture with an empty title band (big_done_bg) plus a small title picture
    # per count (big_done_t1..48), cut from the full picture so it lies exactly over the band.
    DONE_LINES = ["Please close the project and open it again.", "Do not save it first, or the map is lost."]
    big("done_bg", "", DONE_LINES, BTN_GREEN, True)
    for n in range(1, 49):   # v1.93.0: up to 48 (6 per track)
        big("done_tmp", "%d VALUE%s MAPPED" % (n, "" if n == 1 else "S"), DONE_LINES, BTN_GREEN, True)
        Image.open("images/big_done_tmp.png").convert("RGB").crop(DONE_TITLE).save("images/big_done_t%d.png" % n, optimize=True)
    os.remove("images/big_done_tmp.png")

# PATTERN EDIT: CLEAR / STORE, TIMING (the PATTERN 1..8 cells are the patc pictures further down)
if os.path.exists(FONT):
    btn("CLEAR", GREY, False).save("images/pat_clear_off.png"); btn("CLEAR", (200, 48, 48), False).save("images/pat_clear_on.png")
    btn("STORE", GREY, False).save("images/pat_store_off.png"); btn("STORE", BTN_GREEN, False).save("images/pat_store_on.png")   # v1.72.0
    # v1.77.0 TIMING: the mode button, the two arrows (teal, a triangle), RND TIMING / RND RATCHETS
    seg(False, "TIMING").save("images/timing_edit_off.png"); seg(True, "TIMING").save("images/timing_edit_on.png")
    for nm, sgn in (("left", -1), ("right", 1)):   # v1.82.0: grey, 100 wide; v1.98.0: 80x33
        for on in (False, True):
            im = btn("", GREY, on, BTN_W=80, BTN_H=33).convert("RGBA")
            d = ImageDraw.Draw(im)
            cx, cy, r = 40, 16.5, 9
            pts = [(cx - sgn * r, cy - r), (cx - sgn * r, cy + r), (cx + sgn * r, cy)]
            d.polygon(pts, fill=(253, 243, 234, 255))
            im.save("images/tm_%s%s.png" % (nm, "_on" if on else ""))
    btn("RND TIMING", EDIT, False, BTN_W=200).save("images/rnd_timing.png"); btn("RND TIMING", EDIT, True, BTN_W=200).save("images/rnd_timing_on.png")
    btn("RND RATCHETS", EDIT, False, BTN_W=200).save("images/rnd_ratch.png"); btn("RND RATCHETS", EDIT, True, BTN_W=200).save("images/rnd_ratch_on.png")
    btn("CLR TIMING", GREY, False, BTN_W=200).save("images/clr_timing.png"); btn("CLR TIMING", GREY, True, BTN_W=200).save("images/clr_timing_on.png")   # v1.78.0
    btn("CLR RATCHETS", GREY, False, BTN_W=200).save("images/clr_ratch.png"); btn("CLR RATCHETS", GREY, True, BTN_W=200).save("images/clr_ratch_on.png")
    fim = Image.new("RGBA", (148, 68), (0, 0, 0, 0))   # v1.78.0: the active STATE's frame (the button covers the middle); v1.98.0: 148x68
    ImageDraw.Draw(fim).rounded_rectangle([0, 0, 147, 67], radius=8, outline=(255, 255, 255, 255), width=3)
    fim.save("images/pst_frame.png")

# v1.82.0: FWD / BWD / BOTH, narrower: 78 wide (TRACK EDIT violet, pattern setup orange), 100 wide (EUCLIDIAN green);
# PATTERN cells as pictures: EMPTY, FULL, PLAYING (the play-head step's look), CUED (the play-head bar on a dark cell)
if os.path.exists(FONT):
    for nm in ("FWD", "BWD", "BOTH"):
        k = nm.lower()
        btn(nm, GREY, False, BTN_W=78).save("images/dir_%s_s.png" % k)
        btn(nm, TRACKC, False, BTN_W=78).save("images/dir_%s_st.png" % k); btn(nm, TRIG, False, BTN_W=78).save("images/dir_%s_sp.png" % k)
        btn(nm, GREY, False, BTN_W=132).save("images/dir_%s_m.png" % k); btn(nm, EUC, False, BTN_W=132).save("images/dir_%s_me.png" % k)   # v1.98.0: 132x39
        btn(nm, (52, 52, 52), False, ink=(110, 110, 110), BTN_W=132).save("images/dir_%s_md.png" % k)   # v1.83.0: ENABLE off: dim
    # v1.83.0: EMPTY dark; FULL the pale play-head colour, no bar; PLAYING the accent orange with the bar at the BOTTOM;
    # CUED pale with a grey bar at the bottom
    def patcell(fill, bar, k, ink):
        im = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.rounded_rectangle([2 * SS, 2 * SS, (W - 2) * SS - 1, (H - 2) * SS - 1], radius=8 * SS, fill=fill + (255,), outline=OFF_EDGE + (255,), width=1 * SS)
        if bar: d.rounded_rectangle([9 * SS, (H - 18) * SS, (W - 9) * SS - 1, (H - 8) * SS - 1], radius=4 * SS, fill=bar + (255,))
        d.text((W * SS / 2, H * SS / 2 - 4 * SS), str(k), font=ImageFont.truetype(FONT.replace("SemiBold", "Bold"), 30 * SS), fill=ink + (255,), anchor="mm")
        return im.resize((W, H), Image.LANCZOS)
    for k in range(1, 9):
        for st, (fill, bar, ink) in enumerate(((OFF, None, (150, 144, 132)), (PLAY_TRIG, None, (28, 26, 23)),
                                                (TRIG, (255, 255, 255), (28, 26, 23)), (PLAY_TRIG, (120, 116, 108), (28, 26, 23)),
                                                (PLAY_TRIG, TRIG, (28, 26, 23)))):   # 4: BLINK (v1.92.0), the off half of PLAYING
            patcell(fill, bar, k, ink).save("images/patc%d_%d.png" % (k, st))

if os.path.exists(FONT):   # v1.83.0: RANDOM SETUP in the SETUP panel (opens the random settings)
    btn("RANDOM SETUP", GREY, False, BTN_W=250, BTN_H=44).save("images/rnd_setup.png"); btn("RANDOM SETUP", GREY, True, BTN_W=250, BTN_H=44).save("images/rnd_setup_on.png")

# v1.0.1: the StepRunner-8 logo for the left section on SETUP (art/logo.png is 476x335, the size of the section's frame;
# 1 px is cropped all round so the frame's own border stays visible)
_logo = Image.open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "art", "logo.png")).convert("RGB")
_logo.crop((1, 1, _logo.width - 1, _logo.height - 1)).resize((474, 333), Image.LANCZOS).save("images/logo.png")

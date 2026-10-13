#!/usr/bin/env python3
"""v1.1.1 (low RAM): make identical skin pictures one file. MPC decodes every picture of the skin when the plugin window
opens; the generated skin has many byte-identical files (a button's on and off picture when img and img_on are the
same, repeated looks), each decoded on its own. This keeps one file per content, points every reference in the skin's
JSON files at it and deletes the copies. Run after tools/gen_vst.py (build.sh does).
Usage: skin_dedupe.py <skin dir> (the folder holding TUI.json)"""
import hashlib, json, os, sys


def main(d):
    pngs = sorted(f for f in os.listdir(d) if f.endswith(".png"))
    canon, dup = {}, {}
    for f in pngs:
        h = hashlib.sha256(open(os.path.join(d, f), "rb").read()).hexdigest()
        if h in canon:
            dup[f] = canon[h]
        else:
            canon[h] = f
    if not dup:
        print("skin_dedupe: nothing to do")
        return
    jsons = [f for f in os.listdir(d) if f.endswith(".json")]

    for f in jsons:   # plain text replacement of the quoted file names: the files stay byte for byte as written otherwise
        p = os.path.join(d, f)
        raw = open(p, encoding="utf-8").read()
        new = raw
        for a, b in dup.items():
            new = new.replace('"%s"' % a, '"%s"' % b)
        if new != raw:
            json.loads(new)   # still valid
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(new)
    left = "".join(open(os.path.join(d, f), encoding="utf-8").read() for f in jsons)
    for f in dup:
        if '"%s"' % f in left:
            sys.exit("skin_dedupe: %s is still referenced" % f)
        os.remove(os.path.join(d, f))
    print("skin_dedupe: %d copies removed, %d pictures left" % (len(dup), len(pngs) - len(dup)))


if __name__ == "__main__":
    main(sys.argv[1])

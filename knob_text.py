#!/usr/bin/env python3
"""v1.2.0: a smaller VALUE text under every knob, the knob's NAME unchanged. The tools' label_scale= sizes both at once
(their default 1.15: name 19.55, value 25.3). Close knobs (90 px apart) need the shorter value line ("16 CUT (127)"),
while the name ("Track CC 1") reads better at its old size, so this sets only the value font of the knob definitions
(shKnob*) to VALUE_H; the boxes stay as the tools made them. Run after tools/gen_vst.py (build.sh does).
Usage: knob_text.py <skin dir> (the folder holding TUI.json)"""
import json, os, sys

VALUE_H = 22.0   # the value font at label_scale=1.0


def value_labels(comp):
    """the Value label(s) inside one knob definition"""
    for c in comp.get("value", {}).get("componentsData", []):
        d = c.get("componentData", {})
        if d.get("type") == "Label" and d.get("data", {}).get("type") == "Value":
            yield d["data"]


def main(d):
    p = os.path.join(d, "TUI.json")
    obj = json.load(open(p, encoding="utf-8"))
    n = 0
    for comp in obj["pageData"]["componentDefinitions"]["localComponentDefinitions"]:
        if not str(comp.get("key", "")).startswith("shKnob"):
            continue
        for lab in value_labels(comp):
            lab["textStyle"]["font"]["height"] = VALUE_H
            n += 1
    if not n:
        sys.exit("knob_text: no knob value label found in %s (did the tools change?)" % p)
    json.dump(obj, open(p, "w", encoding="utf-8"), indent=4)
    print("knob_text: %d knob value labels at %g" % (n, VALUE_H))


if __name__ == "__main__":
    main(sys.argv[1])

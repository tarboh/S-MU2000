#!/usr/bin/env python3
"""Differential VST2 opcode parity gate: real Steinberg SDK vs clean-room compat.

Extracts every enumerator (name -> numeric value) from the real SDK headers
(aeffect.h + aeffectx.h, with VST_FORCE_DEPRECATED and all VST_2_x_EXTENSIONS
enabled — exactly how our VST2 targets compile the SDK) and from the clean-room
compat headers (cmake/vst2_compat), then verifies:

  * every SDK enumerator (including the __nameDeprecated spellings produced by
    DECLARE_VST_DEPRECATED under FORCE_DEPRECATED) exists in compat with the
    same value;
  * no compat label sits on a number the SDK assigns to a different enumerator
    in the same opcode space (the number space is the ABI).

Read-only; prints a report and exits non-zero on any mismatch.
The SDK directory is only ever read on a dev box holding a licensed SDK —
nothing from it is written out or committed.

Usage: python vst2_abi_check.py --sdk D:/opt/vst/vstsdk2.4/pluginterfaces/vst2.x --compat cmake/vst2_compat
"""
import argparse
import re
import sys
from pathlib import Path

MACROS = {"VST_2_1_EXTENSIONS", "VST_2_3_EXTENSIONS", "VST_2_4_EXTENSIONS",
          "VST_FORCE_DEPRECATED"}

def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text

def apply_declare(text: str) -> str:
    # SDK and compat spell deprecated entries identically through the macro.
    return re.sub(r"DECLARE_VST_DEPRECATED\s*\(\s*(\w+)\s*\)", r"__\1Deprecated", text)

def eval_cond(expr: str) -> bool:
    expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)",
                  lambda m: "1" if m.group(1) in MACROS else "0", expr)
    expr = re.sub(r"\bdefined\s+(\w+)",
                  lambda m: "1" if m.group(1) in MACROS else "0", expr)
    # VST_VERSION and any other unknown macro/identifier -> 0 (unset).
    expr = re.sub(r"[A-Za-z_]\w*", "0", expr)
    expr = expr.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    try:
        return bool(eval(expr))
    except Exception:
        return False

def active_text(src: str) -> str:
    """Drop #else/#elif branches inactive under MACROS; keep the rest."""
    out = []
    # stack entries: (parent_active, taken_in_a_previous_branch)
    stack = []
    for line in src.splitlines():
        s = line.strip()
        if s.startswith("#"):
            m = re.match(r"#(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)", s)
            if not m:
                continue  # #define/#include/#pragma: content lines pass through
            d, arg = m.group(1), m.group(2).strip()
            parent = all(a for a, _ in stack)
            if d in ("if", "ifdef", "ifndef"):
                if d == "if":
                    r = eval_cond(arg)
                else:
                    r = arg.split()[0] in MACROS
                    if d == "ifndef":
                        r = not r
                stack.append((parent and r, r))
                continue
            if not stack:
                continue
            if d == "elif":
                pa, taken = stack.pop()
                r = (not taken) and eval_cond(arg)
                stack.append((all(a for a, _ in stack) and r, taken or r))
                continue
            if d == "else":
                pa, taken = stack.pop()
                stack.append((all(a for a, _ in stack) and not taken, True))
                continue
            if d == "endif":
                stack.pop()
                continue
        parent_active = all(a for a, _ in stack)
        if parent_active:
            out.append(line)
    return "\n".join(out)

ENUM_RE = re.compile(r"\benum\s+(?:\w+\s*)?\{(.*?)\}", re.S)

def resolve(expr: str, out: dict) -> int:
    e = expr.strip()
    for prev, val in out.items():  # chained numbering across enums
        e = re.sub(rf"\b{re.escape(prev)}\b", str(val), e)
    e = e.replace("&&", " and ").replace("||", " or ")
    return int(eval(e))

def extract(path: Path, seed: dict | None = None) -> dict:
    # SDK chains across files (aeffectx starts `effProcessEvents = effSetChunk + 1`),
    # so the previous file's parsed values seed this one.
    out = dict(seed or {})
    text = active_text(apply_declare(strip_comments(
        path.read_text(encoding="utf-8", errors="replace"))))
    for body in ENUM_RE.findall(text):
        prev = None
        for part in body.split(","):
            part = re.sub(r"/\*.*?\*/", "", part, flags=re.S).strip()  # remnants
            if not part:
                continue
            m = re.match(r"(\w+)\s*=\s*([\s\S]+)$", part)
            if m:
                try:
                    prev = resolve(m.group(2), out)
                except Exception:
                    prev = None
                if prev is not None:
                    out[m.group(1)] = prev
                continue
            m = re.match(r"(\w+)$", part)
            if m and prev is not None:  # implicit increment
                out[m.group(1)] = prev + 1
                prev += 1
    return out

def load(dirpath: Path, names):
    merged = {}
    for n in names:
        merged.update(extract(dirpath / n, merged))
    return merged

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--compat", required=True)
    args = ap.parse_args()

    sdk = load(Path(args.sdk), ["aeffect.h", "aeffectx.h"])
    compat = load(Path(args.compat), ["compat_aeffect_core.h", "compat_aeffect_extended.h"])

    fails = 0
    for name, val in sorted(sdk.items()):
        if name not in compat:
            print(f"MISSING   {name} = {val} (SDK has it, compat does not)")
            fails += 1
        elif compat[name] != val:
            print(f"MISMATCH  {name}: SDK={val} compat={compat[name]}")
            fails += 1

    # Number-space conflicts. Only the SHARED call spaces matter (a dispatcher
    # opcode, a master opcode, an AEffect flag bit, an event type): those
    # numbers cross the ABI boundary inside one integer. Values in unrelated
    # enums (smpte rates vs key codes vs max lengths...) may repeat freely in
    # the SDK itself, so they are excluded.
    def space_of(name: str):
        if name.startswith("__effFlags") or name.startswith("effFlags"):
            return "aeffect-flags"
        if re.match(r"^eff[A-Z]", name):
            return "dispatcher"
        if re.match(r"^__eff[A-Z]\w+Deprecated$", name):
            return "dispatcher"
        if name.startswith("audioMaster") or re.match(r"^__audioMaster\w+Deprecated$", name):
            return "master"
        if name in ("kVstMidiType", "kVstSysExType") or \
           re.match(r"^__kVst\w+TypeDeprecated$", name):
            return "event-types"
        return None

    conflicts = 0
    for space in ("dispatcher", "master", "aeffect-flags", "event-types"):
        by_val = {}
        for name, val in sdk.items():
            if space_of(name) == space:
                by_val.setdefault(val, set()).add(name)
        for name, val in sorted(compat.items()):
            if name in sdk or space_of(name) != space:
                continue
            clash = by_val.get(val, set())
            if clash:
                print(f"CONFLICT  compat '{name}' = {val} collides with SDK "
                      f"{sorted(clash)} in the {space} space")
                conflicts += 1
    fails += conflicts

    print(f"checked {len(sdk)} SDK enumerators against {len(compat)} compat labels")
    if fails:
        print(f"ABI PARITY FAIL: {fails} problem(s)")
        return 1
    print("ABI PARITY OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())

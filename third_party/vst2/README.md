# third_party/vst2 — clean-room VST 2.4 ABI declarations

The VST 2.4 plugin format is what our VST2 target talks to hosts with, but the
discontinued Steinberg VST2 SDK headers (`aeffect.h`/`aeffectx.h`) are
proprietary and are **not** committed anywhere in this repo (AGENTS.md hard rule
#3 keeps even locally-dropped copies out of git).

What lives here instead:

- `vst2_abi.h` — an independently written declaration of the VST 2.4 host/plugin
  binary interface (namespace `smu2000::vsti`), BSD-3-Clause, taken verbatim
  from upstream PR [tarboh/S-MU2000#16](https://github.com/tarboh/S-MU2000/pull/16)
  (author drel4, commit `983d2d8`). It proves the ABI shapes with
  `static_assert`s (struct sizes/offsets for both ILP32 and LP64).
- `../../cmake/vst2_compat/` — the clean-room compatibility layer the CMake
  harness uses when no real SDK is present: it declares the same public ABI
  facts (opcode numbers, struct layouts, callback signatures — all documented
  interface facts required for interoperability) under our own file names.

At configure time `cmake/iplug2_paths.cmake` copies (and thereby renames) those
files into the *untracked* `iPlug2/Dependencies/IPlug/VST2_SDK/` include dir so
iPlug2's `IPlugVST2.cpp` can `#include` the names its upstream expects. No SDK
file name or SDK text is ever committed; the names only appear as generated
build-machine artifacts next to the build tree.

Correctness gates — run `tools/vst2_abi_check.ps1 -Sdk <path-to-vstsdk2.4/pluginterfaces/vst2.x>`
on a dev box holding a licensed SDK (the SDK is only ever *read*, never copied
into the repo):

1. `tools/vst2_abi_check.py` — arch-independent enumerator parity. Parses both
   header sets (SDK guards evaluated with `VST_FORCE_DEPRECATED` + all
   `VST_2_x_EXTENSIONS` on, exactly how our targets compile) and requires every
   SDK enumerator — including every `__nameDeprecated` spelling the macro
   produces — to exist in the compat layer with the identical value, and checks
   the four shared number spaces (dispatcher opcodes, master opcodes, AEffect
   flag bits, event types) for label collisions. Currently 377 SDK enumerators,
   zero mismatches.
2. `tools/vst2_abi_check.cpp` (via the ps1) — differential struct parity: the
   same TU compiled twice (SDK `/I` vs compat `/I`) at **both** x64 and Win32,
   diffing 55 `sizeof`/`offsetof` facts per arch.
3. `tools/vst2_host_probe.cpp` + `tools/vst2_probe_run.ps1` — loads the actual
   compat-built plugin DLL in a minimal native host and exercises the full
   editor attach/paint/detach round-trip through the shim.


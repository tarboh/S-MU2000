// vst2_abi_check.cpp — differential STRUCT parity: real Steinberg SDK headers
// vs the clean-room compat headers (cmake/vst2_compat).
//
// The SAME translation unit is compiled twice (see tools/vst2_abi_check.ps1):
// once with /I on the real SDK (SIDE_SDK defined), once with /I on the compat
// drop dir. Each exe prints its sizeof/offsetof table with a side tag; the
// ps1 diffs the two dumps. Member spellings are shared by both sides, so an
// SDK-side compile error is itself a parity finding (compat must speak the
// exact SDK member names iPlug2 compiles against).
//
// Opcode/flag *numbers* are gated separately (and arch-independently) by
// tools/vst2_abi_check.py. Never ship or commit the SDK side.
#include <cstddef>
#include <cstdio>
#include <cstdint>

#define VST_2_1_EXTENSIONS 1
#define VST_2_3_EXTENSIONS 1
#define VST_2_4_EXTENSIONS 1
#define VST_FORCE_DEPRECATED 1

#ifdef SIDE_SDK
  #include "aeffect.h"
  #include "aeffectx.h"
  // (ERect is fully defined by the SDK's aeffect.h itself — same as compat.)
  #define SIDE_TAG "sdk"
#else
  #include "compat_aeffect_core.h"
  #include "compat_aeffect_extended.h"
  #define SIDE_TAG "compat"
#endif

#define P(name, expr) printf(SIDE_TAG " %-40s %lld\n", name, (long long)(expr));

static void dump() {
  // AEffect — the load-time contract. Field order IS the ABI.
  P("sizeof(AEffect)", sizeof(AEffect));
  P("off AEffect::magic", offsetof(AEffect, magic));
  P("off AEffect::dispatcher", offsetof(AEffect, dispatcher));
  P("off AEffect::__processDeprecated", offsetof(AEffect, DECLARE_VST_DEPRECATED(process)));
  P("off AEffect::setParameter", offsetof(AEffect, setParameter));
  P("off AEffect::getParameter", offsetof(AEffect, getParameter));
  P("off AEffect::numPrograms", offsetof(AEffect, numPrograms));
  P("off AEffect::numParams", offsetof(AEffect, numParams));
  P("off AEffect::numInputs", offsetof(AEffect, numInputs));
  P("off AEffect::numOutputs", offsetof(AEffect, numOutputs));
  P("off AEffect::flags", offsetof(AEffect, flags));
  P("off AEffect::resvd1", offsetof(AEffect, resvd1));
  P("off AEffect::resvd2", offsetof(AEffect, resvd2));
  P("off AEffect::initialDelay", offsetof(AEffect, initialDelay));
  P("off AEffect::__realQualitiesDeprecated", offsetof(AEffect, DECLARE_VST_DEPRECATED(realQualities)));
  P("off AEffect::__offQualitiesDeprecated", offsetof(AEffect, DECLARE_VST_DEPRECATED(offQualities)));
  P("off AEffect::__ioRatioDeprecated", offsetof(AEffect, DECLARE_VST_DEPRECATED(ioRatio)));
  P("off AEffect::object", offsetof(AEffect, object));
  P("off AEffect::user", offsetof(AEffect, user));
  P("off AEffect::uniqueID", offsetof(AEffect, uniqueID));
  P("off AEffect::version", offsetof(AEffect, version));
  P("off AEffect::processReplacing", offsetof(AEffect, processReplacing));
  P("off AEffect::processDoubleReplacing", offsetof(AEffect, processDoubleReplacing));
  P("off AEffect::future", offsetof(AEffect, future));

  // Events.
  P("sizeof(VstEvent)", sizeof(VstEvent));
  P("sizeof(VstEvents)", sizeof(VstEvents));
  P("off VstEvents::events", offsetof(VstEvents, events));
  P("sizeof(VstMidiEvent)", sizeof(VstMidiEvent));
  P("off VstMidiEvent::midiData", offsetof(VstMidiEvent, midiData));
  P("off VstMidiEvent::detune", offsetof(VstMidiEvent, detune));
  P("sizeof(VstMidiSysexEvent)", sizeof(VstMidiSysexEvent));
  P("off VstMidiSysexEvent::sysexDump", offsetof(VstMidiSysexEvent, sysexDump));

  // Transport.
  P("sizeof(VstTimeInfo)", sizeof(VstTimeInfo));
  P("off VstTimeInfo::samplePos", offsetof(VstTimeInfo, samplePos));
  P("off VstTimeInfo::ppqPos", offsetof(VstTimeInfo, ppqPos));
  P("off VstTimeInfo::timeSigNumerator", offsetof(VstTimeInfo, timeSigNumerator));
  P("off VstTimeInfo::samplesToNextClock", offsetof(VstTimeInfo, samplesToNextClock));
  P("off VstTimeInfo::flags", offsetof(VstTimeInfo, flags));
  P("sizeof(VstVariableIo)", sizeof(VstVariableIo));

  // Properties / MIDI tables / surround / editor.
  P("sizeof(VstParameterProperties)", sizeof(VstParameterProperties));
  P("off VstParameterProperties::label", offsetof(VstParameterProperties, label));
  P("off VstParameterProperties::shortLabel", offsetof(VstParameterProperties, shortLabel));
  P("off VstParameterProperties::categoryLabel", offsetof(VstParameterProperties, categoryLabel));
  P("off VstParameterProperties::future", offsetof(VstParameterProperties, future));
  P("sizeof(VstPinProperties)", sizeof(VstPinProperties));
  P("off VstPinProperties::future", offsetof(VstPinProperties, future));
  P("sizeof(MidiProgramName)", sizeof(MidiProgramName));
  P("off MidiProgramName::parentCategoryIndex", offsetof(MidiProgramName, parentCategoryIndex));
  P("sizeof(MidiProgramCategory)", sizeof(MidiProgramCategory));
  P("sizeof(MidiKeyName)", sizeof(MidiKeyName));
  P("sizeof(VstSpeakerProperties)", sizeof(VstSpeakerProperties));
  P("off VstSpeakerProperties::type", offsetof(VstSpeakerProperties, type));
  P("sizeof(VstSpeakerArrangement)", sizeof(VstSpeakerArrangement));
  P("sizeof(VstKeyCode)", sizeof(VstKeyCode));
  P("sizeof(ERect)", sizeof(ERect));
}

int main() { dump(); return 0; }

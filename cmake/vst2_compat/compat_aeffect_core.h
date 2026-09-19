// license:BSD-3-Clause
//
// SMU2000 clean-room VST 2.4 interface — core layer.
//
// An independent declaration of the public, documented VST 2.4 host/plug-in C
// ABI (callback shapes, the AEffect layout, and the opcode/flag number spaces)
// — the interoperability facts needed to exchange audio, MIDI, editor and
// state with a VST2 host. No proprietary SDK text or file is included, copied
// or derived here; see third_party/vst2/README.md for provenance and the
// differential ABI gate (tools/vst2_abi_check.cpp).
//
// This file is copied (and renamed) by cmake/iplug2_paths.cmake into the
// UNTRACKED iPlug2/Dependencies/IPlug/VST2_SDK/ include directory at configure
// time so that iPlug2's stock IPlugVST2.cpp — which includes the two canonical
// SDK header names — resolves them to these declarations. Only the generated
// copy on the build machine ever carries those names.
//
// Deprecated members carry the `__nameDeprecated` spelling: that is the
// contract hosts and SDK-era code (iPlug2 included) expect for every
// entry declared through DECLARE_VST_DEPRECATED. Here the macro is always
// live — like the SDK built with VST_FORCE_DEPRECATED enabled, the mode our
// VST2 targets configure — and deprecated entries are DECLARED, never
// elided, so the opcode/flag number space is stable and implicit numbering
// can never drift.
//
// Layout authority: the parallel declarations in third_party/vst2/vst2_abi.h
// (upstream PR #16) and the SDK-vs-cleanroom differential gate. Every struct
// below is guarded by static_assert on BOTH architectures; any mismatch fails
// the build, not the host.

#ifndef SMU2000_VST2_COMPAT_CORE_H
#define SMU2000_VST2_COMPAT_CORE_H

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// The VST2 C ABI is cdecl on x86 Windows. On x64 every Windows calling
// convention is already compatible with the cdecl spelling, and non-Windows
// builds (no VST2 host ships for this plug-in off Windows) get the default.
#if defined(_WIN32)
#define VSTCALLBACK __cdecl
#else
#define VSTCALLBACK
#endif

// SDK-era marker for deprecated interface entries; always maps to the
// `__nameDeprecated` spelling hosts/toolchain code expect.
#define DECLARE_VST_DEPRECATED(name) __##name##Deprecated

// The SDK exposes the same fixed-width set; VstIntPtr is the pointer-sized one.
typedef std::int16_t VstInt16;
typedef std::int32_t VstInt32;
typedef std::int64_t VstInt64;
typedef std::intptr_t VstIntPtr; // 32-bit hosts exchange 32-bit, 64-bit hosts 64-bit

// Four-character code, e.g. for AEffect::uniqueID.
#define CCONST(a, b, c, d) \
	((((VstInt32)(a)) << 24) | (((VstInt32)(b)) << 16) | (((VstInt32)(c)) << 8) | (((VstInt32)(d)) << 0))

// Magic that identifies a valid AEffect (fourCC 'VstP').
#define kEffectMagic CCONST('V', 's', 't', 'P')

// This header declares the 2.4 interface.
#define kVstVersion 2400

// The plug-in to host callback, and the AEffect function pointer types.
struct AEffect;

typedef VstIntPtr(VSTCALLBACK *audioMasterCallback)(AEffect *effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void *ptr, float opt);
typedef VstIntPtr(VSTCALLBACK *AEffectDispatcherProc)(AEffect *effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void *ptr, float opt);
typedef void(VSTCALLBACK *AEffectProcessProc)(AEffect *effect, float **inputs, float **outputs, VstInt32 sampleFrames);
typedef void(VSTCALLBACK *AEffectProcessDoubleProc)(AEffect *effect, double **inputs, double **outputs, VstInt32 sampleFrames);
typedef void(VSTCALLBACK *AEffectSetParameterProc)(AEffect *effect, VstInt32 index, float parameter);
typedef float(VSTCALLBACK *AEffectGetParameterProc)(AEffect *effect, VstInt32 index);

// 8-byte natural alignment everywhere (MSVC default on both arches; MinGW
// i686 must be pinned so the extended layer's double-bearing structs keep the
// host-visible layout).
#if defined(_WIN32)
#pragma pack(push, 8)
#endif

// The single structure exchanged at load time: the host owns its read side,
// the plug-in fills it in VSTPluginMain. Field order IS the ABI.
struct AEffect
{
	VstInt32 magic;                    // must be kEffectMagic
	AEffectDispatcherProc dispatcher;  // host -> plug-in control channel
	AEffectProcessProc DECLARE_VST_DEPRECATED(process); // accumulating mode, unused in 2.4
	AEffectSetParameterProc setParameter;
	AEffectGetParameterProc getParameter;

	VstInt32 numPrograms;
	VstInt32 numParams;
	VstInt32 numInputs;
	VstInt32 numOutputs;
	VstInt32 flags;                    // VstAEffectFlags below

	VstIntPtr resvd1;                  // host-owned, must be 0
	VstIntPtr resvd2;                  // host-owned, must be 0

	VstInt32 initialDelay;             // latency in samples

	VstInt32 DECLARE_VST_DEPRECATED(realQualities);
	VstInt32 DECLARE_VST_DEPRECATED(offQualities);
	float DECLARE_VST_DEPRECATED(ioRatio);

	void *object;                      // plug-in instance pointer
	void *user;                        // plug-in private

	VstInt32 uniqueID;                 // CCONST id of this plug-in
	VstInt32 version;

	AEffectProcessProc processReplacing;
	AEffectProcessDoubleProc processDoubleReplacing;

	char future[56];                   // reserved, zeroed
};

static_assert(sizeof(AEffect) == (sizeof(void *) == 8 ? 192 : 144), "AEffect size mismatch");
static_assert(offsetof(AEffect, dispatcher) == (sizeof(void *) == 8 ? 8 : 4), "AEffect.dispatcher mismatch");
static_assert(offsetof(AEffect, setParameter) == (sizeof(void *) == 8 ? 24 : 12), "AEffect.setParameter mismatch");
static_assert(offsetof(AEffect, numPrograms) == (sizeof(void *) == 8 ? 40 : 20), "AEffect.numPrograms mismatch");
static_assert(offsetof(AEffect, flags) == (sizeof(void *) == 8 ? 56 : 36), "AEffect.flags mismatch");
static_assert(offsetof(AEffect, object) == (sizeof(void *) == 8 ? 96 : 64), "AEffect.object mismatch");
static_assert(offsetof(AEffect, processReplacing) == (sizeof(void *) == 8 ? 120 : 80), "AEffect.processReplacing mismatch");
static_assert(offsetof(AEffect, processDoubleReplacing) == (sizeof(void *) == 8 ? 128 : 84), "AEffect.processDoubleReplacing mismatch");
static_assert(offsetof(AEffect, future) == (sizeof(void *) == 8 ? 136 : 88), "AEffect.future mismatch");

// AEffect::flags bits. Deprecated bits keep their numbers (they occupy the
// number space of a field a host may still write).
enum VstAEffectFlags
{
	effFlagsHasEditor = 1 << 0,       // plug-in has an editor
	effFlagsCanReplacing = 1 << 4,    // implements processReplacing
	effFlagsProgramChunks = 1 << 5,   // state travels as opaque chunks
	effFlagsIsSynth = 1 << 8,         // VSTi
	effFlagsNoSoundInStop = 1 << 9,   // silent when input is silent
	effFlagsCanDoubleReplacing = 1 << 12 // implements processDoubleReplacing
};

enum VstAEffectFlagsDeprecated
{
	DECLARE_VST_DEPRECATED(effFlagsHasClip) = 1 << 1,
	DECLARE_VST_DEPRECATED(effFlagsHasVu) = 1 << 2,
	DECLARE_VST_DEPRECATED(effFlagsCanMono) = 1 << 3,
	DECLARE_VST_DEPRECATED(effFlagsExtIsAsync) = 1 << 10,
	DECLARE_VST_DEPRECATED(effFlagsExtHasBuffer) = 1 << 11
};

static_assert(effFlagsHasEditor == 1, "effFlagsHasEditor mismatch");
static_assert(__effFlagsHasClipDeprecated == 1 << 1, "effFlagsHasClip mismatch");
static_assert(effFlagsCanReplacing == 1 << 4, "effFlagsCanReplacing mismatch");
static_assert(effFlagsProgramChunks == 1 << 5, "effFlagsProgramChunks mismatch");
static_assert(effFlagsIsSynth == 1 << 8, "effFlagsIsSynth mismatch");
static_assert(effFlagsCanDoubleReplacing == 1 << 12, "effFlagsCanDoubleReplacing mismatch");

// Dispatcher opcodes 0..25 (host -> plug-in). Number space shared with the
// extended layer's AEffectXOpcodes; deprecated entries consume their numbers
// (declared in the second enum so the first stays readable).
enum AEffectOpcodes
{
	effOpen = 0,
	effClose = 1,

	effSetProgram = 2,
	effGetProgram = 3,
	effSetProgramName = 4,
	effGetProgramName = 5,

	effGetParamLabel = 6,
	effGetParamDisplay = 7,
	effGetParamName = 8,

	effSetSampleRate = 10,
	effSetBlockSize = 11,
	effMainsChanged = 12,

	effEditGetRect = 13,
	effEditOpen = 14,
	effEditClose = 15,

	effEditIdle = 19,

	effGetChunk = 23,
	effSetChunk = 24,

	effNumOpcodes = 25
};

enum AEffectOpcodesDeprecated
{
	DECLARE_VST_DEPRECATED(effGetVu) = 9,

	DECLARE_VST_DEPRECATED(effEditDraw) = 16,
	DECLARE_VST_DEPRECATED(effEditMouse) = 17,
	DECLARE_VST_DEPRECATED(effEditKey) = 18,

	DECLARE_VST_DEPRECATED(effEditTop) = 20,
	DECLARE_VST_DEPRECATED(effEditSleep) = 21,
	DECLARE_VST_DEPRECATED(effIdentify) = 22
};

// Plug-in -> host callback opcodes 0..4 (the extended range continues in
// AudioMasterOpcodesX in the extended layer).
enum AudioMasterOpcodes
{
	audioMasterAutomate = 0,
	audioMasterVersion = 1,
	audioMasterCurrentId = 2,
	audioMasterIdle = 3
};

enum AudioMasterOpcodesDeprecated
{
	DECLARE_VST_DEPRECATED(audioMasterPinConnected) = 4
};

// Editor size returned through effEditGetRect.
struct ERect
{
	VstInt16 top;
	VstInt16 left;
	VstInt16 bottom;
	VstInt16 right;
};
static_assert(sizeof(ERect) == 8, "ERect size mismatch");

// Fixed-size char buffers exchanged with the host.
enum VstStringConstants
{
	kVstMaxProgNameLen = 24,
	kVstMaxParamStrLen = 8,
	kVstMaxVendorStrLen = 64,
	kVstMaxProductStrLen = 64,
	kVstMaxEffectNameLen = 32
};

// Pointer <-> VstIntPtr casts (the SDK hosts use these when stuffing handles).
template <class T>
inline T *FromVstPtr(VstIntPtr &arg)
{
	VstIntPtr *address = &arg;
	return *reinterpret_cast<T **>(address);
}

template <class T>
inline VstIntPtr ToVstPtr(T *ptr)
{
	VstIntPtr *address = reinterpret_cast<VstIntPtr *>(&ptr);
	return *address;
}

// Null-terminated bounded string helpers (same guarantee the SDK's inline
// helpers gave: dst[maxLen] is always '\0').
inline char *vst_strncpy(char *dst, const char *src, size_t maxLen)
{
	char *result = std::strncpy(dst, src, maxLen);
	dst[maxLen] = 0;
	return result;
}

inline char *vst_strncat(char *dst, const char *src, size_t maxLen)
{
	char *result = std::strncat(dst, src, maxLen);
	dst[maxLen] = 0;
	return result;
}

#if defined(_WIN32)
#pragma pack(pop)
#endif

#endif // SMU2000_VST2_COMPAT_CORE_H

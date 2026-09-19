// license:BSD-3-Clause
//
// SMU2000 clean-room VST 2.4 interface — extended layer (events, time info,
// properties, MIDI program/key tables, surround, editor keys).
//
// Independent declaration of the documented VST 2.x extended C ABI — the same
// number space and layouts every VST2 host uses. See
// third_party/vst2/README.md (provenance + differential SDK-vs-clean-room ABI
// parity gate) and compat_aeffect_core.h (core layer; always included first).
// Copied + renamed by cmake/iplug2_paths.cmake; committed only under this
// clean-room file name.

#ifndef SMU2000_VST2_COMPAT_EXTENDED_H
#define SMU2000_VST2_COMPAT_EXTENDED_H

#pragma once

// When dropped as the pair into the untracked SDK include dir, the core header
// sits next to it under the canonical name; a translation unit that included
// the core layer under its own name is simply already past it.
#ifndef SMU2000_VST2_COMPAT_CORE_H
#include "aeffect.h"
#endif

#if defined(_WIN32)
#pragma pack(push, 8)
#endif

// ---------------------------------------------------------------------------
// Events (effProcessEvents / audioMasterProcessEvents)
// ---------------------------------------------------------------------------

struct VstEvent
{
	VstInt32 type;       // VstEventTypes
	VstInt32 byteSize;   // size excluding the type+byteSize fields (host contract)
	VstInt32 deltaFrames;
	VstInt32 flags;
	char data[16];
};

enum VstEventTypes
{
	kVstMidiType = 1,
	kVstSysExType = 6
};

// Deprecated event types 2..5 occupy the number space.
enum VstEventTypesDeprecated
{
	DECLARE_VST_DEPRECATED(kVstAudioType) = 2,
	DECLARE_VST_DEPRECATED(kVstVideoType) = 3,
	DECLARE_VST_DEPRECATED(kVstParameterType) = 4,
	DECLARE_VST_DEPRECATED(kVstTriggerType) = 5
};

struct VstEvents
{
	VstInt32 numEvents;
	VstIntPtr reserved;
	VstEvent *events[2]; // variable-length array in practice
};

struct VstMidiEvent
{
	VstInt32 type;
	VstInt32 byteSize;
	VstInt32 deltaFrames;
	VstInt32 flags;
	VstInt32 noteLength;
	VstInt32 noteOffset;
	char midiData[4];
	char detune;
	char noteOffVelocity;
	char reserved1;
	char reserved2;
};

enum VstMidiEventFlags
{
	kVstMidiEventIsRealtime = 1 << 0
};

// Byte-size contract a VstMidiEvent carries in VstEvent::byteSize (24 even
// though the struct itself is 32 bytes on both arches).
static_assert(sizeof(VstMidiEvent) == 32, "VstMidiEvent size mismatch");
static_assert(offsetof(VstMidiEvent, midiData) == 24, "VstMidiEvent.midiData mismatch");

struct VstMidiSysexEvent
{
	VstInt32 type;
	VstInt32 byteSize;
	VstInt32 deltaFrames;
	VstInt32 flags;
	VstInt32 dumpBytes;
	VstIntPtr resvd1;
	char *sysexDump;
	VstIntPtr resvd2;
};

static_assert(sizeof(VstEvent) == 32, "VstEvent size mismatch");
static_assert(sizeof(VstEvents) == (sizeof(void *) == 8 ? 32 : 16), "VstEvents size mismatch");
static_assert(offsetof(VstEvents, events) == (sizeof(void *) == 8 ? 16 : 8), "VstEvents.events mismatch");
static_assert(sizeof(VstMidiSysexEvent) == (sizeof(void *) == 8 ? 48 : 32), "VstMidiSysexEvent size mismatch");
static_assert(offsetof(VstMidiSysexEvent, sysexDump) == (sizeof(void *) == 8 ? 32 : 24), "VstMidiSysexEvent.sysexDump mismatch");

// ---------------------------------------------------------------------------
// Transport / time info (audioMasterGetTime)
// ---------------------------------------------------------------------------

struct VstTimeInfo
{
	double samplePos;             // current position in samples
	double sampleRate;            // e.g. 44100.0
	double nanoSeconds;           // nanoseconds of the day
	double ppqPos;                // musical position in quarter notes
	double tempo;                 // BPM
	double barStartPos;           // ppq position of the current bar start
	double cycleStartPos;         // ppq loop start
	double cycleEndPos;           // ppq loop end
	VstInt32 timeSigNumerator;
	VstInt32 timeSigDenominator;
	VstInt32 smpteOffset;
	VstInt32 smpteFrameRate;      // VstSmpteFrameRate
	VstInt32 samplesToNextClock;  // to next MIDI clock (24 ppq), may be negative
	VstInt32 flags;               // VstTimeInfoFlags
};

static_assert(sizeof(VstTimeInfo) == 88, "VstTimeInfo size mismatch");

enum VstTimeInfoFlags
{
	kVstTransportChanged = 1,
	kVstTransportPlaying = 1 << 1,
	kVstTransportCycleActive = 1 << 2,
	kVstTransportRecording = 1 << 3,
	kVstAutomationWriting = 1 << 6,
	kVstAutomationReading = 1 << 7,
	kVstNanosValid = 1 << 8,
	kVstPpqPosValid = 1 << 9,
	kVstTempoValid = 1 << 10,
	kVstBarsValid = 1 << 11,
	kVstCyclePosValid = 1 << 12,
	kVstTimeSigValid = 1 << 13,
	kVstSmpteValid = 1 << 14,
	kVstClockValid = 1 << 15
};

enum VstSmpteFrameRate
{
	kVstSmpte24fps = 0,
	kVstSmpte25fps = 1,
	kVstSmpte2997fps = 2,
	kVstSmpte30fps = 3,
	kVstSmpte2997dfps = 4,
	kVstSmpte30dfps = 5,
	kVstSmpteFilm16mm = 6,
	kVstSmpteFilm35mm = 7,
	kVstSmpte239fps = 10,
	kVstSmpte249fps = 11,
	kVstSmpte599fps = 12,
	kVstSmpte60fps = 13
};

// Variable IO block for effProcessVarIo (offline processing).
struct VstVariableIo
{
	float **inputs;
	float **outputs;
	VstInt32 numSamplesInput;
	VstInt32 numSamplesOutput;
	VstInt32 *numSamplesInputProcessed;
	VstInt32 *numSamplesOutputProcessed;
};

// Host language code returned by audioMasterGetLanguage.
enum VstHostLanguage
{
	kVstLangEnglish = 1,
	kVstLangGerman = 2,
	kVstLangFrench = 3,
	kVstLangItalian = 4,
	kVstLangSpanish = 5,
	kVstLangJapanese = 6
};

// ---------------------------------------------------------------------------
// Extended dispatcher opcodes (host -> plug-in), continuing AEffectOpcodes.
// Every value written out explicitly — the number space is the interface.
// ---------------------------------------------------------------------------
enum AEffectXOpcodes
{
	effProcessEvents = 25,
	effCanBeAutomated = 26,
	effString2Parameter = 27,

	effGetProgramNameIndexed = 29,

	effGetInputProperties = 33,
	effGetOutputProperties = 34,
	effGetPlugCategory = 35,

	// VST2.3 offline trio: the SDK keeps these under their PLAIN names even
	// with VST_FORCE_DEPRECATED (iPlug2's opcode logger references them).
	effOfflineNotify = 38,
	effOfflinePrepare = 39,
	effOfflineRun = 40,

	effProcessVarIo = 41,
	effSetSpeakerArrangement = 42,

	effSetBypass = 44,
	effGetEffectName = 45,
	effGetVendorString = 47,
	effGetProductString = 48,
	effGetVendorVersion = 49,
	effVendorSpecific = 50,
	effCanDo = 51,
	effGetTailSize = 52,

	effGetParameterProperties = 56,

	effGetVstVersion = 58,

	effEditKeyDown = 59,
	effEditKeyUp = 60,
	effSetEditKnobMode = 61,

	effGetMidiProgramName = 62,
	effGetCurrentMidiProgram = 63,
	effGetMidiProgramCategory = 64,
	effHasMidiProgramsChanged = 65,
	effGetMidiKeyName = 66,

	effBeginSetProgram = 67,
	effEndSetProgram = 68,

	effGetSpeakerArrangement = 69,
	effShellGetNextPlugin = 70,

	effStartProcess = 71,
	effStopProcess = 72,

	effSetPanLaw = 74,

	effBeginLoadBank = 75,
	effBeginLoadProgram = 76,

	effSetProcessPrecision = 77,
	effGetNumMidiInputChannels = 78,
	effGetNumMidiOutputChannels = 79
};

// Deprecated entries of the extended opcode range; they consume their numbers
// in the shared dispatcher space and carry the `__nameDeprecated` spelling.
enum AEffectXOpcodesDeprecated
{
	DECLARE_VST_DEPRECATED(effGetNumProgramCategories) = 28,

	DECLARE_VST_DEPRECATED(effCopyProgram) = 30,
	DECLARE_VST_DEPRECATED(effConnectInput) = 31,
	DECLARE_VST_DEPRECATED(effConnectOutput) = 32,

	DECLARE_VST_DEPRECATED(effGetCurrentPosition) = 36,
	DECLARE_VST_DEPRECATED(effGetDestinationBuffer) = 37,

	DECLARE_VST_DEPRECATED(effSetBlockSizeAndSampleRate) = 43,

	DECLARE_VST_DEPRECATED(effGetErrorText) = 46,

	DECLARE_VST_DEPRECATED(effIdle) = 53,
	DECLARE_VST_DEPRECATED(effGetIcon) = 54,
	DECLARE_VST_DEPRECATED(effSetViewPosition) = 55,

	DECLARE_VST_DEPRECATED(effKeysRequired) = 57,

	DECLARE_VST_DEPRECATED(effSetTotalSampleToProcess) = 73
};

// Symbolic precision constants for effSetProcessPrecision.
enum VstProcessPrecision
{
	kVstProcessPrecision32 = 0,
	kVstProcessPrecision64 = 1
};

// Process levels returned by audioMasterGetCurrentProcessLevel.
enum VstProcessLevels
{
	kVstProcessLevelUnknown = 0,
	kVstProcessLevelUser = 1,     // user thread (GUI)
	kVstProcessLevelRealtime = 2, // audio thread
	kVstProcessLevelPrefetch = 3,
	kVstProcessLevelOffline = 4
};

// Automation states returned by audioMasterGetAutomationState. The number
// space starts at Unsupported — a host that answers 0 means "unsupported",
// not "off"; mislabelling this range misreads transport state.
enum VstAutomationStates
{
	kVstAutomationUnsupported = 0,
	kVstAutomationOff = 1,
	kVstAutomationRead = 2,
	kVstAutomationWrite = 3,
	kVstAutomationReadWrite = 4
};

enum VstPanLawType
{
	kLinearPanLaw = 0,
	kEqualPowerPanLaw = 1
};

// ---------------------------------------------------------------------------
// Offline rendering option flags (audioMasterOffline*). The VST2.3 offline
// TASK structs are deliberately not declared (this plug-in never renders
// offline through the host); the flag spaces are completed here because
// getChunk/offline enums share documented numbers.
// ---------------------------------------------------------------------------

enum VstOfflineTaskFlags
{
	kVstOfflineUnvalidParameter = 1 << 0, // set by host
	kVstOfflineNewFile = 1 << 1,          // set by host

	kVstOfflinePlugError = 1 << 10,       // set by plug-in
	kVstOfflineInterleavedAudio = 1 << 11,
	kVstOfflineTempOutputFile = 1 << 12,
	kVstOfflineFloatOutputFile = 1 << 13,
	kVstOfflineRandomWrite = 1 << 14,
	kVstOfflineStretch = 1 << 15,
	kVstOfflineNoThread = 1 << 16
};

// Option passed to audioMasterOfflineRead / OfflineWrite.
enum VstOfflineOption
{
	kVstOfflineAudio = 0,
	kVstOfflinePeaks = 1,
	kVstOfflineParameter = 2,
	kVstOfflineMarker = 3,
	kVstOfflineCursor = 4,
	kVstOfflineSelection = 5,
	kVstOfflineQueryFiles = 6
};

enum VstAudioFileFlags
{
	kVstOfflineReadOnly = 1 << 0,           // set by host
	kVstOfflineNoRateConversion = 1 << 1,   // set by host
	kVstOfflineNoChannelChange = 1 << 2,    // set by host

	kVstOfflineCanProcessSelection = 1 << 10, // set by plug-in
	kVstOfflineNoCrossfade = 1 << 11,
	kVstOfflineWantRead = 1 << 12,
	kVstOfflineWantWrite = 1 << 13,
	kVstOfflineWantWriteMarker = 1 << 14,
	kVstOfflineWantMoveCursor = 1 << 15,
	kVstOfflineWantSelect = 1 << 16
};

// ---------------------------------------------------------------------------
// File selector (audioMasterOpenFileSelector)
// ---------------------------------------------------------------------------

enum VstFileSelectCommand
{
	kVstFileLoad = 0,
	kVstFileSave = 1,
	kVstMultipleFilesLoad = 2,
	kVstDirectorySelect = 3
};

enum VstFileSelectType
{
	kVstFileType = 0 // regular file selector
};

struct VstFileType
{
	char name[128];
	char macType[8];
	char dosType[8];
	char unixType[8];
	char mimeType1[128];
	char mimeType2[128];
};

static_assert(sizeof(VstFileType) == 408, "VstFileType size mismatch");

struct VstFileSelect
{
	VstInt32 command;          // VstFileSelectCommand
	VstInt32 type;             // VstFileSelectType
	VstInt32 macCreator;       // optional: 0 = no creator
	VstInt32 nbFileTypes;
	VstFileType *fileTypes;
	char title[1024];
	char *initialPath;
	char *returnPath;          // null: host allocates; caller must CloseFileSelector
	VstInt32 sizeReturnPath;
	char **returnMultiplePaths;
	VstInt32 nbReturnPath;
	VstIntPtr reserved;        // host-internal

	char future[116];
};

static_assert(sizeof(VstFileSelect) == (sizeof(void *) == 8 ? 1216 : 1184),
              "VstFileSelect size mismatch");
static_assert(offsetof(VstFileSelect, title) == (sizeof(void *) == 8 ? 24 : 20),
              "VstFileSelect.title mismatch");
static_assert(offsetof(VstFileSelect, future) == (sizeof(void *) == 8 ? 1096 : 1068),
              "VstFileSelect.future mismatch");

// ---------------------------------------------------------------------------
// Extended plug-in -> host opcodes, continuing AudioMasterOpcodes.
// ---------------------------------------------------------------------------
enum AudioMasterOpcodesX
{
	audioMasterGetTime = 7,
	audioMasterProcessEvents = 8,

	audioMasterIOChanged = 13,

	audioMasterSizeWindow = 15,
	audioMasterGetSampleRate = 16,
	audioMasterGetBlockSize = 17,
	audioMasterGetInputLatency = 18,
	audioMasterGetOutputLatency = 19,

	audioMasterGetCurrentProcessLevel = 23,
	audioMasterGetAutomationState = 24,

	audioMasterOfflineStart = 25,
	audioMasterOfflineRead = 26,
	audioMasterOfflineWrite = 27,
	audioMasterOfflineGetCurrentPass = 28,
	audioMasterOfflineGetCurrentMetaPass = 29,

	audioMasterGetVendorString = 32,
	audioMasterGetProductString = 33,
	audioMasterGetVendorVersion = 34,
	audioMasterVendorSpecific = 35,

	audioMasterCanDo = 37,
	audioMasterGetLanguage = 38,

	audioMasterGetDirectory = 41,
	audioMasterUpdateDisplay = 42,
	audioMasterBeginEdit = 43,
	audioMasterEndEdit = 44,
	audioMasterOpenFileSelector = 45,
	audioMasterCloseFileSelector = 46
};

// Deprecated entries of the extended master opcode range.
enum AudioMasterOpcodesXDeprecated
{
	DECLARE_VST_DEPRECATED(audioMasterWantMidi) = 6, // = audioMasterPinConnected + 2

	DECLARE_VST_DEPRECATED(audioMasterSetTime) = 9,
	DECLARE_VST_DEPRECATED(audioMasterTempoAt) = 10,
	DECLARE_VST_DEPRECATED(audioMasterGetNumAutomatableParameters) = 11,
	DECLARE_VST_DEPRECATED(audioMasterGetParameterQuantization) = 12,

	DECLARE_VST_DEPRECATED(audioMasterNeedIdle) = 14,

	DECLARE_VST_DEPRECATED(audioMasterGetPreviousPlug) = 20,
	DECLARE_VST_DEPRECATED(audioMasterGetNextPlug) = 21,
	DECLARE_VST_DEPRECATED(audioMasterWillReplaceOrAccumulate) = 22,

	DECLARE_VST_DEPRECATED(audioMasterSetOutputSampleRate) = 30,
	DECLARE_VST_DEPRECATED(audioMasterGetOutputSpeakerArrangement) = 31,

	DECLARE_VST_DEPRECATED(audioMasterSetIcon) = 36,

	DECLARE_VST_DEPRECATED(audioMasterOpenWindow) = 39,
	DECLARE_VST_DEPRECATED(audioMasterCloseWindow) = 40,

	DECLARE_VST_DEPRECATED(audioMasterEditFile) = 47,
	DECLARE_VST_DEPRECATED(audioMasterGetChunkFile) = 48,
	DECLARE_VST_DEPRECATED(audioMasterGetInputSpeakerArrangement) = 49
};

// ---------------------------------------------------------------------------
// Parameter / pin properties
// ---------------------------------------------------------------------------

enum VstMaxExtendedStringConstants
{
	kVstMaxNameLen = 64,
	kVstMaxLabelLen = 64,
	kVstMaxShortLabelLen = 8,
	kVstMaxCategLabelLen = 24,
	kVstMaxFileNameLen = 100
};

struct VstParameterProperties
{
	float stepFloat;
	float smallStepFloat;
	float largeStepFloat;
	char label[kVstMaxLabelLen];
	VstInt32 flags;
	VstInt32 minInteger;
	VstInt32 maxInteger;
	VstInt32 stepInteger;
	VstInt32 largeStepInteger;
	char shortLabel[kVstMaxShortLabelLen];
	VstInt16 displayIndex;
	VstInt16 category;
	VstInt16 numParametersInCategory;
	VstInt16 reserved;
	char categoryLabel[kVstMaxCategLabelLen];
	char future[16];
};

// 12 floats/ints head + label(64) + 4 ints(20->96) + shortLabel(8) + 4 shorts(8)
// + categoryLabel(24) + future(16) = 152, identical on both arches (no 8-byte
// members; VstIntPtr appears nowhere in this struct).
static_assert(sizeof(VstParameterProperties) == 152, "VstParameterProperties size mismatch");

enum VstParameterFlags
{
	kVstParameterIsSwitch = 1 << 0,
	kVstParameterUsesIntegerMinMax = 1 << 1,
	kVstParameterUsesFloatStep = 1 << 2,
	kVstParameterUsesIntStep = 1 << 3,
	kVstParameterSupportsDisplayIndex = 1 << 4,
	kVstParameterSupportsDisplayCategory = 1 << 5,
	kVstParameterCanRamp = 1 << 6
};

struct VstPinProperties
{
	char label[kVstMaxLabelLen];
	VstInt32 flags;
	VstInt32 arrangementType; // VstSpeakerArrangementType
	char shortLabel[kVstMaxShortLabelLen];
	char future[48];
};

static_assert(sizeof(VstPinProperties) == 128, "VstPinProperties size mismatch");

enum VstPinPropertiesFlags
{
	kVstPinIsActive = 1 << 0,
	kVstPinIsStereo = 1 << 1,
	kVstPinUseSpeaker = 1 << 2
};

// ---------------------------------------------------------------------------
// Plug-in categories
// ---------------------------------------------------------------------------
enum VstPlugCategory
{
	kPlugCategUnknown = 0,
	kPlugCategEffect = 1,
	kPlugCategSynth = 2,
	kPlugCategAnalysis = 3,
	kPlugCategMastering = 4,
	kPlugCategSpacializer = 5,
	kPlugCategRoomFx = 6,
	kPlugSurroundFx = 7,
	kPlugCategRestoration = 8,
	kPlugCategOfflineProcess = 9,
	kPlugCategShell = 10,
	kPlugCategGenerator = 11,
	kPlugCategMaxCount = 12
};

// ---------------------------------------------------------------------------
// MIDI program / key tables (effGetMidiProgramName & friends)
// ---------------------------------------------------------------------------

struct MidiProgramName
{
	VstInt32 thisProgramIndex;
	char name[kVstMaxNameLen];
	char midiProgram;
	char midiBankMsb;
	char midiBankLsb;
	char reserved;
	VstInt32 parentCategoryIndex;
	VstInt32 flags; // VstMidiProgramNameFlags
};

static_assert(sizeof(MidiProgramName) == 80, "MidiProgramName size mismatch");

enum VstMidiProgramNameFlags
{
	kMidiIsOmni = 1
};

struct MidiProgramCategory
{
	VstInt32 thisCategoryIndex;
	char name[kVstMaxNameLen];
	VstInt32 parentCategoryIndex;
	VstInt32 flags;
};

static_assert(sizeof(MidiProgramCategory) == 76, "MidiProgramCategory size mismatch");

struct MidiKeyName
{
	VstInt32 thisProgramIndex;
	VstInt32 thisKeyNumber;
	char keyName[kVstMaxNameLen];
	VstInt32 reserved;
	VstInt32 flags;
};

static_assert(sizeof(MidiKeyName) == 80, "MidiKeyName size mismatch");

// ---------------------------------------------------------------------------
// Surround setup
// ---------------------------------------------------------------------------

struct VstSpeakerProperties
{
	float azimuth;    // radians, origin right (0)
	float elevation;  // radians
	float radius;     // 0..1
	float reserved;
	char name[kVstMaxNameLen];
	VstInt32 type; // VstSpeakerType / VstUserSpeakerType
	char future[28];
};

static_assert(sizeof(VstSpeakerProperties) == 112, "VstSpeakerProperties size mismatch");

struct VstSpeakerArrangement
{
	VstInt32 type; // VstSpeakerArrangementType
	VstInt32 numChannels;
	VstSpeakerProperties speakers[8];
};

static_assert(sizeof(VstSpeakerArrangement) == 904, "VstSpeakerArrangement size mismatch");

enum VstSpeakerType
{
	kSpeakerUndefined = 0x7fffffff,
	kSpeakerM = 0,
	kSpeakerL = 1,
	kSpeakerR = 2,
	kSpeakerC = 3,
	kSpeakerLfe = 4,
	kSpeakerLs = 5,
	kSpeakerRs = 6,
	kSpeakerLc = 7,
	kSpeakerRc = 8,
	kSpeakerS = 9,
	kSpeakerCs = kSpeakerS,
	kSpeakerSl = 10,
	kSpeakerSr = 11,
	kSpeakerTm = 12,
	kSpeakerTfl = 13,
	kSpeakerTfc = 14,
	kSpeakerTfr = 15,
	kSpeakerTrl = 16,
	kSpeakerTrc = 17,
	kSpeakerTrr = 18,
	kSpeakerLfe2 = 19
};

// Negative-range user speakers; |n| maps to the speaker type with that index.
enum VstUserSpeakerType
{
	kSpeakerU32 = -32,
	kSpeakerU31 = -31,
	kSpeakerU30 = -30,
	kSpeakerU29 = -29,
	kSpeakerU28 = -28,
	kSpeakerU27 = -27,
	kSpeakerU26 = -26,
	kSpeakerU25 = -25,
	kSpeakerU24 = -24,
	kSpeakerU23 = -23,
	kSpeakerU22 = -22,
	kSpeakerU21 = -21,
	kSpeakerU20 = -20,
	kSpeakerU19 = -19,
	kSpeakerU18 = -18,
	kSpeakerU17 = -17,
	kSpeakerU16 = -16,
	kSpeakerU15 = -15,
	kSpeakerU14 = -14,
	kSpeakerU13 = -13,
	kSpeakerU12 = -12,
	kSpeakerU11 = -11,
	kSpeakerU10 = -10,
	kSpeakerU9 = -9,
	kSpeakerU8 = -8,
	kSpeakerU7 = -7,
	kSpeakerU6 = -6,
	kSpeakerU5 = -5,
	kSpeakerU4 = -4,
	kSpeakerU3 = -3,
	kSpeakerU2 = -2,
	kSpeakerU1 = -1
};

enum VstSpeakerArrangementType
{
	kSpeakerArrUserDefined = -2,
	kSpeakerArrEmpty = -1,
	kSpeakerArrMono = 0,
	kSpeakerArrStereo = 1,
	kSpeakerArrStereoSurround = 2,
	kSpeakerArrStereoCenter = 3,
	kSpeakerArrStereoSide = 4,
	kSpeakerArrStereoCLfe = 5,
	kSpeakerArr30Cine = 6,
	kSpeakerArr30Music = 7,
	kSpeakerArr31Cine = 8,
	kSpeakerArr31Music = 9,
	kSpeakerArr40Cine = 10,
	kSpeakerArr40Music = 11,
	kSpeakerArr41Cine = 12,
	kSpeakerArr41Music = 13,
	kSpeakerArr50 = 14,
	kSpeakerArr51 = 15,
	kSpeakerArr60Cine = 16,
	kSpeakerArr60Music = 17,
	kSpeakerArr61Cine = 18,
	kSpeakerArr61Music = 19,
	kSpeakerArr70Cine = 20,
	kSpeakerArr70Music = 21,
	kSpeakerArr71Cine = 22,
	kSpeakerArr71Music = 23,
	kSpeakerArr80Cine = 24,
	kSpeakerArr80Music = 25,
	kSpeakerArr81Cine = 26,
	kSpeakerArr81Music = 27,
	kSpeakerArr102 = 28,
	kNumSpeakerArr = 29
};

// ---------------------------------------------------------------------------
// Editor keyboard input (effEditKeyDown / effEditKeyUp)
// ---------------------------------------------------------------------------

struct VstKeyCode
{
	VstInt32 character;
	unsigned char virt;     // VstVirtualKey
	unsigned char modifier; // VstModifierKey
};

static_assert(sizeof(VstKeyCode) == 8, "VstKeyCode size mismatch");

enum VstVirtualKey
{
	VKEY_BACK = 1,
	VKEY_TAB = 2,
	VKEY_CLEAR = 3,
	VKEY_RETURN = 4,
	VKEY_PAUSE = 5,
	VKEY_ESCAPE = 6,
	VKEY_SPACE = 7,
	VKEY_NEXT = 8,
	VKEY_END = 9,
	VKEY_HOME = 10,
	VKEY_LEFT = 11,
	VKEY_UP = 12,
	VKEY_RIGHT = 13,
	VKEY_DOWN = 14,
	VKEY_PAGEUP = 15,
	VKEY_PAGEDOWN = 16,
	VKEY_SELECT = 17,
	VKEY_PRINT = 18,
	VKEY_ENTER = 19,
	VKEY_SNAPSHOT = 20,
	VKEY_INSERT = 21,
	VKEY_DELETE = 22,
	VKEY_HELP = 23,
	VKEY_NUMPAD0 = 24,
	VKEY_NUMPAD1 = 25,
	VKEY_NUMPAD2 = 26,
	VKEY_NUMPAD3 = 27,
	VKEY_NUMPAD4 = 28,
	VKEY_NUMPAD5 = 29,
	VKEY_NUMPAD6 = 30,
	VKEY_NUMPAD7 = 31,
	VKEY_NUMPAD8 = 32,
	VKEY_NUMPAD9 = 33,
	VKEY_MULTIPLY = 34,
	VKEY_ADD = 35,
	VKEY_SEPARATOR = 36,
	VKEY_SUBTRACT = 37,
	VKEY_DECIMAL = 38,
	VKEY_DIVIDE = 39,
	VKEY_F1 = 40,
	VKEY_F2 = 41,
	VKEY_F3 = 42,
	VKEY_F4 = 43,
	VKEY_F5 = 44,
	VKEY_F6 = 45,
	VKEY_F7 = 46,
	VKEY_F8 = 47,
	VKEY_F9 = 48,
	VKEY_F10 = 49,
	VKEY_F11 = 50,
	VKEY_F12 = 51,
	VKEY_NUMLOCK = 52,
	VKEY_SCROLL = 53,
	VKEY_SHIFT = 54,
	VKEY_CONTROL = 55,
	VKEY_ALT = 56,
	VKEY_EQUALS = 57
};

enum VstModifierKey
{
	MODIFIER_SHIFT = 1 << 0,
	MODIFIER_ALTERNATE = 1 << 1,
	MODIFIER_COMMAND = 1 << 2,
	MODIFIER_CONTROL = 1 << 3
};

#if defined(_WIN32)
#pragma pack(pop)
#endif

#endif // SMU2000_VST2_COMPAT_EXTENDED_H

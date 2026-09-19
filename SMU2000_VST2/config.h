// config.h — SMU2000_VST2 iPlug2 plugin config (ported from ../sw10_plug SW10_PLUG/config.h).
// PLUG_HAS_UI is owned by the SMU2000_ENABLE_GUI toggle (P4): the CMake build passes
// -DSMU2000_ENABLE_GUI (and thus may pre-define PLUG_HAS_UI) — the guard below keeps the
// OFF default (0) for plain compiles. Do not hardcode 0/1 unconditionally.

#define PLUG_NAME "SMU2000_VST2"
#define PLUG_MFR "tarboh"
#define PLUG_VERSION_HEX 0x00000000
#define PLUG_VERSION_STR "0.0.1"
#define PLUG_UNIQUE_ID 'SMU2'
#define PLUG_MFR_ID 'tarb'
#define PLUG_URL_STR "https://github.com/tarboh/S-MU2000"
#define PLUG_EMAIL_STR "spam@me.com"
#define PLUG_COPYRIGHT_STR "(C) the S-MU2000 authors"
#define PLUG_CLASS_NAME SMU2000_VST2

#define BUNDLE_NAME "SMU2000_VST2"
#define BUNDLE_MFR "tarboh"
#define BUNDLE_DOMAIN "com"

#define PLUG_CHANNEL_IO "0-2"
#define SHARED_RESOURCES_SUBPATH "SMU2000_VST2"

#define PLUG_LATENCY 0
#define PLUG_TYPE 1
#define PLUG_DOES_MIDI_IN 1
#define PLUG_DOES_MIDI_OUT 1
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 1
#if !defined(PLUG_HAS_UI)
#if defined(SMU2000_ENABLE_GUI)
#define PLUG_HAS_UI 1
#else
#define PLUG_HAS_UI 0
#endif
#endif
// Editor is the native GDI panel (ui::panel), logical 1000x400 = 2.5:1. The window
// tracks that ratio (WM_GETMINMAXINFO in ui/SMU2000Editor.cpp); PLUG_HOST_RESIZE stays 0.
#define PLUG_WIDTH 1250
#define PLUG_HEIGHT 500
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 0

#define VST3_SUBCATEGORY "Instrument|Synth"
#define CLAP_MANUAL_URL "https://github.com/tarboh/S-MU2000"
#define CLAP_SUPPORT_URL "https://github.com/tarboh/S-MU2000/issues"
#define CLAP_DESCRIPTION "Yamaha MU2000 (SWP30) software synth"
#define CLAP_FEATURES "instrument"

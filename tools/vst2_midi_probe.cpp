// Headless VST2 MIDI probe — feeds effProcessEvents + processReplacing and reports
// output peak, to catch "MIDI no longer read" regressions in the plug-in's MIDI queue.
// Build (x64): vcvars64 && cl /O2 /std:c++17 /EHsc vst2_midi_probe.cpp /I <vstsdk> /Fe:vst2_midi_probe.exe
// Usage: vst2_midi_probe.exe [dll] [blocks(512)]
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include "aeffect.h"
#include "aeffectx.h"

typedef VstIntPtr (VSTCALLBACK *MasterCallback)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
typedef AEffect*  (VSTCALLBACK *MainEntry)(MasterCallback);

static VstIntPtr VSTCALLBACK master(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{
  switch (op)
  {
    case audioMasterVersion: return 0x2400;
    case audioMasterGetSampleRate: return 44100;
    case audioMasterGetBlockSize: return 512;
    default: return 0;
  }
}

// This SDK's VstEvents has events[2] inline (variable-size); over-allocate the tail.
struct EventsBuf { VstInt32 numEvents; VstIntPtr reserved; VstEvent* ev[16]; };

static void add_midi(EventsBuf& buf, int n, int delta, int b0, int b1, int b2)
{
  VstMidiEvent* me = (VstMidiEvent*) calloc(1, sizeof(VstMidiEvent));
  me->type = kVstMidiType;
  me->byteSize = (VstInt32) sizeof(VstMidiEvent);
  me->deltaFrames = delta;
  me->midiData[0] = b0; me->midiData[1] = b1; me->midiData[2] = b2;
  buf.ev[n] = (VstEvent*) me;
}

static void send(AEffect* a, EventsBuf& buf, int count)
{
  buf.numEvents = count; buf.reserved = 0;
  a->dispatcher(a, effProcessEvents, 0, 0, (VstEvents*) &buf, 0.f);
}

int main(int argc, char** argv)
{
  const char* dll = (argc > 1) ? argv[1]
    : "D:\\Projects\\vst\\S-MU2000-vst2-iplug\\build-cmake\\vst2\\x64\\Release\\SMU2000_VST2.dll";
  const int nblocks = (argc > 2) ? atoi(argv[2]) : 600;  // 600*512/44100 ≈ 7 s

  HINSTANCE mod = LoadLibraryA(dll);
  if (!mod) { printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
  MainEntry entry = (MainEntry) GetProcAddress(mod, "VSTPluginMain");
  if (!entry) { printf("no VSTPluginMain\n"); return 2; }
  AEffect* a = entry(master);
  if (!a) { printf("null AEffect\n"); return 2; }
  printf("AEffect magic=%08X\n", a->magic); fflush(stdout);

  a->dispatcher(a, effOpen, 0, 0, nullptr, 0.f);
  a->dispatcher(a, effSetSampleRate, 0, 0, nullptr, 44100.f);
  a->dispatcher(a, effSetBlockSize, 512, 0, nullptr, 0.f);
  a->dispatcher(a, effMainsChanged, 1, 0, nullptr, 0.f);

  EventsBuf buf{}; buf.numEvents = 0; buf.reserved = 0;

  const int BS = 512;
  std::vector<float> L(BS, 0.f), R(BS, 0.f);
  float* outs[2] = { L.data(), R.data() };
  double peak = 0.0; int peak_blk = -1;
  double pre_peak = 0.0;

  for (int blk = 0; blk < nblocks; ++blk)
  {
    if (blk == 4)   // bank/piano on ch0
    {
      add_midi(buf, 0, 0, 0xB0, 0, 0);    // CC0 bank MSB
      add_midi(buf, 1, 1, 0xB0, 32, 0);   // CC32 bank LSB
      add_midi(buf, 2, 2, 0xC0, 0, 0);    // PC piano
      send(a, buf, 3);
    }
    if (blk == 16)  // two notes, second mid-block — exercises segmented fill
    {
      add_midi(buf, 0, 0, 0x90, 60, 100);
      add_midi(buf, 1, 256, 0x90, 67, 90);
      send(a, buf, 2);
    }
    if (blk == 400)
    {
      add_midi(buf, 0, 0, 0x80, 60, 0);
      add_midi(buf, 1, 0, 0x80, 67, 0);
      send(a, buf, 2);
    }

    double bp = 0.0;
    a->processReplacing(a, nullptr, outs, BS);
    for (int i = 0; i < BS; ++i)
    {
      double m = std::fabs(L[i]) > std::fabs(R[i]) ? std::fabs(L[i]) : std::fabs(R[i]);
      if (m > bp) bp = m;
    }
    if (blk < 15) pre_peak = pre_peak < bp ? bp : pre_peak;
    if (bp > peak) { peak = bp; peak_blk = blk; }
    if (blk == 16 || blk == 20 || blk == 100)
      printf("blk %3d peak=%.6f\n", blk, bp), fflush(stdout);
  }

  printf("pre-note peak=%.6f  post-note peak=%.6f @blk %d\n", pre_peak, peak, peak_blk);
  bool pass = peak > 1e-3 && peak_blk > 16;
  printf("\nRESULT: %s\n", pass ? "PASS — MIDI audible" : "FAIL — MIDI silent / not read");
  return pass ? 0 : 1;
}

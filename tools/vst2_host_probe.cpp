// Native VST2 host probe — validates the SMU2000 GUI-ON editor attaches/paints/detaches.
// Build with vcvars64 + cl. Usage: host.exe <path-to-vst2.dll>
#include <windows.h>
#include <cstdio>
#include "aeffect.h"
// ERect is fully defined by aeffect.h in both the real SDK (2.4) and the
// clean-room compat headers — no aeditel.h needed.

typedef VstIntPtr (VSTCALLBACK *MasterCallback)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
typedef AEffect*  (VSTCALLBACK *MainEntry)(MasterCallback);

static HINSTANCE g_mod;
static AEffect*  g_a;
static const char* g_cls = "SMU2000IPlugView";
static int g_pump_ms = 2500;

static VstIntPtr VSTCALLBACK master(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float) { return 0; }

static LRESULT CALLBACK hostWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  return DefWindowProc(h, m, w, l);
}

int main(int argc, char** argv) {
  const char* dll = (argc > 1) ? argv[1]
    : "D:\\Projects\\vst\\S-MU2000-vst2-iplug\\build-cmake\\vst2\\x64\\Release\\SMU2000_VST2.dll";
  if (argc > 2) g_pump_ms = atoi(argv[2]);

  g_mod = LoadLibraryA(dll);
  if (!g_mod) { printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
  MainEntry entry = (MainEntry)GetProcAddress(g_mod, "VSTPluginMain");
  if (!entry) { printf("no VSTPluginMain\n"); return 2; }

  g_a = entry(master);
  if (!g_a) { printf("entry returned null\n"); return 2; }
  printf("AEffect=%p magic=%08X numOutputs=%d uniqueID=%08X\n",
         (void*)g_a, g_a->magic, g_a->numOutputs, g_a->uniqueID);
  fflush(stdout);

  g_a->dispatcher(g_a, effOpen, 0, 0, nullptr, 0.f);

  // effEditGetRect -> ERect**
  ERect* rect = nullptr;
  VstIntPtr gr = g_a->dispatcher(g_a, effEditGetRect, 0, 0, &rect, 0.f);
  if (rect)
    printf("effEditGetRect ret=%lld ERect=(top=%d,left=%d,bottom=%d,right=%d) => %dx%d\n",
           (long long)gr, rect->top, rect->left, rect->bottom, rect->right,
           rect->right-rect->left, rect->bottom-rect->top);
  else
    printf("effEditGetRect ret=%lld rect=NULL (HasUI false?)\n", (long long)gr);
  fflush(stdout);

  WNDCLASSA wc{}; wc.lpfnWndProc = hostWndProc; wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "SMU2000ProbeHost"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassA(&wc);
  RECT wr{ 0,0,1250,500 }; AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
  HWND host = CreateWindowA("SMU2000ProbeHost", "SMU2000 native VST2 host",
                           WS_OVERLAPPEDWINDOW, 100,100,
                           wr.right-wr.left, wr.bottom-wr.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(host, SW_SHOW); UpdateWindow(host);

  VstIntPtr eo = g_a->dispatcher(g_a, effEditOpen, 0, 0, (void*)host, 0.f);
  printf("effEditOpen ret=%lld\n", (long long)eo); fflush(stdout);

  HWND kid = nullptr;
  for (int i=0; i<40 && !kid; ++i) { kid = FindWindowExA(host, nullptr, g_cls, nullptr); if(!kid) Sleep(50); }
  if (kid) {
    RECT kr; GetWindowRect(kid, &kr);
    printf("CHILD HWND=%p visible=%d %dx%d\n", (void*)kid, IsWindowVisible(kid),
           kr.right-kr.left, kr.bottom-kr.top); fflush(stdout);
  } else {
    printf("NO child panel window created\n"); fflush(stdout);
  }

  // pump ~g_pump_ms of messages + effEditIdle (drives child WM_TIMER paint/tick)
  ULONGLONG end = GetTickCount64() + (ULONGLONG)g_pump_ms;
  int ticks = 0;
  while (GetTickCount64() < end) {
    MSG msg; while (PeekMessageA(&msg, nullptr, 0,0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    g_a->dispatcher(g_a, effEditIdle, 0,0, nullptr, 0.f); ++ticks; Sleep(5);
  }
  bool alive = kid && IsWindow(kid);
  printf("pumped %d idles; child alive=%d\n", ticks, alive); fflush(stdout);

  g_a->dispatcher(g_a, effEditClose, 0,0, nullptr, 0.f);
  bool gone = !kid || !IsWindow(kid);
  printf("after effEditClose child destroyed=%d\n", gone); fflush(stdout);

  g_a->dispatcher(g_a, effClose, 0,0, nullptr, 0.f);
  DestroyWindow(host);
  FreeLibrary(g_mod);

  bool pass = (eo != 0) && (kid != nullptr) && alive && gone;
  printf("\nRESULT: %s\n", pass ? "PASS — native editor attaches, paints, detaches" : "FAIL");
  return pass ? 0 : 1;
}

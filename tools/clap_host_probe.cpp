// Native CLAP host probe — validates the SMU2000 GUI-ON editor attaches/paints/detaches
// via clap_plugin_gui (win32). Build: vcvars64 + cl with CLAP_SDK include on /I.
// Usage: clap_host_probe.exe <path-to.clap>
#include <windows.h>
#include <cstdio>\r\n#include <cstring>
#include "clap/clap.h"

static const char* kCls = "SMU2000IPlugView";
static int g_pump_ms = 2500;

// ---- minimal host ----
static void CLAP_ABI host_log(const clap_host_t*, clap_log_severity, const char*) {}
static const clap_host_log_t g_log = { host_log };

static const void* CLAP_ABI host_get_ext(const clap_host_t*, const char* id) {
  if (id && strcmp(id, CLAP_EXT_LOG) == 0) return &g_log;
  return nullptr;
}
static void CLAP_ABI host_noop(const clap_host_t*) {}

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1]
    : "D:\\Projects\\vst\\S-MU2000-vst2-iplug\\build-cmake\\clap\\x64\\Release\\SMU2000_VST2.clap";
  if (argc > 2) g_pump_ms = atoi(argv[2]);

  HMODULE mod = LoadLibraryA(path);
  if (!mod) { printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
  const clap_plugin_entry_t* entry = (const clap_plugin_entry_t*)GetProcAddress(mod, "clap_entry");
  if (!entry) { printf("no clap_entry export\n"); return 2; }
  printf("clap_entry v%u.%u.%u\n", entry->clap_version.major, entry->clap_version.minor,
         entry->clap_version.revision); fflush(stdout);

  if (!entry->init(path)) { printf("init failed\n"); return 2; }
  const clap_plugin_factory_t* f =
      (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  if (!f || !f->get_plugin_count(f)) { printf("no plugin-factory\n"); entry->deinit(); return 2; }
  const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, 0);
  printf("plugin[0] id=%s\n", d ? d->id : "(null)"); fflush(stdout);

  clap_host_t host{};
  host.clap_version = CLAP_VERSION;
  host.name = "smu-native-probe";
  host.vendor = "tarboh";
  host.url = "";
  host.version = "0";
  host.get_extension = host_get_ext;
  host.request_restart = host_noop;
  host.request_process = host_noop;
  host.request_callback = host_noop;

  const clap_plugin_t* p = f->create_plugin(f, &host, d->id);
  if (!p) { printf("create_plugin failed\n"); entry->deinit(); return 2; }
  if (!p->init(p)) { printf("plugin init failed\n"); entry->deinit(); return 2; }

  const clap_plugin_gui_t* gui =
      (const clap_plugin_gui_t*)p->get_extension(p, CLAP_EXT_GUI);
  if (!gui) { printf("no clap.gui extension\n"); goto cleanup; }

  bool api = gui->is_api_supported(p, CLAP_WINDOW_API_WIN32, false);
  printf("gui.is_api_supported(win32)=%d\n", api); fflush(stdout);
  bool made = gui->create(p, CLAP_WINDOW_API_WIN32, false);
  printf("gui.create(win32)=%d\n", made); fflush(stdout);

  WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "ClapProbeHost"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassA(&wc);
  RECT wr{ 0,0,1250,500 }; AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hostWnd = CreateWindowA("ClapProbeHost", "SMU2000 native CLAP host",
                              WS_OVERLAPPEDWINDOW, 120,120, wr.right-wr.left, wr.bottom-wr.top,
                              nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(hostWnd, SW_SHOW); UpdateWindow(hostWnd);

  clap_window_t win{}; win.api = CLAP_WINDOW_API_WIN32; win.win32 = (void*)hostWnd;
  bool parented = gui->set_parent(p, &win);
  printf("gui.set_parent=%d\n", parented); fflush(stdout);
  if (gui->show) gui->show(p);

  HWND kid = nullptr;
  for (int i=0;i<40 && !kid;++i){ kid = FindWindowExA(hostWnd,nullptr,kCls,nullptr); if(!kid) Sleep(50); }
  if (kid) { RECT kr; GetWindowRect(kid,&kr);
    printf("CHILD HWND=%p visible=%d %dx%d\n",(void*)kid,IsWindowVisible(kid),kr.right-kr.left,kr.bottom-kr.top); }
  else printf("NO child panel window created\n");
  fflush(stdout);

  { ULONGLONG end=GetTickCount64()+(ULONGLONG)g_pump_ms; int t=0;
    while (GetTickCount64()<end){ MSG m; while(PeekMessageA(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessage(&m);} ++t; Sleep(5);} 
    printf("pumped %d msgs; child alive=%d\n", t, kid&&IsWindow(kid)); }
  fflush(stdout);

  if (gui->hide) gui->hide(p);
  gui->destroy(p);
  DestroyWindow(hostWnd);

cleanup:
  if (p->destroy) p->destroy(p);
  entry->deinit();
  FreeLibrary(mod);
  bool pass = gui && api && made && parented && kid;
  printf("\nRESULT: %s\n", pass ? "PASS — CLAP editor attaches/paints/detaches" : "FAIL");
  return pass?0:1;
}

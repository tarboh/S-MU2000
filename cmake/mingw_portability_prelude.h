// mingw_portability_prelude.h — MinGW-only force-include (see cmake/mingw_compat.cmake).
//
// MSVC's SDK headers happen to pull a number of standard headers in transitively, so the
// iPlug2 / Cockos WDL / (and here) the SMU2000 engine sources compile on MSVC while omitting
// the explicit includes. libstdc++ does not, which surfaces as "std::unique_ptr is not a
// member of std", "M_PI was not declared", "clock_gettime", etc. Force-including this
// tiny prelude into every C++ translation unit fixes all of those in one place, without
// touching the iPlug2 submodule (the edits would be lost on a submodule checkout anyway).
//
// Guarded to GCC/Clang only; never included by the MSVC build.
// (Ported from ../sw10_plug/cmake/mingw_portability_prelude.h, SW10_* -> SMU2000_*.)
#if defined(__GNUC__) || defined(__clang__)
#ifndef SMU2000_MINGW_PORTABILITY_PRELUDE_H
#define SMU2000_MINGW_PORTABILITY_PRELUDE_H

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES  // M_PI / M_PI_2 from <cmath>/<math.h> under -std=c++20 (__STRICT_ANSI__)
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <string>
#include <memory>
#include <functional>
#include <algorithm>
#include <vector>
#include <array>

#if defined(__clang__) && (defined(_WIN32) || defined(__CYGWIN__))
// Clang (unlike GCC's -fpermissive) makes the FARPROC->void* conversion that iPlug's
// DPI-delay-load glue performs (  *(void **)&fn = GetProcAddress(h, "name");  ) a HARD
// error in -std=c++17/20 that no -Wno-* / -fpermissive / gnu++17 flag can downgrade. All the
// call sites we compile either already route through *(void**)& or use an explicit C-style
// (FuncType) cast, so casting GetProcAddress' result to void* here is safe and correct.
// The macro is defined only AFTER including <windows.h> so the header's own declaration of
// GetProcAddress is not clobbered (windows.h is include-guarded, so the real include that
// follows is a no-op). GCC/MinGW does not need this and is left on its proven -fpermissive path.
#include <windows.h>
#ifndef GetProcAddress
#define GetProcAddress(h, name) ((void*)::GetProcAddress((HMODULE)(h), (LPCSTR)(name)))
#endif
#endif

#endif  // SMU2000_MINGW_PORTABILITY_PRELUDE_H
#endif  // __GNUC__ || __clang__

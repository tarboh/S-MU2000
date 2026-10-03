// license:BSD-3-Clause
//
// The places the program looks for files of its own.
//
//   exe_dir()          next to the binary. This is where panel.txt, the VST3
//                      bundle and the ROM dump live when someone is working on
//                      a checkout rather than an install
//   config_dir()       the per-user settings directory, where gui.ini and a
//                      user-edited panel.txt are kept
//   shared_config_dir() the machine-wide counterpart, where a shared copy of
//                      the ROMs can live (read only -- see below)
//
// All come back with the trailing separator already on, so callers just
// append a file name. Any can be empty if the platform will not say.
//
// Windows: %LOCALAPPDATA%\S-MU2000   and  %ProgramData%\S-MU2000
// macOS:   ~/Library/Application Support/S-MU2000
//          /Library/Application Support/S-MU2000
//
// Windows is what this grew up on; the macOS answers are the same two ideas
// expressed the way that platform expects.

#ifndef S_MU2000_COMPAT_PATHS_H
#define S_MU2000_COMPAT_PATHS_H

#pragma once

#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <dlfcn.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  else
#    include <unistd.h>
#  endif
#  include <sys/stat.h>
#  include <climits>
#  include <cstdlib>
#  include <ctime>
#  include <vector>
#endif

namespace smu2000 {

namespace detail {

// Everything up to and including the last separator, or "" if there is none
inline std::string dir_of(const std::string &path)
{
	const size_t slash = path.find_last_of("\\/");
	return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
}

} // namespace detail

// The directory the running binary sits in, with a trailing separator
inline std::string exe_dir()
{
#if defined(_WIN32)
	char buf[MAX_PATH] = {};
	const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return {};
	return detail::dir_of(std::string(buf, n));
#elif !defined(__APPLE__)
	// Linux: the kernel keeps the running binary's path here, symlinks resolved
	char real[PATH_MAX] = {};
	const ssize_t n = ::readlink("/proc/self/exe", real, sizeof(real) - 1);
	if (n <= 0)
		return {};
	return detail::dir_of(std::string(real, size_t(n)));
#else
	// _NSGetExecutablePath may hand back a path with symlinks in it, so resolve
	// it before taking the directory: argv[0]-style paths are not enough once
	// the bundle is launched by Finder rather than from a shell
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> buf(size + 1, '\0');
	if (_NSGetExecutablePath(buf.data(), &size) != 0)
		return {};
	char real[PATH_MAX] = {};
	if (!::realpath(buf.data(), real))
		return detail::dir_of(std::string(buf.data()));
	return detail::dir_of(std::string(real));
#endif
}

// The per-user settings directory, with a trailing separator. Nothing is
// created by asking
inline std::string config_dir()
{
#if defined(_WIN32)
	const char *base = std::getenv("LOCALAPPDATA");
	if (!base || !*base)
		return {};
	return std::string(base) + "\\S-MU2000\\";
#elif !defined(__APPLE__)
	// Linux: where the desktop specification (XDG) puts a program's own data
	const char *data = std::getenv("XDG_DATA_HOME");
	if (data && *data)
		return std::string(data) + "/S-MU2000/";
	const char *home = std::getenv("HOME");
	if (!home || !*home)
		return {};
	return std::string(home) + "/.local/share/S-MU2000/";
#else
	// Inside a sandboxed AUv3 this is the one place the extension can reach
	// outside its own bundle: macOS points $HOME at the extension's container,
	// so this lands on ~/Library/Containers/<appex id>/Data/Library/
	// Application Support/S-MU2000 -- the same directory the engine already
	// writes log.txt and boot snapshots into. The container app puts the ROMs
	// there once (src/auv3/main_app.mm).
	const char *home = std::getenv("HOME");
	if (!home || !*home)
		return {};
	return std::string(home) + "/Library/Application Support/S-MU2000/";
#endif
}

// The machine-wide counterpart, with a trailing separator.
//
// Same idea as config_dir(), one level up: on macOS the location Apple gives
// shared application data (/Library/Application Support), on Windows the same
// thing (%ProgramData%). This is where a copy of the ROMs can live so that
// **every user on the machine, and every plug-in instance**, finds them without
// each one being told separately.
//
// Nothing here is written: creating it takes an installer with the rights to.
// It is only ever read.
//
// macOS:   /Library/Application Support/S-MU2000
// Linux:   /usr/local/share/S-MU2000
// Windows: %ProgramData%\S-MU2000
inline std::string shared_config_dir()
{
#if defined(_WIN32)
	const char *base = std::getenv("ProgramData");
	if (!base || !*base)
		return {};
	return std::string(base) + "\\S-MU2000\\";
#elif !defined(__APPLE__)
	return "/usr/local/share/S-MU2000/";
#else
	return "/Library/Application Support/S-MU2000/";
#endif
}

// Joins a relative name onto a directory. The relative part is written with
// forward slashes, which is what the sources use, and they are turned into the
// platform's separator as the last step
inline std::string join(const std::string &dir, const std::string &rel)
{
#if defined(_WIN32)
	const char sep = '\\';
#else
	const char sep = '/';
#endif
	std::string out = dir;
	while (!out.empty() && (out.back() == '/' || out.back() == '\\'))
		out.pop_back();
	if (rel.empty())
		return out;
	out += sep;
	for (char c : rel)
		out += (c == '/') ? sep : c;
	return out;
}

// An environment variable, or "" if it is not set
inline std::string env(const char *name)
{
	// Read the Win32 environment variable to avoid
	// incompatibilites with different C runtimes
	// (MSVCRT and UCRT)
#if defined(_WIN32)
	char buf[PATH_MAX] = {};
	const char *v = buf;
	DWORD ret = GetEnvironmentVariableA(name, buf, sizeof(buf));
	if (ret == 0) {
		buf[0] = '\0';
		v = nullptr;
	}
#else
	const char *v = std::getenv(name);
#endif
	return (v && *v) ? std::string(v) : std::string();
}

// The user's home directory, or "" if the platform will not say
inline std::string home_dir()
{
#if defined(_WIN32)
	const std::string p = env("USERPROFILE");
	return p.empty() ? env("HOMEDRIVE") + env("HOMEPATH") : p;
#else
	return env("HOME");
#endif
}

inline bool is_file(const std::string &p)
{
	if (p.empty())
		return false;
#if defined(_WIN32)
	const DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st{};
	return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

inline bool is_dir(const std::string &p)
{
	if (p.empty())
		return false;
#if defined(_WIN32)
	const DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st{};
	return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// S-MU2000: そのディレクトリの中のファイルを、名前と最終更新の組で並べる。
// ディレクトリそのものは入れない。古いものを間引くために使う（bootcache.h）
struct dir_entry { std::string name; unsigned long long mtime; };

inline std::vector<dir_entry> list_dir(const std::string &dir)
{
	std::vector<dir_entry> out;
	if (dir.empty())
		return out;
#if defined(_WIN32)
	WIN32_FIND_DATAA fd{};
	const HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return out;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		const unsigned long long t =
			((unsigned long long)(fd.ftLastWriteTime.dwHighDateTime) << 32) |
			fd.ftLastWriteTime.dwLowDateTime;
		out.push_back({ fd.cFileName, t });
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = ::opendir(dir.c_str());
	if (!d)
		return out;
	while (const struct dirent *e = ::readdir(d)) {
		const std::string name = e->d_name;
		if (name == "." || name == "..")
			continue;
		struct stat st{};
		if (::stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		out.push_back({ name, (unsigned long long)st.st_mtime });
	}
	::closedir(d);
#endif
	return out;
}

// One directory level, no parents. Already existing counts as success
inline bool make_dir(const std::string &p)
{
	if (p.empty())
		return false;
#if defined(_WIN32)
	return CreateDirectoryA(p.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
	return ::mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

// A directory that has to exist, made if it does not, parents included. The
// NVRAM file lives two levels under the settings directory, and on a machine
// that has never run this before neither level is there yet
inline bool ensure_dir(const std::string &path)
{
	if (path.empty())
		return false;
	if (is_dir(path))
		return true;
	std::string at = path;
	while (!at.empty() && (at.back() == '/' || at.back() == '\\'))
		at.pop_back();
	for (size_t i = 1; i < at.size(); i++) {
		if (at[i] != '/' && at[i] != '\\')
			continue;
		make_dir(at.substr(0, i));
	}
	return make_dir(at);
}

// Rename, replacing the destination if it is already there. std::rename does
// not replace on Windows, and the NVRAM write leans on the replacement being
// one step, so that side goes through MoveFileEx
inline bool replace_file(const std::string &from, const std::string &to)
{
#if defined(_WIN32)
	return MoveFileExA(from.c_str(), to.c_str(),
	                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// Makes a path absolute and resolves "." and ".." where the platform allows
// it. Falls back to the input
inline std::string full_path(const std::string &p)
{
	if (p.empty())
		return p;
#if defined(_WIN32)
	char buf[4096] = {};
	const DWORD n = GetFullPathNameA(p.c_str(), sizeof(buf), buf, nullptr);
	return (n && n < sizeof(buf)) ? std::string(buf, n) : p;
#else
	char buf[PATH_MAX] = {};
	return ::realpath(p.c_str(), buf) ? std::string(buf) : p;
#endif
}

// The directory holding the shared object (or executable) that the given
// address belongs to. The address is what identifies the image: on Windows a
// module handle, on macOS the Mach-O header dladdr() finds for it.
//
// Returns "" if the platform will not say. No trailing separator.
inline std::string module_dir(const void *addr_in_module)
{
	if (!addr_in_module)
		return {};
#if defined(_WIN32)
	HMODULE self = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCSTR>(addr_in_module), &self))
		return {};
	char buf[4096] = {};
	const DWORD n = GetModuleFileNameA(self, buf, sizeof(buf));
	if (!n || n >= sizeof(buf))
		return {};
	std::string s(buf, n);
	const size_t slash = s.find_last_of("\\/");
	return (slash == std::string::npos) ? std::string() : s.substr(0, slash);
#else
	Dl_info info{};
	if (!dladdr(addr_in_module, &info) || !info.dli_fname)
		return {};
	// dli_fname can have symlinks and "." segments in it; resolve before
	// taking the directory, or "../Resources" walks off somewhere else
	char real[PATH_MAX] = {};
	const std::string p = ::realpath(info.dli_fname, real) ? std::string(real)
	                                                       : std::string(info.dli_fname);
	const size_t slash = p.find_last_of("\\/");
	return (slash == std::string::npos) ? std::string() : p.substr(0, slash);
#endif
}

// "YYYY-MM-DD HH:MM:SS" in local time. For log lines
inline std::string local_time()
{
	char buf[32] = {};
#if defined(_WIN32)
	SYSTEMTIME t;
	GetLocalTime(&t);
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
	              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
#else
	time_t now = ::time(nullptr);
	struct tm t{};
	localtime_r(&now, &t);
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
#endif
	return buf;
}

// config_dir(), created if it is not there yet. Returns "" if it cannot be made
inline std::string ensure_config_dir()
{
	const std::string dir = config_dir();
	if (dir.empty())
		return {};

#if defined(_WIN32)
	CreateDirectoryA(dir.c_str(), nullptr);      // already existing is fine
#else
	// make the intermediate "Application Support" too if this is a fresh HOME
	std::string at = dir;
	while (!at.empty() && (at.back() == '/' || at.back() == '\\'))
		at.pop_back();
	for (size_t i = 1; i < at.size(); i++) {
		if (at[i] != '/')
			continue;
		::mkdir(at.substr(0, i).c_str(), 0755);
	}
	::mkdir(at.c_str(), 0755);
#endif
	return dir;
}

} // namespace smu2000

#endif // S_MU2000_COMPAT_PATHS_H

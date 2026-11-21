// ============================================================================
// VoxPerf.h — optional instrumentation (temporary diagnosis tooling).
//
// OFF by default: with VOXPERF_ENABLED undefined every entry point is an
// inline no-op that the compiler eliminates — zero cost, and no
// voxperf.log file is created. Enable with -DOPTION_VOXPERF=ON (CMake) to
// get the timestamped voxperf.log next to the working directory again.
//
// When enabled: appends timestamped lines to "voxperf.log" next to the
// working directory (GUI app: stderr is unreachable).
// ============================================================================

#ifndef VOX_PERF_H
#define VOX_PERF_H

#ifdef VOXPERF_ENABLED

#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace voxperf {

inline std::FILE* file()
{
	static std::FILE* f = std::fopen("voxperf.log", "a");
	return f;
}

inline double nowMs()
{
	return std::chrono::duration<double, std::milli>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

inline void log(const char* fmt, ...)
{
	std::FILE* f = file();
	if (!f) {
		return;
	}
	std::fprintf(f, "[%12.1f] ", nowMs());
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(f, fmt, ap);
	va_end(ap);
	std::fputc('\n', f);
	std::fflush(f);
}

/// RAII start/finish scope: logs "name: X ms" on destruction.
struct Scope
{
	const char* name;
	double t0;
	bool enabled;
	explicit Scope(const char* n, bool on = true)
	    : name(n), t0(nowMs()), enabled(on && file() != nullptr)
	{
		if (enabled) {
			log("%s START", name);
		}
	}
	~Scope()
	{
		if (enabled) {
			log("%s DONE: %.2f ms", name, nowMs() - t0);
		}
	}
};

} // namespace voxperf

#else // !VOXPERF_ENABLED — no-op stubs, compiled away.

namespace voxperf {

inline double nowMs()
{
	return 0.0;
}

inline void log(const char*, ...)
{
}

struct Scope
{
	explicit Scope(const char*, bool = true)
	{
	}
};

} // namespace voxperf

#endif // VOXPERF_ENABLED

#endif // VOX_PERF_H

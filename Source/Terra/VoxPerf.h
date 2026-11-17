// ============================================================================
// VoxPerf.h — temporary instrumentation for the EXTDEMO skirmish-start hang.
// Appends timestamped lines to "voxperf.log" next to the working directory
// (GUI app: stderr is unreachable). REMOVE after diagnosis.
// ============================================================================

#ifndef VOX_PERF_H
#define VOX_PERF_H

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

#endif // VOX_PERF_H

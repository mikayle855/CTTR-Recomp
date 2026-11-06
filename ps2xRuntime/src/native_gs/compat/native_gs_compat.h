/* [native_gs] Forced into every file of the transplanted renderer (/FI): the POSIX bits it uses that Windows lacks. */
#ifndef NATIVE_GS_COMPAT_H
#define NATIVE_GS_COMPAT_H
#include <time.h>
#ifdef _WIN32
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
/* declared here, not through <windows.h>: its "far" / "near" macros break the renderer's variable names */
union _LARGE_INTEGER;
__declspec(dllimport) int __stdcall QueryPerformanceCounter(union _LARGE_INTEGER *count);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(union _LARGE_INTEGER *freq);
static __inline int clock_gettime(int clk, struct timespec *ts) {
    static long long f;
    long long c;
    (void)clk;
    if (f == 0) { QueryPerformanceFrequency((union _LARGE_INTEGER *)&f); }
    QueryPerformanceCounter((union _LARGE_INTEGER *)&c);
    ts->tv_sec = (time_t)(c / f);
    ts->tv_nsec = (long)((c % f) * 1000000000LL / f);
    return 0;
}
#endif
#endif
#endif

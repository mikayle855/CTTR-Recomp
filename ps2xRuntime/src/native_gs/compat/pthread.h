/* [native_gs] The few pthread calls gs_core.c's optional render thread (BT3_GS_THREAD) uses, over Win32. */
#ifndef NATIVE_GS_PTHREAD_SHIM_H
#define NATIVE_GS_PTHREAD_SHIM_H
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#include <stdlib.h>

typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

static __inline int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
static __inline int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }
static __inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); return 0; }
static __inline int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(c); return 0; }
static __inline int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(c); return 0; }

typedef struct { void *(*fn)(void *); void *arg; } native_gs_thread_start;
static unsigned __stdcall native_gs_thread_tramp(void *p) {
    native_gs_thread_start s = *(native_gs_thread_start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
static __inline int pthread_create(pthread_t *t, const void *attr, void *(*fn)(void *), void *arg) {
    native_gs_thread_start *s = (native_gs_thread_start *)malloc(sizeof(*s));
    (void)attr;
    if (s == NULL) { return -1; }
    s->fn = fn;
    s->arg = arg;
    *t = (HANDLE)_beginthreadex(NULL, 0, native_gs_thread_tramp, s, 0, NULL);
    return *t ? 0 : -1;
}
#else
#include_next <pthread.h>
#endif
#endif

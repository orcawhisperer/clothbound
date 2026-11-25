#pragma once

#ifndef CLOTHBOUND_TSAN_ACTIVE
#if defined(__linux__)
#if defined(__SANITIZE_THREAD__)
#define CLOTHBOUND_TSAN_ACTIVE 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CLOTHBOUND_TSAN_ACTIVE 1
#endif
#endif
#endif
#endif

#ifdef CLOTHBOUND_TSAN_ACTIVE
#include <errno.h>
#include <pthread.h>
#include <time.h>

// Qt 6.12's QThread::wait() calls glibc's pthread_clockjoin_np(), which neither
// GCC nor Clang's libtsan intercepts yet (only pthread_join / pthread_tryjoin_np /
// pthread_timedjoin_np are intercepted). Without forwarding to the intercepted join
// primitives, TSan never sees QThread joins, causing spurious CHECK failures in
// sanitizer_thread_registry.cpp:186 when glibc recycles a joined pthread_t address.
extern "C" __attribute__((visibility("default"))) int pthread_clockjoin_np(pthread_t thread, void **retval,
                                                                           clockid_t clockid,
                                                                           const struct timespec *abstime) {
    if (!abstime)
        return pthread_join(thread, retval);
    if (clockid == CLOCK_REALTIME)
        return pthread_timedjoin_np(thread, retval, abstime);

    struct timespec nowClk {};
    if (clock_gettime(clockid, &nowClk) != 0)
        return pthread_join(thread, retval);

    long long diffSec = static_cast<long long>(abstime->tv_sec) - static_cast<long long>(nowClk.tv_sec);
    long long diffNsec = static_cast<long long>(abstime->tv_nsec) - static_cast<long long>(nowClk.tv_nsec);
    if (diffNsec < 0) {
        --diffSec;
        diffNsec += 1000000000LL;
    }
    if (diffSec < 0 || (diffSec == 0 && diffNsec == 0))
        return pthread_tryjoin_np(thread, retval);
    if (diffSec > 3600)
        return pthread_join(thread, retval);

    struct timespec nowRt {};
    if (clock_gettime(CLOCK_REALTIME, &nowRt) != 0)
        return pthread_join(thread, retval);

    struct timespec rtDeadline {};
    rtDeadline.tv_sec = static_cast<time_t>(static_cast<long long>(nowRt.tv_sec) + diffSec);
    long long totalNsec = static_cast<long long>(nowRt.tv_nsec) + diffNsec;
    if (totalNsec >= 1000000000LL) {
        ++rtDeadline.tv_sec;
        totalNsec -= 1000000000LL;
    }
    rtDeadline.tv_nsec = static_cast<long>(totalNsec);
    return pthread_timedjoin_np(thread, retval, &rtDeadline);
}
#endif

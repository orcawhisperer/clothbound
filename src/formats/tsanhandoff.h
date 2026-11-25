#pragma once

#include <QRunnable>
#include <QThreadPool>
#include <type_traits>
#include <utility>

#if defined(__SANITIZE_THREAD__) || defined(CLOTHBOUND_TSAN_ACTIVE)
#include <sanitizer/tsan_interface.h>
#define CLOTHBOUND_TSAN_RELEASE(ptr) __tsan_release(ptr)
#define CLOTHBOUND_TSAN_ACQUIRE(ptr) __tsan_acquire(ptr)
#else
#define CLOTHBOUND_TSAN_RELEASE(ptr) ((void)(ptr))
#define CLOTHBOUND_TSAN_ACQUIRE(ptr) ((void)(ptr))
#endif

namespace Clothbound {

// Starts a callable on a QThreadPool while annotating the enqueue->dequeue
// happens-before edge for ThreadSanitizer when system libQt6Core is uninstrumented.
// Compiles to zero overhead in non-TSan builds and never suppresses real races
// inside `fn`.
template <typename Fn>
inline void startPoolTask(QThreadPool *pool, Fn &&fn, int priority = 0) {
    struct Task final : public QRunnable {
        std::decay_t<Fn> fn;
        explicit Task(Fn &&f) : fn(std::forward<Fn>(f)) { setAutoDelete(true); }
        void run() override {
            CLOTHBOUND_TSAN_ACQUIRE(static_cast<QRunnable *>(this));
            fn();
        }
    };
    auto *task = new Task(std::forward<Fn>(fn));
    CLOTHBOUND_TSAN_RELEASE(static_cast<QRunnable *>(task));
    pool->start(task, priority);
}

inline void startPoolRunnable(QThreadPool *pool, QRunnable *runnable, int priority = 0) {
    CLOTHBOUND_TSAN_RELEASE(runnable);
    pool->start(runnable, priority);
}

inline void acquirePoolRunnable(QRunnable *runnable) { CLOTHBOUND_TSAN_ACQUIRE(runnable); }

}  // namespace Clothbound

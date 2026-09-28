/* k32thread.c - kernel32: threads, waiting and locks.
 *
 * A Windows program that starts a thread expects four things to be true: the
 * thread runs the function it was given, a handle to it can be waited on, the
 * locks it takes are honoured, and each thread has storage of its own that the
 * others cannot see.  All four are built here on this system's threads, its
 * futexes, and a thread environment block per thread laid out where Windows
 * code looks for it.
 *
 * The locks are the interesting part.  Windows programs take a critical
 * section around every small thing, so the uncontended case has to be cheap -
 * one atomic instruction and no system call.  Only a thread that actually has
 * to wait pays for a call, and it sleeps rather than spinning.
 */
#include "win.h"

DWORD win_error_from_errno(int e);

/* --------------------------------------------------- the thread environment */

/* Windows code finds its own thread's data through GS, at offsets that have
 * been the same since the sixty-four-bit port.  Compiler-generated code reads
 * some of these directly, so they have to be right rather than merely
 * present. */
#define TEB_STACK_BASE   0x0008
#define TEB_STACK_LIMIT  0x0010
#define TEB_SELF         0x0030
#define TEB_PID          0x0040
#define TEB_TID          0x0048
#define TEB_TLS_POINTER  0x0058
#define TEB_PEB          0x0060
#define TEB_LAST_ERROR   0x0068
#define TEB_TLS_SLOTS    0x1480
#define TEB_BYTES        0x1800

#define PEB_BEING_DEBUGGED 0x0002
#define PEB_IMAGE_BASE     0x0010
#define PEB_LDR            0x0018
#define PEB_PARAMS         0x0020
#define PEB_BYTES          0x0400

#define MAX_THREADS 32
#define TLS_SLOTS   64

typedef struct {
    bool     used;
    int      tid;
    uint8_t *teb;
    void    *tls[TLS_SLOTS];
    object_t *self;              /* the handle other threads wait on */
} thread_slot_t;

static thread_slot_t threads[MAX_THREADS];
static uint8_t *process_peb;
static uint32_t tls_taken[TLS_SLOTS];

static thread_slot_t *slot_for(int tid, bool create) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].used && threads[i].tid == tid) return &threads[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].used) continue;
        memset(&threads[i], 0, sizeof threads[i]);
        threads[i].used = true;
        threads[i].tid = tid;
        return &threads[i];
    }
    return NULL;
}

static thread_slot_t *this_thread(void) { return slot_for(thread_id(), true); }

/* Building the block a thread's own code reads through GS.  The stack bounds
 * matter: code that probes its stack, and every exception unwind, reads
 * them. */
static void build_teb(thread_slot_t *t, uint64_t stack_base, uint64_t stack_limit) {
    if (t->teb) return;
    t->teb = calloc(1, TEB_BYTES);
    if (!t->teb) win_fail("out of memory building a thread environment block");

    if (!process_peb) {
        process_peb = calloc(1, PEB_BYTES);
        if (!process_peb) win_fail("out of memory building a process environment block");
        process_peb[PEB_BEING_DEBUGGED] = 0;
        *(uint64_t *)(process_peb + PEB_IMAGE_BASE) =
            win_main_module ? (uint64_t)(uintptr_t)win_main_module->base : 0;
    }

    *(uint64_t *)(t->teb + TEB_STACK_BASE) = stack_base;
    *(uint64_t *)(t->teb + TEB_STACK_LIMIT) = stack_limit;
    *(uint64_t *)(t->teb + TEB_SELF) = (uint64_t)(uintptr_t)t->teb;
    *(uint64_t *)(t->teb + TEB_PID) = (uint64_t)getpid();
    *(uint64_t *)(t->teb + TEB_TID) = (uint64_t)t->tid;
    *(uint64_t *)(t->teb + TEB_TLS_POINTER) = (uint64_t)(uintptr_t)t->tls;
    *(uint64_t *)(t->teb + TEB_PEB) = (uint64_t)(uintptr_t)process_peb;

    thread_set_gs(t->teb);
}

/* Called once from the main thread before the program starts. */
void k32_thread_start_main(uint64_t stack_base, uint64_t stack_limit) {
    thread_slot_t *t = this_thread();
    if (t) build_teb(t, stack_base, stack_limit);
}

/* ------------------------------------------------------------------ atomics */

static inline uint32_t atomic_load(volatile uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void atomic_store(volatile uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline bool atomic_cas(volatile uint32_t *p, uint32_t expect, uint32_t want) {
    return __atomic_compare_exchange_n(p, &expect, want, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* --------------------------------------------------------------- waiting */

/* One place where every kind of waitable object is acquired, so that
 * WaitForSingleObject and WaitForMultipleObjects cannot drift apart.  Returns
 * true if the object was taken. */
static bool try_acquire(object_t *o) {
    switch (o->kind) {
    case OBJ_THREAD:
        return atomic_load(&o->signalled) != 0;

    case OBJ_EVENT:
        if (o->manual_reset) return atomic_load(&o->signalled) != 0;
        /* An auto-reset event releases exactly one waiter, so taking it and
         * clearing it has to happen together. */
        return atomic_cas(&o->signalled, 1, 0);

    case OBJ_MUTEX:
        if (o->owner_tid == thread_id()) { o->recursion++; return true; }
        if (atomic_cas(&o->signalled, 1, 0)) {
            o->owner_tid = thread_id();
            o->recursion = 1;
            return true;
        }
        return false;

    case OBJ_SEMAPHORE:
        for (;;) {
            int have = o->count;
            if (have <= 0) return false;
            if (__atomic_compare_exchange_n(&o->count, &have, have - 1, false,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                if (have - 1 == 0) atomic_store(&o->signalled, 0);
                return true;
            }
        }

    default:
        return true;
    }
}

DWORD win_wait_one(object_t *o, DWORD timeout_ms) {
    if (!o) return WAIT_FAILED;

    /* Another program is not an object this system signals - it is a child,
     * and waiting for it is asking the system to wait for it.  There is no
     * word to sleep on, so this is the one kind of wait that goes straight
     * through rather than through the futex below. */
    if (o->kind == OBJ_PROCESS) {
        if (o->signalled) return WAIT_OBJECT_0;
        int status = 0;
        int done = wait(o->tid, &status);
        if (done < 0) return WAIT_FAILED;
        o->exit_code = (DWORD)status;
        o->signalled = 1;
        return WAIT_OBJECT_0;
    }

    uint64_t deadline = (timeout_ms == INFINITE) ? 0 : uptime_ms() + timeout_ms;
    for (;;) {
        if (try_acquire(o)) return WAIT_OBJECT_0;
        if (timeout_ms == 0) return WAIT_TIMEOUT;

        int remaining = -1;
        if (timeout_ms != INFINITE) {
            uint64_t now = uptime_ms();
            if (now >= deadline) return WAIT_TIMEOUT;
            remaining = (int)(deadline - now);
        }

        /* Sleeping on the object's own word: a waker changes it and wakes us,
         * and if it changed between the test above and here, the kernel side
         * notices and returns at once rather than sleeping through it. */
        futex_wait(&o->signalled, atomic_load(&o->signalled), remaining);
    }
}

static DWORD WINAPI w_WaitForSingleObject(HANDLE h, DWORD timeout) {
    object_t *o = obj_any(h);
    if (!o) { win_set_error(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    return win_wait_one(o, timeout);
}
static DWORD WINAPI w_WaitForSingleObjectEx(HANDLE h, DWORD timeout, BOOL alertable) {
    (void)alertable; return w_WaitForSingleObject(h, timeout);
}

static DWORD WINAPI w_WaitForMultipleObjects(DWORD count, const HANDLE *handles,
                                             BOOL wait_all, DWORD timeout) {
    if (!handles || !count || count > 64) { win_set_error(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    uint64_t deadline = (timeout == INFINITE) ? 0 : uptime_ms() + timeout;

    for (;;) {
        if (wait_all) {
            /* All of them, which means checking that every one is ready
             * before taking any: taking some and then blocking would leave
             * them held by a thread that is not running. */
            bool all = true;
            for (DWORD i = 0; i < count; i++) {
                object_t *o = obj_any(handles[i]);
                if (!o) { win_set_error(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
                if (!atomic_load(&o->signalled)) { all = false; break; }
            }
            if (all) {
                for (DWORD i = 0; i < count; i++) try_acquire(obj_any(handles[i]));
                return WAIT_OBJECT_0;
            }
        } else {
            for (DWORD i = 0; i < count; i++) {
                object_t *o = obj_any(handles[i]);
                if (!o) { win_set_error(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
                if (try_acquire(o)) return WAIT_OBJECT_0 + i;
            }
        }

        if (timeout == 0) return WAIT_TIMEOUT;
        if (timeout != INFINITE && uptime_ms() >= deadline) return WAIT_TIMEOUT;

        /* With several objects there is no single word to sleep on, so this
         * waits on the first and gives up the processor briefly otherwise. */
        object_t *first = obj_any(handles[0]);
        futex_wait(&first->signalled, atomic_load(&first->signalled), 5);
    }
}

/* --------------------------------------------------------------- threads */

typedef DWORD WINAPI (*thread_start_fn)(void *);

/* The guest's thread function uses Microsoft's convention and this system's
 * threads use its own, so the two meet here. */
static void thread_bootstrap(void *raw) {
    object_t *o = raw;
    thread_slot_t *t = this_thread();
    if (t) {
        t->self = o;
        /* The stack bounds for this thread, which the block below reports to
         * any Windows code that asks. */
        uint64_t here = (uint64_t)(uintptr_t)&raw;
        build_teb(t, (here + 0xFFFF) & ~0xFFFFULL, here & ~0x3FFFFULL);
    }

    pe_run_tls_callbacks(NULL, 2 /* DLL_THREAD_ATTACH */);
    DWORD code = ((thread_start_fn)o->start)(o->param);
    pe_run_tls_callbacks(NULL, 3 /* DLL_THREAD_DETACH */);

    o->exit_code = code;
    atomic_store(&o->signalled, 1);
    futex_wake(&o->signalled, MAX_THREADS);
    if (t) t->used = false;
    thread_exit();
}

static HANDLE WINAPI w_CreateThread(void *sa, size_t stack, void *start, void *param,
                                    DWORD flags, DWORD *out_tid) {
    (void)sa; (void)stack;
    if (!start) { win_set_error(ERROR_INVALID_PARAMETER); return NULL; }
    if (flags & 0x00000004 /* CREATE_SUSPENDED */) {
        /* Starting suspended and resuming later is a shape this cannot honour
         * without pretending; saying so beats running the thread early. */
        win_trace("CreateThread was asked for a suspended thread, which is not supported");
        win_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    object_t *o;
    HANDLE h = obj_alloc(OBJ_THREAD, &o);
    if (!h) { win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    o->start = start;
    o->param = param;
    o->signalled = 0;

    int tid = thread_create(thread_bootstrap, o);
    if (tid < 0) { obj_release(h); win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    o->tid = tid;
    if (out_tid) *out_tid = (DWORD)tid;
    return h;
}

static void WINAPI w_ExitThread(DWORD code) {
    thread_slot_t *t = this_thread();
    if (t && t->self) {
        t->self->exit_code = code;
        atomic_store(&t->self->signalled, 1);
        futex_wake(&t->self->signalled, MAX_THREADS);
    }
    if (t) t->used = false;
    thread_exit();
}

static HANDLE WINAPI w_GetCurrentThread(void) { return (HANDLE)(uintptr_t)-2; }
static DWORD  WINAPI w_GetCurrentThreadId(void) { return (DWORD)thread_id(); }

static BOOL WINAPI w_GetExitCodeThread(HANDLE h, DWORD *out) {
    object_t *o = obj_get(h, OBJ_THREAD);
    if (!o || !out) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    *out = atomic_load(&o->signalled) ? o->exit_code : 259 /* STILL_ACTIVE */;
    return WIN_TRUE;
}

static void WINAPI w_Sleep(DWORD ms) {
    if (!ms) { yield(); return; }
    sleep_ms(ms);
}
static DWORD WINAPI w_SleepEx(DWORD ms, BOOL alertable) { (void)alertable; w_Sleep(ms); return 0; }
static BOOL  WINAPI w_SwitchToThread(void) { yield(); return WIN_TRUE; }
static BOOL  WINAPI w_SetThreadPriority(HANDLE h, int pri) { (void)h; (void)pri; return WIN_TRUE; }
static int   WINAPI w_GetThreadPriority(HANDLE h) { (void)h; return 0; }

/* ------------------------------------------------------- thread-local storage */

static DWORD WINAPI w_TlsAlloc(void) {
    for (DWORD i = 0; i < TLS_SLOTS; i++)
        if (!tls_taken[i]) { tls_taken[i] = 1; return i; }
    win_set_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0xFFFFFFFF;
}
static BOOL WINAPI w_TlsFree(DWORD index) {
    if (index >= TLS_SLOTS) return WIN_FALSE;
    tls_taken[index] = 0;
    return WIN_TRUE;
}
static void *WINAPI w_TlsGetValue(DWORD index) {
    thread_slot_t *t = this_thread();
    if (index >= TLS_SLOTS || !t) { win_set_error(ERROR_INVALID_PARAMETER); return NULL; }
    win_set_error(ERROR_SUCCESS);
    return t->tls[index];
}
static BOOL WINAPI w_TlsSetValue(DWORD index, void *value) {
    thread_slot_t *t = this_thread();
    if (index >= TLS_SLOTS || !t) { win_set_error(ERROR_INVALID_PARAMETER); return WIN_FALSE; }
    t->tls[index] = value;
    return WIN_TRUE;
}

/* ------------------------------------------------------------ the interlocks */

static LONG WINAPI w_InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static LONG WINAPI w_InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static LONG WINAPI w_InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static LONG WINAPI w_InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static LONG WINAPI w_InterlockedCompareExchange(volatile LONG *p, LONG want, LONG expect) {
    __atomic_compare_exchange_n(p, &expect, want, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expect;                          /* the value that was there */
}
static void *WINAPI w_InterlockedCompareExchangePointer(void *volatile *p, void *want, void *expect) {
    __atomic_compare_exchange_n((volatile uint64_t *)p, (uint64_t *)&expect, (uint64_t)want,
                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expect;
}
static void *WINAPI w_InterlockedExchangePointer(void *volatile *p, void *v) {
    return (void *)__atomic_exchange_n((volatile uint64_t *)p, (uint64_t)v, __ATOMIC_SEQ_CST);
}
static int64_t WINAPI w_InterlockedIncrement64(volatile int64_t *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int64_t WINAPI w_InterlockedDecrement64(volatile int64_t *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }

/* ----------------------------------------------------------- critical sections */

/* The real structure is forty bytes and programs allocate it themselves, so
 * the layout has to match even though only two fields are used.  LockCount is
 * the word threads sleep on: 0 free, 1 held, 2 held with somebody waiting. */
typedef struct {
    void    *DebugInfo;
    LONG     LockCount;
    LONG     RecursionCount;
    HANDLE   OwningThread;
    HANDLE   LockSemaphore;
    UINT_PTR SpinCount;
} CRITICAL_SECTION;

static void WINAPI w_InitializeCriticalSection(CRITICAL_SECTION *cs) {
    if (!cs) return;
    memset(cs, 0, sizeof *cs);
}
static BOOL WINAPI w_InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs, DWORD spin) {
    w_InitializeCriticalSection(cs);
    if (cs) cs->SpinCount = spin;
    return WIN_TRUE;
}
static BOOL WINAPI w_InitializeCriticalSectionEx(CRITICAL_SECTION *cs, DWORD spin, DWORD flags) {
    (void)flags; return w_InitializeCriticalSectionAndSpinCount(cs, spin);
}
static void WINAPI w_DeleteCriticalSection(CRITICAL_SECTION *cs) { (void)cs; }

static void WINAPI w_EnterCriticalSection(CRITICAL_SECTION *cs) {
    if (!cs) return;
    HANDLE me = (HANDLE)(uintptr_t)thread_id();

    if (cs->OwningThread == me && cs->LockCount) { cs->RecursionCount++; return; }

    volatile uint32_t *word = (volatile uint32_t *)&cs->LockCount;
    /* The uncontended case: one atomic instruction and no system call, which
     * is the whole point - Windows code takes these constantly. */
    if (atomic_cas(word, 0, 1)) {
        cs->OwningThread = me;
        cs->RecursionCount = 1;
        return;
    }
    for (;;) {
        /* Mark it contended so the releaser knows to wake somebody. */
        uint32_t seen = atomic_load(word);
        if (seen == 1) { if (!atomic_cas(word, 1, 2)) continue; seen = 2; }
        if (seen == 0) {
            if (atomic_cas(word, 0, 2)) { cs->OwningThread = me; cs->RecursionCount = 1; return; }
            continue;
        }
        futex_wait(word, 2, -1);
        if (atomic_cas(word, 0, 2)) { cs->OwningThread = me; cs->RecursionCount = 1; return; }
    }
}

static BOOL WINAPI w_TryEnterCriticalSection(CRITICAL_SECTION *cs) {
    if (!cs) return WIN_FALSE;
    HANDLE me = (HANDLE)(uintptr_t)thread_id();
    if (cs->OwningThread == me && cs->LockCount) { cs->RecursionCount++; return WIN_TRUE; }
    if (atomic_cas((volatile uint32_t *)&cs->LockCount, 0, 1)) {
        cs->OwningThread = me;
        cs->RecursionCount = 1;
        return WIN_TRUE;
    }
    return WIN_FALSE;
}

static void WINAPI w_LeaveCriticalSection(CRITICAL_SECTION *cs) {
    if (!cs) return;
    if (--cs->RecursionCount > 0) return;
    cs->OwningThread = NULL;
    volatile uint32_t *word = (volatile uint32_t *)&cs->LockCount;
    if (__atomic_exchange_n(word, 0, __ATOMIC_SEQ_CST) == 2)
        futex_wake(word, 1);                /* somebody was waiting */
}

/* ------------------------------------------------------------- slim locks */

/* A slim reader-writer lock is a single pointer-sized word the program
 * allocates.  Readers are admitted one at a time here rather than in parallel:
 * slower under contention, and correct, which is the half that matters. */
typedef struct { void *Ptr; } SRWLOCK;

static void WINAPI w_InitializeSRWLock(SRWLOCK *l) { if (l) l->Ptr = NULL; }

static void WINAPI w_AcquireSRWLockExclusive(SRWLOCK *l) {
    if (!l) return;
    volatile uint32_t *word = (volatile uint32_t *)&l->Ptr;
    while (!atomic_cas(word, 0, 1)) futex_wait(word, 1, -1);
}
static void WINAPI w_ReleaseSRWLockExclusive(SRWLOCK *l) {
    if (!l) return;
    volatile uint32_t *word = (volatile uint32_t *)&l->Ptr;
    atomic_store(word, 0);
    futex_wake(word, 1);
}
static BOOL WINAPI w_TryAcquireSRWLockExclusive(SRWLOCK *l) {
    if (!l) return WIN_FALSE;
    return atomic_cas((volatile uint32_t *)&l->Ptr, 0, 1) ? WIN_TRUE : WIN_FALSE;
}

/* -------------------------------------------------------- condition variables */

typedef struct { void *Ptr; } CONDITION_VARIABLE;

static void WINAPI w_InitializeConditionVariable(CONDITION_VARIABLE *cv) { if (cv) cv->Ptr = NULL; }

static BOOL WINAPI w_SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms) {
    if (!cv || !cs) return WIN_FALSE;
    volatile uint32_t *word = (volatile uint32_t *)&cv->Ptr;
    uint32_t seen = atomic_load(word);
    /* The lock is dropped and the wait entered without a gap between them:
     * the value read above is passed to the kernel, which refuses to sleep if
     * a signal changed it in the meantime. */
    w_LeaveCriticalSection(cs);
    int r = futex_wait(word, seen, ms == INFINITE ? -1 : (int)ms);
    w_EnterCriticalSection(cs);
    if (r < 0) { win_set_error(WAIT_TIMEOUT); return WIN_FALSE; }
    return WIN_TRUE;
}
static BOOL WINAPI w_SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, DWORD flags) {
    (void)flags;
    if (!cv || !l) return WIN_FALSE;
    volatile uint32_t *word = (volatile uint32_t *)&cv->Ptr;
    uint32_t seen = atomic_load(word);
    w_ReleaseSRWLockExclusive(l);
    int r = futex_wait(word, seen, ms == INFINITE ? -1 : (int)ms);
    w_AcquireSRWLockExclusive(l);
    return r < 0 ? WIN_FALSE : WIN_TRUE;
}
static void WINAPI w_WakeConditionVariable(CONDITION_VARIABLE *cv) {
    if (!cv) return;
    volatile uint32_t *word = (volatile uint32_t *)&cv->Ptr;
    __atomic_add_fetch(word, 1, __ATOMIC_SEQ_CST);
    futex_wake(word, 1);
}
static void WINAPI w_WakeAllConditionVariable(CONDITION_VARIABLE *cv) {
    if (!cv) return;
    volatile uint32_t *word = (volatile uint32_t *)&cv->Ptr;
    __atomic_add_fetch(word, 1, __ATOMIC_SEQ_CST);
    futex_wake(word, MAX_THREADS);
}

/* ---------------------------------------------------------------- one-time */

typedef struct { void *Ptr; } INIT_ONCE;

static BOOL WINAPI w_InitOnceExecuteOnce(INIT_ONCE *once, void *fn, void *param, void **context) {
    if (!once || !fn) return WIN_FALSE;
    volatile uint32_t *word = (volatile uint32_t *)&once->Ptr;
    if (atomic_load(word) == 2) return WIN_TRUE;
    if (atomic_cas(word, 0, 1)) {
        typedef BOOL WINAPI (*once_fn)(INIT_ONCE *, void *, void **);
        BOOL ok = ((once_fn)fn)(once, param, context);
        atomic_store(word, ok ? 2 : 0);
        futex_wake(word, MAX_THREADS);
        return ok;
    }
    while (atomic_load(word) != 2) futex_wait(word, 1, 5);
    return WIN_TRUE;
}

/* ---------------------------------------------------------------- the events */

static HANDLE WINAPI w_CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name) {
    (void)sa; (void)name;
    object_t *o;
    HANDLE h = obj_alloc(OBJ_EVENT, &o);
    if (!h) { win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    o->manual_reset = manual != 0;
    o->signalled = initial ? 1 : 0;
    return h;
}
static HANDLE WINAPI w_CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name) {
    (void)name; return w_CreateEventA(sa, manual, initial, NULL);
}
static BOOL WINAPI w_SetEvent(HANDLE h) {
    object_t *o = obj_get(h, OBJ_EVENT);
    if (!o) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    atomic_store(&o->signalled, 1);
    futex_wake(&o->signalled, o->manual_reset ? MAX_THREADS : 1);
    return WIN_TRUE;
}
static BOOL WINAPI w_ResetEvent(HANDLE h) {
    object_t *o = obj_get(h, OBJ_EVENT);
    if (!o) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    atomic_store(&o->signalled, 0);
    return WIN_TRUE;
}
static BOOL WINAPI w_PulseEvent(HANDLE h) {
    object_t *o = obj_get(h, OBJ_EVENT);
    if (!o) return WIN_FALSE;
    atomic_store(&o->signalled, 1);
    futex_wake(&o->signalled, MAX_THREADS);
    atomic_store(&o->signalled, 0);
    return WIN_TRUE;
}

/* --------------------------------------------------------------- the mutexes */

static HANDLE WINAPI w_CreateMutexA(void *sa, BOOL own, const char *name) {
    (void)sa; (void)name;
    object_t *o;
    HANDLE h = obj_alloc(OBJ_MUTEX, &o);
    if (!h) { win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (own) { o->signalled = 0; o->owner_tid = thread_id(); o->recursion = 1; }
    else o->signalled = 1;
    return h;
}
static HANDLE WINAPI w_CreateMutexW(void *sa, BOOL own, const WCHAR *name) {
    (void)name; return w_CreateMutexA(sa, own, NULL);
}
static BOOL WINAPI w_ReleaseMutex(HANDLE h) {
    object_t *o = obj_get(h, OBJ_MUTEX);
    if (!o || o->owner_tid != thread_id()) { win_set_error(ERROR_ACCESS_DENIED); return WIN_FALSE; }
    if (--o->recursion > 0) return WIN_TRUE;
    o->owner_tid = 0;
    atomic_store(&o->signalled, 1);
    futex_wake(&o->signalled, 1);
    return WIN_TRUE;
}

/* ----------------------------------------------------------- the semaphores */

static HANDLE WINAPI w_CreateSemaphoreA(void *sa, LONG initial, LONG max, const char *name) {
    (void)sa; (void)name;
    if (initial < 0 || max <= 0 || initial > max) { win_set_error(ERROR_INVALID_PARAMETER); return NULL; }
    object_t *o;
    HANDLE h = obj_alloc(OBJ_SEMAPHORE, &o);
    if (!h) { win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    o->count = initial;
    o->max_count = max;
    o->signalled = initial > 0 ? 1 : 0;
    return h;
}
static BOOL WINAPI w_ReleaseSemaphore(HANDLE h, LONG release, LONG *previous) {
    object_t *o = obj_get(h, OBJ_SEMAPHORE);
    if (!o || release <= 0) { win_set_error(ERROR_INVALID_PARAMETER); return WIN_FALSE; }
    if (o->count + release > o->max_count) { win_set_error(ERROR_INVALID_PARAMETER); return WIN_FALSE; }
    if (previous) *previous = o->count;
    __atomic_add_fetch(&o->count, release, __ATOMIC_SEQ_CST);
    atomic_store(&o->signalled, 1);
    futex_wake(&o->signalled, release);
    return WIN_TRUE;
}

/* The C runtime starts and ends threads through its own names, which have to
 * end up in exactly the same place as kernel32's. */
HANDLE win_thread_create(void *start, void *arg, unsigned flags, unsigned *tid) {
    DWORD id = 0;
    HANDLE h = w_CreateThread(NULL, 0, start, arg, flags, &id);
    if (tid) *tid = (unsigned)id;
    return h;
}
void win_thread_exit(unsigned code) { w_ExitThread((DWORD)code); }

/* ---------------------------------------------------------------- the table */

const win_export_t k32_thread_exports[] = {
    { "CreateThread",                        (void *)w_CreateThread },
    { "ExitThread",                          (void *)w_ExitThread },
    { "GetCurrentThread",                    (void *)w_GetCurrentThread },
    { "GetCurrentThreadId",                  (void *)w_GetCurrentThreadId },
    { "GetExitCodeThread",                   (void *)w_GetExitCodeThread },
    { "Sleep",                               (void *)w_Sleep },
    { "SleepEx",                             (void *)w_SleepEx },
    { "SwitchToThread",                      (void *)w_SwitchToThread },
    { "SetThreadPriority",                   (void *)w_SetThreadPriority },
    { "GetThreadPriority",                   (void *)w_GetThreadPriority },

    { "WaitForSingleObject",                 (void *)w_WaitForSingleObject },
    { "WaitForSingleObjectEx",               (void *)w_WaitForSingleObjectEx },
    { "WaitForMultipleObjects",              (void *)w_WaitForMultipleObjects },

    { "TlsAlloc",                            (void *)w_TlsAlloc },
    { "TlsFree",                             (void *)w_TlsFree },
    { "TlsGetValue",                         (void *)w_TlsGetValue },
    { "TlsSetValue",                         (void *)w_TlsSetValue },

    { "InterlockedIncrement",                (void *)w_InterlockedIncrement },
    { "InterlockedDecrement",                (void *)w_InterlockedDecrement },
    { "InterlockedExchange",                 (void *)w_InterlockedExchange },
    { "InterlockedExchangeAdd",              (void *)w_InterlockedExchangeAdd },
    { "InterlockedCompareExchange",          (void *)w_InterlockedCompareExchange },
    { "InterlockedCompareExchangePointer",   (void *)w_InterlockedCompareExchangePointer },
    { "InterlockedExchangePointer",          (void *)w_InterlockedExchangePointer },
    { "InterlockedIncrement64",              (void *)w_InterlockedIncrement64 },
    { "InterlockedDecrement64",              (void *)w_InterlockedDecrement64 },

    { "InitializeCriticalSection",           (void *)w_InitializeCriticalSection },
    { "InitializeCriticalSectionAndSpinCount", (void *)w_InitializeCriticalSectionAndSpinCount },
    { "InitializeCriticalSectionEx",         (void *)w_InitializeCriticalSectionEx },
    { "DeleteCriticalSection",               (void *)w_DeleteCriticalSection },
    { "EnterCriticalSection",                (void *)w_EnterCriticalSection },
    { "TryEnterCriticalSection",             (void *)w_TryEnterCriticalSection },
    { "LeaveCriticalSection",                (void *)w_LeaveCriticalSection },

    { "InitializeSRWLock",                   (void *)w_InitializeSRWLock },
    { "AcquireSRWLockExclusive",             (void *)w_AcquireSRWLockExclusive },
    { "ReleaseSRWLockExclusive",             (void *)w_ReleaseSRWLockExclusive },
    { "TryAcquireSRWLockExclusive",          (void *)w_TryAcquireSRWLockExclusive },
    { "AcquireSRWLockShared",                (void *)w_AcquireSRWLockExclusive },
    { "ReleaseSRWLockShared",                (void *)w_ReleaseSRWLockExclusive },

    { "InitializeConditionVariable",         (void *)w_InitializeConditionVariable },
    { "SleepConditionVariableCS",            (void *)w_SleepConditionVariableCS },
    { "SleepConditionVariableSRW",           (void *)w_SleepConditionVariableSRW },
    { "WakeConditionVariable",               (void *)w_WakeConditionVariable },
    { "WakeAllConditionVariable",            (void *)w_WakeAllConditionVariable },
    { "InitOnceExecuteOnce",                 (void *)w_InitOnceExecuteOnce },

    { "CreateEventA",                        (void *)w_CreateEventA },
    { "CreateEventW",                        (void *)w_CreateEventW },
    { "SetEvent",                            (void *)w_SetEvent },
    { "ResetEvent",                          (void *)w_ResetEvent },
    { "PulseEvent",                          (void *)w_PulseEvent },
    { "CreateMutexA",                        (void *)w_CreateMutexA },
    { "CreateMutexW",                        (void *)w_CreateMutexW },
    { "ReleaseMutex",                        (void *)w_ReleaseMutex },
    { "CreateSemaphoreA",                    (void *)w_CreateSemaphoreA },
    { "ReleaseSemaphore",                    (void *)w_ReleaseSemaphore },
    { NULL, NULL }
};

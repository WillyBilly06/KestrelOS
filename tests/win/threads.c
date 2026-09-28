/* threads - real threads, real locks, real waiting.
 *
 * The point of each check here is a specific way threading goes wrong: a
 * counter incremented without a lock loses updates, an event that does not
 * wake its waiter hangs, a mutex that is not recursive deadlocks against
 * itself, and thread-local storage that is not local is just a global. */
#include "winapi.h"

static CRITICAL_SECTION lock;
static volatile LONG guarded_counter;
static volatile LONG atomic_counter;
static HANDLE go_event;
static HANDLE done_event;
static DWORD tls_index;
static volatile LONG tls_failures;

#define WORKERS 4
#define BUMPS   2000

/* Each worker bumps a shared counter under a lock and another one with an
 * interlocked instruction.  If either count comes out short, updates were
 * lost, which is exactly what a broken lock looks like. */
static DWORD WINAPI bumper(void *arg) {
    long long which = (long long)arg;

    /* Storage of its own, written before anything else runs and read after
     * every other thread has had a turn. */
    TlsSetValue(tls_index, (void *)(which + 100));

    for (int i = 0; i < BUMPS; i++) {
        EnterCriticalSection(&lock);
        LONG was = guarded_counter;
        /* A deliberate gap between reading and writing: without the lock this
         * is where the updates would be lost. */
        guarded_counter = was + 1;
        LeaveCriticalSection(&lock);

        InterlockedIncrement(&atomic_counter);
    }

    if ((long long)TlsGetValue(tls_index) != which + 100) InterlockedIncrement(&tls_failures);
    return (DWORD)(which + 1);
}

/* A thread that waits to be told to run, so the event has something to do. */
static DWORD WINAPI waiter(void *arg) {
    (void)arg;
    if (WaitForSingleObject(go_event, 5000) != WAIT_OBJECT_0) return 0;
    SetEvent(done_event);
    return 7;
}

static CRITICAL_SECTION recursive;

int main(void) {
    printf("threads: threading\n");

    InitializeCriticalSection(&lock);
    InitializeCriticalSection(&recursive);
    tls_index = TlsAlloc();
    check(tls_index != 0xFFFFFFFF, "TlsAlloc gives out a slot");

    printf(" a critical section is recursive\n");
    EnterCriticalSection(&recursive);
    EnterCriticalSection(&recursive);          /* would deadlock if it were not */
    LeaveCriticalSection(&recursive);
    LeaveCriticalSection(&recursive);
    check(1, "the same thread entered twice and left twice");

    printf(" four threads bumping two counters\n");
    HANDLE workers[WORKERS];
    for (long long i = 0; i < WORKERS; i++) {
        DWORD tid = 0;
        workers[i] = CreateThread(NULL, 0, bumper, (void *)i, 0, &tid);
        check(workers[i] != NULL, "the thread started");
    }

    DWORD waited = WaitForMultipleObjects(WORKERS, workers, TRUE, 20000);
    check(waited == WAIT_OBJECT_0, "waiting for all four returned");

    check(guarded_counter == WORKERS * BUMPS, "the locked counter lost nothing");
    if (guarded_counter != WORKERS * BUMPS)
        printf("       expected %d, got %d\n", WORKERS * BUMPS, (int)guarded_counter);
    check(atomic_counter == WORKERS * BUMPS, "the interlocked counter lost nothing");
    check(tls_failures == 0, "every thread saw its own storage");

    DWORD code = 0;
    check(GetExitCodeThread(workers[0], &code) && code == 1, "a thread's return value came back");
    for (int i = 0; i < WORKERS; i++) CloseHandle(workers[i]);

    printf(" events\n");
    go_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    check(go_event && done_event, "two events were created");

    check(WaitForSingleObject(go_event, 0) == WAIT_TIMEOUT, "an unset event does not wake anybody");

    DWORD tid = 0;
    HANDLE w = CreateThread(NULL, 0, waiter, NULL, 0, &tid);
    check(w != NULL, "the waiting thread started");
    Sleep(20);                                  /* let it reach the wait */
    SetEvent(go_event);
    check(WaitForSingleObject(done_event, 5000) == WAIT_OBJECT_0, "it woke and answered");
    check(WaitForSingleObject(w, 5000) == WAIT_OBJECT_0, "and then finished");
    CloseHandle(w);

    printf(" mutexes and semaphores\n");
    HANDLE mutex = CreateMutexA(NULL, FALSE, NULL);
    check(WaitForSingleObject(mutex, 1000) == WAIT_OBJECT_0, "the mutex was taken");
    check(WaitForSingleObject(mutex, 1000) == WAIT_OBJECT_0, "and taken again by its owner");
    check(ReleaseMutex(mutex) && ReleaseMutex(mutex), "and released twice");

    HANDLE sem = CreateSemaphoreA(NULL, 2, 2, NULL);
    check(WaitForSingleObject(sem, 100) == WAIT_OBJECT_0, "the first count was taken");
    check(WaitForSingleObject(sem, 100) == WAIT_OBJECT_0, "the second count was taken");
    check(WaitForSingleObject(sem, 100) == WAIT_TIMEOUT, "and there was no third");
    LONG previous = 0;
    check(ReleaseSemaphore(sem, 2, &previous) && previous == 0, "releasing two restored it");
    check(WaitForSingleObject(sem, 100) == WAIT_OBJECT_0, "which can be taken again");

    printf(" interlocked operations\n");
    volatile LONG v = 10;
    check(InterlockedExchangeAdd(&v, 5) == 10 && v == 15, "exchange-add returns the old value");
    check(InterlockedCompareExchange(&v, 20, 15) == 15 && v == 20, "compare-exchange swaps on a match");
    check(InterlockedCompareExchange(&v, 30, 15) == 20 && v == 20, "and leaves it alone otherwise");

    DeleteCriticalSection(&lock);
    DeleteCriticalSection(&recursive);
    TlsFree(tls_index);
    return report("threads");
}

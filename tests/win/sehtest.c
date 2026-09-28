/* sehtest - structured exception handling.
 *
 * A Windows program is written in a language where a hardware fault is
 * catchable.  Code that reads a pointer inside a __try and carries on when it
 * turns out to be null is ordinary, not exotic, and a loader that kills the
 * program instead has changed what the program means.
 *
 * Each check here faults deliberately and expects to still be running
 * afterwards, which is only possible if the fault reached the handler the
 * compiler generated. */
#include "winapi.h"

#define EXCEPTION_EXECUTE_HANDLER    1
#define EXCEPTION_CONTINUE_SEARCH    0
#define EXCEPTION_ACCESS_VIOLATION   0xC0000005u
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u

typedef struct EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    void *ExceptionAddress;
    DWORD NumberParameters, __align;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD;

typedef struct { EXCEPTION_RECORD *ExceptionRecord; void *ContextRecord; } EXCEPTION_POINTERS;

/* _exception_code and _exception_info are built in to the compiler, and
 * declaring them here would clash with what it already knows. */

/* Kept out of the compiler's reach so it cannot fold the fault away. */
static volatile int *null_pointer;
static volatile int zero;
static volatile int side_effect;

static volatile int depth_reached;

/* Three calls, each doing enough that the compiler cannot fold them into one:
 * the fault is at the bottom and nothing between here and there knows what to
 * do about it. */
static void deep_three(void) {
    depth_reached = 3;
    *null_pointer = 1;
}
static void deep_two(void) {
    depth_reached = 2;
    deep_three();
    side_effect++;
}
static void deep_one(void) {
    depth_reached = 1;
    deep_two();
    side_effect++;
}

static DWORD caught_code;
static ULONG_PTR caught_address;

/* A filter that records what it was handed and asks for the handler to run. */
static int record_and_handle(EXCEPTION_POINTERS *info) {
    caught_code = info->ExceptionRecord->ExceptionCode;
    caught_address = info->ExceptionRecord->NumberParameters >= 2
                   ? info->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(void) {
    printf("sehtest: structured exception handling\n");

    printf(" reading through a null pointer\n");
    int reached_handler = 0;
    side_effect = 0;
    __try {
        side_effect = 1;
        int value = *null_pointer;        /* this faults */
        side_effect = value;              /* never reached */
    } __except (record_and_handle((EXCEPTION_POINTERS *)_exception_info())) {
        reached_handler = 1;
    }
    check(reached_handler, "the __except block ran");
    check(side_effect == 1, "the statement before the fault had taken effect");
    check(caught_code == EXCEPTION_ACCESS_VIOLATION, "the code was an access violation");
    if (caught_code != EXCEPTION_ACCESS_VIOLATION)
        printf("       code was %08x\n", caught_code);
    check(caught_address == 0, "at the address that was read");

    printf(" writing through a null pointer\n");
    reached_handler = 0;
    __try {
        *null_pointer = 5;
    } __except (record_and_handle((EXCEPTION_POINTERS *)_exception_info())) {
        reached_handler = 1;
    }
    check(reached_handler, "the __except block ran");
    check(caught_code == EXCEPTION_ACCESS_VIOLATION, "with an access violation");

    printf(" dividing by zero\n");
    reached_handler = 0;
    __try {
        side_effect = 100 / zero;
    } __except (record_and_handle((EXCEPTION_POINTERS *)_exception_info())) {
        reached_handler = 1;
    }
    check(reached_handler, "the __except block ran");
    check(caught_code == EXCEPTION_INT_DIVIDE_BY_ZERO, "with a divide-by-zero");
    if (caught_code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        printf("       code was %08x\n", caught_code);

    printf(" a constant filter\n");
    reached_handler = 0;
    __try {
        int value = *null_pointer;
        side_effect = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        reached_handler = 1;
    }
    check(reached_handler, "__except(EXCEPTION_EXECUTE_HANDLER) caught it too");

    printf(" carrying on afterwards\n");
    int sum = 0;
    for (int i = 1; i <= 10; i++) {
        __try {
            if (i == 5) sum += *null_pointer;
            else sum += i;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sum += 100;
        }
    }
    check(sum == 150, "ten iterations, one of which faulted, all completed");
    if (sum != 150) printf("       sum was %d\n", sum);

    check(1, "the program is still running after five faults");
    /* ------------------------------------------- a fault several calls down
     *
     * Every case above faults inside the __try itself.  This one faults three
     * calls deeper, which is where a real program faults: something goes
     * wrong a long way from whoever is prepared to deal with it.  Catching it
     * means walking back up the stack, undoing each function's prologue on the
     * way, and that is a different thing entirely from looking at the frame
     * the fault happened in.
     */
    reached_handler = 0;
    caught_code = 0;
    depth_reached = 0;
    __try {
        deep_one();
    } __except (record_and_handle((EXCEPTION_POINTERS *)_exception_info())) {
        reached_handler = 1;
    }
    check(reached_handler, "a fault three calls down is caught further up");
    check(depth_reached == 3, "and all three calls really happened");
    check(caught_code == EXCEPTION_ACCESS_VIOLATION,
          "with the fault it actually was");

    return report("sehtest");
}

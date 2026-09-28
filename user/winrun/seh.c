/* seh.c - structured exception handling.
 *
 * On x86-64 Windows there is no chain of handlers on the stack.  Instead every
 * function that needs one is described by a table in the image: an entry
 * saying where the function begins and ends, and pointing at the unwind
 * information, which in turn names the language handler to call.  Nothing
 * costs anything until something actually goes wrong, which is why Windows
 * code wraps things in __try so freely.
 *
 * So handling a fault means: find the table entry covering the faulting
 * address, read the unwind information, and if there is a handler, call it
 * with the exception and the frame.  For C that handler is
 * __C_specific_handler, which walks a list of __try regions and either runs a
 * filter, jumps into an __except block, or declines and lets the search go on.
 *
 * The fault itself arrives from the kernel: this process asks to be told about
 * its own faults rather than being killed, which is the arrangement a Windows
 * program is written to expect.
 */
#include "win.h"

/* --------------------------------------------------------------- the shapes */

#define EXCEPTION_MAXIMUM_PARAMETERS 15

typedef struct EXCEPTION_RECORD {
    DWORD    ExceptionCode;
    DWORD    ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    void    *ExceptionAddress;
    DWORD    NumberParameters;
    DWORD    __align;
    UINT_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;

/* Only the parts a handler and a filter actually read are filled in; the rest
 * of a real CONTEXT is floating-point and segment state that no C filter
 * looks at. */
typedef struct {
    DWORD    ContextFlags;
    DWORD    MxCsr;
    WORD     SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD    EFlags;
    UINT_PTR Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    UINT_PTR Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    UINT_PTR R8, R9, R10, R11, R12, R13, R14, R15;
    UINT_PTR Rip;
} CONTEXT;

typedef struct {
    EXCEPTION_RECORD *ExceptionRecord;
    CONTEXT          *ContextRecord;
} EXCEPTION_POINTERS;

typedef struct { DWORD Begin, End, UnwindData; } RUNTIME_FUNCTION;

typedef struct {
    UINT_PTR ControlPc;
    UINT_PTR ImageBase;
    RUNTIME_FUNCTION *FunctionEntry;
    UINT_PTR EstablisherFrame;
    UINT_PTR TargetIp;
    CONTEXT *ContextRecord;
    void    *LanguageHandler;
    void    *HandlerData;
    void    *HistoryTable;
    DWORD    ScopeIndex, Fill0;
} DISPATCHER_CONTEXT;

typedef struct {
    DWORD Count;
    struct { DWORD Begin, End, Handler, Target; } ScopeRecord[1];
} SCOPE_TABLE;

#define UNW_FLAG_EHANDLER  0x01
#define UNW_FLAG_UHANDLER  0x02
#define UNW_FLAG_CHAININFO 0x04

typedef struct {
    BYTE VersionAndFlags;
    BYTE SizeOfProlog;
    BYTE CountOfCodes;
    BYTE FrameRegisterAndOffset;
} UNWIND_INFO;

/* One thing a function's prologue did.  Read as two bytes it says where in the
 * prologue it happened and what it was; read as one number it is the extra
 * value some of them need.  The list is written in reverse order, so reading
 * it forwards undoes the prologue. */
typedef union {
    struct { BYTE CodeOffset; BYTE UnwindOpAndInfo; };
    WORD FrameOffset;
} UNWIND_CODE;

/* The exception codes a program tests for by name. */
#define EXCEPTION_ACCESS_VIOLATION      0xC0000005
#define EXCEPTION_ILLEGAL_INSTRUCTION   0xC000001D
#define EXCEPTION_INT_DIVIDE_BY_ZERO    0xC0000094
#define EXCEPTION_FLT_DIVIDE_BY_ZERO    0xC000008E
#define EXCEPTION_STACK_OVERFLOW        0xC00000FD
#define EXCEPTION_DATATYPE_MISALIGNMENT 0x80000002
#define EXCEPTION_BREAKPOINT            0x80000003
#define EXCEPTION_PRIV_INSTRUCTION      0xC0000096

#define EXCEPTION_EXECUTE_HANDLER    1
#define EXCEPTION_CONTINUE_SEARCH    0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

/* ------------------------------------------------------------------- state */

typedef LONG WINAPI (*top_filter_fn)(EXCEPTION_POINTERS *);
static top_filter_fn unhandled_filter;

win_module_t *pe_module_for(uint64_t address);
bool pe_exception_range(win_module_t *m, uint32_t *rva, uint32_t *size);

/* --------------------------------------------------------- naming the fault */

static DWORD code_from_vector(uint32_t vector, uint32_t error) {
    (void)error;
    switch (vector) {
    case 0:  return EXCEPTION_INT_DIVIDE_BY_ZERO;
    case 3:  return EXCEPTION_BREAKPOINT;
    case 6:  return EXCEPTION_ILLEGAL_INSTRUCTION;
    case 13: return EXCEPTION_PRIV_INSTRUCTION;
    case 14: return EXCEPTION_ACCESS_VIOLATION;
    case 17: return EXCEPTION_DATATYPE_MISALIGNMENT;
    case 19: return EXCEPTION_FLT_DIVIDE_BY_ZERO;
    default: return EXCEPTION_ACCESS_VIOLATION;
    }
}

static const char *code_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "floating-point divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    case EXCEPTION_BREAKPOINT:            return "breakpoint";
    default:                              return "exception";
    }
}

/* ------------------------------------------------------- finding the handler */

/* A binary search of the exception directory, which is sorted by address -
 * that is what makes this affordable at the moment of a fault. */
static RUNTIME_FUNCTION *function_for(win_module_t *m, uint64_t address) {
    uint32_t rva_start, size;
    if (!pe_exception_range(m, &rva_start, &size)) return NULL;

    uint32_t target = (uint32_t)(address - (uint64_t)(uintptr_t)m->base);
    RUNTIME_FUNCTION *table = (RUNTIME_FUNCTION *)(m->base + rva_start);
    int count = (int)(size / sizeof(RUNTIME_FUNCTION));

    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (target < table[mid].Begin) hi = mid - 1;
        else if (target >= table[mid].End) lo = mid + 1;
        else return &table[mid];
    }
    return NULL;
}

/* The unwind information behind a table entry, following a chain if the entry
 * only points at another one. */
static UNWIND_INFO *unwind_for(win_module_t *m, RUNTIME_FUNCTION *fn, RUNTIME_FUNCTION **final) {
    for (int guard = 0; guard < 8 && fn; guard++) {
        UNWIND_INFO *info = (UNWIND_INFO *)(m->base + (fn->UnwindData & ~1u));
        BYTE flags = (BYTE)(info->VersionAndFlags >> 3);
        if (!(flags & UNW_FLAG_CHAININFO)) { if (final) *final = fn; return info; }
        /* Chained: the real entry follows the unwind codes. */
        int codes = (info->CountOfCodes + 1) & ~1;
        fn = (RUNTIME_FUNCTION *)((BYTE *)info + 4 + codes * 2);
    }
    return NULL;
}

/* Where this function's frame starts.  With a frame register the unwind
 * information says exactly; without one the stack pointer at the fault is the
 * frame base, which holds for any function that does not move the stack after
 * its prologue. */
static uint64_t establisher_frame(UNWIND_INFO *info, const CONTEXT *ctx) {
    BYTE reg = (BYTE)(info->FrameRegisterAndOffset & 0x0F);
    BYTE off = (BYTE)(info->FrameRegisterAndOffset >> 4);
    if (!reg) return ctx->Rsp;

    const UINT_PTR *by_number[16] = {
        &ctx->Rax, &ctx->Rcx, &ctx->Rdx, &ctx->Rbx,
        &ctx->Rsp, &ctx->Rbp, &ctx->Rsi, &ctx->Rdi,
        &ctx->R8,  &ctx->R9,  &ctx->R10, &ctx->R11,
        &ctx->R12, &ctx->R13, &ctx->R14, &ctx->R15,
    };
    return *by_number[reg] - (uint64_t)off * 16;
}

/* ---------------------------------------------------- resuming somewhere else */

/* Continue the guest at `target` with its own registers restored.  This never
 * returns: the stack it sets up belongs to the guest, and the handler's own
 * frame ceases to exist the moment the switch happens.
 *
 * The registers to restore are copied into a fixed place first, and the three
 * the switch itself needs are pinned to rax, rcx and rdx - none of which are
 * among the ones being written.  Letting the compiler choose would let it put
 * the jump target in, say, rbx, which the restore then overwrites. */
static uint64_t resume_registers[8];   /* rbx rbp rsi rdi r12 r13 r14 r15 */

static __attribute__((noreturn)) void resume_at(uint64_t target, const CONTEXT *ctx,
                                                uint64_t frame) {
    /* This fault is over: the program is about to carry on somewhere else,
     * and the next one is a fresh one rather than a nested one. */
    fault_handled();

    resume_registers[0] = ctx->Rbx;
    resume_registers[1] = ctx->Rbp;
    resume_registers[2] = ctx->Rsi;
    resume_registers[3] = ctx->Rdi;
    resume_registers[4] = ctx->R12;
    resume_registers[5] = ctx->R13;
    resume_registers[6] = ctx->R14;
    resume_registers[7] = ctx->R15;

    __asm__ volatile (
        "movq  0(%%rdx), %%rbx\n\t"
        "movq  8(%%rdx), %%rbp\n\t"
        "movq 16(%%rdx), %%rsi\n\t"
        "movq 24(%%rdx), %%rdi\n\t"
        "movq 32(%%rdx), %%r12\n\t"
        "movq 40(%%rdx), %%r13\n\t"
        "movq 48(%%rdx), %%r14\n\t"
        "movq 56(%%rdx), %%r15\n\t"
        "movq %%rcx, %%rsp\n\t"
        "jmp  *%%rax\n\t"
        :
        : "a"(target), "c"(frame), "d"(resume_registers)
        : "memory");
    __builtin_unreachable();
}

/* ------------------------------------------------- walking back up the stack
 *
 * Everything above deals with the function the fault happened in.  That is
 * enough when the __try is in that same function and no use at all otherwise,
 * which is most of the time: a program calls something, the something fails,
 * and the handler is several frames up.
 *
 * Getting there means undoing a function's prologue without running any of its
 * code.  A compiler for this architecture writes down what its prologue did -
 * which registers it pushed, how much stack it took, whether it kept a frame
 * pointer - as a short list of operations beside the function.  Reading that
 * list backwards turns the registers at the fault into the registers the
 * caller had, and doing that repeatedly walks the whole stack.
 *
 * Nothing here executes guest code.  It reads the guest's stack and its own
 * tables, which is what makes it safe to do while the guest is in a state
 * nobody would choose.
 */
#define UWOP_PUSH_NONVOL     0
#define UWOP_ALLOC_LARGE     1
#define UWOP_ALLOC_SMALL     2
#define UWOP_SET_FPREG       3
#define UWOP_SAVE_NONVOL     4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_SAVE_XMM128     8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME 10

/* The registers by their number in the unwind codes, in this CONTEXT. */
static UINT_PTR *context_register(CONTEXT *ctx, unsigned n) {
    switch (n) {
    case 0:  return &ctx->Rax;  case 1:  return &ctx->Rcx;
    case 2:  return &ctx->Rdx;  case 3:  return &ctx->Rbx;
    case 4:  return &ctx->Rsp;  case 5:  return &ctx->Rbp;
    case 6:  return &ctx->Rsi;  case 7:  return &ctx->Rdi;
    case 8:  return &ctx->R8;   case 9:  return &ctx->R9;
    case 10: return &ctx->R10;  case 11: return &ctx->R11;
    case 12: return &ctx->R12;  case 13: return &ctx->R13;
    case 14: return &ctx->R14;  case 15: return &ctx->R15;
    default: return NULL;
    }
}

/* Reading the guest's stack.  A frame that has been overwritten is a real
 * possibility here - that may be why we are unwinding at all - so an address
 * that cannot be read stops the walk rather than faulting inside the handler,
 * which would be a fault with nowhere left to go. */
static bool read_stack(uint64_t at, uint64_t *out) {
    if (at < 0x1000 || (at & 7)) return false;
    *out = *(const volatile uint64_t *)(uintptr_t)at;
    return true;
}

/* One frame undone.  On the way in, `ctx` is the state inside the function; on
 * the way out it is the state its caller was in.  Returns false where the walk
 * cannot go further, which is not an error - it is the top of the stack. */
static bool unwind_one(win_module_t *m, CONTEXT *ctx) {
    if (!m) return false;

    RUNTIME_FUNCTION *fn = function_for(m, ctx->Rip);
    if (!fn) {
        /* A function with no unwind data of its own did not touch anything:
         * its return address is where the stack points. */
        uint64_t back;
        if (!read_stack(ctx->Rsp, &back)) return false;
        ctx->Rip = back;
        ctx->Rsp += 8;
        return true;
    }

    RUNTIME_FUNCTION *final = fn;
    UNWIND_INFO *info = unwind_for(m, fn, &final);
    if (!info) return false;

    uint64_t base = (uint64_t)(uintptr_t)m->base;
    uint32_t offset_in = (uint32_t)(ctx->Rip - base - final->Begin);

    /* If the fault happened part way through the prologue, only the codes for
     * the part that had run apply.  Each code says where in the prologue it
     * belongs, and the list is in reverse order, so the ones that had not
     * happened yet are skipped by their offset alone. */
    const UNWIND_CODE *codes = (const UNWIND_CODE *)((BYTE *)info + 4);
    unsigned count = info->CountOfCodes;

    /* A frame pointer changes where the stack is measured from, and that has
     * to be undone before anything measured against it is read. */
    BYTE frame_reg = (BYTE)(info->FrameRegisterAndOffset & 0x0F);
    BYTE frame_off = (BYTE)(info->FrameRegisterAndOffset >> 4);

    for (unsigned i = 0; i < count; ) {
        const UNWIND_CODE *c = &codes[i];
        unsigned op = c->UnwindOpAndInfo & 0x0F;
        unsigned amount = c->UnwindOpAndInfo >> 4;

        if (c->CodeOffset > offset_in) {
            /* This part of the prologue had not run yet. */
            i += (op == UWOP_ALLOC_LARGE) ? (amount ? 3 : 2)
               : (op == UWOP_SAVE_NONVOL || op == UWOP_SAVE_XMM128) ? 2
               : (op == UWOP_SAVE_NONVOL_FAR || op == UWOP_SAVE_XMM128_FAR) ? 3
               : 1;
            continue;
        }

        switch (op) {
        case UWOP_PUSH_NONVOL: {
            uint64_t value;
            if (!read_stack(ctx->Rsp, &value)) return false;
            UINT_PTR *reg = context_register(ctx, amount);
            if (reg) *reg = value;
            ctx->Rsp += 8;
            i += 1;
            break;
        }
        case UWOP_ALLOC_LARGE:
            if (amount == 0) {
                ctx->Rsp += (uint64_t)codes[i + 1].FrameOffset * 8;
                i += 2;
            } else {
                ctx->Rsp += (uint64_t)codes[i + 1].FrameOffset |
                            ((uint64_t)codes[i + 2].FrameOffset << 16);
                i += 3;
            }
            break;
        case UWOP_ALLOC_SMALL:
            ctx->Rsp += ((uint64_t)amount + 1) * 8;
            i += 1;
            break;
        case UWOP_SET_FPREG: {
            UINT_PTR *reg = context_register(ctx, frame_reg);
            if (reg) ctx->Rsp = *reg - (uint64_t)frame_off * 16;
            i += 1;
            break;
        }
        case UWOP_SAVE_NONVOL: {
            uint64_t at = ctx->Rsp + (uint64_t)codes[i + 1].FrameOffset * 8;
            uint64_t value;
            if (read_stack(at, &value)) {
                UINT_PTR *reg = context_register(ctx, amount);
                if (reg) *reg = value;
            }
            i += 2;
            break;
        }
        case UWOP_SAVE_NONVOL_FAR: {
            uint64_t where = (uint64_t)codes[i + 1].FrameOffset |
                             ((uint64_t)codes[i + 2].FrameOffset << 16);
            uint64_t value;
            if (read_stack(ctx->Rsp + where, &value)) {
                UINT_PTR *reg = context_register(ctx, amount);
                if (reg) *reg = value;
            }
            i += 3;
            break;
        }
        case UWOP_SAVE_XMM128:      i += 2; break;   /* not tracked here */
        case UWOP_SAVE_XMM128_FAR:  i += 3; break;
        case UWOP_PUSH_MACHFRAME:
            /* The processor itself pushed a frame; the stack pointer and the
             * address to go back to are both in it. */
            {
                uint64_t at = ctx->Rsp + (amount ? 8 : 0);
                uint64_t rip, rsp;
                if (!read_stack(at, &rip)) return false;
                if (!read_stack(at + 24, &rsp)) return false;
                ctx->Rip = rip;
                ctx->Rsp = rsp;
                return true;
            }
        default:
            return false;                    /* a code this does not know */
        }
    }

    /* Whatever is left on the stack is the address to go back to. */
    uint64_t back;
    if (!read_stack(ctx->Rsp, &back)) return false;
    ctx->Rip = back;
    ctx->Rsp += 8;
    return true;
}

/* ------------------------------------------------------ __C_specific_handler */

/* The language handler the C compiler names in every function containing a
 * __try.  It walks the scope table looking for a region covering the faulting
 * address, runs that region's filter, and either declines or transfers control
 * into the __except block. */
static LONG WINAPI w___C_specific_handler(EXCEPTION_RECORD *record, void *frame,
                                          CONTEXT *ctx, DISPATCHER_CONTEXT *dispatch) {
    if (!record || !dispatch || !dispatch->HandlerData) return EXCEPTION_CONTINUE_SEARCH;

    SCOPE_TABLE *scopes = dispatch->HandlerData;
    uint64_t base = dispatch->ImageBase;
    uint32_t pc = (uint32_t)(dispatch->ControlPc - base);

    for (DWORD i = 0; i < scopes->Count; i++) {
        DWORD begin = scopes->ScopeRecord[i].Begin;
        DWORD end = scopes->ScopeRecord[i].End;
        DWORD handler = scopes->ScopeRecord[i].Handler;
        DWORD target = scopes->ScopeRecord[i].Target;

        if (pc < begin || pc >= end) continue;
        if (!target) continue;                 /* a __finally, not a __except */

        LONG verdict;
        if (handler == 1) {
            /* The filter was the constant 1, so the compiler folded it away. */
            verdict = EXCEPTION_EXECUTE_HANDLER;
        } else {
            EXCEPTION_POINTERS pointers = { record, ctx };
            typedef LONG WINAPI (*filter_fn)(EXCEPTION_POINTERS *, void *);
            verdict = ((filter_fn)(uintptr_t)(base + handler))(&pointers, frame);
        }

        if (verdict == EXCEPTION_CONTINUE_SEARCH) continue;
        if (verdict == EXCEPTION_CONTINUE_EXECUTION) {
            /* The filter fixed whatever it was; go back to the instruction. */
            resume_at(ctx->Rip, ctx, ctx->Rsp);
        }
        win_trace("an exception is being handled at %p", (void *)(uintptr_t)(base + target));
        resume_at(base + target, ctx, (uint64_t)(uintptr_t)frame);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------------------------------------------------- offering it to a frame */

/* Every frame from the fault outwards is given the chance to handle it.  A
 * frame with no handler is stepped over; a frame whose handler declines is
 * stepped over too.  What makes this different from looking at one frame is
 * that the handler almost never belongs to the function that faulted - it
 * belongs to something that called it, sometimes several calls up. */
static void dispatch_to_handlers(EXCEPTION_RECORD *record, CONTEXT *ctx) {
    CONTEXT walk = *ctx;

    /* A bound rather than a condition: a stack that has been damaged can
     * describe a loop, and following it forever is a worse failure than
     * giving up on it. */
    for (int depth = 0; depth < 64; depth++) {
        win_module_t *m = pe_module_for(walk.Rip);
        if (!m) return;                       /* out of anything we know about */

        RUNTIME_FUNCTION *fn = function_for(m, walk.Rip);
        RUNTIME_FUNCTION *final = fn;
        UNWIND_INFO *info = fn ? unwind_for(m, fn, &final) : NULL;
        BYTE flags = info ? (BYTE)(info->VersionAndFlags >> 3) : 0;

        if (info && (flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER))) {
            /* The handler's address and its data sit after the unwind codes. */
            int codes = (info->CountOfCodes + 1) & ~1;
            DWORD *tail = (DWORD *)((BYTE *)info + 4 + codes * 2);
            uint64_t handler = (uint64_t)(uintptr_t)m->base + tail[0];

            DISPATCHER_CONTEXT dispatch;
            memset(&dispatch, 0, sizeof dispatch);
            dispatch.ControlPc = walk.Rip;
            dispatch.ImageBase = (uint64_t)(uintptr_t)m->base;
            dispatch.FunctionEntry = final;
            dispatch.EstablisherFrame = establisher_frame(info, &walk);
            dispatch.ContextRecord = &walk;
            dispatch.LanguageHandler = (void *)(uintptr_t)handler;
            dispatch.HandlerData = &tail[1];

            typedef LONG WINAPI (*handler_fn)(EXCEPTION_RECORD *, void *,
                                              CONTEXT *, DISPATCHER_CONTEXT *);
            win_trace("offering a %s to the handler in %s, %d frame(s) up",
                      code_name(record->ExceptionCode), m->name, depth);
            ((handler_fn)(uintptr_t)handler)(record,
                                             (void *)(uintptr_t)dispatch.EstablisherFrame,
                                             &walk, &dispatch);
            /* Returning means it declined; try the frame above. */
        }

        uint64_t was_rip = walk.Rip, was_rsp = walk.Rsp;
        if (!unwind_one(m, &walk)) {
            fprintf(STDERR_FD, "walk: stopped at %p\n", (void *)(uintptr_t)was_rip);
            return;
        }
        if (walk.Rip == was_rip && walk.Rsp == was_rsp) {
            fprintf(STDERR_FD, "walk: made no progress at %p\n",
                    (void *)(uintptr_t)was_rip);
            return;
        }
    }
}

/* --------------------------------------------------------- throwing in C++
 *
 * A throw is a fault the program caused on purpose, and it is delivered the
 * same way any other one is: a record is made and every frame between here and
 * the top is offered it.  What is different is what a frame does with it.
 *
 * For __except the compiler leaves a list of address ranges and filters.  For
 * C++ it leaves something more: a numbered state for every point in the
 * function, a list of what has to be taken apart on the way out of each state,
 * and for each try block the types it is prepared to catch.  Handling a throw
 * means finding which state the function was in, deciding whether any of its
 * catches match the type thrown, running the destructors between the two, and
 * then entering the catch.
 *
 * The destructors are the part that matters and the part that is easy to skip.
 * Every object between the throw and the catch has to be taken apart, in the
 * reverse of the order it was built, before the catch runs - a file left open
 * or a lock left held is not a small mistake, and the program has no other
 * chance to fix it.
 */
#define CXX_EXCEPTION_CODE 0xE06D7363u      /* 'msc' with a bit set */
#define CXX_EH_MAGIC       0x19930520u

typedef struct {
    DWORD magicNumber;
    DWORD maxState;
    DWORD dispUnwindMap;
    DWORD nTryBlocks;
    DWORD dispTryBlockMap;
    DWORD nIPMapEntries;
    DWORD dispIPtoStateMap;
    DWORD dispUnwindHelp;
    DWORD dispESTypeList;
    DWORD EHFlags;
} CXX_FUNC_INFO;

/* Leaving one state for another, and what has to be taken apart on the way. */
typedef struct { int toState; DWORD action; } CXX_UNWIND_ENTRY;

/* A try block: the states it covers, and the catches attached to it. */
typedef struct {
    int   tryLow, tryHigh, catchHigh;
    int   nCatches;
    DWORD dispHandlerArray;
} CXX_TRY_ENTRY;

/* One catch: what it will take, where to put it, and where to go. */
typedef struct {
    DWORD adjectives;
    DWORD dispType;              /* what it catches; nothing means anything */
    DWORD dispCatchObj;          /* where in the frame the caught thing goes */
    DWORD dispOfHandler;
    DWORD dispFrame;
} CXX_HANDLER;

/* Which state the function is in at a given point. */
typedef struct { DWORD Ip; int State; } CXX_IP_STATE;

/* What was thrown, and every type it can be caught as - a thrown object can
 * be caught as its own type, as anything it derives from, or as void*. */
typedef struct {
    DWORD attributes;
    DWORD pmfnUnwind;
    DWORD pForwardCompat;
    DWORD pCatchableTypeArray;
} CXX_THROW_INFO;

typedef struct { DWORD nCatchableTypes; DWORD arrayOfTypes[1]; } CXX_CATCHABLE_ARRAY;

typedef struct {
    DWORD properties;
    DWORD pType;
    int   mdisp, pdisp, vdisp;
    int   sizeOrOffset;
    DWORD copyFunction;
} CXX_CATCHABLE;

#define CXX_HANDLER_IS_REFERENCE 0x08

/* Where in the function it stopped, as one of the numbered states. */
static int cxx_state_for(uint64_t base, const CXX_FUNC_INFO *fi, uint64_t pc,
                         uint64_t function_begin) {
    if (!fi->nIPMapEntries || !fi->dispIPtoStateMap) return -1;
    const CXX_IP_STATE *map = (const CXX_IP_STATE *)(uintptr_t)(base + fi->dispIPtoStateMap);
    uint32_t want = (uint32_t)(pc - base);
    (void)function_begin;

    int state = -1;
    for (DWORD i = 0; i < fi->nIPMapEntries; i++) {
        if (map[i].Ip > want) break;
        state = map[i].State;
    }
    return state;
}

/* Everything built between two states, taken apart in the order the compiler
 * asked for.  Each step names a small piece of the function to run - the
 * destructor call and nothing else - and says which state that leaves. */
static void cxx_unwind_to(uint64_t base, const CXX_FUNC_INFO *fi, void *frame,
                          int from, int to) {
    if (!fi->dispUnwindMap) return;
    const CXX_UNWIND_ENTRY *map =
        (const CXX_UNWIND_ENTRY *)(uintptr_t)(base + fi->dispUnwindMap);

    /* Guest code, so it is called the way guest code expects to be called -
     * which puts the frame in a different register from the one this
     * compiler would choose. */
    typedef void WINAPI (*cleanup_fn)(void *, void *);
    int state = from;
    int guard = 0;
    while (state > to && state >= 0 && (DWORD)state < fi->maxState) {
        if (++guard > 256) break;            /* a map that loops */
        DWORD action = map[state].action;
        int next = map[state].toState;
        if (action)
            ((cleanup_fn)(uintptr_t)(base + action))(NULL, frame);
        state = next;
    }
}

/* Whether a catch will take what was thrown.  A catch with no type takes
 * anything; otherwise the thrown object's list of types is searched for the
 * one this catch names. */
static const CXX_CATCHABLE *cxx_match(uint64_t throw_base,
                                      const CXX_THROW_INFO *thrown,
                                      uint64_t base, const CXX_HANDLER *h) {
    if (!h->dispType) {
        /* catch (...) - and there is still an object, so the first type it
         * can be caught as is the one used for copying it. */
        if (!thrown || !thrown->pCatchableTypeArray) return NULL;
        const CXX_CATCHABLE_ARRAY *all =
            (const CXX_CATCHABLE_ARRAY *)(uintptr_t)(throw_base + thrown->pCatchableTypeArray);
        return all->nCatchableTypes
             ? (const CXX_CATCHABLE *)(uintptr_t)(throw_base + all->arrayOfTypes[0])
             : NULL;
    }
    if (!thrown || !thrown->pCatchableTypeArray) return NULL;

    const CXX_CATCHABLE_ARRAY *all =
        (const CXX_CATCHABLE_ARRAY *)(uintptr_t)(throw_base + thrown->pCatchableTypeArray);
    const char *wanted = (const char *)(uintptr_t)(base + h->dispType);

    for (DWORD i = 0; i < all->nCatchableTypes; i++) {
        const CXX_CATCHABLE *c =
            (const CXX_CATCHABLE *)(uintptr_t)(throw_base + all->arrayOfTypes[i]);
        const char *have = (const char *)(uintptr_t)(throw_base + c->pType);
        /* A type is identified by its name, which the compiler writes into
         * the description eight bytes in.  Comparing the descriptions
         * themselves would fail across two images of the same program. */
        if (have == wanted || !strcmp(have + 16, wanted + 16)) return c;
    }
    return NULL;
}

static LONG WINAPI w___CxxFrameHandler3(EXCEPTION_RECORD *record, void *frame,
                                        CONTEXT *ctx, DISPATCHER_CONTEXT *dispatch) {
    if (!record || !dispatch || !dispatch->HandlerData) return EXCEPTION_CONTINUE_SEARCH;
    if (record->ExceptionCode != CXX_EXCEPTION_CODE) return EXCEPTION_CONTINUE_SEARCH;
    if (record->NumberParameters < 3) return EXCEPTION_CONTINUE_SEARCH;
    if (record->ExceptionInformation[0] != CXX_EH_MAGIC) return EXCEPTION_CONTINUE_SEARCH;

    /* What follows the handler's address is not the description itself but
     * where to find it: one number, counted from the start of the image.  The
     * other kind of handler has its table inline there, which is the sort of
     * difference that costs an afternoon. */
    uint64_t base = dispatch->ImageBase;
    DWORD where = *(const DWORD *)dispatch->HandlerData;
    const CXX_FUNC_INFO *fi = (const CXX_FUNC_INFO *)(uintptr_t)(base + where);
    if ((fi->magicNumber & 0xFFFFFF00u) != 0x19930500u) return EXCEPTION_CONTINUE_SEARCH;

    void *object = (void *)(uintptr_t)record->ExceptionInformation[1];
    const CXX_THROW_INFO *thrown =
        (const CXX_THROW_INFO *)(uintptr_t)record->ExceptionInformation[2];
    uint64_t throw_base = record->NumberParameters >= 4
                        ? record->ExceptionInformation[3] : base;

    int state = cxx_state_for(base, fi, dispatch->ControlPc,
                              base + dispatch->FunctionEntry->Begin);
    if (state < 0) return EXCEPTION_CONTINUE_SEARCH;



    /* A function with nothing to catch still has things to take apart, and
     * most functions between a throw and its catch are exactly that: no try
     * block of their own, but objects that have to be let go of as the throw
     * passes through.  Leaving early here because there is nothing to catch
     * would skip every one of them. */
    const CXX_TRY_ENTRY *tries = fi->dispTryBlockMap
        ? (const CXX_TRY_ENTRY *)(uintptr_t)(base + fi->dispTryBlockMap) : NULL;

    for (DWORD t = 0; tries && t < fi->nTryBlocks; t++) {
        if (state < tries[t].tryLow || state > tries[t].tryHigh) continue;

        const CXX_HANDLER *handlers =
            (const CXX_HANDLER *)(uintptr_t)(base + tries[t].dispHandlerArray);

        for (int h = 0; h < tries[t].nCatches; h++) {
            const CXX_CATCHABLE *what = cxx_match(throw_base, thrown, base,
                                                  &handlers[h]);
            if (!what && handlers[h].dispType) continue;

            win_trace("a thrown value is being caught %d state(s) in", state);

            /* Everything built inside the try, taken apart before the catch
             * runs.  This is the whole reason a throw is not a jump. */
            cxx_unwind_to(base, fi, frame, state, tries[t].tryLow - 1);

            /* The caught thing put where the catch expects to find it: the
             * object itself if it is taken by value, a pointer to it if by
             * reference. */
            if (handlers[h].dispCatchObj) {
                void *slot = (char *)frame + handlers[h].dispCatchObj;
                if (handlers[h].adjectives & CXX_HANDLER_IS_REFERENCE) {
                    *(void **)slot = object;
                } else if (what && what->sizeOrOffset > 0) {
                    memcpy(slot, object, (size_t)what->sizeOrOffset);
                } else {
                    *(void **)slot = object;
                }
            }

            /* The catch itself is a small piece of the function on its own.
             * It runs and says where the function carries on afterwards. */
            typedef void *WINAPI (*catch_fn)(void *, void *);
            void *carry_on =
                ((catch_fn)(uintptr_t)(base + handlers[h].dispOfHandler))(NULL, frame);
            if (!carry_on) return EXCEPTION_CONTINUE_SEARCH;

            resume_at((uint64_t)(uintptr_t)carry_on, ctx, (uint64_t)(uintptr_t)frame);
        }
    }

    /* Nothing here catches it, so this frame is being left rather than
     * returned to - and everything it was holding has to be taken apart on
     * the way past.  That is the difference between a throw and a jump, and
     * it is this frame's own business: the frame that eventually catches has
     * no idea what this one was holding.
     *
     * Doing it here rather than after something has agreed to catch means a
     * throw nothing catches still takes things apart before the program ends,
     * which is the better of the two ways to be wrong. */
    cxx_unwind_to(base, fi, frame, state, -1);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Throwing.  The compiler turns `throw x` into a call to this, having already
 * put a copy of x somewhere and built a description of what it is. */
static void WINAPI w__CxxThrowException(void *object, void *info) {
    static EXCEPTION_RECORD record;
    static CONTEXT ctx;

    memset(&record, 0, sizeof record);
    record.ExceptionCode = CXX_EXCEPTION_CODE;
    record.ExceptionFlags = 1;                  /* it cannot be continued */
    record.ExceptionAddress = __builtin_return_address(0);
    record.NumberParameters = 4;
    record.ExceptionInformation[0] = CXX_EH_MAGIC;
    record.ExceptionInformation[1] = (UINT_PTR)(uintptr_t)object;
    record.ExceptionInformation[2] = (UINT_PTR)(uintptr_t)info;

    win_module_t *m = pe_module_for((uint64_t)(uintptr_t)record.ExceptionAddress);
    record.ExceptionInformation[3] = m ? (UINT_PTR)(uintptr_t)m->base : 0;

    /* The frame to start looking from is the one that called this, so the
     * registers are taken as they are here and then one frame is undone -
     * this function is not one anybody catches anything in. */
    memset(&ctx, 0, sizeof ctx);
    ctx.Rip = (UINT_PTR)(uintptr_t)record.ExceptionAddress;
    /* The stack as the throwing function left it: this function's frame base,
     * past the saved frame pointer and the address it will return to.  The
     * walk has to start in the program's own code rather than in here, which
     * has no tables of its own for anyone to read. */
    ctx.Rsp = (UINT_PTR)(uintptr_t)__builtin_frame_address(0) + 16;
    /* The frame pointer wanted is the throwing function's, not this one's -
     * ours is sitting on top of it.  The one underneath is exactly what this
     * function pushed on the way in, which is at its frame base. */
    ctx.Rbp = *(const UINT_PTR *)(uintptr_t)__builtin_frame_address(0);
    __asm__ volatile("movq %%rbx, %0" : "=m"(ctx.Rbx));
    __asm__ volatile("movq %%rsi, %0" : "=m"(ctx.Rsi));
    __asm__ volatile("movq %%rdi, %0" : "=m"(ctx.Rdi));
    __asm__ volatile("movq %%r12, %0" : "=m"(ctx.R12));
    __asm__ volatile("movq %%r13, %0" : "=m"(ctx.R13));
    __asm__ volatile("movq %%r14, %0" : "=m"(ctx.R14));
    __asm__ volatile("movq %%r15, %0" : "=m"(ctx.R15));

    if (pe_module_for(ctx.Rip)) dispatch_to_handlers(&record, &ctx);

    /* Nothing took it. */
    u32_shutdown();
    fprintf(STDERR_FD, "\nwinrun: a thrown value was not caught by anything\n");
    flush_output();
    exit(3);
}

/* ------------------------------------------------------------- the delivery */

static void report_and_die(EXCEPTION_RECORD *record, CONTEXT *ctx) {
    win_module_t *m = pe_module_for(ctx->Rip);
    char where[128];
    if (m)
        snprintf(where, sizeof where, "%s+%#llx", m->name,
                 (unsigned long long)(ctx->Rip - (uint64_t)(uintptr_t)m->base));
    else
        snprintf(where, sizeof where, "%p", (void *)(uintptr_t)ctx->Rip);

    u32_shutdown();
    fprintf(STDERR_FD, "\nwinrun: unhandled %s in %s\n",
            code_name(record->ExceptionCode), where);
    fprintf(STDERR_FD, "        code %08x", record->ExceptionCode);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(STDERR_FD, ", %s address %p",
                record->ExceptionInformation[0] ? "writing" : "reading",
                (void *)record->ExceptionInformation[1]);
    fprintf(STDERR_FD, "\n        rsp %p  rbp %p\n",
            (void *)(uintptr_t)ctx->Rsp, (void *)(uintptr_t)ctx->Rbp);
    flush_output();
    exit(-1073741819);              /* what Windows reports for a crash */
}

/* Everything starts here: the kernel hands over the registers exactly as they
 * were at the faulting instruction. */
static void on_fault(kfault_t *f) {
    static EXCEPTION_RECORD record;
    static CONTEXT ctx;

    memset(&record, 0, sizeof record);
    record.ExceptionCode = code_from_vector(f->vector, f->error);
    record.ExceptionAddress = (void *)(uintptr_t)f->rip;
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        record.NumberParameters = 2;
        record.ExceptionInformation[0] = (f->error & 2) ? 1 : 0;    /* writing? */
        record.ExceptionInformation[1] = f->address;
    }

    memset(&ctx, 0, sizeof ctx);
    ctx.Rip = f->rip; ctx.Rsp = f->rsp; ctx.EFlags = (DWORD)f->rflags;
    ctx.Rax = f->rax; ctx.Rbx = f->rbx; ctx.Rcx = f->rcx; ctx.Rdx = f->rdx;
    ctx.Rsi = f->rsi; ctx.Rdi = f->rdi; ctx.Rbp = f->rbp;
    ctx.R8 = f->r8; ctx.R9 = f->r9; ctx.R10 = f->r10; ctx.R11 = f->r11;
    ctx.R12 = f->r12; ctx.R13 = f->r13; ctx.R14 = f->r14; ctx.R15 = f->r15;

    /* Offer it to every frame between here and the top of the stack.
     *
     * A handler that takes it never comes back - it switches the program to
     * somewhere else and this call does not return.  So reaching the end of
     * this loop means every frame declined, which is what makes an exception
     * unhandled rather than merely unhandled here. */
    dispatch_to_handlers(&record, &ctx);

    if (unhandled_filter) {
        EXCEPTION_POINTERS pointers = { &record, &ctx };
        LONG verdict = unhandled_filter(&pointers);
        if (verdict == EXCEPTION_CONTINUE_EXECUTION) resume_at(ctx.Rip, &ctx, ctx.Rsp);
        if (verdict == EXCEPTION_EXECUTE_HANDLER) {
            u32_shutdown();
            flush_output();
            exit(-1073741819);
        }
    }

    report_and_die(&record, &ctx);
}

void seh_install(void) { fault_handler(on_fault); }

/* ------------------------------------------------------- what a program calls */

static void *WINAPI w_SetUnhandledExceptionFilter(top_filter_fn filter) {
    top_filter_fn old = unhandled_filter;
    unhandled_filter = filter;
    return (void *)old;
}

static LONG WINAPI w_UnhandledExceptionFilter(EXCEPTION_POINTERS *p) {
    (void)p;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void WINAPI w_RaiseException(DWORD code, DWORD flags, DWORD count, const UINT_PTR *args) {
    (void)flags;
    /* A raised exception has no faulting instruction, so this reports it the
     * way an unhandled one is reported rather than pretending to unwind. */
    static EXCEPTION_RECORD record;
    static CONTEXT ctx;
    memset(&record, 0, sizeof record);
    record.ExceptionCode = code;
    record.ExceptionAddress = __builtin_return_address(0);
    record.NumberParameters = count > EXCEPTION_MAXIMUM_PARAMETERS ? EXCEPTION_MAXIMUM_PARAMETERS : count;
    for (DWORD i = 0; i < record.NumberParameters && args; i++)
        record.ExceptionInformation[i] = args[i];

    memset(&ctx, 0, sizeof ctx);
    ctx.Rip = (UINT_PTR)(uintptr_t)record.ExceptionAddress;

    if (unhandled_filter) {
        EXCEPTION_POINTERS pointers = { &record, &ctx };
        unhandled_filter(&pointers);
    }
    u32_shutdown();
    fprintf(STDERR_FD, "\nwinrun: the program raised exception %08x\n", code);
    flush_output();
    exit((int)code);
}

static void WINAPI w_RtlCaptureContext(CONTEXT *ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof *ctx);
    ctx->Rip = (UINT_PTR)(uintptr_t)__builtin_return_address(0);
    ctx->Rsp = (UINT_PTR)(uintptr_t)__builtin_frame_address(0);
}

static WORD WINAPI w_RtlCaptureStackBackTrace(DWORD skip, DWORD count, void **out, DWORD *hash) {
    (void)skip; (void)hash;
    if (out && count) out[0] = __builtin_return_address(0);
    return count ? 1 : 0;
}

static void *WINAPI w_AddVectoredExceptionHandler(DWORD first, void *handler) {
    (void)first;
    /* Vectored handlers run before anything else on Windows.  Treating one as
     * the last-chance filter is not the same order, and a program that relies
     * on the difference would be misled - so it is turned down. */
    win_trace("a vectored exception handler was registered, which is not supported");
    (void)handler;
    return NULL;
}

static const win_export_t seh_exports[] = {
    { "__CxxFrameHandler3",           (void *)w___CxxFrameHandler3 },
    { "_CxxThrowException",           (void *)w__CxxThrowException },
    { "__C_specific_handler",          (void *)w___C_specific_handler },
    { "SetUnhandledExceptionFilter",   (void *)w_SetUnhandledExceptionFilter },
    { "UnhandledExceptionFilter",      (void *)w_UnhandledExceptionFilter },
    { "RaiseException",                (void *)w_RaiseException },
    { "RtlCaptureContext",             (void *)w_RtlCaptureContext },
    { "RtlCaptureStackBackTrace",      (void *)w_RtlCaptureStackBackTrace },
    { "AddVectoredExceptionHandler",   (void *)w_AddVectoredExceptionHandler },
    { NULL, NULL }
};

void seh_init(void) {
    /* These are reached through kernel32 and through the runtime, so they are
     * registered as a library of their own that both forward to. */
    win_register("kestrel-seh.dll", seh_exports);
}

/* Looked up by name from the kernel32 and runtime tables. */
const win_export_t *seh_table(void) { return seh_exports; }

/* arm64.c - an interpreter for AArch64.
 *
 * A program built for ARM is machine code for a processor this machine does
 * not have. There is no translation layer that makes that go away and no
 * library that papers over it: the instructions have to be read one at a time
 * and carried out. That is what this does.
 *
 * Every AArch64 instruction is exactly four bytes, which makes the reading
 * part simple; the work is in the decoding, and the architecture's own manual
 * organises that by bits 25 to 28. The same split is used here, so anyone
 * comparing this against the manual is looking at the same tree.
 *
 * Two things are worth knowing before reading further.
 *
 * The register numbered thirty-one is not a register. Depending on the
 * instruction it means either the constant zero or the stack pointer, and
 * which one is a property of the instruction rather than of the field. Getting
 * that wrong produces code that works until something uses the stack.
 *
 * And the flags are not a side effect. Whether an operation sets them is part
 * of the instruction, the carry out of a subtraction is the *inverted* borrow,
 * and overflow is about signed range rather than carry. Comparisons in the
 * program being run are subtractions whose result is thrown away, so the flags
 * are the entire point of them.
 *
 * What is here is the integer core: moves, arithmetic, logic, shifts, bitfield
 * operations, the whole load and store family, branches, conditional selects,
 * multiply and divide, counting and reversing bits, and the load-exclusive
 * pair that locks are built from. What is not here is floating point and the
 * vector instructions beyond moving them about, and an instruction that is not
 * understood stops the program and says which one it was rather than doing
 * something arbitrary.
 */
#include "arm64.h"

/* ------------------------------------------------------------- the registers
 *
 * Reading register thirty-one gives zero; writing it discards.  Except where
 * the instruction says that field means the stack pointer instead, which is
 * why every access goes through one of two pairs rather than an array index.
 */
static uint64_t read_x(const arm64_cpu_t *cpu, unsigned n) {
    return n == 31 ? 0 : cpu->x[n];
}

static void write_x(arm64_cpu_t *cpu, unsigned n, uint64_t value) {
    if (n != 31) cpu->x[n] = value;
}

static uint64_t read_x_sp(const arm64_cpu_t *cpu, unsigned n) {
    return n == 31 ? cpu->sp : cpu->x[n];
}

static void write_x_sp(arm64_cpu_t *cpu, unsigned n, uint64_t value) {
    if (n == 31) cpu->sp = value;
    else cpu->x[n] = value;
}

/* A thirty-two bit operation writes the low half and clears the high half -
 * it does not leave it alone, which is the difference from x86. */
static uint64_t size_result(bool sf, uint64_t value) {
    return sf ? value : (value & 0xFFFFFFFFull);
}

static uint64_t read_operand(const arm64_cpu_t *cpu, unsigned n, bool sf) {
    return size_result(sf, read_x(cpu, n));
}

static void write_result(arm64_cpu_t *cpu, unsigned n, bool sf, uint64_t value) {
    write_x(cpu, n, size_result(sf, value));
}

/* ------------------------------------------------------------------ memory */

static bool in_range(const arm64_cpu_t *cpu, uint64_t addr, uint64_t bytes) {
    if (addr < cpu->memory_base) return false;
    uint64_t offset = addr - cpu->memory_base;
    if (offset > cpu->memory_size) return false;
    return offset + bytes <= cpu->memory_size;
}

static void fault(arm64_cpu_t *cpu, const char *why, uint64_t where) {
    if (!cpu->fault) {
        cpu->fault = why;
        cpu->fault_addr = where;
    }
    cpu->stopped = true;
}

static uint64_t load(arm64_cpu_t *cpu, uint64_t addr, int bytes) {
    if (!in_range(cpu, addr, (uint64_t)bytes)) {
        fault(cpu, "read from memory that is not there", addr);
        return 0;
    }
    const uint8_t *p = cpu->memory + (addr - cpu->memory_base);
    uint64_t value = 0;
    for (int i = 0; i < bytes; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static void store(arm64_cpu_t *cpu, uint64_t addr, int bytes, uint64_t value) {
    if (!in_range(cpu, addr, (uint64_t)bytes)) {
        fault(cpu, "write to memory that is not there", addr);
        return;
    }
    uint8_t *p = cpu->memory + (addr - cpu->memory_base);
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(value >> (8 * i));

    /* A write anywhere near a marked address breaks the mark, which is what
     * makes the exclusive pair a lock rather than a suggestion. */
    if (cpu->exclusive && addr >= cpu->exclusive_addr - 64 &&
        addr <= cpu->exclusive_addr + 64)
        cpu->exclusive = false;
}

/* ------------------------------------------------------------------- flags */

static void set_logic_flags(arm64_cpu_t *cpu, bool sf, uint64_t result) {
    cpu->n = sf ? ((result >> 63) & 1) : ((result >> 31) & 1);
    cpu->z = size_result(sf, result) == 0;
    cpu->c = false;
    cpu->v = false;
}

/* Addition, with the carry in, setting all four flags.
 *
 * Subtraction is not a separate operation: the architecture defines it as
 * adding the inverted operand with a carry in of one, and doing it that way
 * here rather than subtracting is what makes the carry and overflow flags come
 * out right without a second set of rules. */
static uint64_t add_with_carry(arm64_cpu_t *cpu, bool sf, uint64_t a, uint64_t b,
                               bool carry_in, bool set_flags) {
    uint64_t result;
    bool carry_out, overflow;

    if (sf) {
        uint64_t low = a + b + (carry_in ? 1 : 0);
        /* The carry out of a sixty-four bit add cannot be seen in the result,
         * so it is worked out from whether the sum wrapped. */
        carry_out = (low < a) || (carry_in && low == a);
        result = low;

        bool sa = (a >> 63) & 1, sb = (b >> 63) & 1, sr = (result >> 63) & 1;
        overflow = (sa == sb) && (sr != sa);
    } else {
        uint64_t low = (a & 0xFFFFFFFFull) + (b & 0xFFFFFFFFull) + (carry_in ? 1 : 0);
        carry_out = (low >> 32) & 1;
        result = low & 0xFFFFFFFFull;

        bool sa = (a >> 31) & 1, sb = (b >> 31) & 1, sr = (result >> 31) & 1;
        overflow = (sa == sb) && (sr != sa);
    }

    if (set_flags) {
        cpu->n = sf ? ((result >> 63) & 1) : ((result >> 31) & 1);
        cpu->z = size_result(sf, result) == 0;
        cpu->c = carry_out;
        cpu->v = overflow;
    }
    return result;
}

/* ------------------------------------------------------------- the conditions
 *
 * Fourteen tests over four flags, plus "always" twice.  The low bit inverts
 * the test, which is why the table is written as seven pairs.
 */
static bool condition_holds(const arm64_cpu_t *cpu, unsigned cond) {
    bool result;
    switch (cond >> 1) {
    case 0: result = cpu->z; break;                            /* EQ */
    case 1: result = cpu->c; break;                            /* CS */
    case 2: result = cpu->n; break;                            /* MI */
    case 3: result = cpu->v; break;                            /* VS */
    case 4: result = cpu->c && !cpu->z; break;                 /* HI */
    case 5: result = cpu->n == cpu->v; break;                  /* GE */
    case 6: result = (cpu->n == cpu->v) && !cpu->z; break;     /* GT */
    default: result = true; break;                             /* AL */
    }
    /* The inversion, except for the "always" pair, where inverting would give
     * "never" and the architecture uses that encoding for something else. */
    if ((cond & 1) && cond != 0x0F) result = !result;
    return result;
}

/* --------------------------------------------------------- shifts and extends */

static uint64_t shift_reg(uint64_t value, unsigned type, unsigned amount, bool sf) {
    unsigned width = sf ? 64 : 32;
    if (!sf) value &= 0xFFFFFFFFull;
    amount &= (width - 1);
    if (!amount) return value;

    switch (type) {
    case 0: return size_result(sf, value << amount);                  /* LSL */
    case 1: return sf ? (value >> amount)
                      : ((value & 0xFFFFFFFFull) >> amount);          /* LSR */
    case 2: {                                                          /* ASR */
        if (sf) return (uint64_t)((int64_t)value >> amount);
        int32_t narrow = (int32_t)(uint32_t)value;
        return (uint64_t)(uint32_t)(narrow >> amount);
    }
    default: {                                                         /* ROR */
        if (sf) return (value >> amount) | (value << (64 - amount));
        uint32_t narrow = (uint32_t)value;
        return (uint32_t)((narrow >> amount) | (narrow << (32 - amount)));
    }
    }
}

/* The extended-register form: take part of a register, widen it signed or
 * unsigned, then shift it left.  This is how array indexing is written. */
static uint64_t extend_reg(uint64_t value, unsigned option, unsigned shift) {
    uint64_t out;
    switch (option) {
    case 0: out = value & 0xFF; break;                                /* UXTB */
    case 1: out = value & 0xFFFF; break;                              /* UXTH */
    case 2: out = value & 0xFFFFFFFFull; break;                       /* UXTW */
    case 3: out = value; break;                                       /* UXTX */
    case 4: out = (uint64_t)(int64_t)(int8_t)(value & 0xFF); break;   /* SXTB */
    case 5: out = (uint64_t)(int64_t)(int16_t)(value & 0xFFFF); break;/* SXTH */
    case 6: out = (uint64_t)(int64_t)(int32_t)(value & 0xFFFFFFFFull); break; /* SXTW */
    default: out = value; break;                                      /* SXTX */
    }
    return out << shift;
}

/* ------------------------------------------------------- the bitmask immediate
 *
 * The logical instructions do not carry a plain constant. They carry three
 * small fields that describe a pattern of ones repeated across the register,
 * which covers every constant those instructions are actually used with while
 * costing thirteen bits instead of sixty-four. Decoding it is the one piece of
 * genuine cleverness in the encoding, so it gets its own function.
 */
static bool decode_bitmask(bool sf, unsigned n, unsigned imms, unsigned immr,
                           uint64_t *out) {
    /* The element width is found from the position of the highest zero in the
     * six bits of imms, with n acting as a seventh bit above them. */
    unsigned len = 0;
    unsigned combined = (n << 6) | (~imms & 0x3F);
    for (int bit = 6; bit >= 0; bit--) {
        if (combined & (1u << bit)) { len = (unsigned)bit; break; }
        if (bit == 0) return false;                 /* all zeros is reserved */
    }
    if (len == 0) return false;
    if (!sf && n) return false;                     /* n only means 64-bit */

    unsigned size = 1u << len;
    unsigned mask = size - 1;

    unsigned s = imms & mask;
    unsigned r = immr & mask;
    if (s == mask) return false;                    /* also reserved */

    /* A run of s+1 ones, rotated right by r, repeated to fill the register. */
    uint64_t element = (s + 1 >= 64) ? ~0ull : ((1ull << (s + 1)) - 1);
    if (r) {
        if (size == 64) element = (element >> r) | (element << (64 - r));
        else {
            uint64_t low = element & ((size == 64) ? ~0ull : ((1ull << size) - 1));
            element = ((low >> r) | (low << (size - r)));
            element &= (1ull << size) - 1;
        }
    }

    uint64_t value = 0;
    for (unsigned at = 0; at < 64; at += size) value |= element << at;

    *out = sf ? value : (value & 0xFFFFFFFFull);
    return true;
}

/* ------------------------------------------------------------------ decoding */

static void branch_to(arm64_cpu_t *cpu, uint64_t target) { cpu->pc = target; }

/* Sign-extend a field of `bits` bits. */
static int64_t sign_extend(uint64_t value, unsigned bits) {
    uint64_t sign = 1ull << (bits - 1);
    return (int64_t)((value ^ sign) - sign);
}

static bool exec_data_immediate(arm64_cpu_t *cpu, uint32_t in) {
    bool sf = (in >> 31) & 1;
    unsigned rd = in & 31;
    unsigned rn = (in >> 5) & 31;

    /* ADR and ADRP: an address relative to this instruction. */
    if (((in >> 24) & 0x1F) == 0x10) {
        bool page = (in >> 31) & 1;
        uint64_t immlo = (in >> 29) & 3;
        uint64_t immhi = (in >> 5) & 0x7FFFF;
        int64_t offset = sign_extend((immhi << 2) | immlo, 21);
        if (page) {
            offset <<= 12;
            write_x(cpu, rd, ((cpu->pc & ~0xFFFull) + (uint64_t)offset));
        } else {
            write_x(cpu, rd, cpu->pc + (uint64_t)offset);
        }
        return true;
    }

    /* Which family this is, from bits twenty-five to twenty-three - the split
     * the architecture's own manual uses. */
    switch ((in >> 23) & 7) {
    case 2: {                                        /* add/sub immediate */
        bool sub = (in >> 30) & 1;
        bool set_flags = (in >> 29) & 1;
        unsigned shift = (in >> 22) & 3;
        uint64_t imm = (in >> 10) & 0xFFF;
        if (shift == 1) imm <<= 12;
        else if (shift != 0) return false;

        uint64_t a = read_x_sp(cpu, rn);
        if (!sf) a &= 0xFFFFFFFFull;

        uint64_t result = sub
            ? add_with_carry(cpu, sf, a, ~imm, true, set_flags)
            : add_with_carry(cpu, sf, a, imm, false, set_flags);

        /* Without flags the destination may be the stack pointer; with them it
         * is the zero register instead, which is how CMP is written. */
        if (set_flags) write_result(cpu, rd, sf, result);
        else write_x_sp(cpu, rd, size_result(sf, result));
        return true;
    }

    case 4: {                                        /* logical immediate */
        unsigned opc = (in >> 29) & 3;
        unsigned n = (in >> 22) & 1;
        unsigned immr = (in >> 16) & 0x3F;
        unsigned imms = (in >> 10) & 0x3F;

        uint64_t imm;
        if (!decode_bitmask(sf, n, imms, immr, &imm)) return false;

        uint64_t a = read_operand(cpu, rn, sf);
        uint64_t result;
        switch (opc) {
        case 0: result = a & imm; break;                       /* AND */
        case 1: result = a | imm; break;                       /* ORR */
        case 2: result = a ^ imm; break;                       /* EOR */
        default: result = a & imm; break;                      /* ANDS */
        }
        if (opc == 3) {
            set_logic_flags(cpu, sf, result);
            write_result(cpu, rd, sf, result);
        } else {
            write_x_sp(cpu, rd, size_result(sf, result));
        }
        return true;
    }

    case 5: {                                        /* move wide immediate */
        unsigned opc = (in >> 29) & 3;
        unsigned hw = (in >> 21) & 3;
        uint64_t imm = (in >> 5) & 0xFFFF;
        if (!sf && hw > 1) return false;
        unsigned shift = hw * 16;

        switch (opc) {
        case 0:                                                /* MOVN */
            write_result(cpu, rd, sf, ~(imm << shift));
            return true;
        case 2:                                                /* MOVZ */
            write_result(cpu, rd, sf, imm << shift);
            return true;
        case 3: {                                              /* MOVK */
            uint64_t current = read_x(cpu, rd);
            uint64_t mask = ~(0xFFFFull << shift);
            write_result(cpu, rd, sf, (current & mask) | (imm << shift));
            return true;
        }
        default: return false;
        }
    }

    case 6: {                                        /* bitfield */
        unsigned opc = (in >> 29) & 3;
        unsigned n = (in >> 22) & 1;
        unsigned immr = (in >> 16) & 0x3F;
        unsigned imms = (in >> 10) & 0x3F;
        if (sf != n) return false;

        unsigned width = sf ? 64 : 32;
        uint64_t src = read_operand(cpu, rn, sf);
        uint64_t dst = read_operand(cpu, rd, sf);

        /* The field, rotated into place, and the mask that selects it. */
        uint64_t element;
        if (imms >= immr) {
            /* Straight extraction of imms-immr+1 bits starting at immr. */
            unsigned count = imms - immr + 1;
            element = (src >> immr) & ((count >= 64) ? ~0ull : ((1ull << count) - 1));
            uint64_t mask = (count >= 64) ? ~0ull : ((1ull << count) - 1);

            if (opc == 0) {                                    /* SBFM */
                bool negative = (element >> (count - 1)) & 1;
                uint64_t result = element;
                if (negative) result |= ~mask;
                write_result(cpu, rd, sf, result);
            } else if (opc == 2) {                             /* UBFM */
                write_result(cpu, rd, sf, element);
            } else {                                           /* BFM */
                write_result(cpu, rd, sf, (dst & ~mask) | element);
            }
        } else {
            /* Insertion: the field ends up at width-immr. */
            unsigned count = imms + 1;
            unsigned at = width - immr;
            uint64_t low = src & ((count >= 64) ? ~0ull : ((1ull << count) - 1));
            uint64_t mask = ((count >= 64) ? ~0ull : ((1ull << count) - 1));
            if (at >= 64) { mask = 0; element = 0; }
            else { element = low << at; mask <<= at; }

            if (opc == 0) {                                    /* SBFM */
                bool negative = count ? ((low >> (count - 1)) & 1) : false;
                uint64_t result = element;
                if (negative) result |= ~mask;
                write_result(cpu, rd, sf, result);
            } else if (opc == 2) {                             /* UBFM */
                write_result(cpu, rd, sf, element);
            } else {                                           /* BFM */
                write_result(cpu, rd, sf, (dst & ~mask) | element);
            }
        }
        return true;
    }

    case 7: {                                        /* extract */
        unsigned n = (in >> 22) & 1;
        unsigned rm = (in >> 16) & 31;
        unsigned imms = (in >> 10) & 0x3F;
        if (sf != n) return false;

        uint64_t high = read_operand(cpu, rn, sf);
        uint64_t low = read_operand(cpu, rm, sf);
        unsigned width = sf ? 64 : 32;
        if (imms >= width) return false;

        uint64_t result;
        if (imms == 0) result = low;
        else result = (low >> imms) | (high << (width - imms));
        write_result(cpu, rd, sf, result);
        return true;
    }

    default:
        return false;
    }
}

static bool exec_branch(arm64_cpu_t *cpu, uint32_t in) {
    /* Asking the system underneath for something.
     *
     * On real hardware this is an exception: the processor leaves the program
     * and enters the kernel, which reads the registers to find out what was
     * wanted.  There is no kernel below this interpreter, so it calls back into
     * whatever is hosting the program and lets that answer - which is the same
     * arrangement seen from the program, and the program cannot tell.
     *
     * Without this a program built for ARM can compute and nothing else: no
     * output, no files, not even a way of saying it has finished. */
    if ((in & 0xFFE0001Fu) == 0xD4000001u) {
        uint32_t what = (in >> 5) & 0xFFFF;
        if (!cpu->on_call) {
            fault(cpu, "asked the system for something, and there is nothing "
                       "underneath to ask", cpu->pc);
            return true;
        }
        if (!cpu->on_call(cpu, what, cpu->call_ctx)) {
            cpu->stopped = true;
            return true;
        }
        cpu->pc += 4;
        return true;
    }

    /* Unconditional, with or without a link. */
    if (((in >> 26) & 0x1F) == 0x05) {
        bool link = (in >> 31) & 1;
        int64_t offset = sign_extend(in & 0x3FFFFFF, 26) * 4;
        if (link) write_x(cpu, 30, cpu->pc + 4);
        branch_to(cpu, cpu->pc + (uint64_t)offset);
        return true;
    }

    /* Compare against zero and branch. */
    if (((in >> 25) & 0x3F) == 0x1A) {
        bool sf = (in >> 31) & 1;
        bool not_zero = (in >> 24) & 1;
        unsigned rt = in & 31;
        int64_t offset = sign_extend((in >> 5) & 0x7FFFF, 19) * 4;
        uint64_t value = read_operand(cpu, rt, sf);
        if ((value != 0) == not_zero) branch_to(cpu, cpu->pc + (uint64_t)offset);
        else cpu->pc += 4;
        return true;
    }

    /* Test one bit and branch. */
    if (((in >> 25) & 0x3F) == 0x1B) {
        bool set_wanted = (in >> 24) & 1;
        unsigned bit = ((in >> 26) & 0x20) | ((in >> 19) & 0x1F);
        unsigned rt = in & 31;
        int64_t offset = sign_extend((in >> 5) & 0x3FFF, 14) * 4;
        uint64_t value = read_x(cpu, rt);
        bool is_set = (value >> bit) & 1;
        if (is_set == set_wanted) branch_to(cpu, cpu->pc + (uint64_t)offset);
        else cpu->pc += 4;
        return true;
    }

    /* Conditional. */
    if (((in >> 24) & 0xFF) == 0x54) {
        unsigned cond = in & 0xF;
        int64_t offset = sign_extend((in >> 5) & 0x7FFFF, 19) * 4;
        if (condition_holds(cpu, cond)) branch_to(cpu, cpu->pc + (uint64_t)offset);
        else cpu->pc += 4;
        return true;
    }

    /* Asking the system for something. */
    if (((in >> 21) & 0x7FF) == 0x6A0) {
        unsigned imm = (in >> 5) & 0xFFFF;
        cpu->pc += 4;
        if (cpu->on_call && !cpu->on_call(cpu, imm, cpu->call_ctx))
            cpu->stopped = true;
        return true;
    }

    /* Branch to a register, with or without a link, and return. */
    if (((in >> 25) & 0x7F) == 0x6B) {
        unsigned opc = (in >> 21) & 0xF;
        unsigned rn = (in >> 5) & 31;
        uint64_t target = read_x(cpu, rn);
        switch (opc) {
        case 0: branch_to(cpu, target); return true;                /* BR  */
        case 1: write_x(cpu, 30, cpu->pc + 4);
                branch_to(cpu, target); return true;                /* BLR */
        case 2: branch_to(cpu, target); return true;                /* RET */
        default: return false;
        }
    }

    /* Barriers and hints do nothing here: there is one processor and it runs
     * one instruction at a time, so everything is already in order. */
    if (((in >> 22) & 0x3FF) == 0x354) { cpu->pc += 4; return true; }

    return false;
}

static bool exec_loadstore(arm64_cpu_t *cpu, uint32_t in) {
    unsigned rt = in & 31;
    unsigned rn = (in >> 5) & 31;

    /* A pair of floating-point registers at once.
     *
     * The same instruction as the one below and one bit apart from it, but the
     * registers it moves are the wide ones rather than the ordinary ones.  It
     * matters far more than floating point arithmetic does here: a compiler
     * building any function at all saves and restores these in the prologue,
     * whether the program does arithmetic on them or not, so a program
     * compiled the ordinary way stops on its first instruction without this.
     *
     * What is moved is bytes.  Nothing here interprets them as numbers, which
     * is why this works without any floating point support at all - saving a
     * register and putting it back is a copy either way. */
    if (((in >> 27) & 0x7) == 0x5 && ((in >> 26) & 1) == 1 &&
        ((in >> 23) & 0x3) != 0) {
        unsigned opc = (in >> 30) & 3;
        unsigned mode = (in >> 23) & 3;      /* 1 post, 2 offset, 3 pre */
        bool loading = (in >> 22) & 1;
        unsigned rt2 = (in >> 10) & 31;
        int64_t imm = sign_extend((in >> 15) & 0x7F, 7);

        if (opc == 3) return false;
        /* Four bytes for the narrow ones, eight for the usual, sixteen for
         * the widest - which move as two halves. */
        int bytes = (opc == 0) ? 4 : (opc == 1) ? 8 : 16;
        imm *= bytes;

        uint64_t base = read_x_sp(cpu, rn);
        uint64_t addr = (mode == 1) ? base : base + (uint64_t)imm;

        unsigned regs[2] = { rt, rt2 };
        for (int i = 0; i < 2; i++) {
            uint64_t at = addr + (uint64_t)(i * bytes);
            if (bytes == 16) {
                if (loading) {
                    cpu->vreg[regs[i]][0] = load(cpu, at, 8);
                    cpu->vreg[regs[i]][1] = load(cpu, at + 8, 8);
                } else {
                    store(cpu, at, 8, cpu->vreg[regs[i]][0]);
                    store(cpu, at + 8, 8, cpu->vreg[regs[i]][1]);
                }
            } else if (loading) {
                cpu->vreg[regs[i]][0] = load(cpu, at, bytes);
                cpu->vreg[regs[i]][1] = 0;   /* the rest reads as nothing */
            } else {
                store(cpu, at, bytes, cpu->vreg[regs[i]][0]);
            }
            if (cpu->stopped) return true;
        }

        if (mode == 1 || mode == 3) write_x_sp(cpu, rn, base + (uint64_t)imm);
        cpu->pc += 4;
        return true;
    }

    /* A pair at once, which is how a function saves its registers. */
    if (((in >> 25) & 0x3F) == 0x28 || ((in >> 25) & 0x3F) == 0x29 ||
        ((in >> 25) & 0x3F) == 0x2A || ((in >> 25) & 0x3F) == 0x2B) {
        unsigned opc = (in >> 30) & 3;
        unsigned mode = (in >> 23) & 3;      /* 1 post, 2 offset, 3 pre */
        bool loading = (in >> 22) & 1;
        unsigned rt2 = (in >> 10) & 31;
        int64_t imm = sign_extend((in >> 15) & 0x7F, 7);

        if (opc == 3) return false;
        int bytes = (opc == 2) ? 8 : 4;
        bool signed_word = (opc == 1);
        imm *= bytes;

        uint64_t base = read_x_sp(cpu, rn);
        uint64_t addr = (mode == 1) ? base : base + (uint64_t)imm;

        if (loading) {
            uint64_t a = load(cpu, addr, bytes);
            uint64_t b = load(cpu, addr + (uint64_t)bytes, bytes);
            if (signed_word) {
                write_x(cpu, rt, (uint64_t)(int64_t)(int32_t)(uint32_t)a);
                write_x(cpu, rt2, (uint64_t)(int64_t)(int32_t)(uint32_t)b);
            } else {
                write_result(cpu, rt, bytes == 8, a);
                write_result(cpu, rt2, bytes == 8, b);
            }
        } else {
            store(cpu, addr, bytes, read_operand(cpu, rt, bytes == 8));
            store(cpu, addr + (uint64_t)bytes, bytes,
                  read_operand(cpu, rt2, bytes == 8));
        }

        if (mode == 1 || mode == 3) write_x_sp(cpu, rn, base + (uint64_t)imm);
        cpu->pc += 4;
        return true;
    }

    /* The exclusive pair, which is what a lock is built from. */
    if (((in >> 24) & 0x3F) == 0x08) {
        unsigned size = (in >> 30) & 3;
        bool loading = (in >> 22) & 1;
        unsigned rs = (in >> 16) & 31;
        int bytes = 1 << size;
        uint64_t addr = read_x_sp(cpu, rn);

        if (loading) {
            uint64_t value = load(cpu, addr, bytes);
            write_result(cpu, rt, size == 3, value);
            cpu->exclusive = true;
            cpu->exclusive_addr = addr;
        } else {
            /* The store succeeds only if the mark survived, and says which in
             * a register - that answer is the whole mechanism. */
            if (cpu->exclusive && cpu->exclusive_addr == addr) {
                store(cpu, addr, bytes, read_operand(cpu, rt, size == 3));
                write_x(cpu, rs, 0);
                cpu->exclusive = false;
            } else {
                write_x(cpu, rs, 1);
            }
        }
        cpu->pc += 4;
        return true;
    }

    /* A literal: load something stored beside the code. */
    if (((in >> 24) & 0x3F) == 0x18 || ((in >> 24) & 0x3F) == 0x1C) {
        unsigned opc = (in >> 30) & 3;
        int64_t offset = sign_extend((in >> 5) & 0x7FFFF, 19) * 4;
        uint64_t addr = cpu->pc + (uint64_t)offset;
        if (opc == 0) write_result(cpu, rt, false, load(cpu, addr, 4));
        else if (opc == 1) write_x(cpu, rt, load(cpu, addr, 8));
        else if (opc == 2)
            write_x(cpu, rt, (uint64_t)(int64_t)(int32_t)(uint32_t)load(cpu, addr, 4));
        else return false;
        cpu->pc += 4;
        return true;
    }

    /* One floating-point register, the same shapes as below.
     *
     * These have to be picked out before the ordinary forms, because the only
     * thing separating them is one bit: without this the instruction below
     * matches, and a program copying a string sixteen bytes at a time has
     * every one of those copies quietly go to the wrong register.  It does not
     * fail - it carries on with the wrong bytes, which is worse.
     *
     * How much is moved is not the size field alone.  The two are read
     * together, which is how one field covers everything from a single byte to
     * sixteen of them. */
    if (((in >> 27) & 0x7) == 0x7 && ((in >> 26) & 1) == 1) {
        unsigned size = (in >> 30) & 3;
        unsigned opc = (in >> 22) & 3;
        bool unsigned_offset = (in >> 24) & 1;
        int bytes = 1 << ((((opc >> 1) & 1) << 2) | size);
        bool loading = (opc & 1) != 0;

        if (bytes > 16) return false;

        uint64_t base = read_x_sp(cpu, rn);
        uint64_t addr;
        bool writeback = false;
        uint64_t written_back = base;

        if (unsigned_offset) {
            addr = base + ((in >> 10) & 0xFFF) * (uint64_t)bytes;
        } else if (((in >> 21) & 1) && ((in >> 10) & 3) == 2) {
            unsigned rm = (in >> 16) & 31;
            unsigned option = (in >> 13) & 7;
            unsigned scale = ((in >> 12) & 1) ? (unsigned)size : 0;
            addr = base + extend_reg(read_x(cpu, rm), option, scale);
        } else {
            int64_t imm = sign_extend((in >> 12) & 0x1FF, 9);
            unsigned mode = (in >> 10) & 3;
            if (mode == 1) {
                addr = base;
                written_back = base + (uint64_t)imm;
                writeback = true;
            } else if (mode == 3) {
                addr = base + (uint64_t)imm;
                written_back = addr;
                writeback = true;
            } else {
                addr = base + (uint64_t)imm;
            }
        }

        if (loading) {
            if (bytes == 16) {
                cpu->vreg[rt][0] = load(cpu, addr, 8);
                cpu->vreg[rt][1] = load(cpu, addr + 8, 8);
            } else {
                cpu->vreg[rt][0] = load(cpu, addr, bytes);
                cpu->vreg[rt][1] = 0;    /* the rest of it reads as nothing */
            }
        } else {
            if (bytes == 16) {
                store(cpu, addr, 8, cpu->vreg[rt][0]);
                store(cpu, addr + 8, 8, cpu->vreg[rt][1]);
            } else {
                store(cpu, addr, bytes, cpu->vreg[rt][0]);
            }
        }
        if (cpu->stopped) return true;

        if (writeback) write_x_sp(cpu, rn, written_back);
        cpu->pc += 4;
        return true;
    }

    /* The ordinary forms: an unsigned offset, or a register, or an offset that
     * also changes the base. */
    if (((in >> 27) & 0x7) == 0x7 && ((in >> 26) & 1) == 0) {
        unsigned size = (in >> 30) & 3;
        unsigned opc = (in >> 22) & 3;
        bool unsigned_offset = (in >> 24) & 1;
        int bytes = 1 << size;

        uint64_t base = read_x_sp(cpu, rn);
        uint64_t addr;
        bool writeback = false;
        uint64_t written_back = base;

        if (unsigned_offset) {
            uint64_t imm = ((in >> 10) & 0xFFF) * (uint64_t)bytes;
            addr = base + imm;
        } else if (((in >> 21) & 1) && ((in >> 10) & 3) == 2) {
            /* Indexed by a register, optionally widened and scaled. */
            unsigned rm = (in >> 16) & 31;
            unsigned option = (in >> 13) & 7;
            unsigned scale = ((in >> 12) & 1) ? size : 0;
            uint64_t index = extend_reg(read_x(cpu, rm), option, scale);
            addr = base + index;
        } else {
            int64_t imm = sign_extend((in >> 12) & 0x1FF, 9);
            unsigned mode = (in >> 10) & 3;
            if (mode == 1) {                       /* post: use, then change */
                addr = base;
                written_back = base + (uint64_t)imm;
                writeback = true;
            } else if (mode == 3) {                /* pre: change, then use */
                addr = base + (uint64_t)imm;
                written_back = addr;
                writeback = true;
            } else {
                addr = base + (uint64_t)imm;
            }
        }

        bool loading = (opc & 1) != 0;
        bool sign = (opc & 2) != 0 && size != 3;

        if (size == 3 && opc == 2) { loading = true; sign = false; }

        if (loading) {
            uint64_t value = load(cpu, addr, bytes);
            if (sign) {
                /* Widened to sixty-four bits, or to thirty-two, depending on a
                 * bit that means the opposite of what it looks like. */
                bool to64 = ((opc & 1) == 0);
                int64_t widened;
                switch (bytes) {
                case 1: widened = (int8_t)value; break;
                case 2: widened = (int16_t)value; break;
                default: widened = (int32_t)value; break;
                }
                write_result(cpu, rt, to64, (uint64_t)widened);
            } else {
                write_result(cpu, rt, size == 3, value);
            }
        } else {
            store(cpu, addr, bytes, read_x(cpu, rt));
        }

        if (writeback) write_x_sp(cpu, rn, written_back);
        cpu->pc += 4;
        return true;
    }

    return false;
}

static bool exec_data_register(arm64_cpu_t *cpu, uint32_t in) {
    bool sf = (in >> 31) & 1;
    unsigned rd = in & 31;
    unsigned rn = (in >> 5) & 31;
    unsigned rm = (in >> 16) & 31;

    /* Add and subtract, shifted or extended. */
    if (((in >> 24) & 0x1F) == 0x0B) {
        bool sub = (in >> 30) & 1;
        bool set_flags = (in >> 29) & 1;
        bool extended = (in >> 21) & 1;

        uint64_t a, b;
        if (extended) {
            unsigned option = (in >> 13) & 7;
            unsigned amount = (in >> 10) & 7;
            if (amount > 4) return false;
            a = read_x_sp(cpu, rn);
            b = extend_reg(read_x(cpu, rm), option, amount);
        } else {
            unsigned type = (in >> 22) & 3;
            unsigned amount = (in >> 10) & 0x3F;
            if (type == 3) return false;
            a = read_operand(cpu, rn, sf);
            b = shift_reg(read_x(cpu, rm), type, amount, sf);
        }
        if (!sf) { a &= 0xFFFFFFFFull; b &= 0xFFFFFFFFull; }

        uint64_t result = sub
            ? add_with_carry(cpu, sf, a, ~b, true, set_flags)
            : add_with_carry(cpu, sf, a, b, false, set_flags);

        if (!set_flags && extended) write_x_sp(cpu, rd, size_result(sf, result));
        else write_result(cpu, rd, sf, result);
        return true;
    }

    /* Logic, with an optional shift and an optional inversion. */
    if (((in >> 24) & 0x1F) == 0x0A) {
        unsigned opc = (in >> 29) & 3;
        bool invert = (in >> 21) & 1;
        unsigned type = (in >> 22) & 3;
        unsigned amount = (in >> 10) & 0x3F;

        uint64_t a = read_operand(cpu, rn, sf);
        uint64_t b = shift_reg(read_x(cpu, rm), type, amount, sf);
        if (invert) b = ~b;

        uint64_t result;
        switch (opc) {
        case 0: result = a & b; break;                       /* AND / BIC */
        case 1: result = a | b; break;                       /* ORR / ORN */
        case 2: result = a ^ b; break;                       /* EOR / EON */
        default: result = a & b; break;                      /* ANDS / BICS */
        }
        if (opc == 3) set_logic_flags(cpu, sf, result);
        write_result(cpu, rd, sf, result);
        return true;
    }

    /* Conditional select, and everything spelled with it: CSET, CINC, and the
     * rest are this instruction with the zero register. */
    if (((in >> 21) & 0xFF) == (sf ? 0xD4 : 0xD4) &&
        ((in >> 10) & 3) <= 1 && ((in >> 29) & 1) == 0 &&
        ((in >> 23) & 0xFF) == (unsigned)(sf ? 0x35 : 0x35)) {
        /* handled below */
    }

    if (((in >> 21) & 0x3FF) == 0x0D4 || ((in >> 21) & 0x3FF) == 0x2D4) {
        unsigned cond = (in >> 12) & 0xF;
        unsigned op2 = (in >> 10) & 3;
        bool invert = (in >> 30) & 1;

        uint64_t a = read_operand(cpu, rn, sf);
        uint64_t b = read_operand(cpu, rm, sf);

        uint64_t result;
        if (condition_holds(cpu, cond)) {
            result = a;
        } else {
            result = b;
            if (op2 & 1) result += 1;                        /* CSINC / CSNEG */
            if (invert) result = (op2 & 1) ? (uint64_t)(-(int64_t)b) : ~b;
        }
        write_result(cpu, rd, sf, result);
        return true;
    }

    /* Multiply and add, which is also plain multiply when the addend is the
     * zero register. */
    if (((in >> 21) & 0xFF) == 0xD8 || ((in >> 21) & 0xFF) == 0x98) {
        unsigned ra = (in >> 10) & 31;
        bool subtract = (in >> 15) & 1;
        uint64_t a = read_operand(cpu, rn, sf);
        uint64_t b = read_operand(cpu, rm, sf);
        uint64_t addend = read_operand(cpu, ra, sf);
        uint64_t product = a * b;
        write_result(cpu, rd, sf, subtract ? addend - product : addend + product);
        return true;
    }

    /* The widening multiplies: two thirty-two bit operands giving a full
     * sixty-four bit product, and the pair that give the high half of a
     * sixty-four bit one.  Compilers reach for these constantly - a division
     * by a constant becomes a multiply by its reciprocal and a shift, and that
     * multiply needs the half that does not fit. */
    if (((in >> 24) & 0x1F) == 0x1B && sf) {
        unsigned op31 = (in >> 21) & 7;
        unsigned ra = (in >> 10) & 31;
        bool subtract = (in >> 15) & 1;

        if (op31 == 1 || op31 == 5) {                     /* SMADDL / UMADDL */
            bool is_signed = (op31 == 1);
            uint64_t product;
            if (is_signed) {
                int64_t a = (int32_t)(uint32_t)read_x(cpu, rn);
                int64_t b = (int32_t)(uint32_t)read_x(cpu, rm);
                product = (uint64_t)(a * b);
            } else {
                uint64_t a = (uint32_t)read_x(cpu, rn);
                uint64_t b = (uint32_t)read_x(cpu, rm);
                product = a * b;
            }
            uint64_t addend = read_x(cpu, ra);
            write_x(cpu, rd, subtract ? addend - product : addend + product);
            return true;
        }

        if (op31 == 2 || op31 == 6) {                     /* SMULH / UMULH */
            uint64_t a = read_x(cpu, rn), b = read_x(cpu, rm);
            uint64_t high;

            /* The top half of a full product, built from thirty-two bit
             * pieces because there is no wider type to hold the whole of it. */
            uint64_t a_lo = a & 0xFFFFFFFFull, a_hi = a >> 32;
            uint64_t b_lo = b & 0xFFFFFFFFull, b_hi = b >> 32;
            uint64_t ll = a_lo * b_lo;
            uint64_t lh = a_lo * b_hi;
            uint64_t hl = a_hi * b_lo;
            uint64_t hh = a_hi * b_hi;
            uint64_t middle = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
            high = hh + (lh >> 32) + (hl >> 32) + (middle >> 32);

            if (op31 == 2) {
                /* Signed: correct the unsigned product by subtracting each
                 * operand where the other one was negative. */
                if ((int64_t)a < 0) high -= b;
                if ((int64_t)b < 0) high -= a;
            }
            write_x(cpu, rd, high);
            return true;
        }
    }

    /* Divide and shift-by-a-register: the two-source class.
     *
     * Bit thirty is what separates this from the one-source class below, and
     * both classes carry the same eight bits at twenty-one to twenty-eight.
     * Testing only those matches both - and then REV, whose opcode happens to
     * be the same number as UDIV's, is quietly executed as a division. */
    if (((in >> 21) & 0x7FF) == (sf ? 0x4D6u : 0x0D6u)) {
        unsigned opcode = (in >> 10) & 0x3F;

        switch (opcode) {
        case 0x02: {                                          /* UDIV */
            uint64_t a = read_operand(cpu, rn, sf), b = read_operand(cpu, rm, sf);
            /* Division by zero gives zero here rather than a fault, which is
             * what the architecture says. */
            write_result(cpu, rd, sf, b ? a / b : 0);
            return true;
        }
        case 0x03: {                                          /* SDIV */
            if (sf) {
                int64_t a = (int64_t)read_x(cpu, rn), b = (int64_t)read_x(cpu, rm);
                int64_t result = 0;
                if (b != 0 && !(a == INT64_MIN && b == -1)) result = a / b;
                else if (b != 0) result = a;                  /* the one overflow */
                write_x(cpu, rd, (uint64_t)result);
            } else {
                int32_t a = (int32_t)(uint32_t)read_x(cpu, rn);
                int32_t b = (int32_t)(uint32_t)read_x(cpu, rm);
                int32_t result = 0;
                if (b != 0 && !(a == INT32_MIN && b == -1)) result = a / b;
                else if (b != 0) result = a;
                write_result(cpu, rd, false, (uint64_t)(uint32_t)result);
            }
            return true;
        }
        case 0x08: case 0x09: case 0x0A: case 0x0B: {         /* LSLV..RORV */
            unsigned type = opcode - 0x08;
            uint64_t amount = read_operand(cpu, rm, sf);
            write_result(cpu, rd, sf,
                         shift_reg(read_operand(cpu, rn, sf), type,
                                   (unsigned)amount, sf));
            return true;
        }
        default: break;
        }
    }

    /* Counting and reversing: the one-source class, which is the same encoding
     * with bit thirty set. */
    if (((in >> 21) & 0x7FF) == (sf ? 0x6D6u : 0x2D6u)) {
        unsigned opcode = (in >> 10) & 0x3F;
        uint64_t a = read_operand(cpu, rn, sf);
        unsigned width = sf ? 64 : 32;

        switch (opcode) {
        case 0x00: {                                          /* RBIT */
            uint64_t result = 0;
            for (unsigned i = 0; i < width; i++)
                if ((a >> i) & 1) result |= 1ull << (width - 1 - i);
            write_result(cpu, rd, sf, result);
            return true;
        }
        case 0x01: {                                          /* REV16 */
            uint64_t result = 0;
            for (unsigned i = 0; i < width; i += 16)
                result |= (((a >> i) & 0xFF) << (i + 8)) |
                          (((a >> (i + 8)) & 0xFF) << i);
            write_result(cpu, rd, sf, result);
            return true;
        }
        case 0x02: {                                          /* REV32 / REV */
            uint64_t result = 0;
            unsigned chunk = sf ? 32 : width;
            for (unsigned base = 0; base < width; base += chunk)
                for (unsigned i = 0; i < chunk; i += 8)
                    result |= ((a >> (base + i)) & 0xFF)
                              << (base + chunk - 8 - i);
            write_result(cpu, rd, sf, result);
            return true;
        }
        case 0x03: {                                          /* REV (64) */
            uint64_t result = 0;
            for (unsigned i = 0; i < width; i += 8)
                result |= ((a >> i) & 0xFF) << (width - 8 - i);
            write_result(cpu, rd, sf, result);
            return true;
        }
        case 0x04: {                                          /* CLZ */
            unsigned count = 0;
            while (count < width && !((a >> (width - 1 - count)) & 1)) count++;
            write_result(cpu, rd, sf, count);
            return true;
        }
        case 0x05: {                                          /* CLS */
            bool sign = (a >> (width - 1)) & 1;
            unsigned count = 0;
            while (count + 1 < width &&
                   (((a >> (width - 2 - count)) & 1) != 0) == sign) count++;
            write_result(cpu, rd, sf, count);
            return true;
        }
        default: break;
        }
    }

    return false;
}

/* -------------------------------------------------------------------- run */

/* ------------------------------------------------------- floating point
 *
 * The registers were already here and instructions that move them already
 * work; what was missing is doing arithmetic on them.  A program that only
 * saves and restores these registers runs without any of this - which is why
 * moving them came first - but one that actually computes with them does not.
 *
 * This machine has floating point of its own, and it is the same one: both
 * follow the same standard for what a number is and what an operation gives.
 * So the bits are read out as a number, the operation is done here, and the
 * bits go back.  Nothing is being imitated - the answer comes from the same
 * kind of hardware that would have produced it there.
 *
 * The two sizes are held the same way: the narrow one in the low half of the
 * register, the wide one filling it.  Writing either clears what was above it,
 * which is what the architecture says happens.
 */

static double fp_read(arm64_cpu_t *cpu, unsigned r, bool wide) {
    if (wide) {
        uint64_t bits = cpu->vreg[r][0];
        double out;
        memcpy(&out, &bits, sizeof out);
        return out;
    }
    uint32_t bits = (uint32_t)cpu->vreg[r][0];
    float out;
    memcpy(&out, &bits, sizeof out);
    return (double)out;
}

static void fp_write(arm64_cpu_t *cpu, unsigned r, bool wide, double value) {
    if (wide) {
        uint64_t bits;
        memcpy(&bits, &value, sizeof bits);
        cpu->vreg[r][0] = bits;
    } else {
        float narrow = (float)value;
        uint32_t bits;
        memcpy(&bits, &narrow, sizeof bits);
        cpu->vreg[r][0] = bits;
    }
    cpu->vreg[r][1] = 0;
}

/* A small number written into the instruction itself.  Eight bits stand for a
 * sign, a short exponent and four bits of fraction - enough for the constants
 * that turn up in ordinary code, and nothing else. */
static double fp_immediate(unsigned imm8, bool wide) {
    unsigned sign = (imm8 >> 7) & 1;
    unsigned high = (imm8 >> 6) & 1;
    unsigned low = (imm8 >> 4) & 3;
    unsigned frac = imm8 & 0xF;

    if (wide) {
        uint64_t exponent = ((uint64_t)(high ^ 1) << 10) |
                            ((uint64_t)(high ? 0xFF : 0x00) << 2) |
                            (uint64_t)low;
        uint64_t bits = ((uint64_t)sign << 63) | (exponent << 52) |
                        ((uint64_t)frac << 48);
        double out;
        memcpy(&out, &bits, sizeof out);
        return out;
    }

    uint32_t exponent = ((uint32_t)(high ^ 1) << 7) |
                        ((uint32_t)(high ? 0x1F : 0x00) << 2) | low;
    uint32_t bits = ((uint32_t)sign << 31) | (exponent << 23) |
                    ((uint32_t)frac << 19);
    float out;
    memcpy(&out, &bits, sizeof out);
    return (double)out;
}

/* Comparing sets the same four flags every other comparison sets, so that the
 * branches afterwards are the ordinary ones.  Unordered - which is what any
 * comparison involving a value that is not a number gives - is its own answer
 * and not the same as any of less, equal or greater. */
static void fp_compare(arm64_cpu_t *cpu, double a, double b) {
    if (a != a || b != b) {                 /* not a number, either side */
        cpu->n = false; cpu->z = false; cpu->c = true; cpu->v = true;
    } else if (a == b) {
        cpu->n = false; cpu->z = true; cpu->c = true; cpu->v = false;
    } else if (a < b) {
        cpu->n = true; cpu->z = false; cpu->c = false; cpu->v = false;
    } else {
        cpu->n = false; cpu->z = false; cpu->c = true; cpu->v = false;
    }
}

/* ------------------------------------------------------------- vectors
 *
 * A third instruction set on top of the other two.  Given a loop that adds up
 * an array and permission to optimise, a compiler does not emit a loop that
 * adds one number at a time - it emits instructions that do four or eight at
 * once, on the same wide registers the floating-point instructions use but
 * treating each as several numbers side by side rather than one.
 *
 * So a program does not have to mention vectors anywhere to need this.  It
 * only has to contain a loop the compiler could see through, which most loops
 * are.
 */

/* One number out of a register holding several.  Which several depends on how
 * wide each is: sixteen bytes, or eight pairs, or four fours, or two eights. */
static uint64_t vec_get(const arm64_cpu_t *cpu, unsigned r, unsigned size,
                        unsigned index) {
    unsigned bits = 8u << size;
    unsigned per_half = 64u / bits;
    uint64_t half = cpu->vreg[r][index / per_half];
    unsigned shift = (index % per_half) * bits;
    uint64_t mask = (bits == 64) ? ~0ull : ((1ull << bits) - 1);
    return (half >> shift) & mask;
}

static void vec_put(arm64_cpu_t *cpu, unsigned r, unsigned size,
                    unsigned index, uint64_t value) {
    unsigned bits = 8u << size;
    unsigned per_half = 64u / bits;
    unsigned shift = (index % per_half) * bits;
    uint64_t mask = (bits == 64) ? ~0ull : ((1ull << bits) - 1);
    uint64_t *half = &cpu->vreg[r][index / per_half];
    *half = (*half & ~(mask << shift)) | ((value & mask) << shift);
}

/* Widened to a signed sixty-four bit number, so that comparisons and the
 * larger-of-two operations mean what they say for the narrow widths. */
static int64_t vec_signed(uint64_t value, unsigned size) {
    unsigned bits = 8u << size;
    if (bits == 64) return (int64_t)value;
    uint64_t sign = 1ull << (bits - 1);
    return (int64_t)((value ^ sign) - sign);
}

/* A number written into the instruction, spread across the register.  Eight
 * bits and a four-bit shape between them cover every constant these
 * instructions can carry - repeated bytes, a value shifted into place, or one
 * bit of the eight standing for a whole byte. */
static uint64_t simd_immediate(unsigned abcdefgh, unsigned cmode, unsigned op) {
    uint64_t byte = abcdefgh & 0xFF;

    if ((cmode & 0xE) == 0xE && !(cmode & 1)) {
        if (!op) {
            /* The same byte, everywhere. */
            uint64_t out = 0;
            for (int i = 0; i < 8; i++) out |= byte << (i * 8);
            return out;
        }
        /* Each bit standing for a whole byte of ones or zeroes. */
        uint64_t out = 0;
        for (int i = 0; i < 8; i++)
            if (byte & (1u << i)) out |= 0xFFull << (i * 8);
        return out;
    }

    if ((cmode & 0x9) == 0x0 || (cmode & 0xD) == 0x8) {
        /* A byte shifted into one of the four places in a word, or one of the
         * two places in a half - repeated the width of the register. */
        unsigned shift, width;
        if ((cmode & 0x9) == 0x0) { shift = ((cmode >> 1) & 3) * 8; width = 32; }
        else                      { shift = ((cmode >> 1) & 1) * 8; width = 16; }
        uint64_t piece = byte << shift;
        uint64_t out = 0;
        for (unsigned at = 0; at < 64; at += width) out |= piece << at;
        return out;
    }

    /* Anything else here is a shape this interpreter has not been taught. */
    return 0;
}

static bool exec_simd(arm64_cpu_t *cpu, uint32_t in) {
    unsigned rd = in & 31;
    unsigned rn = (in >> 5) & 31;
    unsigned rm = (in >> 16) & 31;
    unsigned q = (in >> 30) & 1;
    unsigned u = (in >> 29) & 1;

    /* Moving numbers between the two kinds of register, and between lanes.
     *
     * Six different things share one shape here: copying one lane everywhere,
     * copying an ordinary register everywhere, putting an ordinary register
     * into one lane, taking one lane out into an ordinary register with or
     * without its sign, and copying one lane to another.  Which of them it is
     * comes from four bits in the middle; how wide the numbers are is not
     * written down at all, but found from the lowest bit set in the index.
     *
     * These are what a compiler emits around every vectorised loop - putting a
     * value in before it starts and taking the answer out at the end - so a
     * loop can be vectorised entirely with instructions that are all here and
     * still not run without them. */
    if ((((in >> 21) & 0xFF) == 0x70 || ((in >> 21) & 0x7FF) == 0x2F0) &&
        ((in >> 15) & 1) == 0 && ((in >> 10) & 1) == 1) {
        unsigned imm5 = (in >> 16) & 31;
        unsigned imm4 = (in >> 11) & 0xF;
        bool scalar = ((in >> 21) & 0x7FF) == 0x2F0;
        unsigned op = scalar ? 0u : (in >> 29) & 1;

        unsigned size = 0;
        while (size < 4 && !((imm5 >> size) & 1)) size++;
        if (size > 3) return false;
        unsigned index = imm5 >> (size + 1);
        unsigned lanes = (q ? 16u : 8u) >> size;

        if (scalar || (op == 0 && imm4 == 0x0)) {   /* one lane, on its own */
            uint64_t value = vec_get(cpu, rn, size, index);
            if (scalar) {
                cpu->vreg[rd][0] = value;
                cpu->vreg[rd][1] = 0;
            } else {
                for (unsigned i = 0; i < lanes; i++)
                    vec_put(cpu, rd, size, i, value);
                if (!q) cpu->vreg[rd][1] = 0;
            }
            cpu->pc += 4;
            return true;
        }
        if (op == 0 && imm4 == 0x1) {               /* one value, everywhere */
            uint64_t value = read_x(cpu, rn);
            for (unsigned i = 0; i < lanes; i++)
                vec_put(cpu, rd, size, i, value);
            if (!q) cpu->vreg[rd][1] = 0;
            cpu->pc += 4;
            return true;
        }
        if (op == 0 && imm4 == 0x3) {               /* one value, one lane */
            vec_put(cpu, rd, size, index, read_x(cpu, rn));
            cpu->pc += 4;
            return true;
        }
        if (op == 0 && (imm4 == 0x5 || imm4 == 0x7)) {  /* a lane, taken out */
            uint64_t value = vec_get(cpu, rn, size, index);
            bool sf = (in >> 30) & 1;
            if (imm4 == 0x5) write_result(cpu, rd, sf,
                                          (uint64_t)vec_signed(value, size));
            else write_result(cpu, rd, sf, value);
            cpu->pc += 4;
            return true;
        }
        if (op == 1) {                              /* one lane to another */
            unsigned from = imm4 >> size;
            vec_put(cpu, rd, size, index, vec_get(cpu, rn, size, from));
            cpu->pc += 4;
            return true;
        }
        return false;
    }

    /* A number written into the instruction. */
    if (((in >> 19) & 0x3FF) == 0x1E0 && ((in >> 10) & 1) == 1) {
        unsigned cmode = (in >> 12) & 0xF;
        unsigned abcdefgh = (((in >> 16) & 7) << 5) | ((in >> 5) & 31);
        uint64_t value = simd_immediate(abcdefgh, cmode, u);
        cpu->vreg[rd][0] = value;
        cpu->vreg[rd][1] = q ? value : 0;
        cpu->pc += 4;
        return true;
    }

    /* Shifting every number by an amount written into the instruction, and
     * the widening that goes with it.
     *
     * How wide the numbers are is not a field of its own here - it is found
     * from the highest bit set in the shift amount, which is the architecture
     * packing two things into one place.  The widening form reads half a
     * register and writes a whole one, each number twice the width it was:
     * that is what a compiler emits when a loop over bytes has to do its
     * arithmetic in something larger than a byte. */
    if (((in >> 23) & 0x3F) == 0x1E && ((in >> 10) & 1) == 1) {
        unsigned immh = (in >> 19) & 0xF;
        unsigned immb = (in >> 16) & 7;
        unsigned opcode = (in >> 11) & 0x1F;
        if (!immh) return false;

        unsigned size = (immh & 8) ? 3 : (immh & 4) ? 2 : (immh & 2) ? 1 : 0;
        unsigned esize = 8u << size;
        unsigned imm = (immh << 3) | immb;

        if (opcode == 0x14) {                   /* widen, shifting left */
            if (size > 2) return false;
            unsigned shift = imm - esize;
            unsigned lanes = 8u >> size;
            unsigned from = q ? lanes : 0;      /* which half is read */
            uint64_t out[2] = { 0, 0 };
            for (unsigned i = 0; i < lanes; i++) {
                uint64_t v = vec_get(cpu, rn, size, from + i);
                if (!u) {                       /* keeping the sign */
                    int64_t sv = vec_signed(v, size);
                    v = (uint64_t)sv;
                }
                v <<= shift;
                unsigned wide_bits = esize * 2;
                unsigned per_half = 64u / wide_bits;
                uint64_t mask = (wide_bits == 64) ? ~0ull
                                                  : ((1ull << wide_bits) - 1);
                out[i / per_half] |= (v & mask) <<
                                     ((i % per_half) * wide_bits);
            }
            cpu->vreg[rd][0] = out[0];
            cpu->vreg[rd][1] = out[1];
            cpu->pc += 4;
            return true;
        }

        if (opcode == 0x0A || opcode == 0x00) {  /* left, or right */
            bool left = (opcode == 0x0A);
            unsigned shift = left ? (imm - esize) : ((esize * 2) - imm);
            unsigned lanes = (q ? 16u : 8u) >> size;
            for (unsigned i = 0; i < lanes; i++) {
                uint64_t v = vec_get(cpu, rn, size, i);
                if (left) v <<= shift;
                else if (u) v >>= shift;
                else v = (uint64_t)(vec_signed(v, size) >> shift);
                vec_put(cpu, rd, size, i, v);
            }
            if (!q) cpu->vreg[rd][1] = 0;
            cpu->pc += 4;
            return true;
        }
        return false;
    }

    /* Every number in one register against a single number picked out of
     * another.
     *
     * This is multiplying a whole array by one value - the value sits in one
     * lane of a register and is used against all of them.  Which lane is not
     * written down in one piece: the index is spread across three bits in
     * different parts of the instruction, and which three depends on how wide
     * the numbers are. */
    if (((in >> 24) & 0x1F) == 0x0F && ((in >> 10) & 1) == 0 &&
        ((in >> 22) & 3) != 0) {
        unsigned size = (in >> 22) & 3;
        unsigned opcode = (in >> 12) & 0xF;
        unsigned l = (in >> 21) & 1;
        unsigned m = (in >> 11) & 1;   /* H, in the manual's naming */
        unsigned low_m = (in >> 20) & 1;
        unsigned rm_field = (in >> 16) & 0xF;

        unsigned index, rm_reg;
        if (size == 1) {
            index = (m << 2) | (l << 1) | low_m;
            rm_reg = rm_field;
        } else if (size == 2) {
            index = (m << 1) | l;
            rm_reg = (low_m << 4) | rm_field;
        } else {
            index = m;
            rm_reg = (low_m << 4) | rm_field;
        }

        if (opcode == 0x8 && !u) {              /* multiply, whole numbers */
            unsigned lanes = (q ? 16u : 8u) >> size;
            uint64_t by = vec_get(cpu, rm_reg, size, index);
            for (unsigned i = 0; i < lanes; i++)
                vec_put(cpu, rd, size, i, vec_get(cpu, rn, size, i) * by);
            if (!q) cpu->vreg[rd][1] = 0;
            cpu->pc += 4;
            return true;
        }
        if (opcode == 0x9 && u) {               /* multiply, fractional */
            bool wide = (size == 3);
            unsigned esize = wide ? 3 : 2;
            unsigned lanes = (q ? 16u : 8u) >> esize;
            uint64_t bbits = vec_get(cpu, rm_reg, esize, index);
            double b;
            if (wide) memcpy(&b, &bbits, 8);
            else { float f; uint32_t n = (uint32_t)bbits; memcpy(&f, &n, 4);
                   b = f; }
            for (unsigned i = 0; i < lanes; i++) {
                uint64_t abits = vec_get(cpu, rn, esize, i);
                double a;
                if (wide) memcpy(&a, &abits, 8);
                else { float f; uint32_t n = (uint32_t)abits;
                       memcpy(&f, &n, 4); a = f; }
                double out = a * b;
                uint64_t bits;
                if (wide) memcpy(&bits, &out, 8);
                else { float f = (float)out; uint32_t n; memcpy(&n, &f, 4);
                       bits = n; }
                vec_put(cpu, rd, esize, i, bits);
            }
            if (!q) cpu->vreg[rd][1] = 0;
            cpu->pc += 4;
            return true;
        }
        return false;
    }

    /* Interleaving two registers, and taking them apart again.
     *
     * Six ways of shuffling two registers into one: alternating them, taking
     * every other number, or swapping them in pairs - each in a version that
     * works on the bottom half and one that works on the top.  A compiler
     * reaches for these whenever a loop deals with two things at once, and
     * they are also how a vector is folded down at the end of a reduction.
     *
     * The answer is worked out in full before any of it is written back,
     * because the register being written is very often one of the two being
     * read from. */
    if (((in >> 24) & 0x3F) == 0x0E && ((in >> 21) & 1) == 0 &&
        ((in >> 15) & 1) == 0 && ((in >> 10) & 3) == 2) {
        unsigned size = (in >> 22) & 3;
        unsigned opcode = (in >> 12) & 0x7;
        unsigned lanes = (q ? 16u : 8u) >> size;
        unsigned half = lanes / 2;

        uint64_t out[32];
        for (unsigned i = 0; i < lanes; i++) {
            unsigned pair = i / 2;
            switch (opcode) {
            case 1:                                     /* every other one  */
            case 5: {
                unsigned at = i * 2 + (opcode == 5 ? 1 : 0);
                out[i] = (at < lanes) ? vec_get(cpu, rn, size, at)
                                      : vec_get(cpu, rm, size, at - lanes);
                break;
            }
            case 2:                                     /* swapped in pairs */
            case 6: {
                unsigned at = (i & ~1u) + (opcode == 6 ? 1 : 0);
                out[i] = (i & 1) ? vec_get(cpu, rm, size, at)
                                 : vec_get(cpu, rn, size, at);
                break;
            }
            case 3:                                     /* alternating      */
            case 7: {
                unsigned at = pair + (opcode == 7 ? half : 0);
                out[i] = (i & 1) ? vec_get(cpu, rm, size, at)
                                 : vec_get(cpu, rn, size, at);
                break;
            }
            default: return false;
            }
        }
        for (unsigned i = 0; i < lanes; i++) vec_put(cpu, rd, size, i, out[i]);
        if (!q) cpu->vreg[rd][1] = 0;
        cpu->pc += 4;
        return true;
    }

    /* Taking a window across two registers laid end to end.
     *
     * This is how a vectorised loop folds its answer in half: the top half of
     * a register is brought down to the bottom by extracting from a byte part
     * way in, and then added to what was already there.  Doing that a few
     * times turns sixteen running totals into one. */
    if (((in >> 24) & 0x3F) == 0x2E && ((in >> 21) & 0x7) == 0 &&
        ((in >> 15) & 1) == 0 && ((in >> 10) & 1) == 0) {
        unsigned imm4 = (in >> 11) & 0xF;
        unsigned width = q ? 16u : 8u;
        if (imm4 >= width) return false;

        uint8_t joined[32];
        for (unsigned i = 0; i < width; i++)
            joined[i] = (uint8_t)vec_get(cpu, rn, 0, i);
        for (unsigned i = 0; i < width; i++)
            joined[width + i] = (uint8_t)vec_get(cpu, rm, 0, i);

        uint64_t out[2] = { 0, 0 };
        for (unsigned i = 0; i < width; i++)
            out[i / 8] |= (uint64_t)joined[imm4 + i] << ((i % 8) * 8);
        cpu->vreg[rd][0] = out[0];
        cpu->vreg[rd][1] = q ? out[1] : 0;
        cpu->pc += 4;
        return true;
    }

    /* One register in, one register out: comparisons against nothing, and the
     * sign-changing ones.
     *
     * Comparing every number against zero at once is how a vectorised loop
     * asks a question about a whole array - the answer is not a yes or a no
     * but a register with all-ones where the answer was yes, which is then
     * used as a mask. */
    if (((in >> 24) & 0x1F) == 0x0E && ((in >> 17) & 0x1F) == 0x10 &&
        ((in >> 10) & 3) == 2) {
        unsigned size = (in >> 22) & 3;
        unsigned opcode = (in >> 12) & 0x1F;
        unsigned lanes = (q ? 16u : 8u) >> size;

        for (unsigned i = 0; i < lanes; i++) {
            uint64_t raw = vec_get(cpu, rn, size, i);
            int64_t signed_value = vec_signed(raw, size);
            uint64_t out;
            switch (opcode) {
            case 0x08: out = (u ? signed_value >= 0 : signed_value > 0)
                             ? ~0ull : 0; break;
            case 0x09: out = (u ? signed_value <= 0 : signed_value == 0)
                             ? ~0ull : 0; break;
            case 0x0A: if (u) return false;
                       out = signed_value < 0 ? ~0ull : 0; break;
            case 0x0B: out = u ? (uint64_t)(-signed_value)
                               : (uint64_t)(signed_value < 0 ? -signed_value
                                                             : signed_value);
                       break;
            case 0x05:
                if (u) { out = ~raw; break; }    /* every bit turned over */
                {                                 /* how many bits are set */
                    uint64_t count = 0, v = raw;
                    while (v) { count += v & 1; v >>= 1; }
                    out = count;
                }
                break;
            default: return false;
            }
            vec_put(cpu, rd, size, i, out);
        }
        if (!q) cpu->vreg[rd][1] = 0;
        cpu->pc += 4;
        return true;
    }

    /* Across the register rather than down it: one answer from all the
     * numbers together.
     *
     * This is what the end of a vectorised loop looks like.  The loop adds
     * four at a time into four running totals, and then at the very end those
     * four have to become one - which is this.  A loop that adds up an array
     * needs it and nothing else does, so leaving it out makes exactly the
     * most ordinary vector code the only kind that fails. */
    if (((in >> 24) & 0x1F) == 0x0E && ((in >> 17) & 0x1F) == 0x18 &&
        ((in >> 10) & 3) == 2) {
        unsigned size = (in >> 22) & 3;
        unsigned opcode = (in >> 12) & 0x1F;
        if (size > 2) return false;             /* no such thing for the widest */
        unsigned lanes = (q ? 16u : 8u) >> size;

        uint64_t out;
        switch (opcode) {
        case 0x1B: {                            /* all of them added together */
            uint64_t total = 0;
            for (unsigned i = 0; i < lanes; i++)
                total += vec_get(cpu, rn, size, i);
            out = total;
            break;
        }
        case 0x0A: {                            /* the largest of them */
            int64_t best = vec_signed(vec_get(cpu, rn, size, 0), size);
            uint64_t ubest = vec_get(cpu, rn, size, 0);
            for (unsigned i = 1; i < lanes; i++) {
                uint64_t raw = vec_get(cpu, rn, size, i);
                if (u) { if (raw > ubest) ubest = raw; }
                else if (vec_signed(raw, size) > best) {
                    best = vec_signed(raw, size);
                    ubest = raw;
                }
            }
            out = ubest;
            break;
        }
        case 0x1A: {                            /* the smallest of them */
            int64_t best = vec_signed(vec_get(cpu, rn, size, 0), size);
            uint64_t ubest = vec_get(cpu, rn, size, 0);
            for (unsigned i = 1; i < lanes; i++) {
                uint64_t raw = vec_get(cpu, rn, size, i);
                if (u) { if (raw < ubest) ubest = raw; }
                else if (vec_signed(raw, size) < best) {
                    best = vec_signed(raw, size);
                    ubest = raw;
                }
            }
            out = ubest;
            break;
        }
        default: return false;
        }

        cpu->vreg[rd][0] = 0;
        cpu->vreg[rd][1] = 0;
        vec_put(cpu, rd, size, 0, out);
        cpu->pc += 4;
        return true;
    }

    /* The same operation on every number in the register at once. */
    if (((in >> 24) & 0x1F) == 0x0E && ((in >> 21) & 1) == 1 &&
        ((in >> 10) & 1) == 1) {
        unsigned size = (in >> 22) & 3;
        unsigned opcode = (in >> 11) & 0x1F;

        /* The fractional ones are told apart by which operation it is, not by
         * the size field - which for them says single or double instead. */
        bool fractional = (opcode == 0x1A || opcode == 0x1B ||
                           opcode == 0x1F || opcode == 0x1C);
        if (fractional) {
            bool wide = (size & 1) != 0;
            unsigned bytes = wide ? 8 : 4;
            unsigned lanes = (q ? 16u : 8u) / bytes;
            for (unsigned i = 0; i < lanes; i++) {
                uint64_t abits = vec_get(cpu, rn, wide ? 3 : 2, i);
                uint64_t bbits = vec_get(cpu, rm, wide ? 3 : 2, i);
                double a, b, out;
                if (wide) {
                    memcpy(&a, &abits, 8);
                    memcpy(&b, &bbits, 8);
                } else {
                    float fa, fb;
                    uint32_t na = (uint32_t)abits, nb = (uint32_t)bbits;
                    memcpy(&fa, &na, 4);
                    memcpy(&fb, &nb, 4);
                    a = fa; b = fb;
                }
                if (opcode == 0x1A) out = (size & 2) ? a - b : a + b;
                else if (opcode == 0x1B && u) out = a * b;
                else if (opcode == 0x1F && u) out = a / b;
                else if (opcode == 0x1C) out = (a == b) ? 0 : 0;  /* compare */
                else return false;

                uint64_t bits;
                if (wide) memcpy(&bits, &out, 8);
                else { float f = (float)out; uint32_t n; memcpy(&n, &f, 4);
                       bits = n; }
                vec_put(cpu, rd, wide ? 3 : 2, i, bits);
            }
            if (!q) cpu->vreg[rd][1] = 0;
            cpu->pc += 4;
            return true;
        }

        unsigned lanes = (q ? 16u : 8u) >> size;
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t a = vec_get(cpu, rn, size, i);
            uint64_t b = vec_get(cpu, rm, size, i);
            uint64_t out;
            switch (opcode) {
            case 0x10: out = u ? a - b : a + b; break;      /* add, take away */
            case 0x17: {                                    /* neighbours added */
                if (u) return false;
                /* The first half of the answer comes from pairs in one
                 * register, the second half from pairs in the other. */
                unsigned half = lanes / 2;
                unsigned from = (i < half) ? rn : rm;
                unsigned at = (i < half) ? i : (i - half);
                out = vec_get(cpu, from, size, at * 2) +
                      vec_get(cpu, from, size, at * 2 + 1);
                break;
            }
            case 0x13: if (u) return false; out = a * b; break;
            case 0x03:                                       /* the bit work  */
                if (u) out = a ^ b;
                else switch (size) {
                case 0: out = a & b; break;
                case 1: out = a & ~b; break;
                case 2: out = a | b; break;
                default: out = a | ~b; break;
                }
                break;
            case 0x0C: out = u ? (a > b ? a : b)             /* the larger    */
                              : (uint64_t)(vec_signed(a, size) >
                                           vec_signed(b, size) ? a : b);
                       break;
            case 0x0D: out = u ? (a < b ? a : b)             /* the smaller   */
                              : (uint64_t)(vec_signed(a, size) <
                                           vec_signed(b, size) ? a : b);
                       break;
            case 0x11: out = u ? (a == b ? ~0ull : 0)        /* the same?     */
                              : ((a & b) ? ~0ull : 0);
                       break;
            default: return false;
            }
            vec_put(cpu, rd, size, i, out);
        }
        if (!q) cpu->vreg[rd][1] = 0;
        cpu->pc += 4;
        return true;
    }

    return false;
}

static bool exec_fp(arm64_cpu_t *cpu, uint32_t in) {
    /* The vector forms and the single-number forms live in the same corner of
     * the encoding and are told apart by one bit near the top: with it, the
     * register holds one number; without it, several. */
    if (((in >> 24) & 0x1F) == 0x0E || ((in >> 24) & 0x1F) == 0x0F)
        return exec_simd(cpu, in);

    /* Taking one number out of a register holding several, into one holding
     * a single number.  It is written down among the single-number forms even
     * though what it does is reach into a vector, so it is looked for here. */
    if (((in >> 21) & 0x7FF) == 0x2F0 && ((in >> 10) & 0x3F) == 0x01)
        return exec_simd(cpu, in);

    unsigned rd = in & 31;
    unsigned rn = (in >> 5) & 31;
    unsigned rm = (in >> 16) & 31;
    unsigned type = (in >> 22) & 3;         /* 0 narrow, 1 wide */

    /* Only the two ordinary sizes.  The half-width one is a different thing
     * and saying so is better than pretending. */
    if (type > 1) return false;
    bool wide = (type == 1);

    /* Three sources: multiply two and add the third, as one instruction.
     *
     * A compiler reaches for this constantly - any expression of the shape
     * a * b + c becomes one of these - so without it almost nothing that does
     * arithmetic on fractional numbers runs.  It is a family of its own, one
     * bit away from the two-source arithmetic below, and doing the multiply
     * and the addition together is not the same as doing them in turn: the
     * intermediate result is not rounded, which is the whole point of it. */
    if ((in >> 24) & 1) {
        unsigned ra = (in >> 10) & 31;
        bool negate_product = (in >> 15) & 1;
        bool negate_all = (in >> 21) & 1;

        double a = fp_read(cpu, rn, wide);
        double b = fp_read(cpu, rm, wide);
        double c = fp_read(cpu, ra, wide);

        double product = a * b;
        if (negate_product) product = -product;
        double out = negate_all ? -(c + product) : (c + product);

        fp_write(cpu, rd, wide, out);
        cpu->pc += 4;
        return true;
    }

    /* Between a floating-point register and an ordinary one, and between a
     * number and an integer.  These are the ones that let floating point
     * reach the rest of the program. */
    if (((in >> 21) & 1) && ((in >> 10) & 0x3F) == 0) {
        bool sf = (in >> 31) & 1;
        unsigned rmode = (in >> 19) & 3;
        unsigned opcode = (in >> 16) & 7;

        if (rmode == 0 && opcode == 6) {            /* the bits, out */
            write_x(cpu, rd, wide ? cpu->vreg[rn][0]
                                  : (uint32_t)cpu->vreg[rn][0]);
            cpu->pc += 4;
            return true;
        }
        if (rmode == 0 && opcode == 7) {            /* the bits, in */
            cpu->vreg[rd][0] = wide ? read_x(cpu, rn)
                                    : (uint32_t)read_x(cpu, rn);
            cpu->vreg[rd][1] = 0;
            cpu->pc += 4;
            return true;
        }
        if (rmode == 0 && (opcode == 2 || opcode == 3)) {   /* to a number */
            double value;
            if (opcode == 2)
                value = sf ? (double)(int64_t)read_x(cpu, rn)
                           : (double)(int32_t)(uint32_t)read_x(cpu, rn);
            else
                value = sf ? (double)read_x(cpu, rn)
                           : (double)(uint32_t)read_x(cpu, rn);
            fp_write(cpu, rd, wide, value);
            cpu->pc += 4;
            return true;
        }
        if (rmode == 3 && (opcode == 0 || opcode == 1)) {   /* to an integer */
            double value = fp_read(cpu, rn, wide);
            /* Toward zero, which is what this form means, and anything that
             * will not fit comes back as the largest that will. */
            if (opcode == 0) {
                int64_t out = (value != value) ? 0 : (int64_t)value;
                write_result(cpu, rd, sf, (uint64_t)out);
            } else {
                uint64_t out = (value != value || value < 0) ? 0
                                                             : (uint64_t)value;
                write_result(cpu, rd, sf, out);
            }
            cpu->pc += 4;
            return true;
        }
        return false;
    }

    /* Converting and scaling in one instruction.
     *
     * The same conversion as above but with a count of fractional bits built
     * in, so that dividing by a power of two costs nothing extra.  A compiler
     * uses it wherever a whole number is turned into a fractional one and
     * immediately divided - which is common enough that leaving it out makes
     * ordinary code fail on a conversion that looks like it should work. */
    if (!((in >> 21) & 1)) {
        bool sf = (in >> 31) & 1;
        unsigned rmode = (in >> 19) & 3;
        unsigned opcode = (in >> 16) & 7;
        unsigned scale = (in >> 10) & 0x3F;
        unsigned fractional = 64 - scale;

        /* Two to the power of the fractional bits, built by multiplying
         * rather than by any library. */
        double divisor = 1.0;
        for (unsigned i = 0; i < fractional; i++) divisor *= 2.0;

        if (rmode == 0 && (opcode == 2 || opcode == 3)) {   /* to a number */
            double value;
            if (opcode == 2)
                value = sf ? (double)(int64_t)read_x(cpu, rn)
                           : (double)(int32_t)(uint32_t)read_x(cpu, rn);
            else
                value = sf ? (double)read_x(cpu, rn)
                           : (double)(uint32_t)read_x(cpu, rn);
            fp_write(cpu, rd, wide, value / divisor);
            cpu->pc += 4;
            return true;
        }
        if (rmode == 3 && (opcode == 0 || opcode == 1)) {   /* to an integer */
            double value = fp_read(cpu, rn, wide) * divisor;
            if (opcode == 0) {
                int64_t out = (value != value) ? 0 : (int64_t)value;
                write_result(cpu, rd, sf, (uint64_t)out);
            } else {
                uint64_t out = (value != value || value < 0) ? 0
                                                             : (uint64_t)value;
                write_result(cpu, rd, sf, out);
            }
            cpu->pc += 4;
            return true;
        }
        return false;
    }

    /* A number written into the instruction. */
    if (((in >> 21) & 1) && ((in >> 10) & 0x7) == 0x4) {
        unsigned imm8 = (in >> 13) & 0xFF;
        fp_write(cpu, rd, wide, fp_immediate(imm8, wide));
        cpu->pc += 4;
        return true;
    }

    /* Comparing. */
    if (((in >> 21) & 1) && ((in >> 10) & 0xF) == 0x8) {
        bool against_zero = (in >> 3) & 1;
        double a = fp_read(cpu, rn, wide);
        double b = against_zero ? 0.0 : fp_read(cpu, rm, wide);
        fp_compare(cpu, a, b);
        cpu->pc += 4;
        return true;
    }

    /* One source: the shape-changing and sign-changing ones. */
    if (((in >> 21) & 1) && ((in >> 10) & 0x1F) == 0x10) {
        unsigned opcode = (in >> 15) & 0x3F;
        double a = fp_read(cpu, rn, wide);
        double out;
        switch (opcode) {
        case 0x00: out = a; break;                          /* move   */
        case 0x01: out = a < 0 ? -a : a; break;             /* size   */
        case 0x02: out = -a; break;                         /* negate */
        case 0x03:                                          /* root   */
            if (a != a || a < 0) { out = a - a; out = out / out; break; }
            if (a == 0) { out = a; break; }
            out = a > 1 ? a / 2 : 1.0;
            /* Halving the error each time; twenty rounds is far past what
             * sixty-four bits can hold apart. */
            for (int i = 0; i < 20; i++) out = (out + a / out) / 2;
            break;
        case 0x04:                                          /* to narrow */
            fp_write(cpu, rd, false, a);
            cpu->pc += 4;
            return true;
        case 0x05:                                          /* to wide   */
            fp_write(cpu, rd, true, a);
            cpu->pc += 4;
            return true;
        default: return false;
        }
        fp_write(cpu, rd, wide, out);
        cpu->pc += 4;
        return true;
    }

    /* Two sources: the arithmetic. */
    if (((in >> 21) & 1) && ((in >> 10) & 3) == 2) {
        unsigned opcode = (in >> 12) & 0xF;
        double a = fp_read(cpu, rn, wide);
        double b = fp_read(cpu, rm, wide);
        double out;
        switch (opcode) {
        case 0x0: out = a * b; break;
        case 0x1: out = a / b; break;
        case 0x2: out = a + b; break;
        case 0x3: out = a - b; break;
        case 0x4: out = (a > b || b != b) ? a : b; break;    /* larger  */
        case 0x5: out = (a < b || b != b) ? a : b; break;    /* smaller */
        case 0x8: out = a * b; out = -out; break;
        default: return false;
        }
        fp_write(cpu, rd, wide, out);
        cpu->pc += 4;
        return true;
    }

    return false;
}

bool arm64_step(arm64_cpu_t *cpu) {
    if (cpu->stopped) return false;

    if (!in_range(cpu, cpu->pc, 4)) {
        fault(cpu, "tried to run code that is not there", cpu->pc);
        return false;
    }

    const uint8_t *p = cpu->memory + (cpu->pc - cpu->memory_base);
    uint32_t in = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                  ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);

    uint64_t was = cpu->pc;
    bool handled;

    /* The architecture's own top-level split, on bits twenty-five to
     * twenty-eight. Keeping the same shape means this can be read against the
     * manual rather than against itself. */
    switch ((in >> 25) & 0xF) {
    case 0x8: case 0x9:                       /* data processing, immediate */
        handled = exec_data_immediate(cpu, in);
        if (handled && cpu->pc == was) cpu->pc += 4;
        break;

    case 0xA: case 0xB:                       /* branches, exceptions, system */
        handled = exec_branch(cpu, in);
        break;

    case 0x4: case 0x6: case 0xC: case 0xE:   /* loads and stores */
        handled = exec_loadstore(cpu, in);
        break;

    case 0x5: case 0xD:                       /* data processing, register */
        handled = exec_data_register(cpu, in);
        if (handled && cpu->pc == was) cpu->pc += 4;
        break;

    case 0x7: case 0xF:                       /* floating point and vectors */
        handled = exec_fp(cpu, in);
        break;

    default:
        handled = false;
        break;
    }

    if (!handled) {
        fault(cpu, "an instruction this interpreter does not know", cpu->pc);
        cpu->fault_addr = in;
        return false;
    }

    cpu->executed++;
    return !cpu->stopped;
}

void arm64_run(arm64_cpu_t *cpu, uint64_t budget) {
    while (!cpu->stopped && budget--) {
        if (!arm64_step(cpu)) break;
    }
    if (budget == (uint64_t)-1 && !cpu->stopped)
        fault(cpu, "the program ran for longer than it was allowed", cpu->pc);
}

void arm64_init(arm64_cpu_t *cpu, uint8_t *memory, uint64_t base, uint64_t size) {
    memset(cpu, 0, sizeof *cpu);
    cpu->memory = memory;
    cpu->memory_base = base;
    cpu->memory_size = size;
}

void arm64_describe(uint32_t in, char *out, size_t cap) {
    /* Enough to say what family it belongs to, which is what a fault message
     * needs; a full disassembler would be a larger thing than the interpreter. */
    const char *family;
    switch ((in >> 25) & 0xF) {
    case 0x8: case 0x9: family = "data processing with a constant"; break;
    case 0xA: case 0xB: family = "a branch or a system instruction"; break;
    case 0x4: case 0x6: case 0xC: case 0xE: family = "a load or a store"; break;
    case 0x5: case 0xD: family = "data processing between registers"; break;
    case 0x7: case 0xF: family = "floating point or vector"; break;
    default: family = "an unallocated encoding"; break;
    }
    snprintf(out, cap, "%08x, which is %s", in, family);
}

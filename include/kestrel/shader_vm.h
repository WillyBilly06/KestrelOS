/* Address-free programmable shader bytecode, shared by GLSL and GPU execution.
 * All registers are vec4 float32. GPU scratch is SoA: (reg*4+component)*lanes+id.
 * A native caller must own, bounds-check and fence each supplied allocation;
 * this header contains no user pointers or arbitrary GPU virtual addresses.
 */
#ifndef KESTREL_SHADER_VM_H
#define KESTREL_SHADER_VM_H
#define SR_TEMP 0
#define SR_TEMP_N 64
#define SR_UNIFORM 64
#define SR_UNIFORM_N 48
#define SR_ATTRIB 112
#define SR_ATTRIB_N 16
#define SR_VARYING 128
#define SR_VARYING_N 8
#define SR_CONST 136
#define SR_CONST_N 56
#define SR_POSITION 192
#define SR_FRAGCOLOR 193
#define SR_FRAGCOORD 194
#define SR_POINTSIZE 195
#define SR_REGISTERS 200
#define SH_SWIZZLE_XYZW 0xE4
#define SH_MAX_INSTRUCTIONS 1024

typedef enum {
    SH_END=0, SH_MOV, SH_ADD, SH_SUB, SH_MUL, SH_DIV, SH_MAD,
    SH_DP2, SH_DP3, SH_DP4, SH_MIN, SH_MAX, SH_CROSS,
    SH_RCP, SH_RSQ, SH_SQRT, SH_ABS, SH_NEG, SH_FLOOR, SH_FRACT, SH_SIGN,
    SH_SIN, SH_COS, SH_POW, SH_EXP2, SH_LOG2,
    SH_SLT, SH_SLE, SH_SGT, SH_SGE, SH_SEQ, SH_SNE,
    SH_AND, SH_OR, SH_NOT, SH_TEX, SH_MATMUL,
    SH_JMP, SH_JMPZ, SH_JMPNZ, SH_DISCARD
} sh_op_t;
typedef struct {
    unsigned char op, mask;
    unsigned short dst, src[3];
    unsigned char swizzle[3];
    /* Byte 13 is ABI padding, not executable data. */
    short target;
} sh_instruction_t;

#define KSH_MAX_LANES 4096u
#define KSH_MAX_STEPS 4096u
#define KSH_MAX_WORK 16777216ull
#define KSH_MAX_TEXTURES 4u
#define KSH_PENDING 0u
#define KSH_COMPLETE 1u
#define KSH_DISCARDED 2u
#define KSH_BAD_PROGRAM 3u
#define KSH_STEP_LIMIT 4u
#define KSH_BAD_RESOURCE 5u
typedef struct { unsigned int result, executed; } ksh_status_t;

/* Offset into the separately owned packed ARGB texture allocation, in bytes.
 * Empty descriptors (width=height=0) sample white, matching libgl's unbound
 * texture. Invalid units sample opaque black. Repeat=0, clamp=1; nearest=0,
 * linear=1. Width/height <=32768; pitch is in pixels. */
typedef struct {
    unsigned long long offset;
    unsigned int width, height, pitch, wrap_s, wrap_t, filter;
} ksh_texture_t;
/* Immutable syscall snapshot. code_count entries are used; unused fields are
 * not pointers. The register/status/texture allocations are separate owner-
 * checked surface handles in kg2d_request_t. This fixed-size packet avoids
 * nested userspace reads or trusting a count fetched before the snapshot. */
typedef struct {
    unsigned int lanes, code_count, budget, texture_count;
    unsigned int reserved[4];
    ksh_texture_t textures[KSH_MAX_TEXTURES];
    sh_instruction_t code[SH_MAX_INSTRUCTIONS];
} ksh_dispatch_t;
#if defined(__cplusplus)
static_assert(sizeof(sh_instruction_t)==16,"shader instruction ABI");
static_assert(sizeof(ksh_texture_t)==32,"shader texture ABI");
static_assert(sizeof(ksh_status_t)==8,"shader status ABI");
static_assert(sizeof(ksh_dispatch_t)==16544,"shader dispatch snapshot ABI");
#else
_Static_assert(sizeof(sh_instruction_t)==16,"shader instruction ABI");
_Static_assert(sizeof(ksh_texture_t)==32,"shader texture ABI");
_Static_assert(sizeof(ksh_status_t)==8,"shader status ABI");
_Static_assert(sizeof(ksh_dispatch_t)==16544,"shader dispatch snapshot ABI");
#endif
#endif

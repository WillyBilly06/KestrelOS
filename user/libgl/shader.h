/* shader.h - the programmable pipeline.
 *
 * Everything drawn on a machine built after about 2004 is drawn by a program.
 * Not by turning fixed-function switches on and off, but by two small programs
 * the application writes and the driver compiles: one that runs once per
 * vertex and decides where it lands, and one that runs once per pixel and
 * decides what colour it is.  Every engine, every game and every browser's
 * compositor is built on that and nothing else, which is why a graphics stack
 * without it can draw a cube and cannot draw anything anybody would ship.
 *
 * This is that pipeline.  A GLSL compiler (glsl.c) turns source text into the
 * instruction set below, and an interpreter (shadervm.c) runs it.  The
 * rasteriser interpolates whatever the vertex program wrote and calls the
 * fragment program for every pixel it covers.
 *
 * The instruction set is a register machine over four-component vectors,
 * which is what graphics hardware has always been underneath.  Every register
 * is a vec4; a float is a vec4 whose first component is used.  Operands carry
 * a swizzle and destinations carry a write mask, because that is how the
 * source language works and building it in costs nothing.
 */
#ifndef KESTREL_SHADER_H
#define KESTREL_SHADER_H

#include "../libc/kestrel.h"

/* ------------------------------------------------------------ the registers
 *
 * One flat file, divided into ranges.  Keeping it flat means an operand is a
 * single number and the interpreter's inner loop has no cases in it.
 */
#include "../../include/kestrel/shader_vm.h"
#define SH_MAX_SYMBOLS      32
#define SH_NAME_MAX         32

typedef struct {
    char     name[SH_NAME_MAX];
    uint16_t reg;
    uint8_t  components;   /* 1 to 4, or 16 for a mat4                     */
    bool     is_matrix;
    bool     is_sampler;
    int      location;     /* what the application is handed back           */
} sh_symbol_t;

typedef enum { SH_VERTEX, SH_FRAGMENT } sh_stage_t;

typedef struct {
    sh_stage_t stage;
    bool       compiled;

    sh_instruction_t code[SH_MAX_INSTRUCTIONS];
    int        count;

    float      constants[SR_CONST_N][4];
    int        nconstants;

    sh_symbol_t uniform[SH_MAX_SYMBOLS];   int nuniforms;
    sh_symbol_t attribute[SH_MAX_SYMBOLS]; int nattributes;
    sh_symbol_t varying[SH_MAX_SYMBOLS];   int nvaryings;

    bool       writes_position;
    bool       writes_colour;
    bool       uses_discard;

    char       log[256];
} sh_shader_t;

/* Two compiled shaders, linked: the varyings matched up by name so the vertex
 * program's outputs land in the registers the fragment program reads. */
typedef struct {
    bool linked;
    sh_shader_t *vertex;
    sh_shader_t *fragment;

    /* The union of both stages' uniforms, which is what the application sets
     * and what a location refers to. */
    sh_symbol_t uniform[SH_MAX_SYMBOLS * 2];
    int         nuniforms;
    /* Where each uniform lives in each stage; -1 when that stage has none. */
    int         vertex_reg[SH_MAX_SYMBOLS * 2];
    int         fragment_reg[SH_MAX_SYMBOLS * 2];

    sh_symbol_t attribute[SH_MAX_SYMBOLS];
    int         nattributes;

    int         nvaryings;        /* how many registers actually carry data */
    int         varying_components[SR_VARYING_N];

    char        log[256];
} sh_program_t;

/* --------------------------------------------------------------- compiling */

/* Turns GLSL source into the instruction set above.  Returns false and fills
 * in the log on any error - a shader compiler that guesses at broken source
 * produces a picture that is wrong in a way nobody can trace. */
bool sh_compile(sh_shader_t *out, sh_stage_t stage, const char *source);

/* Matches the two stages' varyings by name and builds the uniform table. */
bool sh_link(sh_program_t *out, sh_shader_t *vertex, sh_shader_t *fragment);

/* ---------------------------------------------------------------- running */

/* How a fragment program reaches a texture.  Supplied by the caller so the VM
 * does not have to know what a texture is. */
typedef void (*sh_sampler_fn)(int unit, float s, float t, float *rgba);

typedef struct {
    float reg[SR_REGISTERS][4];
    sh_sampler_fn sample;
    bool  discarded;
    int   executed;                /* instructions run, for the diagnostics */
} sh_machine_t;

void sh_run(sh_machine_t *m, const sh_shader_t *shader);

/* What a location resolves to, and setting one. */
int  sh_uniform_location(const sh_program_t *p, const char *name);
int  sh_attribute_location(const sh_program_t *p, const char *name);

#endif

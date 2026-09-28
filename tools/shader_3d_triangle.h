/* shader_3d_triangle.h - the graphics shaders for the 3D triangle self-test.
 *
 * THIS IS A HOOK, NOT A SHADER.  It deliberately contains no microcode bytes.
 *
 * The 3D method stream in kernel/nv_3d.c is complete and verified against Mesa's
 * clce97.h (Blackwell BLACKWELL_B, 0xce97): render target, the NDC->screen
 * viewport transform, SET_PIPELINE_SHADER + PROGRAM_ADDRESS (full 40-bit VA of
 * each stage's shader program header), vertex streams/attributes, BEGIN/DRAW/END,
 * and the SET_REPORT_SEMAPHORE_D fence.  What it CANNOT supply from this machine
 * is the shader machine code itself:
 *
 *   - ptxas / nvcc (CUDA 13.3, on the build host) compile sm_120 COMPUTE kernels
 *     (see tools/nvshader.py, tools/shader_writeval.h) - and that path IS proven.
 *   - They do NOT emit GRAPHICS (vertex / fragment) shaders: a VS/FS moves data
 *     through the attribute file (ald/ast/ipa) and the interpolator, which CUDA's
 *     compute target has no path to.  Confirmed at the ISA level (Mesa NAK
 *     ir.rs OpALd/OpASt/OpIpa; NAK's own hw_runner only ever dispatches compute).
 *
 * The ONLY compiler that emits Blackwell VTG/PS microcode is NAK, inside
 * Mesa/NVK, which does not run on Windows and needs the real card via nouveau.
 *
 * ---- CAPTURE RECIPE (fills in the two arrays + SPHs below) -----------------
 *   1. On Linux (or WSL2) with the RTX 5070 Ti and the nouveau kernel driver,
 *      build Mesa with the NVK (nouveau Vulkan) driver.
 *   2. Run any one-triangle Vulkan sample with:
 *        NV50_PROG_DEBUG=1 NAK_DEBUG=print  ./triangle
 *      nvk_shader_dump() (src/nouveau/vulkan/nvk_shader.c) prints, for each of
 *      the vertex and fragment shaders: the 32-word (128-byte) program header
 *      (SPH) followed by the raw microcode words.
 *   3. Paste the vertex shader's SPH (32 u32) into NV3D_VS_SPH, its microcode
 *      bytes into NV3D_VS_CODE; likewise the fragment shader into NV3D_FS_SPH /
 *      NV3D_FS_CODE.  Set NV3D_SHADERS_PRESENT to 1.
 *   4. Load, per stage, the 128-byte SPH immediately followed by the microcode
 *      at a 128-byte-aligned GPU VA, and pass that VA (the SPH start) to
 *      nv_3d_set_program()'s program_va.  reg_count is the shader's GPR count
 *      (also in the NVK dump).
 *
 * Until then NV3D_SHADERS_PRESENT stays 0 and the 3D self-test SKIPS the draw
 * (it does not dispatch garbage - a wrong SPH/program faults the GR engine).
 * The triangle is method-complete and blocked ONLY on this capture step.
 */
#ifndef KESTREL_SHADER_3D_TRIANGLE_H
#define KESTREL_SHADER_3D_TRIANGLE_H

#define NV3D_SHADERS_PRESENT 0   /* set to 1 once the arrays below are filled */

/* Placeholders - intentionally empty; do NOT fill with invented bytes. */
#define NV3D_VS_SPH_WORDS  0
#define NV3D_FS_SPH_WORDS  0
#define NV3D_VS_CODE_BYTES 0
#define NV3D_FS_CODE_BYTES 0

#endif

/* Minimal execution-only Blackwell compute probe.
 *
 * Keep this source beside the generated shader_gpunop.h so the embedded SASS
 * is reproducible with tools/nvshader.py.  The kernel intentionally performs
 * no memory access: reaching EXIT is enough to prove that CWD scheduled a CTA
 * and an SM executed it.
 */
extern "C" __global__ void gpunop(void)
{
}

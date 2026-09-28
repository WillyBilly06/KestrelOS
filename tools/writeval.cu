/* Minimal Blackwell compute data-path probe.
 *
 * The output pointer is the first CUDA kernel argument.  nvshader.py records
 * the real sm_120 code and resource counts in shader_writeval.h.
 */
extern "C" __global__ void writeval(unsigned int *out)
{
    *out = 0xCAFEBABE;
}

/* tri_raster.cu - rasterize a filled triangle into a 32-bpp surface with a
 * COMPUTE shader.  This is KestrelOS's path to 3D output on Blackwell WITHOUT the
 * 0xce97 graphics pipeline (whose vertex/fragment microcode ptxas cannot emit):
 * ptxas DOES emit this compute kernel as real sm_120 machine code, it runs on the
 * card through the proven compute-dispatch path, and the result is byte-verifiable
 * by copying the surface back with the copy engine.  One thread per pixel; a pixel
 * is filled iff it is inside the triangle (all three integer edge functions share
 * a sign).  Integer-only so there is no FP-ABI dependency.
 *
 * Args arrive in constant bank 0 at c[0x0][0x380] in declaration order (CUDA ABI).
 * Build: tools/nvshader.py tri_raster.cu tri_raster -a sm_120 -o tools/shader_tri_raster.h
 */
extern "C" __global__ void tri_raster(
    unsigned int *fb, int W, int H, int pitch_px,
    int ax, int ay, int bx, int by, int cx, int cy,
    unsigned int color)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= W || y >= H) return;

    long w0 = (long)(cx - bx) * (y - by) - (long)(cy - by) * (x - bx);
    long w1 = (long)(ax - cx) * (y - cy) - (long)(ay - cy) * (x - cx);
    long w2 = (long)(bx - ax) * (y - ay) - (long)(by - ay) * (x - ax);

    bool inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                  (w0 <= 0 && w1 <= 0 && w2 <= 0);
    if (inside) fb[y * pitch_px + x] = color;
}

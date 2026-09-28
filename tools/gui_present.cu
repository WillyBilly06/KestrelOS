/* GPU-only viewport presentation, including differently sized mirror outputs.
 * Launch a 16x16 grid over the physical damage clip. Mapping uses full viewport
 * coordinates, never the clipped rectangle's origin, so successive damaged
 * tiles have the same sampling phase as a full-frame presentation.
 */
extern "C" __global__ void gui_present(
    unsigned int *dst, const unsigned int *src,
    unsigned int src_width, unsigned int src_height, unsigned int src_pitch,
    unsigned int dst_width, unsigned int dst_height, unsigned int dst_pitch,
    unsigned int view_x, unsigned int view_y,
    unsigned int view_width, unsigned int view_height,
    unsigned int clip_x, unsigned int clip_y,
    unsigned int clip_width, unsigned int clip_height,
    unsigned int rotation)
{
    unsigned long long tx = (unsigned long long)blockIdx.x * blockDim.x + threadIdx.x;
    unsigned long long ty = (unsigned long long)blockIdx.y * blockDim.y + threadIdx.y;
    unsigned long long x = clip_x + tx, y = clip_y + ty;
    if (!dst || !src || !view_width || !view_height ||
        (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) ||
        !dst_width || !dst_height || src_pitch < src_width || dst_pitch < dst_width ||
        view_x >= src_width || view_y >= src_height ||
        view_width > src_width - view_x || view_height > src_height - view_y ||
        tx >= clip_width || ty >= clip_height || x >= dst_width || y >= dst_height)
        return;
    unsigned long long u=x, v=y;
    unsigned int uw=dst_width, uh=dst_height;
    if (rotation == 90) {
        u=y; v=dst_width-1-x; uw=dst_height; uh=dst_width;
    } else if (rotation == 180) {
        u=dst_width-1-x; v=dst_height-1-y;
    } else if (rotation == 270) {
        u=dst_height-1-y; v=x; uw=dst_height; uh=dst_width;
    }
    unsigned long long sx = view_x + (view_width == uw ? u : u * view_width / uw);
    unsigned long long sy = view_y + (view_height == uh ? v : v * view_height / uh);
    dst[y * dst_pitch + x] = src[sy * src_pitch + sx];
}

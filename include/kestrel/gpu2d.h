/* Fixed-width, address-free drawing records for the native GPU 2D backend.
 * A record references offsets inside a validated immutable source allocation;
 * it cannot name an arbitrary GPU address. */
#ifndef KESTREL_GPU2D_H
#define KESTREL_GPU2D_H

#define KG2D_SOLID       0u
#define KG2D_GRADIENT_H  1u
#define KG2D_GRADIENT_V  2u
#define KG2D_IMAGE       3u
#define KG2D_MASK_A8     4u
#define KG2D_IMAGE_SCALED 5u /* reserved[0:1] = source width,height */
#define KG2D_ARGB_BILINEAR 6u /* source size in reserved[0:1]; colour1=1 tints with colour0 */
#define KG2D_SHADOW 7u /* outer shadow rectangle; source_x=spread, source_y=radius; no source */
#define KG2D_ROUNDED 8u /* source_x=radius; source_y=0 AA fill,1 AA frame,2 top,3 gradient top,4 hard fill,5 hard frame; no source */
#define KG2D_LINE 9u /* inclusive endpoint bounding box; colour1 bit0=reverse X, bit1=reverse Y; no source */
#define KG2D_LINE_AA 10u /* endpoint bounds + one minor-axis fringe pixel; colour1 bit0=steep, bit1=negative slope */
#define KG2D_GLYPH 11u /* source_x/y=unscaled glyph dimensions; colour1=0 A8,1 MSB-first bits; nearest scaling */
#define KG2D_MAX_COMMANDS 256u

typedef struct {
    int x, y, width, height;
    unsigned int op, colour0, colour1, opacity;
    unsigned int source_x, source_y, source_stride, source_offset;
    unsigned int reserved[4];
} kg2d_command_t;

#define KG2D_ABI 1u
#define KG2D_CREATE 0u
#define KG2D_DESTROY 1u
#define KG2D_UPLOAD 2u
#define KG2D_DOWNLOAD 3u
#define KG2D_DRAW 4u
#define KG2D_PRESENT 5u
#define KG2D_DRAW3D 6u /* data=kg3d_command_t[], source=texture, offset=depth handle */
#define KG2D_SHADER_VM 7u /* handle=SoA registers, offset=status handle, source=texture atlas,
                          * data=ksh_dispatch_t, bytes=sizeof(ksh_dispatch_t).
                           * Success means fenced; inspect every lane's status before using output. */
#define KG2D_SHADER_RASTER 8u /* handle=colour, offset=depth, source=texture atlas,
                               * data=kshr_submission_t; exact size in bytes.
                                * Fenced only: every tile status must be COMPLETE. */
#define KG2D_SHADER_GEOMETRY 9u /* data=kshs_submission_t; handles as RASTER.
                                 * Success includes ALL stage/lane semantic checks.
                                  * Returns emitted triangle count in request.count,
                                  * retired fragment instruction total in request.bytes. */
#define KG2D_TRANSFER_MAX (4u * 1024u * 1024u)
/* All handles belong to the calling address space. data is a user pointer,
 * copied into kernel storage before submission. No GPU addresses are exposed.
 * CREATE returns handle/pitch/bytes; GEOMETRY returns count/bytes. Other operations
 * leave it unchanged. Transfers are byte-addressed, DWORD aligned and bounded. */
typedef struct {
    unsigned int version, operation;
    unsigned long long handle, source, data, offset;
    unsigned int x, y, width, height, count, pitch;
    unsigned long long bytes;
} kg2d_request_t;
#if defined(__cplusplus)
static_assert(sizeof(kg2d_request_t) == 72, "GPU surface request ABI");
#else
_Static_assert(sizeof(kg2d_request_t) == 72, "GPU surface request ABI");
#endif

#if defined(__cplusplus)
static_assert(sizeof(kg2d_command_t) == 64, "GPU 2D command ABI");
#else
_Static_assert(sizeof(kg2d_command_t) == 64, "GPU 2D command ABI");
#endif
#endif

/* CTA-861 Short Video Descriptor lookup. */
#ifndef KESTREL_EDID_CEA_H
#define KESTREL_EDID_CEA_H
#include "edid.h"
int edid_cea_mode(u8 vic, edid_mode_t *out);
#endif

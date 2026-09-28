/* kar.h - the KestrelOS archive format used for the initrd.
 *
 * A header, a table of fixed-size entries, then the file data.  Entries are
 * listed parents-first so an extractor can create directories as it goes
 * without a second pass.  Everything is little-endian and the whole archive is
 * meant to be read straight out of the memory the loader put it in.
 */
#ifndef KESTREL_KAR_H
#define KESTREL_KAR_H

#include <stdint.h>

#define KAR_MAGIC   0x3152414B   /* "KAR1" */
#define KAR_NAME_MAX 100

enum { KAR_FILE = 1, KAR_DIR = 2 };

typedef struct {
    uint32_t magic;
    uint32_t entry_count;
    uint64_t total_size;      /* the whole archive, header included */
    uint32_t entry_offset;    /* byte offset of the entry table     */
    uint32_t data_offset;     /* byte offset of the first file body  */
} kar_header;

typedef struct {
    char     name[KAR_NAME_MAX];   /* absolute, '/'-separated, NUL padded */
    uint32_t type;                 /* KAR_FILE or KAR_DIR                 */
    uint32_t mode;                 /* 0755, 0644, ...                     */
    uint64_t size;                 /* 0 for directories                   */
    uint64_t offset;               /* from the start of the archive       */
} kar_entry;                       /* 124 bytes, padded to 128 by the tool */

#define KAR_ENTRY_SIZE 128

#endif

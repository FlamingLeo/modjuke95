/* Which libopenmpt build this exe links. build.sh compiles this file once
 * per edition (-DM95_EDITION_COMMON for the common-formats one); the file
 * types themselves come from libopenmpt at runtime. */
#include "common.h"

const char *m95_edition()
{
#ifdef M95_EDITION_COMMON
    return "common formats";
#else
    return "full";
#endif
}

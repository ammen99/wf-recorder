#include "averr.h"

const char* averr(int err)
{
    static char buf[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(buf, sizeof(buf), err);
    return buf;
}

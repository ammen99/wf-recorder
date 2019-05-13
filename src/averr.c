#include "averr.h"

const char* averr(int err)
{
    return av_err2str(err);
}

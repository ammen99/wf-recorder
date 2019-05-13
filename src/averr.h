#include <libavutil/error.h>

/* the macro av_err2str doesn't work in C++, so we have a wrapper for it here */
#ifdef __cplusplus
extern "C"
{
#endif
    const char* averr(int err);
#ifdef __cplusplus
}
#endif

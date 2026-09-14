#include "mediainterface.h"

extern "C" {
#include "libavutil/mem.h"
}

void AvFreeDeleter::operator()(uint8_t* p) const
{
    if (p)
    {
        av_free(p);
    }
}
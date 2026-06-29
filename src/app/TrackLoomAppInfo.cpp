#include "TrackLoomAppInfo.h"

namespace trackloom {
namespace {

#ifndef TRACKLOOM_APP_VERSION
#define TRACKLOOM_APP_VERSION "0.0.0"
#endif

}

DesktopAppInfo desktopAppInfo()
{
    return {
        "TrackLoom",
        TRACKLOOM_APP_VERSION,
        "TrackLoom"
    };
}

}

#pragma once
// Config
// Information Level: 0 smallest - 2 - biggest
#include <common/version/version.h>
#ifdef NDEBUG
#define CRASHHANDLER_DMP_LEVEL 0
#else
#define CRASHHANDLER_DMP_LEVEL 0
#endif

#define CRASHHANDLER_WEBSVC_ENABLED 1
#define CRASHHANDLER_WEBSVC_URL "https://crashout.alpinefaction.com/submit"
#define CRASHHANDLER_WEBSVC_AGENT AF_USER_AGENT_SUFFIX("Crash")

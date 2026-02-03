#pragma once

#ifndef TOSTRING
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#endif

// clang-format off

#define VERSION_TYPE_DEV      0
#define VERSION_TYPE_ALPHA    1
#define VERSION_TYPE_BETA     2
#define VERSION_TYPE_RC       3
#define VERSION_TYPE_RELEASE  4

// Variables to be modified during the release process
#define PRODUCT_NAME           "Alpine Faction"
#define VERSION_MAJOR          1
#define VERSION_MINOR          3
#define VERSION_PATCH          0
#define VERSION_TYPE           VERSION_TYPE_DEV
#define VERSION_TYPE_REVISION  2
#define VERSION_CODE           "Bakeapple"

// Version used for Alpine settings files
#define ADS_VERSION            2      // .toml dedicated server config files
#define AFS_VERSION            12     // alpine_settings.ini
#define AFCC_VERSION           1      // alpine_system.ini
#define ASG_VERSION            1      // .asg savegame files

// Increment only when absolutely necessary! Will break level compatibility for older clients.
// Stock RF1 uses versions 156 - 200. RF2 uses version 295
// AF can load levels with version 40 - 200, OR 300 - MAXIMUM_RFL_VERSION
// 180 = Volition RF PC official
// 200 = Community RF PC pre-Alpine
// 300 = Alpine 1.0.0
// 301 = Alpine 1.1.0
// 302 = Alpine 1.2.0
// 303 = Alpine 1.2.2
// 304 = Alpine 1.3.0
#define MAXIMUM_RFL_VERSION    304

// clang-format on

#if VERSION_TYPE == VERSION_TYPE_DEV
#define VERSION_SUFFIX        "-dev"
#elif VERSION_TYPE == VERSION_TYPE_ALPHA
#define VERSION_SUFFIX        "-alpha" TOSTRING(VERSION_TYPE_REVISION)
#elif VERSION_TYPE == VERSION_TYPE_BETA
#define VERSION_SUFFIX        "-beta" TOSTRING(VERSION_TYPE_REVISION)
#elif VERSION_TYPE == VERSION_TYPE_RC
#define VERSION_SUFFIX        "-rc" TOSTRING(VERSION_TYPE_REVISION)
#elif VERSION_TYPE == VERSION_TYPE_RELEASE
#define VERSION_SUFFIX        ""
#else
#error Unknown version type
#endif

#define VERSION_STR TOSTRING(VERSION_MAJOR) "." TOSTRING(VERSION_MINOR) "." TOSTRING(VERSION_PATCH) VERSION_SUFFIX
#define PRODUCT_NAME_VERSION PRODUCT_NAME " " VERSION_STR
#define AF_USER_AGENT_SUFFIX(suffix) PRODUCT_NAME " v" VERSION_STR " " suffix

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
#define VERSION_MINOR          2
#define VERSION_PATCH          0
#define VERSION_TYPE           VERSION_TYPE_DEV
#define VERSION_TYPE_REVISION  0
#define VERSION_CODE           "Willow"

// Version used for Alpine settings files
#define ADS_VERSION            1      // .toml dedicated server config files
#define AFS_VERSION            8      // alpine_settings.ini
#define AFCC_VERSION           1      // alpine_system.ini

// Increment only when absolutely necessary! Will break level compatibility for older clients.
// Stock RF1 uses versions 156 - 200. RF2 uses version 295
// AF can load levels with version 40 - 200 or 300 - MAXIMUM_RFL_VERSION
#define MAXIMUM_RFL_VERSION    302

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

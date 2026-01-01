#pragma once

#include <windows.h>

/*  To use this exported function of dll, include this header
 *  in your project.
 */

// Note: DLL_EXPORT macro is used by pthread.h header in MinGW...
#ifdef BUILD_DLL
    #define AF_DLL_EXPORT __declspec(dllexport)
#else
    #define AF_DLL_EXPORT __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C"
{
#endif

DWORD AF_DLL_EXPORT Init(void* unused);

#ifdef __cplusplus
}
#endif


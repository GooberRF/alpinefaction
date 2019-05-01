
// DashFactionLauncher.h : main header file for the PROJECT_NAME application
//

#pragma once

#ifndef __AFXWIN_H__
	#error "include 'stdafx.h' before including this file for PCH"
#endif

#include "resource.h"		// main symbols


// LauncherApp:
// See DashFactionLauncher.cpp for the implementation of this class
//

class LauncherApp : public CWinApp
{
public:
	LauncherApp();

// Overrides
public:
    virtual BOOL InitInstance();

// Implementation
    bool LaunchGame(HWND hwnd, const char* mod_name = nullptr);
    bool LaunchEditor(HWND hwnd, const char* mod_name = nullptr);

	DECLARE_MESSAGE_MAP()

private:
    void MigrateConfig();
    int Message(HWND hwnd, const char *pszText, const char *pszTitle, int Flags);
};

extern LauncherApp theApp;

#pragma once

#include <wxx_dialog.h>
#include <wxx_wincore.h>

#define WM_FFLINK_CLOSE_DIALOG (WM_USER + 50)

class FFLinkProgressDlg : public CDialog
{
public:
    FFLinkProgressDlg();
    void SetHwndStorage(HWND* hwndStorage) { m_hwnd_storage = hwndStorage; }

protected:
    BOOL OnInitDialog() override;
    LRESULT OnCloseDialog(WPARAM wparam, LPARAM lparam);
    INT_PTR DialogProc(UINT msg, WPARAM wparam, LPARAM lparam) override;

private:
    HWND* m_hwnd_storage = nullptr;
};

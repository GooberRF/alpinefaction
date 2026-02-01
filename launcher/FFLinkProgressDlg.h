#pragma once

#include <wxx_dialog.h>
#include <wxx_wincore.h>
#include <atomic>

#define WM_FFLINK_CLOSE_DIALOG (WM_USER + 50)

class FFLinkProgressDlg : public CDialog
{
public:
    FFLinkProgressDlg();
    void SetHwndStorage(std::atomic<HWND>* hwndStorage) { m_hwnd_storage = hwndStorage; }

protected:
    BOOL OnInitDialog() override;
    LRESULT OnCloseDialog(WPARAM wparam, LPARAM lparam);
    INT_PTR DialogProc(UINT msg, WPARAM wparam, LPARAM lparam) override;

private:
    std::atomic<HWND>* m_hwnd_storage = nullptr;
};

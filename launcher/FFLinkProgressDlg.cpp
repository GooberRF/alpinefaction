#include "FFLinkProgressDlg.h"
#include "resource.h"
#include <commctrl.h>

FFLinkProgressDlg::FFLinkProgressDlg(HWND* pHwndStorage)
    : CDialog(IDD_FFLINK_PROGRESS), m_pHwndStorage(pHwndStorage)
{
}

BOOL FFLinkProgressDlg::OnInitDialog()
{
    CDialog::OnInitDialog();
    SendDlgItemMessage(IDC_FFLINK_PROGRESS_BAR, PBM_SETMARQUEE, TRUE, 20);
    
    // Store HWND if storage pointer was provided
    if (m_pHwndStorage) {
        *m_pHwndStorage = GetHwnd();
    }
    
    return TRUE;
}

LRESULT FFLinkProgressDlg::OnCloseDialog(WPARAM wparam, LPARAM lparam)
{
    EndDialog(IDOK);
    return 0;
}

INT_PTR FFLinkProgressDlg::DialogProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_FFLINK_CLOSE_DIALOG) {
        return OnCloseDialog(wparam, lparam);
    }

    return CDialog::DialogProc(msg, wparam, lparam);
}

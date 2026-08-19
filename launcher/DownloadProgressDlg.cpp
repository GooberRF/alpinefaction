#include "DownloadProgressDlg.h"
#include "resource.h"
#include <format>

static constexpr UINT_PTR TIMER_POLL_ID = 1;

DownloadProgressDlg::DownloadProgressDlg(int file_id, const std::string& file_name, size_t file_size_kb)
    : CDialog(IDD_DOWNLOAD_PROGRESS),
      m_file_id(file_id), m_file_name(file_name), m_file_size_kb(file_size_kb)
{
}

BOOL DownloadProgressDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    // Initialize the progress bar
    SendDlgItemMessage(IDC_PROGRESS_BAR, PBM_SETPOS, 0, 0);

    // Set window title
    std::string title = "Downloading File: " + m_file_name;
    SetWindowText(title.c_str());

    SetTimer(TIMER_POLL_ID, 50, nullptr);

    return TRUE;
}

void DownloadProgressDlg::UpdateProgress(unsigned bytes_received)
{
    // Calculate progress percentage and update progress bar
    unsigned progress_percent =
        static_cast<unsigned>((static_cast<float>(bytes_received) / static_cast<float>(m_file_size_kb * 1024)) * 100);

    progress_percent = std::min(progress_percent, 100u); // Ensure it doesn't exceed 100%

    SendDlgItemMessage(IDC_PROGRESS_BAR, PBM_SETPOS, progress_percent, 0);

    // Update progress text
    std::string progress_text = std::format("Progress: {} KB of {} KB", bytes_received / 1024, m_file_size_kb);
    SetDlgItemTextA(IDC_STATIC_PROGRESS, progress_text.c_str());
}

void DownloadProgressDlg::RequestCancel()
{
    m_cancel_requested.store(true, std::memory_order_relaxed);
    SetDlgItemTextA(IDC_STATIC_PROGRESS, "Cancelling...");
}

void DownloadProgressDlg::OnCancel()
{
    RequestCancel();
}

INT_PTR DownloadProgressDlg::DialogProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_TIMER && wparam == TIMER_POLL_ID) {
        UpdateProgress(m_bytes_received.load(std::memory_order_relaxed));
        if (m_finished.load(std::memory_order_acquire)) {
            KillTimer(TIMER_POLL_ID);
            EndDialog(m_succeeded.load(std::memory_order_relaxed) ? IDOK : IDCANCEL);
        }
        return 0;
    }
    if (msg == WM_CLOSE) {
        RequestCancel();
        return 0;
    }

    return CDialog::DialogProc(msg, wparam, lparam);
}

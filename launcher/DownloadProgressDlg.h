#pragma once

#include <atomic>
#include <string>
#include <wxx_dialog.h>
#include <wxx_wincore.h>

class DownloadProgressDlg : public CDialog
{
public:
    DownloadProgressDlg(int file_id, const std::string& file_name, size_t file_size_kb);
    void UpdateProgress(unsigned bytes_received);

    void SetProgress(unsigned bytes) { m_bytes_received.store(bytes, std::memory_order_relaxed); }
    void SetFinished(bool success)
    {
        m_succeeded.store(success, std::memory_order_relaxed);
        m_finished.store(true, std::memory_order_release);
    }
    bool IsCancelRequested() const { return m_cancel_requested.load(std::memory_order_relaxed); }

protected:
    BOOL OnInitDialog() override;
    void OnCancel() override;
    INT_PTR DialogProc(UINT msg, WPARAM wparam, LPARAM lparam) override;

private:
    void RequestCancel();

    int m_file_id;
    std::string m_file_name;
    size_t m_file_size_kb; // File size in KB
    std::atomic<unsigned> m_bytes_received{0};
    std::atomic<bool> m_cancel_requested{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_succeeded{false};
};

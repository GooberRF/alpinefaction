#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <xlog/xlog.h>
#include "crash_handler_stub/custom_exceptions.h"
#include "crash_handler_stub.h"
#include "crash_handler_stub/WatchDogTimer.h"

class WatchDogTimer::Impl
{
    static constexpr std::chrono::seconds check_interval{1};

    HANDLE m_observed_thread_handle;
    DWORD m_observed_thread_id;
    std::chrono::milliseconds m_timeout;
    std::chrono::time_point<std::chrono::steady_clock> m_last_reset_time;
    std::thread m_checker_thread;
    std::condition_variable m_cond_var;
    std::mutex m_mutex;
    std::atomic<bool> m_exiting;
    bool m_running = false;
    // Touched only by the checker thread: Alt kill-switch arming state, reset when no hang is active
    bool m_kill_armed = false;
    bool m_hang_reported = false;

public:
    Impl(std::chrono::milliseconds timeout) : m_timeout(timeout)
    {}

    void start()
    {
        if (m_running) {
            xlog::error("Trying to start a running watch-dog timer");
            return;
        }

        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_observed_thread_handle, 0,
            FALSE, DUPLICATE_SAME_ACCESS)) {
            xlog::warn("DuplicateHandle failed");
        }
        m_observed_thread_id = GetCurrentThreadId();

        m_last_reset_time = std::chrono::steady_clock::now();
        m_exiting = false;

        m_checker_thread = std::thread{&WatchDogTimer::Impl::checker_thread_proc, this};
        m_running = true;
        xlog::info("Watchdog timer started");
    }

    void stop()
    {
        if (!m_running) {
            xlog::error("Trying to stop a watch-dog timer that is not running");
            return;
        }
        m_exiting = true;
        m_cond_var.notify_all();
        m_checker_thread.join();
        m_running = false;
        xlog::info("Watchdog timer stopped");
    }

    void restart()
    {
        m_last_reset_time = std::chrono::steady_clock::now();
    }

    [[nodiscard]] bool is_running() const
    {
        return m_running;
    }

private:
    void checker_thread_proc()
    {
        while (!m_exiting) {
            if (check_for_time_out()) {
                handle_time_out();
            }
            else {
                // No hang in progress (or it recovered) - the next hang requires a fresh
                // Alt press before it can be killed
                m_kill_armed = false;
                m_hang_reported = false;
            }
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cond_var.wait_for(lk, check_interval);
        }
    }

    bool check_for_time_out()
    {
        auto now = std::chrono::steady_clock::now();
        auto duration = now - m_last_reset_time;
        return duration >= m_timeout;
    }

    void handle_time_out()
    {
        // Only treat Alt as a kill request if it was pressed after the hang began: Alt can be
        // held as a regular game binding, so an Alt that was already down when the game stopped
        // responding must be observed released once before it arms the kill switch.
        bool alt_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (!alt_down) {
            m_kill_armed = true;
        }
        if (alt_down && m_kill_armed) {
            xlog::info("Crash of not responding process has been requested");
            crash_observed_thread();
        }
        else if (!m_hang_reported) {
            xlog::info("Process is not responding! Release the Alt key if it is held, then hold it for a few seconds to kill the process and generate a crash report that will help in debugging the problem...");
            m_hang_reported = true;
        }
    }

    void crash_observed_thread()
    {
        SuspendThread(m_observed_thread_handle);
        CONTEXT ctx;
        ZeroMemory(&ctx, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(m_observed_thread_handle, &ctx)) {
            xlog::warn("GetThreadContext failed");
        }

        // Simulate exception
        EXCEPTION_POINTERS exc_ptrs;
        EXCEPTION_RECORD exc_rec;
        exc_ptrs.ContextRecord = &ctx;
        exc_ptrs.ExceptionRecord = &exc_rec;
        exc_rec.ExceptionCode = custom_exceptions::unresponsive;
        exc_rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        exc_rec.ExceptionRecord = nullptr;
        exc_rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Eip);
        exc_rec.NumberParameters = 0;
        CrashHandlerStubProcessException(&exc_ptrs, m_observed_thread_id);
        ExitProcess(0);
    }
};

WatchDogTimer::WatchDogTimer(unsigned timeout_ms) : m_impl(new WatchDogTimer::Impl(std::chrono::milliseconds{timeout_ms}))
{
}

WatchDogTimer::~WatchDogTimer() 
{
    if (is_running()) {
        stop();
    }
}

void WatchDogTimer::start()
{
    m_impl->start();
}

void WatchDogTimer::stop()
{
    m_impl->stop();
}

void WatchDogTimer::restart()
{
    m_impl->restart();
}

bool WatchDogTimer::is_running()
{
    return m_impl->is_running();
}

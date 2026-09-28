#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <float.h>
#include <xmmintrin.h>
#include "work_pool.h"

namespace
{

// Caller included. Worker stacks are never freed, so this also bounds their reservations.
constexpr unsigned max_threads = 16;

class WorkPool
{
public:
    // Never destroyed: joinable std::threads in a static object abort RED's exit (0xC0000409).
    static WorkPool& get()
    {
        static WorkPool* pool = new WorkPool;
        return *pool;
    }

    void run(int count, const std::function<void(int)>& fn)
    {
        if (count <= 0) {
            return;
        }
        if (threads_.empty() || count == 1 || busy_.exchange(true)) {
            for (int i = 0; i < count; i++) {
                fn(i);
            }
            return;
        }
        {
            std::lock_guard lock{mutex_};
            job_ = &fn;
            count_ = count;
            // Workers take the caller's x87 precision/rounding and SSE state, so no item's result depends
            // on its thread.
            fp_control_ = _control87(0, 0);
            fp_csr_ = _mm_getcsr();
            next_.store(0);
            error_ = nullptr;
            gen_++;
        }
        wake_.notify_all();
        drain(fn, count);
        std::exception_ptr error;
        {
            std::unique_lock lock{mutex_};
            // no worker may still be inside this job when the next one resets the counter
            job_ = nullptr;
            done_.wait(lock, [this] { return active_ == 0; });
            error = std::exchange(error_, nullptr);
        }
        busy_.store(false);
        if (error) {
            std::rethrow_exception(error);
        }
    }

private:
    WorkPool()
    {
        const unsigned n = std::clamp(std::thread::hardware_concurrency(), 1u, max_threads);
        for (unsigned i = 1; i < n; i++) {
            try {
                threads_.emplace_back([this] { worker(); });
            }
            catch (...) {
                break;
            }
        }
    }

    void drain(const std::function<void(int)>& fn, int count)
    {
        for (;;) {
            const int i = next_.fetch_add(1);
            if (i >= count) {
                return;
            }
            try {
                fn(i);
            }
            catch (...) {
                std::lock_guard lock{mutex_};
                if (!error_) {
                    error_ = std::current_exception();
                }
            }
        }
    }

    void worker()
    {
        unsigned seen = 0;
        for (;;) {
            const std::function<void(int)>* fn;
            int count;
            unsigned fp_control;
            unsigned fp_csr;
            {
                std::unique_lock lock{mutex_};
                wake_.wait(lock, [&] { return gen_ != seen; });
                seen = gen_;
                if (!job_) {
                    continue;
                }
                fn = job_;
                count = count_;
                fp_control = fp_control_;
                fp_csr = fp_csr_;
                active_++;
            }
            _control87(fp_control, _MCW_PC | _MCW_RC);
            _mm_setcsr(fp_csr);
            drain(*fn, count);
            {
                std::lock_guard lock{mutex_};
                active_--;
            }
            done_.notify_all();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(int)>* job_ = nullptr;
    int count_ = 0;
    unsigned fp_control_ = 0;
    unsigned fp_csr_ = 0;
    unsigned gen_ = 0;
    int active_ = 0;
    std::atomic<int> next_{0};
    std::atomic<bool> busy_{false};
    std::exception_ptr error_;
};

} // namespace

void work_pool_run(int count, const std::function<void(int)>& fn)
{
    WorkPool::get().run(count, fn);
}

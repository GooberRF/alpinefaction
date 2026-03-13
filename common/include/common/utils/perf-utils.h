#pragma once

#include <memory>
#include <string>
#include <vector>
#include <chrono>

struct PerfAggregator
{
    std::string name_;
    unsigned num_calls_ = 0;
    int64_t total_duration_us_ = 0;
    static std::vector<std::unique_ptr<PerfAggregator>> instances_;

    PerfAggregator(std::string&& name) : name_(name)
    {}

public:
    static PerfAggregator& create(std::string&& name)
    {
        instances_.push_back(std::make_unique<PerfAggregator>(std::move(name)));
        return *instances_.back().get();
    }

    [[nodiscard]] static const std::vector<std::unique_ptr<PerfAggregator>>& get_instances()
    {
        return instances_;
    }

    void add_call(const int64_t duration)
    {
        ++num_calls_;
        total_duration_us_ += duration;
    }

    [[nodiscard]] const std::string& get_name() const
    {
        return name_;
    }

    [[nodiscard]] unsigned get_calls() const
    {
        return num_calls_;
    }

    [[nodiscard]] int64_t get_total_duration_us() const
    {
        return total_duration_us_;
    }

    [[nodiscard]] int64_t get_avg_duration_us() const
    {
        return total_duration_us_ / num_calls_;
    }
};

class ScopedPerfMonitor
{
    PerfAggregator& agg_;
    std::chrono::steady_clock::time_point start_ =
        std::chrono::steady_clock::now();

public:
    ScopedPerfMonitor(PerfAggregator& agg) : agg_(agg) {}

    ~ScopedPerfMonitor()
    {
        const std::chrono::microseconds duration_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_
            );
        agg_.add_call(duration_us.count());
    }
};

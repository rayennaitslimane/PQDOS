#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct BenchmarkParams {
    uint32_t k = 0;
    uint32_t m = 0;
    std::size_t shard_size = 0;
    std::size_t object_size = 0;
    uint32_t failed_nodes = 0;
    uint32_t total_nodes = 0;
};

struct PhaseTiming {
    double crypto_ms = 0.0;
    double transport_ms = 0.0;
    double total_ms = 0.0;
};

struct OperationRecord {
    std::string operation;
    BenchmarkParams params;
    PhaseTiming timing;
    bool success = true;
    double storage_overhead = 0.0;
};

struct ScopedTimer {
    double* target;
    std::chrono::steady_clock::time_point start;

    explicit ScopedTimer(double* t)
        : target(t), start(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        auto elapsed = std::chrono::steady_clock::now() - start;
        *target = std::chrono::duration<double, std::milli>(elapsed).count();
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
};

class MetricsCollector {
public:
    void record(OperationRecord rec);
    void export_csv(const std::string& path, bool append = false) const;
    const std::vector<OperationRecord>& records() const { return records_; }

private:
    std::vector<OperationRecord> records_;
};

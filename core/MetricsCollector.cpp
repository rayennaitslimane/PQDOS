#include "MetricsCollector.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

void MetricsCollector::record(OperationRecord rec) {
    records_.push_back(std::move(rec));
}

void MetricsCollector::export_csv(const std::string& path, bool append) const {
    namespace fs = std::filesystem;
    const bool write_header =
        !append || !fs::exists(path) || fs::file_size(path) == 0;

    const std::ios::openmode mode =
        std::ios::out | (append ? std::ios::app : std::ios::trunc);

    std::ofstream file(path, mode);
    if (!file.is_open()) {
        throw std::runtime_error("MetricsCollector::export_csv: cannot open: " + path);
    }

    if (write_header) {
        file << "operation,k,m,shard_size,object_size,failed_nodes,total_nodes,"
                "crypto_ms,transport_ms,total_ms,success,storage_overhead\n";
    }

    for (const auto& r : records_) {
        file << r.operation << ","
             << r.params.k << ","
             << r.params.m << ","
             << r.params.shard_size << ","
             << r.params.object_size << ","
             << r.params.failed_nodes << ","
             << r.params.total_nodes << ","
             << r.timing.crypto_ms << ","
             << r.timing.transport_ms << ","
             << r.timing.total_ms << ","
             << (r.success ? 1 : 0) << ","
             << r.storage_overhead << "\n";
    }
}



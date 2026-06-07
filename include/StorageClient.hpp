#pragma once

#include "MetadataStore.hpp"
#include "Models.hpp"
#include "StorageNode.hpp"

#include <memory>
#include <string>
#include <unordered_map>

class StorageClient {
public:
    explicit StorageClient(const std::string& metadata_conn_str);

    void init();
    void put(const std::string& object_id, const Bytes& bytes);
    Bytes get(const std::string& object_id);
    bool remove(const std::string& object_id);
    std::vector<ObjectMetadata> list();

    StorageClient(const StorageClient&) = delete;
    StorageClient& operator=(const StorageClient&) = delete;

    StorageClient(StorageClient&&) = delete;
    StorageClient& operator=(StorageClient&&) = delete;

private:
    MetadataStore metadata_store_;
    std::unordered_map<std::string, std::unique_ptr<StorageNode>> nodes_;
};

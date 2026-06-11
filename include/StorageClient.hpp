#pragma once

#include "MetadataStore.hpp"
#include "Models.hpp"

#include <botan/pk_keys.h>

#include <array>
#include <memory>
#include <string>
#include <unordered_map>

class StorageClient {
public:
    explicit StorageClient(const std::string& metadata_conn_str);
    ~StorageClient();

    void init();
    void put(const std::string& object_id, const Bytes& bytes);
    Bytes get(const std::string& object_id);
    bool remove(const std::string& object_id);
    std::vector<ObjectMetadata> list();
    void rotate();

    StorageClient(const StorageClient&) = delete;
    StorageClient& operator=(const StorageClient&) = delete;

    StorageClient(StorageClient&&) = delete;
    StorageClient& operator=(StorageClient&&) = delete;

private:
    MetadataStore metadata_store_;
    std::unordered_map<std::string, std::unique_ptr<Botan::Private_Key>> kek_ring_;
    std::string active_kek_id_;
    std::string kek_file_path_;
};

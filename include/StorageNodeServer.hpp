#pragma once

#include "StorageNode.hpp"

#include <httplib.h>

#include <string>

class StorageNodeServer {
public:
    StorageNodeServer(StorageNode& node, const std::string& host, int port);

    ~StorageNodeServer();

    StorageNodeServer(const StorageNodeServer&) = delete;
    StorageNodeServer& operator=(const StorageNodeServer&) = delete;

    StorageNodeServer(StorageNodeServer&&) = delete;
    StorageNodeServer& operator=(StorageNodeServer&&) = delete;

    void start();

    void stop();

private:
    StorageNode& node_;
    std::string host_;
    int port_;
    httplib::Server server_;

    void setup_routes();
};

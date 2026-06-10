#pragma once

#include "StorageClient.hpp"

#include <httplib.h>

#include <string>

class StorageClientServer {
public:
    StorageClientServer(StorageClient& client, const std::string& host, int port);

    ~StorageClientServer();

    StorageClientServer(const StorageClientServer&) = delete;
    StorageClientServer& operator=(const StorageClientServer&) = delete;

    StorageClientServer(StorageClientServer&&) = delete;
    StorageClientServer& operator=(StorageClientServer&&) = delete;

    void start();

    void stop();

private:
    StorageClient& client_;
    std::string host_;
    int port_;
    httplib::Server server_;

    void setup_routes();
};

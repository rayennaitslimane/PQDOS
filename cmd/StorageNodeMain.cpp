#include "StorageNode.hpp"
#include "StorageNodeServer.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: storage_node <host> <port> <data_path>\n";
        return 1;
    }

    const std::string host = argv[1];
    const int port = std::atoi(argv[2]);
    const char* data_path = argv[3];

    if (port <= 0 || port > 65535) {
        std::cerr << "Error: invalid port number\n";
        return 1;
    }

    StorageNode node(data_path);
    StorageNodeServer server(node, host, port);

    std::cout << "Storage node listening on " << host << ":" << port << "\n";

    server.start();

    return 0;
}

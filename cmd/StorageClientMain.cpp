#include "StorageClient.hpp"
#include "StorageClientServer.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: storage_client <host> <port> <metadata_conn_str>\n";
        return 1;
    }

    const std::string host = argv[1];
    const int port = std::atoi(argv[2]);
    const std::string metadata_conn_str = argv[3];

    if (port <= 0 || port > 65535) {
        std::cerr << "Error: invalid port number\n";
        return 1;
    }

    StorageClient client(metadata_conn_str);
    client.init();

    StorageClientServer server(client, host, port);

    std::cout << "Storage client server listening on " << host << ":" << port << "\n";

    server.start();

    return 0;
}

#include "ShardTransport.hpp"

#include <httplib.h>

#include <cstdlib>
#include <future>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

std::array<std::string, kNumNodes> parseNodeAddresses() {
    const char* env = std::getenv("NODE_ADDRESSES");
    if (!env) {
        return {"localhost:9001", "localhost:9002", "localhost:9003"};
    }
    std::array<std::string, kNumNodes> addresses;
    std::istringstream stream(env);
    std::string token;
    std::size_t i = 0;
    while (std::getline(stream, token, ',') && i < kNumNodes) {
        addresses[i++] = token;
    }
    if (i != kNumNodes) {
        throw std::runtime_error(
            "NODE_ADDRESSES must contain exactly 3 comma-separated addresses"
        );
    }
    return addresses;
}

} // namespace

const std::array<std::string, kNumNodes>& node_addresses() {
    static const std::array<std::string, kNumNodes> addresses = parseNodeAddresses();
    return addresses;
}

std::pair<std::string, int> parse_address(const std::string& address) {
    const auto pos = address.rfind(':');
    if (pos == std::string::npos) {
        throw std::runtime_error("parse_address: invalid address: " + address);
    }

    return {address.substr(0, pos), std::stoi(address.substr(pos + 1))};
}

std::string make_shard_location(const std::string& object_id, const std::string& version, uint32_t shard_index) {
    return object_id + "-" + version + "-" + std::to_string(shard_index);
}

// MVP placement strategy:
// - one shard per node
// - shard i goes to node i
PlacementMap placement_strategy(
    const std::string& object_id,
    const std::string& version,
    const std::vector<Bytes>& serialized_shards
) {
    const auto& addresses = node_addresses();
    const std::size_t total_shards = serialized_shards.size();

    if (total_shards > addresses.size()) {
        throw std::runtime_error(
            "placement_strategy: not enough nodes for number of shards"
        );
    }

    PlacementMap placement;

    for (std::size_t i = 0; i < total_shards; ++i) {
        const std::string& node_address = addresses[i];
        const std::string location = make_shard_location(
            object_id,
            version,
            static_cast<uint32_t>(i)
        );

        placement[node_address].push_back({location, serialized_shards[i]});
    }

    return placement;
}

void apply_placement(const PlacementMap& placement) {
    std::vector<std::future<void>> futures;

    for (const auto& [node_address, entries] : placement) {
        futures.push_back(std::async(std::launch::async,
            [&node_address, &entries]() {
                auto [host, port] = parse_address(node_address);
                httplib::Client client(host, port);

                for (const auto& [location, payload] : entries) {
                    auto res = client.Put(
                        "/shards/" + location,
                        reinterpret_cast<const char*>(payload.data()),
                        payload.size(),
                        "application/octet-stream"
                    );

                    if (!res || res->status != 200) {
                        throw std::runtime_error(
                            "apply_placement: failed to PUT shard to " +
                            node_address + "/shards/" + location
                        );
                    }
                }
            }
        ));
    }

    for (auto& f : futures) {
        f.get();
    }
}

// Build metadata.shard_locations as an ordered vector of "host:port/location".
// The index in the vector equals the shard index.
std::vector<std::string> build_shard_locations(const PlacementMap& placement) {
    std::size_t total_shards = 0;
    for (const auto& [node_address, entries] : placement) {
        total_shards += entries.size();
    }

    std::vector<std::string> shard_locations(total_shards);

    for (const auto& [node_address, entries] : placement) {
        for (const auto& [location, payload] : entries) {
            const auto pos = location.rfind('-');
            if (pos == std::string::npos) {
                throw std::runtime_error("build_shard_locations: invalid location");
            }

            const std::size_t shard_index = std::stoul(location.substr(pos + 1));
            shard_locations.at(shard_index) = node_address + "/" + location;
        }
    }

    return shard_locations;
}

ShardLocationMap parse_shard_locations(const std::vector<std::string>& shard_locations) {
    ShardLocationMap result;

    for (const auto& shard_location : shard_locations) {
        const auto pos = shard_location.find('/');
        if (pos == std::string::npos) {
            throw std::runtime_error("parse_shard_locations: invalid shard location");
        }

        const std::string node_address = shard_location.substr(0, pos);
        const std::string location = shard_location.substr(pos + 1);

        result[node_address].push_back(location);
    }

    return result;
}

std::vector<EncryptedShard> fetch_encrypted_shards(
    const ShardLocationMap& shard_location_map
) {
    std::vector<std::future<std::vector<EncryptedShard>>> futures;

    for (const auto& [node_address, locations] : shard_location_map) {
        futures.push_back(std::async(std::launch::async,
            [&node_address, &locations]() -> std::vector<EncryptedShard> {
                std::vector<EncryptedShard> shards;

                std::pair<std::string, int> addr;
                try {
                    addr = parse_address(node_address);
                } catch (...) {
                    return shards;
                }

                httplib::Client client(addr.first, addr.second);

                for (const auto& location : locations) {
                    try {
                        auto res = client.Get("/shards/" + location);

                        if (!res || res->status != 200) {
                            continue; // missing shard
                        }

                        Bytes payload(res->body.begin(), res->body.end());

                        try {
                            shards.push_back(
                                EncryptedShard::deserialize(payload)
                            );
                        } catch (...) {
                            // malformed/corrupt serialized shard -> ignore
                            continue;
                        }
                    } catch (...) {
                        // node unavailable
                        continue;
                    }
                }

                return shards;
            }
        ));
    }

    std::vector<EncryptedShard> encrypted_shards;
    for (auto& f : futures) {
        auto shards = f.get();
        encrypted_shards.insert(
            encrypted_shards.end(),
            std::make_move_iterator(shards.begin()),
            std::make_move_iterator(shards.end())
        );
    }

    return encrypted_shards;
}

void remove_from_nodes(const ShardLocationMap& shard_location_map) {
    std::vector<std::future<void>> futures;

    for (const auto& [node_address, locations] : shard_location_map) {
        futures.push_back(std::async(std::launch::async,
            [&node_address, &locations]() {
                std::pair<std::string, int> addr;
                try {
                    addr = parse_address(node_address);
                } catch (...) {
                    return;
                }

                httplib::Client client(addr.first, addr.second);

                for (const auto& location : locations) {
                    try {
                        (void)client.Delete("/shards/" + location);
                    } catch (...) {
                        // Ignore missing node / already-deleted shard / backend failure
                        continue;
                    }
                }
            }
        ));
    }

    for (auto& f : futures) {
        try {
            f.get();
        } catch (...) {
            // best-effort deletion
        }
    }
}

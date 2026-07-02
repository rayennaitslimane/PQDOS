#include "ShardTransport.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <future>
#include <limits>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

std::pair<std::string, int> parse_address(const std::string& address) {
    const auto pos = address.rfind(':');
    if (pos == std::string::npos) {
        throw std::runtime_error("parse_address: invalid address: " + address);
    }

    return {address.substr(0, pos), std::stoi(address.substr(pos + 1))};
}

httplib::Client& get_node_client(const std::string& node_address) {
    thread_local std::unordered_map<
        std::string,
        std::unique_ptr<httplib::Client>
    > clients;

    auto it = clients.find(node_address);

    if (it == clients.end()) {
        auto [host, port] = parse_address(node_address);

        auto client = std::make_unique<httplib::Client>(host, port);
        client->set_connection_timeout(5, 0);
        client->set_read_timeout(10, 0);

        auto [inserted_it, _] = clients.emplace(
            node_address,
            std::move(client)
        );

        return *inserted_it->second;
    }

    return *it->second;
}

std::string make_shard_key(const std::string& object_id, const std::string& version, uint32_t shard_index) {
    if (object_id.empty() || version.empty()) {
        throw std::invalid_argument("make_shard_key: object_id and version must be non-empty");
    }

    if (object_id.find('/') != std::string::npos || version.find('/') != std::string::npos) {
        throw std::invalid_argument("make_shard_key: object_id/version must not contain '/'");
    }

    return object_id + "/" + version + "/" + std::to_string(shard_index);
}

ParsedShardKey parse_shard_key(const std::string& location) {
    const auto first = location.find('/');
    const auto second = (first == std::string::npos) ? std::string::npos : location.find('/', first + 1);

    if (first == std::string::npos || second == std::string::npos ||
        location.find('/', second + 1) != std::string::npos) {
        throw std::runtime_error("parse_shard_key: invalid key format");
    }

    ParsedShardKey parsed;
    parsed.object_id = location.substr(0, first);
    parsed.version = location.substr(first + 1, second - first - 1);

    if (parsed.object_id.empty() || parsed.version.empty() || second + 1 >= location.size()) {
        throw std::runtime_error("parse_shard_key: invalid key format");
    }

    try {
        const std::size_t idx = std::stoul(location.substr(second + 1));
        if (idx > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error("parse_shard_key: shard index out of range");
        }
        parsed.shard_index = static_cast<uint32_t>(idx);
    } catch (const std::exception&) {
        throw std::runtime_error("parse_shard_key: invalid shard index");
    }

    return parsed;
}

PlacementMap placement_strategy(
    const std::string& object_id,
    const std::string& version,
    const std::vector<Bytes>& serialized_shards,
    const std::vector<std::string>& eligible_nodes
) {
    const std::size_t total_shards = serialized_shards.size();

    if (total_shards > eligible_nodes.size()) {
        throw std::runtime_error(
            "placement_strategy: not enough eligible nodes for number of shards"
        );
    }

    PlacementMap placement;

    for (std::size_t i = 0; i < total_shards; ++i) {
        const std::string& node_address = eligible_nodes[i];
        const std::string location = make_shard_key(
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
            [node_address, &entries]() {
                auto& client = get_node_client(node_address);

                for (const auto& [location, payload] : entries) {
                    auto res = client.Put(
                        "/shards?location=" + location,
                        reinterpret_cast<const char*>(payload.data()),
                        payload.size(),
                        "application/octet-stream"
                    );

                    if (!res || res->status != 200) {
                        throw std::runtime_error(
                            "apply_placement: failed to PUT shard to " +
                            node_address + "/shards?location=" + location
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

std::vector<std::string> build_shard_locations(const PlacementMap& placement) {
    std::size_t total_shards = 0;
    for (const auto& [node_address, entries] : placement) {
        total_shards += entries.size();
    }

    std::vector<std::string> shard_locations(total_shards);

    for (const auto& [node_address, entries] : placement) {
        for (const auto& [location, payload] : entries) {
            const ParsedShardKey parsed = parse_shard_key(location);
            shard_locations.at(parsed.shard_index) = node_address + "/" + location;
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

        // Invariant: first slash splits node address from shard key. Remaining
        // slashes belong to the shard key format object_id/version/shard_index.
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
            [node_address, &locations]() -> std::vector<EncryptedShard> {
                std::vector<EncryptedShard> shards;

                httplib::Client* client_ptr;
                try {
                    client_ptr = &get_node_client(node_address);
                } catch (...) {
                    return shards;
                }
                auto& client = *client_ptr;

                for (const auto& location : locations) {
                    try {
                        auto res = client.Get("/shards?location=" + location);

                        if (!res || res->status != 200) {
                            continue;
                        }

                        Bytes payload(res->body.begin(), res->body.end());

                        try {
                            // Stored payloads are self-describing StoredShard
                            // envelopes (ADR-0008); decode_shard also tolerates
                            // legacy bare EncryptedShard payloads. Only the
                            // encrypted shard is needed on the read path.
                            shards.push_back(
                                StoredShard::decode_shard(payload).first
                            );
                        } catch (...) {
                            continue;
                        }
                    } catch (...) {
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

std::vector<std::string> list_node_shards(const std::string& node_address) {
    std::vector<std::string> locations;

    try {
        auto& client = get_node_client(node_address);

        auto res = client.Get("/shards/list");
        if (!res || res->status != 200) {
            return locations;
        }

        nlohmann::json parsed = nlohmann::json::parse(res->body, nullptr, false);
        if (!parsed.is_array()) {
            return locations;
        }

        locations.reserve(parsed.size());
        for (const auto& entry : parsed) {
            if (entry.is_string()) {
                locations.push_back(entry.get<std::string>());
            }
        }
    } catch (...) {
        locations.clear();
    }

    return locations;
}

std::optional<ShardManifest> fetch_shard_manifest(
    const std::string& node_address,
    const std::string& location
) {
    try {
        auto& client = get_node_client(node_address);

        auto res = client.Get("/shards?location=" + location);
        if (!res || res->status != 200) {
            return std::nullopt;
        }

        Bytes payload(res->body.begin(), res->body.end());
        return StoredShard::decode_shard(payload).second;
    } catch (...) {
        return std::nullopt;
    }
}

void remove_from_nodes(const ShardLocationMap& shard_location_map) {
    std::vector<std::future<void>> futures;

    for (const auto& [node_address, locations] : shard_location_map) {
        futures.push_back(std::async(std::launch::async,
            [node_address, &locations]() {
                httplib::Client* client_ptr;
                try {
                    client_ptr = &get_node_client(node_address);
                } catch (...) {
                    return;
                }
                auto& client = *client_ptr;

                for (const auto& location : locations) {
                    try {
                        (void)client.Delete("/shards?location=" + location);
                    } catch (...) {
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

bool probe_shard(const std::string& node_address, const std::string& location) {
    try {
        auto& client = get_node_client(node_address);

        auto res = client.Get("/shards?location=" + location);
        return res && res->status == 200;
    } catch (...) {
        return false;
    }
}

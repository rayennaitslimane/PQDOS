#include "StorageNode.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

void StorageNode::check_lmdb(int rc, const char* operation) {
    if (rc != 0) {
        throw std::runtime_error(
            std::string(operation) + " failed: " + mdb_strerror(rc)
        );
    }
}

Bytes StorageNode::bytes_from_mdb_value(const MDB_val& value) {
    if (value.mv_size == 0) {
        return Bytes{};
    }

    const auto* data =
        static_cast<const std::uint8_t*>(value.mv_data);

    return Bytes(data, data + value.mv_size);
}

MDB_dbi StorageNode::open_database(MDB_txn* txn) {
    MDB_dbi dbi{};

    const int rc = mdb_dbi_open(txn, nullptr, 0, &dbi);

    if (rc != 0) {
        mdb_txn_abort(txn);
        check_lmdb(rc, "mdb_dbi_open");
    }

    return dbi;
}

StorageNode::StorageNode(const char* path) {
    check_lmdb(mdb_env_create(&env_), "mdb_env_create");

    try {
        check_lmdb(
            mdb_env_set_mapsize(env_, 1UL * 1024 * 1024 * 1024),
            "mdb_env_set_mapsize"
        );

        check_lmdb(
            mdb_env_open(env_, path, 0, 0664),
            "mdb_env_open"
        );
    } catch (...) {
        if (env_ != nullptr) {
            mdb_env_close(env_);
            env_ = nullptr;
        }

        throw;
    }
}

StorageNode::~StorageNode() {
    if (env_ != nullptr) {
        mdb_env_close(env_);
        env_ = nullptr;
    }
}

void StorageNode::put(
    const std::vector<std::string>& locations,
    const std::vector<Bytes>& payloads
) {
    if (locations.size() != payloads.size()) {
        throw std::invalid_argument(
            "StorageNode::put: locations and payloads must have the same size"
        );
    }

    MDB_txn* txn = nullptr;

    check_lmdb(
        mdb_txn_begin(env_, nullptr, 0, &txn),
        "mdb_txn_begin"
    );

    MDB_dbi dbi = open_database(txn);

    for (std::size_t i = 0; i < locations.size(); ++i) {
        const std::string& location = locations[i];
        const Bytes& payload = payloads[i];

        if (location.empty()) {
            mdb_txn_abort(txn);
            throw std::invalid_argument(
                "StorageNode::put: shard location must not be empty"
            );
        }

        MDB_val key{
            location.size(),
            const_cast<char*>(location.data())
        };

        MDB_val value{
            payload.size(),
            payload.empty()
                ? nullptr
                : static_cast<void*>(
                    const_cast<std::uint8_t*>(payload.data())
                )
        };

        const int rc = mdb_put(txn, dbi, &key, &value, 0);

        if (rc != 0) {
            mdb_txn_abort(txn);
            check_lmdb(rc, "mdb_put");
        }
    }

    check_lmdb(
        mdb_txn_commit(txn),
        "mdb_txn_commit"
    );
}

std::vector<std::optional<Bytes>> StorageNode::get(
    const std::vector<std::string>& locations
) {
    MDB_txn* txn = nullptr;

    check_lmdb(
        mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn),
        "mdb_txn_begin"
    );

    MDB_dbi dbi = open_database(txn);

    std::vector<std::optional<Bytes>> results;
    results.reserve(locations.size());

    for (const std::string& location : locations) {
        if (location.empty()) {
            mdb_txn_abort(txn);
            throw std::invalid_argument(
                "StorageNode::get: shard location must not be empty"
            );
        }

        MDB_val key{
            location.size(),
            const_cast<char*>(location.data())
        };

        MDB_val value{};

        const int rc = mdb_get(txn, dbi, &key, &value);

        if (rc == 0) {
            results.emplace_back(bytes_from_mdb_value(value));
        } else if (rc == MDB_NOTFOUND) {
            results.emplace_back(std::nullopt);
        } else {
            mdb_txn_abort(txn);
            check_lmdb(rc, "mdb_get");
        }
    }

    mdb_txn_abort(txn);
    return results;
}

std::vector<std::optional<Bytes>> StorageNode::remove(
    const std::vector<std::string>& locations
) {
    MDB_txn* txn = nullptr;

    check_lmdb(
        mdb_txn_begin(env_, nullptr, 0, &txn),
        "mdb_txn_begin"
    );

    MDB_dbi dbi = open_database(txn);

    std::vector<std::optional<Bytes>> removed_payloads;
    removed_payloads.reserve(locations.size());

    for (const std::string& location : locations) {
        if (location.empty()) {
            mdb_txn_abort(txn);
            throw std::invalid_argument(
                "StorageNode::remove: shard location must not be empty"
            );
        }

        MDB_val key{
            location.size(),
            const_cast<char*>(location.data())
        };

        MDB_val value{};

        int rc = mdb_get(txn, dbi, &key, &value);

        if (rc == 0) {
            removed_payloads.emplace_back(bytes_from_mdb_value(value));

            rc = mdb_del(txn, dbi, &key, nullptr);

            if (rc != 0) {
                mdb_txn_abort(txn);
                check_lmdb(rc, "mdb_del");
            }
        } else if (rc == MDB_NOTFOUND) {
            removed_payloads.emplace_back(std::nullopt);
        } else {
            mdb_txn_abort(txn);
            check_lmdb(rc, "mdb_get");
        }
    }

    check_lmdb(
        mdb_txn_commit(txn),
        "mdb_txn_commit"
    );

    return removed_payloads;
}
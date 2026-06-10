#include "Models.hpp"
#include "MetadataStore.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

MetadataStore::MetadataStore(const std::string& conn_str)
    : conn_(conn_str)
{
    init_schema();
    prepare_statements();
}

void MetadataStore::init_schema() {
    pqxx::work tx{conn_};

    tx.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS object_metadata (
            id UUID PRIMARY KEY,
            size BIGINT NOT NULL CHECK (size >= 0),
            checksum TEXT NOT NULL,
            erasure_spec BYTEA NOT NULL,
            encrypted_dek BYTEA NOT NULL,
            kek_id TEXT NOT NULL
        );
    )SQL");

    tx.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS object_shard_locations (
            object_id UUID NOT NULL REFERENCES object_metadata(id) ON DELETE CASCADE,
            shard_index INTEGER NOT NULL CHECK (shard_index >= 0),
            location TEXT NOT NULL,
            PRIMARY KEY (object_id, shard_index)
        );
    )SQL");

    tx.exec(R"SQL(
        CREATE INDEX IF NOT EXISTS idx_object_shard_locations_object_id
        ON object_shard_locations(object_id);
    )SQL");

    tx.commit();
}

void MetadataStore::put(const ObjectMetadata& metadata) {
    validate_metadata(metadata);

    pqxx::work tx{conn_};

    const std::int64_t object_size = checked_size_to_i64(metadata.size);
    const Bytes erasure_bytes = metadata.erasure.serialize();

    if (erasure_bytes.empty()) {
        throw std::invalid_argument("ObjectMetadata.erasure serialized to an empty buffer");
    }

    pqxx::bytes erasure_blob;
    erasure_blob.reserve(erasure_bytes.size());

    for (const auto byte : erasure_bytes) {
        erasure_blob.push_back(static_cast<std::byte>(byte));
    }

    pqxx::bytes encrypted_dek_blob;
    encrypted_dek_blob.reserve(metadata.encrypted_dek.size());

    for (const auto byte : metadata.encrypted_dek) {
        encrypted_dek_blob.push_back(static_cast<std::byte>(byte));
    }

    tx.exec(
        pqxx::prepped{"put_object_metadata"},
        pqxx::params{
            metadata.id,
            object_size,
            metadata.checksum,
            erasure_blob,
            encrypted_dek_blob,
            metadata.kek_id
        }
    );

    // Replace shard locations atomically.
    tx.exec(
        pqxx::prepped{"delete_object_shard_locations"},
        pqxx::params{
            metadata.id
        }
    );

    for (std::size_t i = 0; i < metadata.shard_locations.size(); ++i) {
        if (i > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::overflow_error("Too many shard locations for INTEGER shard_index");
        }

        tx.exec(
            pqxx::prepped{"insert_object_shard_location"},
            pqxx::params{
                metadata.id,
                static_cast<std::int32_t>(i),
                metadata.shard_locations[i]
            }
        );
    }

    tx.commit();
}

std::optional<ObjectMetadata> MetadataStore::get(const std::string& id) {
    pqxx::read_transaction tx{conn_};

    pqxx::result metadata_rows = tx.exec(
        pqxx::prepped{"get_object_metadata"},
        pqxx::params{
            id
        }
    );

    if (metadata_rows.empty()) {
        return std::nullopt;
    }

    ObjectMetadata metadata = row_to_object_metadata(metadata_rows[0]);
    metadata.shard_locations = get_shard_locations(tx, id);

    return metadata;
}

bool MetadataStore::remove(const std::string& id) {
    pqxx::work tx{conn_};

    pqxx::result result = tx.exec(
        pqxx::prepped{"remove_object_metadata"},
        pqxx::params{
            id
        }
    );

    const bool deleted = result.affected_rows() > 0;

    tx.commit();

    return deleted;
}

std::vector<ObjectMetadata> MetadataStore::list() {
    pqxx::read_transaction tx{conn_};

    pqxx::result rows = tx.exec(
        pqxx::prepped{"list_object_metadata"},
        pqxx::params{}
    );

    std::vector<ObjectMetadata> objects;
    objects.reserve(rows.size());

    for (const auto& row : rows) {
        ObjectMetadata metadata = row_to_object_metadata(row);
        metadata.shard_locations = get_shard_locations(tx, metadata.id);
        objects.push_back(std::move(metadata));
    }

    return objects;
}

void MetadataStore::prepare_statements() {
    conn_.prepare(
        "put_object_metadata",
        R"SQL(
            INSERT INTO object_metadata (
                id,
                size,
                checksum,
                erasure_spec,
                encrypted_dek,
                kek_id
            )
            VALUES (
                $1::uuid,
                $2,
                $3,
                $4::bytea,
                $5::bytea,
                $6
            )
            ON CONFLICT (id) DO UPDATE
            SET
                size = EXCLUDED.size,
                checksum = EXCLUDED.checksum,
                erasure_spec = EXCLUDED.erasure_spec,
                encrypted_dek = EXCLUDED.encrypted_dek,
                kek_id = EXCLUDED.kek_id
        )SQL"
    );

    conn_.prepare(
        "delete_object_shard_locations",
        R"SQL(
            DELETE FROM object_shard_locations
            WHERE object_id = $1::uuid
        )SQL"
    );

    conn_.prepare(
        "insert_object_shard_location",
        R"SQL(
            INSERT INTO object_shard_locations (
                object_id,
                shard_index,
                location
            )
            VALUES (
                $1::uuid,
                $2,
                $3
            )
        )SQL"
    );

    conn_.prepare(
        "get_object_metadata",
        R"SQL(
            SELECT
                id::text,
                size,
                checksum,
                erasure_spec,
                encrypted_dek,
                kek_id
            FROM object_metadata
            WHERE id = $1::uuid
        )SQL"
    );

    conn_.prepare(
        "get_object_shard_locations",
        R"SQL(
            SELECT location
            FROM object_shard_locations
            WHERE object_id = $1::uuid
            ORDER BY shard_index ASC
        )SQL"
    );

    conn_.prepare(
        "remove_object_metadata",
        R"SQL(
            DELETE FROM object_metadata
            WHERE id = $1::uuid
        )SQL"
    );

    conn_.prepare(
        "list_object_metadata",
        R"SQL(
            SELECT
                id::text,
                size,
                checksum,
                erasure_spec,
                encrypted_dek,
                kek_id
            FROM object_metadata
            ORDER BY id ASC
        )SQL"
    );
}

ObjectMetadata MetadataStore::row_to_object_metadata(const pqxx::row_ref& row) {
    ObjectMetadata metadata;

    metadata.id = row["id"].c_str();

    const std::int64_t db_size = row["size"].as<std::int64_t>();
    if (db_size < 0) {
        throw std::runtime_error("Database returned negative object size");
    }

    metadata.size = static_cast<std::size_t>(db_size);
    metadata.checksum = row["checksum"].c_str();

    const pqxx::bytes erasure_blob = row["erasure_spec"].as<pqxx::bytes>();

    Bytes erasure_bytes;
    erasure_bytes.reserve(erasure_blob.size());

    for (const std::byte byte : erasure_blob) {
        erasure_bytes.push_back(static_cast<std::uint8_t>(byte));
    }

    metadata.erasure = ErasureSpec::deserialize(erasure_bytes);

    const pqxx::bytes encrypted_dek_blob = row["encrypted_dek"].as<pqxx::bytes>();

    metadata.encrypted_dek.reserve(encrypted_dek_blob.size());
    for (const std::byte byte : encrypted_dek_blob) {
        metadata.encrypted_dek.push_back(static_cast<std::uint8_t>(byte));
    }

    metadata.kek_id = row["kek_id"].c_str();

    return metadata;
}

std::vector<std::string> MetadataStore::get_shard_locations(
    pqxx::transaction_base& tx,
    const std::string& object_id
) {
    pqxx::result rows = tx.exec(
        pqxx::prepped{"get_object_shard_locations"},
        pqxx::params{
            object_id
        }
    );

    std::vector<std::string> locations;
    locations.reserve(rows.size());

    for (const auto& row : rows) {
        locations.push_back(row["location"].c_str());
    }

    return locations;
}

std::int64_t MetadataStore::checked_size_to_i64(std::size_t value) {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::overflow_error("ObjectMetadata.size exceeds PostgreSQL BIGINT range");
    }

    return static_cast<std::int64_t>(value);
}

void MetadataStore::validate_metadata(const ObjectMetadata& metadata) {
    if (metadata.id.empty()) {
        throw std::invalid_argument("ObjectMetadata.id must not be empty");
    }

    if (metadata.checksum.empty()) {
        throw std::invalid_argument("ObjectMetadata.checksum must not be empty");
    }

    for (const auto& location : metadata.shard_locations) {
        if (location.empty()) {
            throw std::invalid_argument("ObjectMetadata.shard_locations must not contain empty locations");
        }
    }
}
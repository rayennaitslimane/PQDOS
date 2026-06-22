#include "Models.hpp"
#include "ErasureCodec.hpp"

#include <isa-l/erasure_code.h>

#include <limits>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

std::vector<PlainShard> encode(const Bytes& data, const ErasureSpec& erasure_spec) {
    const uint32_t k = erasure_spec.data_shards;
    const uint32_t m = erasure_spec.parity_shards;
    const uint32_t n = k + m;

    if (k == 0) {
        throw std::invalid_argument("encode: data_shards must be greater than 0");
    }

    if (m == 0) {
        throw std::invalid_argument("encode: parity_shards must be greater than 0");
    }

    if (n > 255) {
        throw std::invalid_argument("encode: total shards must be <= 255 for ISA-L GF(2^8)");
    }

    const size_t shard_size = erasure_spec.shard_size;

    if (shard_size == 0) {
        throw std::invalid_argument("encode: shard_size must be greater than 0");
    }

    if (shard_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("encode: shard_size exceeds ISA-L int limit");
    }

    if (shard_size > std::numeric_limits<size_t>::max() / static_cast<size_t>(k)) {
        throw std::invalid_argument("encode: total data size overflow");
    }

    const size_t total_data_size = static_cast<size_t>(k) * shard_size;

    if (data.size() > total_data_size) {
        throw std::invalid_argument(
            "encode: data size exceeds data_shards * shard_size"
        );
    }

    std::vector<PlainShard> shards(n);

    for (uint32_t i = 0; i < n; ++i) {
        shards[i].index = i;
        shards[i].bytes.resize(shard_size, 0);
    }

    
    // Fill data shards.
    // If the final shard is incomplete, the remaining bytes stay zero-padded.
    for (uint32_t i = 0; i < k; ++i) {
        const size_t offset = static_cast<size_t>(i) * shard_size;
        const size_t remaining =
            offset < data.size() ? data.size() - offset : 0;

        const size_t copy_len = std::min(shard_size, remaining);

        if (copy_len > 0) {
            std::memcpy(
                shards[i].bytes.data(),
                data.data() + offset,
                copy_len
            );
        }
    }

    // Generate Reed-Solomon encoding matrix.
    // Matrix shape: n rows x k columns.
    // First k rows are identity rows for data shards.
    // Remaining m rows are parity rows.
    std::vector<uint8_t> encode_matrix(n * k);
    gf_gen_rs_matrix(encode_matrix.data(), n, k);

    // ISA-L needs precomputed multiplication tables for parity generation.
    // One 32-byte table per coefficient.
    std::vector<uint8_t> g_tbls(k * m * 32);

    ec_init_tables(
        k,
        m,
        encode_matrix.data() + k * k,
        g_tbls.data()
    );

    std::vector<uint8_t*> data_ptrs(k);
    std::vector<uint8_t*> parity_ptrs(m);

    for (uint32_t i = 0; i < k; ++i) {
        data_ptrs[i] = shards[i].bytes.data();
    }

    for (uint32_t i = 0; i < m; ++i) {
        parity_ptrs[i] = shards[k + i].bytes.data();
    }

    ec_encode_data(
        static_cast<int>(shard_size),
        static_cast<int>(k),
        static_cast<int>(m),
        g_tbls.data(),
        data_ptrs.data(),
        parity_ptrs.data()
    );

    return shards;
}

Bytes decode(const std::vector<PlainShard>& shards, const ErasureSpec& erasure_spec, size_t original_size) {
    const uint32_t k = erasure_spec.data_shards;
    const uint32_t m = erasure_spec.parity_shards;
    const uint32_t n = k + m;
    const size_t shard_size = erasure_spec.shard_size;

    if (k == 0) {
        throw std::invalid_argument("decode: data_shards must be greater than 0");
    }

    if (m == 0) {
        throw std::invalid_argument("decode: parity_shards must be greater than 0");
    }

    if (n > 255) {
        throw std::invalid_argument("decode: total shards must be <= 255 for ISA-L GF(2^8)");
    }

    if (shard_size == 0) {
        throw std::invalid_argument("decode: shard_size must be greater than 0");
    }

    if (shard_size > Bytes{}.max_size()) {
        throw std::invalid_argument("decode: shard_size exceeds maximum vector size");
    }

    if (shard_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("decode: shard_size exceeds ISA-L int limit");
    }

    if (shards.size() < k) {
        throw std::runtime_error("decode: at least data_shards shards are required");
    }

    if (shard_size > std::numeric_limits<size_t>::max() / static_cast<size_t>(k)) {
        throw std::invalid_argument("decode: total decoded size overflow");
    }

    const size_t total_decoded_size = static_cast<size_t>(k) * shard_size;

    if (original_size > total_decoded_size) {
        throw std::invalid_argument("decode: original_size exceeds decoded data capacity");
    }

    // Store available shards by original index.
    // std::vector<bool> present(n, false);
    // std::vector<Bytes> shard_bytes(n);

    std::vector<bool> present(n, false);
    std::vector<uint8_t*> shard_ptrs(n, nullptr);

    for (const PlainShard& shard : shards) {
        if (shard.index >= n) {
            throw std::invalid_argument("decode: shard index out of range");
        }

        if (shard.bytes.size() != shard_size) {
            throw std::invalid_argument("decode: shard has invalid size");
        }

        // Preserve existing behavior: first copy wins, duplicates are ignored.
        if (!present[shard.index]) {
            present[shard.index] = true;
            shard_ptrs[shard.index] = const_cast<uint8_t*>(shard.bytes.data());
        }
    }

    // Fast path: all data shards are already present, no matrix work needed.
    bool all_data_present = true;
    for (uint32_t i = 0; i < k; ++i) {
        if (!present[i]) {
            all_data_present = false;
            break;
        }
    }

    if (all_data_present) {
        Bytes decoded;
        decoded.resize(total_decoded_size);

        for (uint32_t i = 0; i < k; ++i) {
            std::memcpy(
                decoded.data() + static_cast<size_t>(i) * shard_size,
                shard_ptrs[i],
                shard_size
            );
        }

        decoded.resize(original_size);
        return decoded;
    }

    // Select any k available shards.
    std::vector<uint32_t> decode_index;
    decode_index.reserve(k);

    // First take data shards (0 .. k-1)
    for (uint32_t i = 0; i < k && decode_index.size() < k; ++i) {
        if (present[i]) {
            decode_index.push_back(i);
        }
    }

    // Then take parity shards (k .. n-1) if needed
    for (uint32_t i = k; i < n && decode_index.size() < k; ++i) {
        if (present[i]) {
            decode_index.push_back(i);
        }
    }

    if (decode_index.size() < k) {
        throw std::runtime_error("decode: not enough unique shards available");
    }

    // Recreate the original RS encoding matrix.
    std::vector<uint8_t> encode_matrix(static_cast<size_t>(n) * k);
    gf_gen_rs_matrix(encode_matrix.data(), n, k);

    // Build the decode matrix from the selected shard rows.
    std::vector<uint8_t> decode_matrix(static_cast<size_t>(k) * k);

    for (uint32_t i = 0; i < k; ++i) {
        const uint32_t shard_index = decode_index[i];
        std::memcpy(
            decode_matrix.data() + static_cast<size_t>(i) * k,
            encode_matrix.data() + static_cast<size_t>(shard_index) * k,
            k
        );
    }

    // Invert the decode matrix.
    std::vector<uint8_t> invert_matrix(static_cast<size_t>(k) * k);

    if (gf_invert_matrix(
            decode_matrix.data(),
            invert_matrix.data(),
            static_cast<int>(k)
        ) != 0) {
        throw std::runtime_error("decode: failed to invert decode matrix");
    }

    // Pointers to the k selected available shards.
    std::vector<uint8_t*> available_ptrs;
    available_ptrs.reserve(k);

    for (uint32_t i = 0; i < k; ++i) {
        // available_ptrs.push_back(shard_bytes[decode_index[i]].data());
        available_ptrs.push_back(shard_ptrs[decode_index[i]]);
    }

    std::vector<uint8_t*> recovered_ptrs(k, nullptr);
    std::vector<Bytes> recovered_storage;
    recovered_storage.reserve(k);

    std::vector<uint32_t> missing_indices;
    missing_indices.reserve(k);

    for (uint32_t i = 0; i < k; ++i) {
        if (present[i]) {
            // no copy: reuse existing shard
            recovered_ptrs[i] = shard_ptrs[i];
        } else {
            // allocate only for missing shards
            recovered_storage.emplace_back(shard_size, 0);
            recovered_ptrs[i] = recovered_storage.back().data();
            missing_indices.push_back(i);
        }
    }

    // Reconstruct all missing data shards in one ISA-L call.
    if (!missing_indices.empty()) {
        const uint32_t missing_count = static_cast<uint32_t>(missing_indices.size());

        std::vector<uint8_t> decode_rows(static_cast<size_t>(missing_count) * k);

        for (uint32_t row = 0; row < missing_count; ++row) {
            const uint32_t data_index = missing_indices[row];
            std::memcpy(
                decode_rows.data() + static_cast<size_t>(row) * k,
                invert_matrix.data() + static_cast<size_t>(data_index) * k,
                k
            );
        }

        std::vector<uint8_t> g_tbls(static_cast<size_t>(k) * missing_count * 32);
        ec_init_tables(
            k,
            missing_count,
            decode_rows.data(),
            g_tbls.data()
        );

        std::vector<uint8_t*> output_ptrs;
        output_ptrs.reserve(missing_count);

        for (uint32_t row = 0; row < missing_count; ++row) {
            output_ptrs.push_back(recovered_ptrs[missing_indices[row]]);
        }

        const int shard_size_i = static_cast<int>(shard_size);
        const int k_i = static_cast<int>(k);
        const int missing_count_i = static_cast<int>(missing_count);

        ec_encode_data(
            shard_size_i,
            k_i,
            missing_count_i,
            g_tbls.data(),
            available_ptrs.data(),
            output_ptrs.data()
        );
    }

    // Concatenate data shards.
    Bytes decoded;
    decoded.resize(total_decoded_size);

    for (uint32_t i = 0; i < k; ++i) {
        std::memcpy(
            decoded.data() + static_cast<size_t>(i) * shard_size,
            recovered_ptrs[i],
            shard_size
        );
    }

    decoded.resize(original_size);
    return decoded;
}
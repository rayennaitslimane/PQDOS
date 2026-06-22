#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

std::vector<PlainShard> encode(const Bytes& data, const ErasureSpec& erasure_spec);

Bytes decode(const std::vector<PlainShard>& shards, const ErasureSpec& erasure_spec, size_t original_size);


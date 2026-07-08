#include "ShardTransport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::vector<std::string> MakeNodes(std::size_t count) {
    std::vector<std::string> nodes;
    nodes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        nodes.push_back("localhost:" + std::to_string(9001 + i));
    }
    return nodes;
}

TEST(HrwIntendedNodesTest, ReturnsRequestedShardCount) {
    const auto nodes = MakeNodes(6);
    const auto placement = hrw_intended_nodes("object-a", nodes, 4);
    EXPECT_EQ(placement.size(), 4u);
}

TEST(HrwIntendedNodesTest, AssignsDistinctNodes) {
    const auto nodes = MakeNodes(6);
    const auto placement = hrw_intended_nodes("object-a", nodes, 4);

    const std::unordered_set<std::string> unique(placement.begin(), placement.end());
    EXPECT_EQ(unique.size(), placement.size());
}

TEST(HrwIntendedNodesTest, OnlySelectsFromEligibleNodes) {
    const auto nodes = MakeNodes(6);
    const std::unordered_set<std::string> eligible(nodes.begin(), nodes.end());

    const auto placement = hrw_intended_nodes("object-a", nodes, 4);
    for (const auto& node : placement) {
        EXPECT_TRUE(eligible.count(node) > 0);
    }
}

TEST(HrwIntendedNodesTest, IsDeterministic) {
    const auto nodes = MakeNodes(8);
    const auto first = hrw_intended_nodes("object-xyz", nodes, 5);
    const auto second = hrw_intended_nodes("object-xyz", nodes, 5);
    EXPECT_EQ(first, second);
}

TEST(HrwIntendedNodesTest, NodeOrderDoesNotAffectPlacement) {
    auto nodes = MakeNodes(6);
    const auto expected = hrw_intended_nodes("object-order", nodes, 3);

    std::reverse(nodes.begin(), nodes.end());
    const auto reordered = hrw_intended_nodes("object-order", nodes, 3);

    // Rendezvous placement depends on the node set, not its ordering.
    EXPECT_EQ(reordered, expected);
}

TEST(HrwIntendedNodesTest, DifferentObjectsSpreadPlacement) {
    const auto nodes = MakeNodes(8);
    const auto a = hrw_intended_nodes("object-a", nodes, 3);
    const auto b = hrw_intended_nodes("object-b", nodes, 3);

    // Two different objects should not deterministically collapse onto the exact
    // same ordered placement across a reasonably sized cluster.
    EXPECT_NE(a, b);
}

TEST(HrwIntendedNodesTest, RemovingAnUnusedNodeDoesNotChangePlacement) {
    // Rendezvous property: dropping a node that was not selected leaves the
    // placement of every shard unchanged (minimal churn).
    const auto nodes = MakeNodes(6);
    const auto original = hrw_intended_nodes("object-churn", nodes, 3);

    const std::unordered_set<std::string> selected(original.begin(), original.end());

    // Find a node that was not selected and drop it.
    std::vector<std::string> reduced;
    bool dropped = false;
    for (const auto& node : nodes) {
        if (!dropped && selected.count(node) == 0) {
            dropped = true;
            continue;
        }
        reduced.push_back(node);
    }
    ASSERT_TRUE(dropped);

    const auto after = hrw_intended_nodes("object-churn", reduced, 3);
    EXPECT_EQ(after, original);
}

TEST(HrwIntendedNodesTest, ThrowsWhenTooFewNodes) {
    const auto nodes = MakeNodes(2);
    EXPECT_THROW(hrw_intended_nodes("object-a", nodes, 3), std::runtime_error);
}

TEST(HrwIntendedNodesTest, UsesEveryNodeWhenCountEqualsShards) {
    const auto nodes = MakeNodes(3);
    const auto placement = hrw_intended_nodes("object-a", nodes, 3);

    const std::unordered_set<std::string> got(placement.begin(), placement.end());
    const std::unordered_set<std::string> expected(nodes.begin(), nodes.end());
    EXPECT_EQ(got, expected);
}

}  // namespace

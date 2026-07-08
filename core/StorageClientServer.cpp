#include "StorageClientServer.hpp"

#include <botan/base64.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

const char* rebalance_status_str(RebalanceStatus status) {
    switch (status) {
        case RebalanceStatus::Balanced:        return "balanced";
        case RebalanceStatus::Moved:           return "moved";
        case RebalanceStatus::DryRun:          return "dry_run";
        case RebalanceStatus::SkippedDegraded: return "skipped_degraded";
        case RebalanceStatus::SkippedUnsafe:   return "skipped_unsafe";
        case RebalanceStatus::SkippedConflict: return "skipped_conflict";
        case RebalanceStatus::SkippedNotFound: return "not_found";
        case RebalanceStatus::Errored:         return "error";
    }
    return "unknown";
}

nlohmann::json rebalance_result_json(const RebalanceObjectResult& result) {
    nlohmann::json item;
    item["object_id"] = result.object_id;
    item["outcome"] = rebalance_status_str(result.status);
    item["shards_total"] = result.shards_total;
    item["shards_misplaced"] = result.shards_misplaced;
    item["shards_moved"] = result.shards_moved;
    item["detail"] = result.detail;
    return item;
}

}  // namespace

StorageClientServer::StorageClientServer(
    StorageClient& client,
    const std::string& host,
    int port
)
    : client_(client), host_(host), port_(port) {
    setup_routes();
}

StorageClientServer::~StorageClientServer() {
    stop();
}

void StorageClientServer::start() {
    server_.listen(host_, port_);
}

void StorageClientServer::stop() {
    server_.stop();
}

void StorageClientServer::setup_routes() {
    server_.Put("/objects/:id", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.path_params.at("id");

        if (id.empty()) {
            nlohmann::json body;
            body["error"] = "id must not be empty";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json input;
        try {
            input = nlohmann::json::parse(req.body);
        } catch (const std::exception&) {
            nlohmann::json body;
            body["error"] = "invalid JSON body";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        if (!input.contains("data") || !input["data"].is_string()) {
            nlohmann::json body;
            body["error"] = "missing or invalid 'data' field";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        const std::string& raw = input["data"].get_ref<const std::string&>();

        Bytes bytes;
        try {
            auto decoded = Botan::base64_decode(raw);
            bytes.assign(decoded.begin(), decoded.end());
        } catch (const std::exception&) {
            nlohmann::json body;
            body["error"] = "invalid base64 in 'data' field";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        if (!input.contains("k") || !input["k"].is_number_unsigned() ||
            !input.contains("m") || !input["m"].is_number_unsigned() ||
            !input.contains("shard_size") || !input["shard_size"].is_number_unsigned()) {
            nlohmann::json body;
            body["error"] = "missing or invalid erasure fields ('k', 'm', 'shard_size')";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        ErasureSpec erasure_spec;
        erasure_spec.data_shards = input["k"].get<uint32_t>();
        erasure_spec.parity_shards = input["m"].get<uint32_t>();
        erasure_spec.shard_size = input["shard_size"].get<std::size_t>();

        try {
            client_.put(id, bytes, erasure_spec);
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json body;
        body["status"] = "ok";
        res.set_content(body.dump(), "application/json");
    });

    server_.Get("/objects/:id", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.path_params.at("id");

        if (id.empty()) {
            nlohmann::json body;
            body["error"] = "id must not be empty";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        Bytes bytes;
        try {
            bytes = client_.get(id);
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json body;
        body["data"] = Botan::base64_encode(bytes.data(), bytes.size());
        res.set_content(body.dump(), "application/json");
    });

    server_.Delete("/objects/:id", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.path_params.at("id");

        if (id.empty()) {
            nlohmann::json body;
            body["error"] = "id must not be empty";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        bool removed = false;
        try {
            removed = client_.remove(id);
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        if (!removed) {
            nlohmann::json body;
            body["error"] = "not found";
            res.status = 404;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json body;
        body["status"] = "ok";
        res.set_content(body.dump(), "application/json");
    });

    server_.Get("/objects", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        std::vector<ObjectMetadata> objects;
        try {
            objects = client_.list();
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json body = nlohmann::json::array();
        for (const auto& obj : objects) {
            nlohmann::json item;
            item["id"] = obj.id;
            item["size"] = obj.size;
            item["checksum"] = obj.checksum;
            body.push_back(item);
        }

        res.set_content(body.dump(), "application/json");
    });

    server_.Get("/objects/:id/health", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.path_params.at("id");

        if (id.empty()) {
            nlohmann::json body;
            body["error"] = "id must not be empty";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        ObjectHealth h;
        try {
            h = client_.health(id);
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json body;
        body["object_id"] = h.object_id;
        body["total_shards"] = h.total_shards;
        body["available_shards"] = h.available_shards;
        body["required_shards"] = h.required_shards;
        body["missing_indices"] = h.missing_indices;
        body["healthy"] = h.healthy;
        body["fully_replicated"] = h.fully_replicated;
        res.set_content(body.dump(), "application/json");
    });

    server_.Post("/objects/:id/repair", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.path_params.at("id");

        if (id.empty()) {
            nlohmann::json body;
            body["error"] = "id must not be empty";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        bool repaired = false;
        try {
            repaired = client_.repair(id);
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json body;
        body["status"] = "ok";
        body["repaired"] = repaired;
        res.set_content(body.dump(), "application/json");
    });

    server_.Post("/admin/reindex", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        ReindexReport report;
        try {
            report = client_.reindex();
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json body;
        body["status"] = "ok";
        body["versions_scanned"] = report.versions_scanned;
        body["objects_recovered"] = report.objects_recovered;
        body["objects_skipped_existing"] = report.objects_skipped_existing;
        body["degraded_objects"] = report.degraded_objects;
        body["unreadable_versions"] = report.unreadable_versions;
        body["errors"] = report.errors;
        res.set_content(body.dump(), "application/json");
    });

    server_.Post("/objects/:id/rebalance", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.path_params.at("id");

        if (id.empty()) {
            nlohmann::json body;
            body["error"] = "id must not be empty";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        // Body is optional; an empty body means default policy.
        nlohmann::json input = nlohmann::json::object();
        if (!req.body.empty()) {
            try {
                input = nlohmann::json::parse(req.body);
            } catch (const std::exception&) {
                nlohmann::json body;
                body["error"] = "invalid JSON body";
                res.status = 400;
                res.set_content(body.dump(), "application/json");
                return;
            }
        }

        RebalancePolicy policy;
        if (input.contains("dry_run")) {
            if (!input["dry_run"].is_boolean()) {
                nlohmann::json body;
                body["error"] = "'dry_run' must be a boolean";
                res.status = 400;
                res.set_content(body.dump(), "application/json");
                return;
            }
            policy.dry_run = input["dry_run"].get<bool>();
        }
        if (input.contains("max_moves")) {
            if (!input["max_moves"].is_number_unsigned()) {
                nlohmann::json body;
                body["error"] = "'max_moves' must be a non-negative integer";
                res.status = 400;
                res.set_content(body.dump(), "application/json");
                return;
            }
            policy.max_moves = input["max_moves"].get<std::size_t>();
        }

        RebalanceObjectResult result;
        try {
            result = client_.rebalance_object(id, policy);
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json body = rebalance_result_json(result);
        body["status"] = "ok";
        res.set_content(body.dump(), "application/json");
    });

    server_.Post("/admin/rebalance", [this](const httplib::Request& req, httplib::Response& res) {
        // Body is optional; an empty body means "rebalance the whole catalog".
        nlohmann::json input = nlohmann::json::object();
        if (!req.body.empty()) {
            try {
                input = nlohmann::json::parse(req.body);
            } catch (const std::exception&) {
                nlohmann::json body;
                body["error"] = "invalid JSON body";
                res.status = 400;
                res.set_content(body.dump(), "application/json");
                return;
            }
        }

        RebalanceScope scope;
        if (input.contains("dry_run")) {
            if (!input["dry_run"].is_boolean()) {
                nlohmann::json body;
                body["error"] = "'dry_run' must be a boolean";
                res.status = 400;
                res.set_content(body.dump(), "application/json");
                return;
            }
            scope.dry_run = input["dry_run"].get<bool>();
        }
        if (input.contains("max_objects")) {
            if (!input["max_objects"].is_number_unsigned()) {
                nlohmann::json body;
                body["error"] = "'max_objects' must be a non-negative integer";
                res.status = 400;
                res.set_content(body.dump(), "application/json");
                return;
            }
            scope.max_objects = input["max_objects"].get<std::size_t>();
        }
        if (input.contains("max_moves")) {
            if (!input["max_moves"].is_number_unsigned()) {
                nlohmann::json body;
                body["error"] = "'max_moves' must be a non-negative integer";
                res.status = 400;
                res.set_content(body.dump(), "application/json");
                return;
            }
            scope.max_moves = input["max_moves"].get<std::size_t>();
        }
        if (input.contains("object_ids")) {
            if (!input["object_ids"].is_array()) {
                nlohmann::json body;
                body["error"] = "'object_ids' must be an array of strings";
                res.status = 400;
                res.set_content(body.dump(), "application/json");
                return;
            }
            for (const auto& entry : input["object_ids"]) {
                if (!entry.is_string()) {
                    nlohmann::json body;
                    body["error"] = "'object_ids' must be an array of strings";
                    res.status = 400;
                    res.set_content(body.dump(), "application/json");
                    return;
                }
                scope.object_ids.push_back(entry.get<std::string>());
            }
        }

        RebalanceReport report;
        try {
            report = client_.rebalance(scope);
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        nlohmann::json body;
        body["status"] = "ok";
        body["dry_run"] = report.dry_run;
        body["objects_scanned"] = report.objects_scanned;
        body["objects_balanced"] = report.objects_balanced;
        body["objects_moved"] = report.objects_moved;
        body["objects_skipped_degraded"] = report.objects_skipped_degraded;
        body["objects_skipped_unsafe"] = report.objects_skipped_unsafe;
        body["objects_skipped_conflict"] = report.objects_skipped_conflict;
        body["objects_not_found"] = report.objects_not_found;
        body["objects_errored"] = report.objects_errored;
        body["shards_moved"] = report.shards_moved;

        nlohmann::json results = nlohmann::json::array();
        for (const auto& r : report.results) {
            results.push_back(rebalance_result_json(r));
        }
        body["results"] = std::move(results);

        res.set_content(body.dump(), "application/json");
    });
}

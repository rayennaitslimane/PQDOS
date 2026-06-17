#include "StorageClientServer.hpp"

#include <botan/base64.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

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
}

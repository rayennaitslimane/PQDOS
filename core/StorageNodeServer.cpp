#include "StorageNodeServer.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

StorageNodeServer::StorageNodeServer(
    StorageNode& node,
    const std::string& host,
    int port
)
    : node_(node), host_(host), port_(port) {
    setup_routes();
}

StorageNodeServer::~StorageNodeServer() {
    stop();
}

void StorageNodeServer::start() {
    server_.listen(host_, port_);
}

void StorageNodeServer::stop() {
    server_.stop();
}

void StorageNodeServer::setup_routes() {
    server_.Get("/health", [](const httplib::Request& /*req*/, httplib::Response& res) {
        nlohmann::json body;
        body["status"] = "ok";
        res.set_content(body.dump(), "application/json");
    });

    server_.Put("/shards/:location", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string location = req.path_params.at("location");

        if (location.empty()) {
            nlohmann::json body;
            body["error"] = "location must not be empty";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        const Bytes payload(req.body.begin(), req.body.end());

        try {
            node_.put({location}, {payload});
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

    server_.Get("/shards/:location", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string location = req.path_params.at("location");

        if (location.empty()) {
            nlohmann::json body;
            body["error"] = "location must not be empty";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        std::vector<std::optional<Bytes>> results;

        try {
            results = node_.get({location});
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        if (results.empty() || !results[0].has_value()) {
            nlohmann::json body;
            body["error"] = "not found";
            res.status = 404;
            res.set_content(body.dump(), "application/json");
            return;
        }

        const Bytes& data = results[0].value();
        res.set_content(
            std::string(data.begin(), data.end()),
            "application/octet-stream"
        );
    });

    server_.Delete("/shards/:location", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string location = req.path_params.at("location");

        if (location.empty()) {
            nlohmann::json body;
            body["error"] = "location must not be empty";
            res.status = 400;
            res.set_content(body.dump(), "application/json");
            return;
        }

        std::vector<std::optional<Bytes>> results;

        try {
            results = node_.remove({location});
        } catch (const std::exception& e) {
            nlohmann::json body;
            body["error"] = e.what();
            res.status = 500;
            res.set_content(body.dump(), "application/json");
            return;
        }

        if (results.empty() || !results[0].has_value()) {
            nlohmann::json body;
            body["error"] = "not found";
            res.status = 404;
            res.set_content(body.dump(), "application/json");
            return;
        }

        const Bytes& data = results[0].value();
        res.set_content(
            std::string(data.begin(), data.end()),
            "application/octet-stream"
        );
    });
}

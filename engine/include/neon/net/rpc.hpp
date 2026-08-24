#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neon::net {

// P2-4 production RPC dispatcher. Handlers are registered by name; Dispatch
// looks one up and invokes it with a JSON argument payload, returning an
// optional reply (name + JSON args) the transport sends back to the caller.
// Built-in server room handlers use the same mechanism ("room.create",
// "room.join", "room.leave", "room.list", "room.broadcast").
class RpcDispatcher {
public:
    // clientId: opaque peer id supplied by the transport (0 when unknown);
    // argsJson: caller-serialized JSON arguments ("{}" when none).
    // Return: optional reply {name, argsJson} delivered to the caller.
    using Handler = std::function<std::optional<std::pair<std::string, std::string>>(
        uint64_t clientId, const std::string& argsJson)>;

    void Register(const std::string& name, Handler handler) {
        handlers_[name] = std::move(handler);
    }

    bool Has(const std::string& name) const { return handlers_.count(name) != 0; }

    // Returns false when no handler is registered for `name`.
    bool Dispatch(uint64_t clientId, const std::string& name, const std::string& argsJson,
                  std::optional<std::pair<std::string, std::string>>* reply) const {
        auto it = handlers_.find(name);
        if (it == handlers_.end()) return false;
        if (reply) *reply = it->second(clientId, argsJson);
        return true;
    }

    std::vector<std::string> Names() const {
        std::vector<std::string> out;
        for (const auto& kv : handlers_) out.push_back(kv.first);
        return out;
    }

private:
    std::map<std::string, Handler> handlers_;
};

} // namespace neon::net

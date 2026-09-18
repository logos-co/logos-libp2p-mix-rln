#include "rln_bridge.h"

#include <cstdio>
#include <logos_protocol.h>
#include <nlohmann/json.hpp>

namespace {
std::string copy(const NimFfiStr& value) {
    return value.data ? std::string(value.data, value.len) : std::string();
}
void responseSubmitted(int, const bool*, const char*, void*) {}
}

MixRlnBridge::~MixRlnBridge() {
    detach();
    if (m_client) lp_client_destroy(m_client);
}

bool MixRlnBridge::attach(LibMixRlnCtx* ctx) {
    detach();
    if (!m_client) {
        m_client = lp_client_create("liblogos_rln_module", "libp2p_mix_rln_module", nullptr, nullptr);
    }
    if (!m_client) return false;
    m_context = std::make_shared<Context>();
    m_context->ctx = ctx;
    return true;
}

void MixRlnBridge::detach() {
    if (m_context) {
        std::lock_guard<std::mutex> guard(m_context->lock);
        m_context->ctx = nullptr;
    }
}

void MixRlnBridge::reply(int ok, const char* text, void* userData) try {
    std::unique_ptr<Pending> pending(static_cast<Pending*>(userData));
    const std::string response = ok && text ? text :
        nlohmann::json{{"error", {{"class", "transient"},
                                 {"message", text ? text : "RLN transport failed"}}}}.dump();
    std::lock_guard<std::mutex> guard(pending->context->lock);
    if (!pending->context->ctx) return;
    RlnModuleResponse result{};
    result.requestId = pending->requestId;
    result.responseJson = {const_cast<char*>(response.data()), response.size()};
    libp2p_mix_rln_ctx_rln_response(pending->context->ctx, &result, responseSubmitted, nullptr);
}

catch (const std::exception& error) {
    fprintf(stderr, "Mix RLN response failed: %s\n", error.what());
}

void MixRlnBridge::request(const RlnModuleRequestEvent& event) {
    const auto method = copy(event.methodName);
    const auto args = copy(event.argsJson);
    auto* pending = new Pending{m_context, event.requestId};
    const int timeout = method == "generate_proof" || method == "get_membership_state" ||
                        method == "register_membership" ? 70000 : 9000;
    const int rc = lp_invoke_async(m_client, method.c_str(), args.c_str(), timeout, reply, pending);
    if (rc != LP_OK) reply(0, "RLN call submission failed", pending);
}

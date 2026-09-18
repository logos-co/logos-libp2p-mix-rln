#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <libp2p_mix_rln.h>

struct lp_client;

// Asynchronous calls keep the Nim event loop free to process replies and stop.
class MixRlnBridge {
public:
    ~MixRlnBridge();
    bool attach(LibMixRlnCtx* ctx);
    void detach();
    void request(const RlnModuleRequestEvent& event);

private:
    struct Context {
        std::mutex lock;
        LibMixRlnCtx* ctx = nullptr;
    };
    struct Pending {
        std::shared_ptr<Context> context;
        int64_t requestId;
    };
    static void reply(int ok, const char* text, void* userData);
    lp_client* m_client = nullptr;
    std::shared_ptr<Context> m_context;
};

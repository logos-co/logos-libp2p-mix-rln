#include "plugin.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// One-shot NimMain
// ---------------------------------------------------------------------------

extern "C" void liblibp2p_mix_rlnNimMain(void);

namespace {
pthread_once_t g_nim_main_once = PTHREAD_ONCE_INIT;
void nim_main_thunk() { liblibp2p_mix_rlnNimMain(); }

constexpr char kModuleVersion[] = "0.1.0";

// Awaits a heap-allocated promise's future. The reply callback OWNS the
// promise: on submit-time failure the callback fires synchronously and
// deletes it; otherwise the dispatch thread does it later. We heap-allocate
// so a timed-out wait doesn't UAF the promise.
SyncResult awaitPromise(std::future<SyncResult>& f, int timeoutMs) {
    if (f.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) {
        return f.get();
    }
    SyncResult r;
    r.message = "timeout";
    return r;
}

// Reclaims a heap-allocated promise after the callback fires.
inline void finish(std::promise<SyncResult>* p, SyncResult r) {
    p->set_value(std::move(r));
    delete p;
}

// Seed a SyncResult from the (err_code, err_msg) preamble every reply gets.
inline SyncResult baseReply(int ec, const char* em) {
    SyncResult r;
    r.ok = (ec == 0);
    if (!r.ok) r.message = em ? em : "FFI call failed";
    return r;
}

// If a submit-time call returns non-OK AND the callback didn't fire
// synchronously (which would delete the promise), reclaim it here.
inline SyncResult reclaimOnSubmitFail(std::promise<SyncResult>* p,
                                      std::future<SyncResult>& f,
                                      int ret, const char* errPrefix) {
    if (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        delete p;
    }
    SyncResult r;
    r.message = std::string(errPrefix) + " submit failed (ret=" + std::to_string(ret) + ")";
    return r;
}

// -----------------------------------------------------------------------
// String bundle: owns std::strings whose NimFfiStr views are borrowed by
// the FFI request struct. Must outlive the call.
// -----------------------------------------------------------------------
NimFfiStr borrowStr(const std::string& s) {
    NimFfiStr v;
    v.data = const_cast<char*>(s.c_str());
    v.len  = s.size();
    return v;
}
} // namespace

namespace {
void ensureNimMain() {
    pthread_once(&g_nim_main_once, nim_main_thunk);
}
} // namespace

namespace {
std::string hexEncodeBytes(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out.push_back(hex[(data[i] >> 4) & 0xF]);
        out.push_back(hex[data[i] & 0xF]);
    }
    return out;
}

// Views borrow from the immutable options and this non-movable scratch storage.
// Both remain alive until the create request has been serialized.
struct FfiConfigBundle {
    std::vector<NimFfiStr> addrsFfi;
    std::string privKeyHex;
    std::string mixPrivKeyHex;
    std::string transport;
    std::string defaultAddr;
    MixRlnConfig cfg{};

    explicit FfiConfigBundle(const Libp2pMixRlnModuleOptions& opts)
        : privKeyHex(hexEncodeBytes(opts.privKey.data(), opts.privKey.size())),
          mixPrivKeyHex(hexEncodeBytes(opts.mix.mixPrivKey.data(), opts.mix.mixPrivKey.size())),
          transport(opts.transport == TransportKind::Quic ? "quic" : "tcp"),
          defaultAddr(opts.transport == TransportKind::Quic
                          ? "/ip4/127.0.0.1/udp/0/quic-v1" : "/ip4/127.0.0.1/tcp/0") {
        for (const auto& addr : opts.addrs) addrsFfi.push_back(borrowStr(addr));
        if (addrsFfi.empty()) addrsFfi.push_back(borrowStr(defaultAddr));
        cfg.addrs.data           = addrsFfi.data();
        cfg.addrs.len            = addrsFfi.size();
        cfg.privKeyHex           = borrowStr(privKeyHex);
        cfg.transport            = borrowStr(transport);
        cfg.maxConnections       = opts.maxConnections;
        cfg.maxConnsPerPeer      = opts.maxConnsPerPeer;

        cfg.mix.mixPrivKeyHex     = borrowStr(mixPrivKeyHex);
        cfg.mix.allowSend         = opts.mix.allowSend;
        cfg.mix.allowExit         = opts.mix.allowExit;
        cfg.mix.coverRateFraction = opts.mix.coverRateFraction;

        cfg.rln.registryId = borrowStr(opts.rln.registryId);
        cfg.rln.rlnIdentifierHex = borrowStr(opts.rln.rlnIdentifierHex);
        cfg.rln.registrationOptionsJson = borrowStr(opts.rln.registrationOptionsJson);
        cfg.rln.epochDurationSeconds      = opts.rln.epochDurationSeconds;
        cfg.rln.maxEpochGap               = opts.rln.maxEpochGap;
        cfg.rln.userMessageLimit          = opts.rln.userMessageLimit;
        cfg.rln.proofMetadataContentTopic = borrowStr(opts.rln.proofMetadataContentTopic);

    }
    FfiConfigBundle(const FfiConfigBundle&) = delete;
    FfiConfigBundle& operator=(const FfiConfigBundle&) = delete;
};

// ---------------------------------------------------------------------------
// Reply trampolines
// ---------------------------------------------------------------------------

static void cbCreate(int ec, LibMixRlnCtx* ctx, const char* em, void* ud) {
    auto* p = static_cast<std::promise<SyncResult>*>(ud);
    SyncResult r = baseReply(ec, em);
    if (r.ok) r.newCtx = ctx;
    finish(p, std::move(r));
}

static void cbBool(int ec, const bool* reply, const char* em, void* ud) {
    auto* p = static_cast<std::promise<SyncResult>*>(ud);
    SyncResult r = baseReply(ec, em);
    if (r.ok && reply) r.boolValue = *reply;
    finish(p, std::move(r));
}

static void cbNodeInfo(int ec, const NodeInfoResponse* reply, const char* em, void* ud) {
    auto* p = static_cast<std::promise<SyncResult>*>(ud);
    SyncResult r = baseReply(ec, em);
    if (r.ok && reply && reply->value.data) {
        r.strValue.assign(reply->value.data, reply->value.len);
    }
    finish(p, std::move(r));
}

static void cbMembership(int ec, const RlnMembershipStatus* reply, const char* em, void* ud) {
    auto* p = static_cast<std::promise<SyncResult>*>(ud);
    SyncResult r = baseReply(ec, em);
    if (r.ok && reply) {
        r.boolValue = reply->registered;
        r.intValue  = reply->index;
    }
    finish(p, std::move(r));
}

static void cbMixSend(int ec, const MixSendResponse* reply, const char* em, void* ud) {
    auto* p = static_cast<std::promise<SyncResult>*>(ud);
    SyncResult r = baseReply(ec, em);
    if (r.ok && reply) {
        std::vector<uint8_t> replyBytes;
        if (reply->reply.data && reply->reply.len > 0) {
            replyBytes.assign(reply->reply.data, reply->reply.data + reply->reply.len);
        }
        r.boolValue = reply->ok;
        r.jsonValue = {{"ok", reply->ok}, {"reply", std::move(replyBytes)}};
    }
    finish(p, std::move(r));
}

static void cbCoverRate(int ec, const CoverRateResponse* reply, const char* em, void* ud) {
    auto* p = static_cast<std::promise<SyncResult>*>(ud);
    SyncResult r = baseReply(ec, em);
    if (r.ok && reply) {
        r.jsonValue = {{"rate", reply->rate}};
    }
    finish(p, std::move(r));
}

static void cbMixPeers(int ec, const MixPeersResponse* reply, const char* em, void* ud) {
    auto* p = static_cast<std::promise<SyncResult>*>(ud);
    SyncResult r = baseReply(ec, em);
    if (r.ok && reply) {
        json arr = json::array();
        for (size_t i = 0; i < reply->peers.len; ++i) {
            const auto& e = reply->peers.data[i];
            json addrs = json::array();
            for (size_t j = 0; j < e.multiaddrs.len; ++j) {
                const auto& a = e.multiaddrs.data[j];
                addrs.push_back(std::string(a.data ? a.data : "", a.data ? a.len : 0));
            }
            arr.push_back({
                {"peerId", std::string(e.peerId.data ? e.peerId.data : "", e.peerId.data ? e.peerId.len : 0)},
                {"multiaddrs", std::move(addrs)},
            });
        }
        r.jsonValue = {{"peers", std::move(arr)}};
    }
    finish(p, std::move(r));
}

// Events -------------------------------------------------------------------
//
// File-scope statics rather than class members: the LIDL codegen scans public
// members and rejects the `void*` param these need. Access private-ish state
// via the public `emitEvent` std::function member of the impl.

namespace {
void hostEmit(Libp2pMixRlnModuleImpl* self, const char* name, const nlohmann::json& payload) {
    if (self && self->emitEvent) self->emitEvent(name, payload.dump());
}
} // namespace

static void onIncomingMixMessage(const IncomingMixMessageEvent* evt, void* ud) {
    auto* self = static_cast<Libp2pMixRlnModuleImpl*>(ud);
    if (!evt || !self) return;
    std::string proto(evt->proto.data ? evt->proto.data : "",
                      evt->proto.data ? evt->proto.len : 0);
    std::vector<uint8_t> payload(evt->payload.data,
                                 evt->payload.data + evt->payload.len);
    std::vector<uint8_t> surb;
    if (evt->surb.data && evt->surb.len > 0) {
        surb.assign(evt->surb.data, evt->surb.data + evt->surb.len);
    }
    // Push-style: fire the host callback if one is registered.
    hostEmit(self, "IncomingMixMessage", {
        {"proto",   proto},
        {"payload", payload},
        {"surb",    surb},
    });
    // Pull-style: buffer for `drainReceivedMessages` (used by shell-driven
    // orchestration that can't subscribe to events).
    {
        std::lock_guard<std::mutex> lk(self->m_backlogMutex);
        self->m_inbox.push_back({std::move(proto), std::move(payload), std::move(surb)});
    }
}

static void onRlnModuleRequest(const RlnModuleRequestEvent* event, void* ud) {
    auto* self = static_cast<Libp2pMixRlnModuleImpl*>(ud);
    if (!self || !event) return;
    try {
        self->m_rlnBridge.request(*event);
    } catch (const std::exception& error) {
        fprintf(stderr, "Mix RLN request failed: %s\n", error.what());
    }
}

static void onRlnPublishRequested(const RlnPublishRequestedEvent* evt, void* ud) {
    auto* self = static_cast<Libp2pMixRlnModuleImpl*>(ud);
    if (!evt || !self) return;
    std::string topic(evt->contentTopic.data ? evt->contentTopic.data : "",
                      evt->contentTopic.data ? evt->contentTopic.len : 0);
    std::vector<uint8_t> payload;
    if (evt->payload.len > 0)
        payload.assign(evt->payload.data, evt->payload.data + evt->payload.len);
    hostEmit(self, "RlnPublishRequested", {
        {"contentTopic", topic},
        {"payload",      payload},
    });
    // Same pull-style buffering so a shell orchestrator can ferry frames
    // between logoscore daemons via `drainCoordBacklog` + `deliverCoordFrame`.
    {
        std::lock_guard<std::mutex> lk(self->m_backlogMutex);
        self->m_coordBacklog.push_back({std::move(topic), std::move(payload)});
    }
}

} // namespace  (closes the file-scope anonymous namespace opened above the
  // FfiConfigBundle / cb* / on* helpers)

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Libp2pMixRlnModuleImpl::Libp2pMixRlnModuleImpl(const Libp2pMixRlnModuleOptions& options)
    : m_options(options) {
    ensureNimMain();
}

Libp2pMixRlnModuleImpl::~Libp2pMixRlnModuleImpl() {
    if (m_ctx) {
        // Stop pending backend work before releasing the FFI context.
        (void)stop();
        m_rlnBridge.detach();
        libp2p_mix_rln_ctx_destroy(m_ctx);
        m_ctx = nullptr;
    }
}

bool Libp2pMixRlnModuleImpl::ok() { return m_initError.empty(); }

StdLogosResult Libp2pMixRlnModuleImpl::status() {
    std::lock_guard<std::mutex> lk(m_callMutex);
    json j = {
        {"version", kModuleVersion},
        {"state",   m_ctx ? "created" : "uninitialized"},
    };
    if (!m_initError.empty()) j["initError"] = m_initError;
    return {true, j, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::createNode(const std::string& configJson) {
    std::lock_guard<std::mutex> lk(m_callMutex);
    // If caller supplied a fresh JSON config, re-parse over m_options.
    if (!configJson.empty()) {
        bool ok = false;
        std::string err;
        auto opts = Libp2pMixRlnModuleOptions::fromJson(configJson, ok, &err);
        if (!ok) return {false, {}, "invalid config: " + err};
        m_options = std::move(opts);
    }

    // Tear down any prior node before rebuilding.
    if (m_ctx) {
        m_rlnBridge.detach();
        libp2p_mix_rln_ctx_destroy(m_ctx);
        m_ctx = nullptr;
    }

    {
        std::lock_guard<std::mutex> backlogLock(m_backlogMutex);
        m_coordBacklog.clear();
        m_inbox.clear();
    }

    FfiConfigBundle bundle(m_options);
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_create(&bundle.cfg, cbCreate, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "createNode");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kCreateTimeoutMs);
    if (!r.ok) return {false, {}, "createNode: " + r.message};
    m_ctx = r.newCtx;

    // Register event listeners now that we have a ctx. Passing `this` as ud so
    // event trampolines can find us.
    libp2p_mix_rln_ctx_add_on_incoming_mix_message_listener(m_ctx, onIncomingMixMessage, this);
    if (!m_rlnBridge.attach(m_ctx)) {
        libp2p_mix_rln_ctx_destroy(m_ctx);
        m_ctx = nullptr;
        return {false, {}, "RLN module transport unavailable"};
    }
    libp2p_mix_rln_ctx_add_on_rln_module_request_listener(m_ctx, onRlnModuleRequest, this);

    libp2p_mix_rln_ctx_add_on_rln_publish_requested_listener(m_ctx, onRlnPublishRequested, this);

    return {true, json{{"created", true}}, ""};
}

// ---------------------------------------------------------------------------
// Generic call helpers
// ---------------------------------------------------------------------------

namespace {
// Common submit / await / translate dance for ops that take no args and
// return a bool (start, stop).
template <class Submit>
StdLogosResult submitBool(LibMixRlnCtx* ctx, const char* errPrefix, int timeoutMs, Submit&& submit) {
    if (!ctx) return {false, {}, std::string(errPrefix) + ": node not created"};
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = submit(p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, errPrefix);
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, timeoutMs);
    if (!r.ok) return {false, {}, std::string(errPrefix) + ": " + r.message};
    return {true, nlohmann::json{{"ok", r.boolValue}}, ""};
}
} // namespace

// ---------------------------------------------------------------------------
// Ops
// ---------------------------------------------------------------------------

StdLogosResult Libp2pMixRlnModuleImpl::start() {
    std::lock_guard<std::mutex> lk(m_callMutex);
    return submitBool(m_ctx, "start", kDefaultOpTimeoutMs, [&](std::promise<SyncResult>* p) {
        return libp2p_mix_rln_ctx_start(m_ctx, cbBool, p);
    });
}

StdLogosResult Libp2pMixRlnModuleImpl::stop() {
    std::lock_guard<std::mutex> lk(m_callMutex);
    return submitBool(m_ctx, "stop", kStopTimeoutMs, [&](std::promise<SyncResult>* p) {
        return libp2p_mix_rln_ctx_stop(m_ctx, cbBool, p);
    });
}

StdLogosResult Libp2pMixRlnModuleImpl::getNodeInfo(const std::string& field) {
    NodeInfoField fld;
    if      (field == "Version")            fld = NODE_INFO_FIELD_NIF_VERSION;
    else if (field == "PeerId")             fld = NODE_INFO_FIELD_NIF_PEER_ID;
    else if (field == "Multiaddrs")         fld = NODE_INFO_FIELD_NIF_MULTIADDRS;
    else if (field == "MixPublicKey")       fld = NODE_INFO_FIELD_NIF_MIX_PUBLIC_KEY;
    else if (field == "RlnMembershipIndex") fld = NODE_INFO_FIELD_NIF_RLN_MEMBERSHIP_INDEX;
    else return {false, {}, "unknown field: " + field};

    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "getNodeInfo: node not created"};
    NodeInfoRequest req{fld};
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_get_node_info(m_ctx, &req, cbNodeInfo, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "getNodeInfo");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "getNodeInfo: " + r.message};
    return {true, r.strValue, ""};
}

// RLN --------------------------------------------------------------------

StdLogosResult Libp2pMixRlnModuleImpl::registerRlnMembership() {
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "registerRlnMembership: node not created"};
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_register_rln_membership(m_ctx, cbMembership, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "registerRlnMembership");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "registerRlnMembership: " + r.message};
    return {true, json{{"registered", r.boolValue}, {"index", r.intValue}}, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::hasRlnMembership() {
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "hasRlnMembership: node not created"};
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_has_rln_membership(m_ctx, cbMembership, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "hasRlnMembership");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "hasRlnMembership: " + r.message};
    return {true, json{{"registered", r.boolValue}, {"index", r.intValue}}, ""};
}

// Mix send ---------------------------------------------------------------

// submitMixSend is a file-scope static so it can call cbMixSend directly.
static StdLogosResult submitMixSend(LibMixRlnCtx* ctx,
                                    const std::string& destPeerId,
                                    const std::string& proto,
                                    const std::vector<uint8_t>& payload,
                                    bool expectReply,
                                    LibMixRlnSendMixMessageReplyFn cb) {
    if (!ctx) return {false, {}, "sendMixMessage: node not created"};
    MixSendRequest req{};
    req.destPeerId    = borrowStr(destPeerId);
    req.proto         = borrowStr(proto);
    req.payload.data  = const_cast<uint8_t*>(payload.data());
    req.payload.len   = payload.size();
    req.expectReply   = expectReply;
    req.numSurbs      = expectReply ? 1 : 0;
    req.timeoutMs     = kDefaultOpTimeoutMs;

    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_send_mix_message(ctx, &req, cb, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "sendMixMessage");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs + 5000);
    if (!r.ok) return {false, {}, "sendMixMessage: " + r.message};
    return {true, std::move(r.jsonValue), ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::sendMixMessage(const std::string& destPeerId,
                                                      const std::string& proto,
                                                      const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lk(m_callMutex);
    return submitMixSend(m_ctx, destPeerId, proto, payload,
                         /*expectReply=*/false, cbMixSend);
}

StdLogosResult Libp2pMixRlnModuleImpl::sendMixMessageWithSurb(const std::string& destPeerId,
                                                              const std::string& proto,
                                                              const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lk(m_callMutex);
    return submitMixSend(m_ctx, destPeerId, proto, payload,
                         /*expectReply=*/true, cbMixSend);
}

StdLogosResult Libp2pMixRlnModuleImpl::sendMixSurbReply(const std::vector<uint8_t>& surb,
                                                        const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "sendMixSurbReply: node not created"};
    MixSurbReplyRequest req{};
    req.surb.data    = const_cast<uint8_t*>(surb.data());
    req.surb.len     = surb.size();
    req.payload.data = const_cast<uint8_t*>(payload.data());
    req.payload.len  = payload.size();
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_send_mix_surb_reply(m_ctx, &req, cbBool, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "sendMixSurbReply");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "sendMixSurbReply: " + r.message};
    return {true, json{{"ok", r.boolValue}}, ""};
}

// Discovery / cover -----------------------------------------------------

StdLogosResult Libp2pMixRlnModuleImpl::listMixPeers() {
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "listMixPeers: node not created"};
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_list_mix_peers(m_ctx, cbMixPeers, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "listMixPeers");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "listMixPeers: " + r.message};
    return {true, r.jsonValue, ""};
}

// Multi-node topology + coord ---------------------------------------------

namespace {
std::vector<uint8_t> hexDecodeStr(const std::string& in, bool& ok) {
    ok = true;
    try {
        return libp2p_mix_rln_config::decodeHex(in);
    } catch (const std::invalid_argument&) {
        ok = false;
        return {};
    }
}

// getLocalMixPeerRecord's reply carries the whole MixPeerRecord struct — pack
// it into a JSON object the CLI can jq into.
void cbGetLocalMixPeerRecord(int ec, const MixPeerRecord* r,
                             const char* em, void* ud) {
    auto* p = static_cast<std::promise<SyncResult>*>(ud);
    SyncResult res = baseReply(ec, em);
    if (res.ok && r) {
        nlohmann::json addrs = nlohmann::json::array();
        for (size_t i = 0; i < r->multiaddrs.len; i++) {
            const auto& s = r->multiaddrs.data[i];
            addrs.push_back(std::string(s.data ? s.data : "", s.data ? s.len : 0));
        }
        res.jsonValue = {
            {"peerId",          std::string(r->peerId.data ? r->peerId.data : "",
                                            r->peerId.data ? r->peerId.len : 0)},
            {"multiaddrs",      std::move(addrs)},
            {"mixPubKeyHex",    hexEncodeBytes(r->mixPubKey.data, r->mixPubKey.len)},
            {"libp2pPubKeyHex", std::string(r->libp2pPubKeyHex.data ? r->libp2pPubKeyHex.data : "",
                                            r->libp2pPubKeyHex.data ? r->libp2pPubKeyHex.len : 0)},
        };
    }
    finish(p, std::move(res));
}
} // namespace

StdLogosResult Libp2pMixRlnModuleImpl::getLocalMixPeerRecord() {
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "getLocalMixPeerRecord: node not created"};
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_get_local_mix_peer_record(m_ctx, cbGetLocalMixPeerRecord, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "getLocalMixPeerRecord");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "getLocalMixPeerRecord: " + r.message};
    return {true, r.jsonValue, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::addMixPeer(const std::string& recordJson)
{
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "addMixPeer: node not created"};

    // Parse the peer record from JSON (shape matches getLocalMixPeerRecord).
    auto j = nlohmann::json::parse(recordJson, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return {false, {}, "addMixPeer: malformed recordJson"};
    auto get_str = [&](const char* k) -> std::string {
        auto it = j.find(k);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : "";
    };
    std::string peerId          = get_str("peerId");
    std::string mixPubKeyHex    = get_str("mixPubKeyHex");
    std::string libp2pPubKeyHex = get_str("libp2pPubKeyHex");
    if (peerId.empty() || mixPubKeyHex.empty() || libp2pPubKeyHex.empty())
        return {false, {}, "addMixPeer: missing peerId/mixPubKeyHex/libp2pPubKeyHex"};

    std::vector<std::string> multiaddrs;
    auto mit = j.find("multiaddrs");
    if (mit == j.end() || !mit->is_array())
        return {false, {}, "addMixPeer: multiaddrs must be an array"};
    for (const auto& v : *mit) {
        if (!v.is_string())
            return {false, {}, "addMixPeer: multiaddrs entries must be strings"};
        multiaddrs.push_back(v.get<std::string>());
    }

    // Decode the hex-encoded mix pub key here (bytes) — the FFI struct takes
    // `NimFfiBytes mixPubKey`, not hex.
    bool okHex = false;
    auto mixPubBytes = hexDecodeStr(mixPubKeyHex, okHex);
    if (!okHex) return {false, {}, "addMixPeer: invalid mixPubKeyHex"};

    // Build the borrowed NimFfiStr array over the C++ std::string vector.
    std::vector<NimFfiStr> addrsFfi;
    addrsFfi.reserve(multiaddrs.size());
    for (const auto& a : multiaddrs) {
        NimFfiStr s{const_cast<char*>(a.c_str()), a.size()};
        addrsFfi.push_back(s);
    }

    MixPeerRecord rec{};
    rec.peerId          = NimFfiStr{const_cast<char*>(peerId.c_str()), peerId.size()};
    rec.multiaddrs.data = addrsFfi.data();
    rec.multiaddrs.len  = addrsFfi.size();
    rec.mixPubKey.data  = mixPubBytes.data();
    rec.mixPubKey.len   = mixPubBytes.size();
    rec.libp2pPubKeyHex = NimFfiStr{const_cast<char*>(libp2pPubKeyHex.c_str()),
                                    libp2pPubKeyHex.size()};

    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_add_mix_peer(m_ctx, &rec, cbBool, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "addMixPeer");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "addMixPeer: " + r.message};
    return {true, nlohmann::json{{"ok", r.boolValue}}, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::mountReceiver(
    const std::string& codec, int64_t maxSize)
{
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "mountReceiver: node not created"};

    MountReceiverRequest req{};
    req.codec   = NimFfiStr{const_cast<char*>(codec.c_str()), codec.size()};
    req.maxSize = maxSize > 0 ? maxSize : 1 << 20;

    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_mount_receiver(m_ctx, &req, cbBool, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "mountReceiver");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "mountReceiver: " + r.message};
    return {true, nlohmann::json{{"ok", r.boolValue}}, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::deliverCoordFrame(
    const std::string& contentTopic, const std::string& payloadHex)
{
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "deliverCoordFrame: node not created"};

    bool ok = false;
    auto payload = hexDecodeStr(payloadHex, ok);
    if (!ok) return {false, {}, "deliverCoordFrame: invalid payloadHex"};

    RlnCoordFrame frame{};
    frame.contentTopic = NimFfiStr{const_cast<char*>(contentTopic.c_str()),
                                   contentTopic.size()};
    frame.data.data    = payload.data();
    frame.data.len     = payload.size();

    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_deliver_coord_frame(m_ctx, &frame, cbBool, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "deliverCoordFrame");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "deliverCoordFrame: " + r.message};
    return {true, nlohmann::json{{"ok", r.boolValue}}, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::drainCoordBacklog() {
    std::vector<CoordBacklogEntry> drained;
    {
        std::lock_guard<std::mutex> lk(m_backlogMutex);
        drained.swap(m_coordBacklog);
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : drained) {
        arr.push_back({
            {"contentTopic", e.contentTopic},
            {"payloadHex",   hexEncodeBytes(e.payload.data(), e.payload.size())},
        });
    }
    return {true, arr, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::drainReceivedMessages() {
    std::vector<InboxEntry> drained;
    {
        std::lock_guard<std::mutex> lk(m_backlogMutex);
        drained.swap(m_inbox);
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : drained) {
        arr.push_back({
            {"proto",      e.proto},
            {"payloadHex", hexEncodeBytes(e.payload.data(), e.payload.size())},
            {"surbHex",    hexEncodeBytes(e.surb.data(), e.surb.size())},
        });
    }
    return {true, arr, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::getCoverTrafficRate() {
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "getCoverTrafficRate: node not created"};
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_get_cover_traffic_rate(m_ctx, cbCoverRate, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "getCoverTrafficRate");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "getCoverTrafficRate: " + r.message};
    return {true, r.jsonValue, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::setCoverTrafficRate(double rate) {
    std::lock_guard<std::mutex> lk(m_callMutex);
    if (!m_ctx) return {false, {}, "setCoverTrafficRate: node not created"};
    SetCoverRateRequest req{rate};
    auto* p = new std::promise<SyncResult>();
    auto f = p->get_future();
    int ret = libp2p_mix_rln_ctx_set_cover_traffic_rate(m_ctx, &req, cbBool, p);
    if (ret != 0) {
        auto r = reclaimOnSubmitFail(p, f, ret, "setCoverTrafficRate");
        return {false, {}, r.message};
    }
    auto r = awaitPromise(f, kDefaultOpTimeoutMs);
    if (!r.ok) return {false, {}, "setCoverTrafficRate: " + r.message};
    return {true, json{{"ok", r.boolValue}}, ""};
}

LogosMap Libp2pMixRlnModuleImpl::collectMetrics() {
    // Nim side doesn't emit metrics through the FFI yet. When it does, this
    // will call a `libp2p_mix_rln_ctx_collect_metrics` and translate the
    // Prometheus-shape JSON into a LogosMap of counters.
    return {};
}

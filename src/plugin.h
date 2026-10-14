#pragma once

// Logos Mixnet Module — public C++ surface exposed to the Logos Core codegen.
//
// Bodies live in plugin.cpp. Every FFI-backed operation is a sync-over-async
// bridge on SDK dispatch workers, keeping the host event loop free for RLN replies.
// Build a std::promise, submit through the nim-ffi typed wrapper (which
// takes a reply callback), await the promise with a timeout, translate the
// reply into StdLogosResult. Follows the same pattern as
// logos-libp2p-module/src/plugin.{h,cpp} but against libp2p_mix_rln.h.

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "logos_result.h"

// The nim-ffi generated header is header-only C: it declares the exported Nim
// symbols inside its own `extern "C"` block and exposes the async API as
// `static inline` wrappers plus C++-linkage callback typedefs. It must NOT be
// wrapped in an extra `extern "C"` here, or the reply-callback typedefs would
// take C linkage and no longer match the C++ static callbacks we pass in.
#include <libp2p_mix_rln.h>

#include "config.h"
#include "rln_bridge.h"

// Timeouts (milliseconds) for the sync-over-async libp2p_mix_rln bridge.
// Sized like logos-libp2p-module — long enough for a network create + RLN
// initialization on a slow machine, short enough that a hang is caught.
inline constexpr int kDefaultOpTimeoutMs = 10000;
inline constexpr int kRlnOpTimeoutMs     = 110000;
inline constexpr int kCreateTimeoutMs    = 30000;   // RLN init + Switch build
inline constexpr int kStopTimeoutMs      = 15000;   // switch.stop over many conns

// Result carried through the sync-over-async bridge. Only the fields the
// invoking op cares about are populated; the rest stay defaulted.
struct SyncResult {
    bool ok = false;
    std::string message;
    // Optional carriers for typed reply data. Each op consumes at most one.
    std::string strValue;
    int64_t     intValue = 0;
    bool        boolValue = false;
    LibMixRlnCtx* newCtx = nullptr;
    // For collection replies where a std::string carrier isn't a good fit.
    nlohmann::json jsonValue;
};

class Libp2pMixRlnModuleImpl {
public:
    Libp2pMixRlnModuleImpl(const Libp2pMixRlnModuleOptions& options = Libp2pMixRlnModuleOptions::load());
    ~Libp2pMixRlnModuleImpl();

    // Set by the codegen glue after construction so the impl can push events.
    // Known event names:
    //   "IncomingMixMessage" — {proto, payload: byte[], surbs: byte[]}
    //   "RlnPublishRequested" — {contentTopic, payload: byte[]}
    std::function<void(const std::string& eventName, const std::string& data)> emitEvent;

    // Health / status ------------------------------------------------------
    StdLogosResult status();

    // Node lifecycle -------------------------------------------------------
    StdLogosResult createNode(const std::string& configJson);
    StdLogosResult start();
    StdLogosResult stop();

    // Node introspection ---------------------------------------------------
    // field ∈ { "Version", "PeerId", "Multiaddrs", "MixPublicKey", "RlnMembershipIndex" }
    StdLogosResult getNodeInfo(const std::string& field);

    // RLN membership -------------------------------------------------------
    StdLogosResult registerRlnMembership();
    StdLogosResult hasRlnMembership();

    // Transport these frames through an external Relay-capable Delivery module.
    // Use either the publish event or the pull backlog for outbound delivery.
    StdLogosResult deliverCoordFrame(const std::string& contentTopic, const std::string& payloadHex);
    StdLogosResult drainCoordBacklog();

    // Logos supports exit == destination only; the destination must run a
    // receiver mounted for `proto`.
    StdLogosResult sendMixMessage(const std::string& destPeerId,
                                  const std::string& proto,
                                  const std::vector<uint8_t>& payload);

    StdLogosResult sendMixMessageWithSurb(const std::string& destPeerId,
                                          const std::string& proto,
                                          const std::vector<uint8_t>& payload);

    // Consumes the opaque incoming bundle; a failure may follow partial delivery.
    StdLogosResult reply(const std::vector<uint8_t>& surbs,
                         const std::vector<uint8_t>& payload);

    // Peer record includes peerId, multiaddrs, mixPubKeyHex, and libp2pPubKeyHex.
    StdLogosResult getLocalMixPeerRecord();

    // Installs a peer record into the local nodePool. Takes the whole record
    // as a JSON string of the shape `getLocalMixPeerRecord` returns
    // (`{peerId, multiaddrs, mixPubKeyHex, libp2pPubKeyHex}`) — passing it as
    // a single string keeps the LIDL args scalar-only, which is what the
    // `logoscore call` CLI knows how to marshal.
    StdLogosResult addMixPeer(const std::string& recordJson);

    // Emits IncomingMixMessage for length-prefixed messages up to maxSize.
    StdLogosResult mountReceiver(const std::string& codec, int64_t maxSize);

    // Mix-node inventory ---------------------------------------------------
    StdLogosResult listMixPeers();

    // ------------------------------------------------------------------
    // Members below are for plugin.cpp's internal helpers. Kept public so
    // file-scope trampolines in plugin.cpp can access them, but they are NOT
    // part of the module's LIDL interface — the codegen skips non-method
    // members. Do not call from other modules.
    // ------------------------------------------------------------------

    MixRlnBridge m_rlnBridge;
    LibMixRlnCtx* m_ctx = nullptr;
    Libp2pMixRlnModuleOptions m_options;

    // Serialize node operations against replacement and teardown.
    std::mutex m_callMutex;

    // status() never waits for FFI operations holding m_callMutex.
    std::mutex m_statusMutex;
    nlohmann::json m_status = {{"version", "0.1.0"}, {"state", "uninitialized"}};

    // Coordination callbacks and drainCoordBacklog run on separate threads.
    std::mutex m_backlogMutex;
    struct CoordBacklogEntry {
        std::string contentTopic;
        std::vector<uint8_t> payload;
    };
    std::vector<CoordBacklogEntry> m_coordBacklog;
};

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

#include "logos_json.h"
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
    //   "IncomingMixMessage" — {proto, payload: byte[], surb: byte[]}
    //   "RlnPublishRequested" — {contentTopic, payload: byte[]}
    //   "RlnMembershipRegistered" — {index, root: byte[]}
    std::function<void(const std::string& eventName, const std::string& data)> emitEvent;

    // Health / status ------------------------------------------------------
    bool ok();
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

    // Application sends, including explicit SURB replies, require mix.allowSend.
    StdLogosResult sendMixMessage(const std::string& destPeerId,
                                  const std::string& destMultiaddr,
                                  const std::string& proto,
                                  const std::vector<uint8_t>& payload);

    // Intra-mixnet send: the exit is the destination. Uses
    // `MixDestination.exitNode(peerId)` on the Nim side — no destination
    // multiaddr needed because the exit runs a mounted receiver protocol
    // and handles the payload itself. Pairs with `mountReceiver`.
    StdLogosResult sendMixMessageToExit(const std::string& destPeerId,
                                        const std::string& proto,
                                        const std::vector<uint8_t>& payload);

    StdLogosResult sendMixMessageWithSurb(const std::string& destPeerId,
                                          const std::string& destMultiaddr,
                                          const std::string& proto,
                                          const std::vector<uint8_t>& payload);

    StdLogosResult sendMixMessageToExitWithSurb(const std::string& destPeerId,
                                                const std::string& proto,
                                                const std::vector<uint8_t>& payload);

    StdLogosResult sendMixSurbReply(const std::vector<uint8_t>& surb,
                                    const std::vector<uint8_t>& payload);

    // Peer record includes peerId, multiaddrs, mixPubKeyHex, libp2pPubKeyHex,
    // and exitEnabled. Only advertised exits are eligible for application delivery.
    StdLogosResult getLocalMixPeerRecord();

    // Installs a peer record into the local nodePool. Takes the whole record
    // as a JSON string of the shape `getLocalMixPeerRecord` returns
    // (`{peerId, multiaddrs, mixPubKeyHex, libp2pPubKeyHex, exitEnabled}`) — passing it as
    // a single string keeps the LIDL args scalar-only, which is what the
    // `logoscore call` CLI knows how to marshal.
    StdLogosResult addMixPeer(const std::string& recordJson);

    // Requires mix.allowExit. Mounts a protocol on `codec`; length-prefixed
    // bytes (up to `maxSize`) are queued into an inbox, drainable via
    // `drainReceivedMessages`. Pairs with `sendMixMessage(isExitDest=true)`.
    StdLogosResult mountReceiver(const std::string& codec, int64_t maxSize);

    // Returns and clears the accumulated `IncomingMixMessage` payloads for
    // any codec mounted via `mountReceiver`, as a JSON array of
    // {proto, payloadHex, surbHex}. Empty array when nothing pending.
    StdLogosResult drainReceivedMessages();

    // Mix-node inventory ---------------------------------------------------
    StdLogosResult listMixPeers();

    // Cover traffic --------------------------------------------------------
    StdLogosResult getCoverTrafficRate();
    StdLogosResult setCoverTrafficRate(double rate);

    // Diagnostics ----------------------------------------------------------
    LogosMap collectMetrics();

    // ------------------------------------------------------------------
    // Members below are for plugin.cpp's internal helpers. Kept public so
    // file-scope trampolines in plugin.cpp can access them, but they are NOT
    // part of the module's LIDL interface — the codegen skips non-method
    // members. Do not call from other modules.
    // ------------------------------------------------------------------

    MixRlnBridge m_rlnBridge;
    LibMixRlnCtx* m_ctx = nullptr;
    Libp2pMixRlnModuleOptions m_options;

    // Set by the constructor if the initial createNode failed. Surfaced by
    // status() since the constructor cannot signal failure to the codegen
    // default-constructor.
    std::string m_initError;

    // Serialize op submission: nim-ffi's C API is thread-safe for the async
    // dispatch, but our sync-over-async waiter would race if two ops queued
    // simultaneously and their replies interleaved into the same promise.
    std::mutex m_callMutex;

    // Incoming-message events fire from the nim-ffi dispatch thread; the
    // drain method runs on the caller thread. Guarded separately so the
    // guarded regions don't nest with m_callMutex.
    struct InboxEntry {
        std::string proto;
        std::vector<uint8_t> payload;
        std::vector<uint8_t> surb;
    };
    std::mutex m_backlogMutex;
    std::vector<InboxEntry>        m_inbox;
    struct CoordBacklogEntry {
        std::string contentTopic;
        std::vector<uint8_t> payload;
    };
    std::vector<CoordBacklogEntry> m_coordBacklog;
};

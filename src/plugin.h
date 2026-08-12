#pragma once

// Logos Mixnet Module — public C++ surface exposed to the Logos Core codegen.
//
// Bodies live in plugin.cpp. Every FFI-backed operation is a sync-over-async
// bridge: build a std::promise, submit through the nim-ffi typed wrapper (which
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
    //   "IncomingMixMessage" — {protocol, payload_b64, surb_b64?}
    //   "RlnMembershipRegistered" — {index, root}
    //   "RlnPublishRequested" — {contentTopic, payload_b64}
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

    // Mixnet send ----------------------------------------------------------
    StdLogosResult sendMixMessage(const std::string& destPeerId,
                                  const std::string& proto,
                                  const std::vector<uint8_t>& payload);

    StdLogosResult sendMixMessageWithSurb(const std::string& destPeerId,
                                          const std::string& proto,
                                          const std::vector<uint8_t>& payload);

    StdLogosResult sendMixSurbReply(const std::vector<uint8_t>& surb,
                                    const std::vector<uint8_t>& payload);

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
};

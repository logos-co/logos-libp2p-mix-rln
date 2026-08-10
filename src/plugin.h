#pragma once

// Logos Mixnet Module — public C++ surface exposed to the Logos Core codegen.
//
// STATUS: scaffold. The FFI-backed body of every method is a stub that returns
// StdLogosResult{false, {}, "not implemented"} until the companion Nim FFI
// facade (`nim-libp2p-mix-rln`, producing `libp2p_mix_rln.{so,dylib,dll}` and
// `lib/libp2p_mix_rln.h`) is built and vendored per metadata.json's
// nix.external_libraries entry. Once wired, this class will follow the same
// sync-over-async bridge pattern as logos-libp2p-module's Libp2pModuleImpl.
//
// The API surface below is what LIP LOGOS-MIXNET requires a mixnet node to
// expose plus the standard node-lifecycle operations Logos Core modules share.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "logos_json.h"
#include "logos_result.h"

#include "config.h"

class Libp2pMixRlnModuleImpl {
public:
    Libp2pMixRlnModuleImpl(const Libp2pMixRlnModuleOptions& options = Libp2pMixRlnModuleOptions::load());
    ~Libp2pMixRlnModuleImpl();

    // Set by the codegen glue after construction so the impl can push events.
    // Known event names:
    //   "IncomingMixMessage" — {protocol, payload_b64, surb_b64?}
    //   "RlnMembershipRegistered" — {index, root}
    //   "CoverTrafficRate" — {rate}
    std::function<void(const std::string& eventName, const std::string& data)> emitEvent;

    // Health / status ------------------------------------------------------
    bool ok();
    StdLogosResult status();

    // Node lifecycle -------------------------------------------------------
    // Rebuilds the underlying node from a JSON config. Accepts the same schema
    // as LIBP2P_MIX_RLN_MODULE_CONFIG.
    StdLogosResult createNode(const std::string& configJson);
    StdLogosResult start();
    StdLogosResult stop();

    // Node introspection ---------------------------------------------------
    // field ∈ { "Version", "PeerId", "Multiaddrs", "MixPublicKey", "RlnMembershipIndex" }
    StdLogosResult getNodeInfo(const std::string& field);

    // RLN membership -------------------------------------------------------
    // Registers this node in the RLN group used by the mixnet. In the Logos
    // deployment, membership is coordinated over the RLN Relay content topic
    // configured under rln.membershipContentTopic.
    StdLogosResult registerRlnMembership();
    // True if this node currently holds a valid RLN membership proof.
    StdLogosResult hasRlnMembership();

    // Mixnet send ----------------------------------------------------------
    // Sends `payload` through a path-length-3 Sphinx circuit to `destPeerId`
    // for protocol `proto` (mounted on the destination as a libp2p protocol
    // id). Each hop generates a fresh RLN proof; the exit unwraps and hands
    // the payload to the destination protocol handler.
    StdLogosResult sendMixMessage(const std::string& destPeerId,
                                  const std::string& proto,
                                  const std::vector<uint8_t>& payload);

    // Same as above but attaches a Single-Use Reply Block so the receiver can
    // reply anonymously. Returns the SURB id.
    StdLogosResult sendMixMessageWithSurb(const std::string& destPeerId,
                                          const std::string& proto,
                                          const std::vector<uint8_t>& payload);

    // Sends `payload` back along a previously received SURB.
    StdLogosResult sendMixSurbReply(const std::vector<uint8_t>& surb,
                                    const std::vector<uint8_t>& payload);

    // Mix-node inventory ---------------------------------------------------
    // Peers discovered via Logos Service Discovery that advertise the mix
    // capability and a routable multiaddr + X25519 pubkey (Extensible Peer
    // Records per LIP LOGOS-MIXNET).
    StdLogosResult listMixPeers();

    // Cover traffic --------------------------------------------------------
    // Reads/updates the CONSTANT_RATE cover-traffic ratio (LIP LOGOS-MIXNET
    // default 0.7).
    StdLogosResult getCoverTrafficRate();
    StdLogosResult setCoverTrafficRate(double rate);

    // Diagnostics ----------------------------------------------------------
    // Aggregated counters. Shape TBD; will follow openmetrics-module's
    // convention once the Nim FFI exposes the underlying counters.
    LogosMap collectMetrics();
};

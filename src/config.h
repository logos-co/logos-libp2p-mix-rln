#pragma once

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// LIP LOGOS-MIXNET protocol constants. Fixed by the spec; not configurable.
namespace lip_mixnet {
inline constexpr int kPathLength = 3;
inline constexpr int kSphinxPacketSize = 4908;
inline constexpr int kSphinxHeaderSize = 924;
inline constexpr int kSphinxMaxPayload = 3984;
inline constexpr int kRlnProofSize = 288;
inline constexpr const char* kCoverStrategy = "constant_rate";
} // namespace lip_mixnet

// libp2p transport selector. Kept as a plain enum here so the header does not
// need to include the Nim FFI header; the plugin translates it before crossing
// the FFI boundary.
enum class TransportKind { Tcp, Quic };

struct MixOptions {
    // Hex-encoded X25519 private key. Empty → generate on start.
    std::vector<uint8_t> mixPrivKey = {};
    // Cover-traffic rate ratio (LIP LOGOS-MIXNET: 0.7).
    double coverRateFraction = 0.7;
};

struct RlnOptions {
    std::string keystorePath;
    std::string keystorePassword;
    std::string treePath;
    std::string rlnResourcesPath;
    std::string rlnIdentifier;

    // LIP LOGOS-MIXNET marks these TBD. Placeholder defaults; MUST be pinned
    // before mainnet use.
    int epochDurationSeconds = 1;
    int period = 1;
    int messagingRate = 10;
    int maxEpochGap = 20;
    int userMessageLimit = 100;
    int acceptableRootWindowSize = 5;
    std::string stakedFund;

    std::string membershipContentTopic = "/mix/rln/membership/v1";
    std::string proofMetadataContentTopic = "/mix/rln/metadata/v1";
    int coordCluster = 0;
};

struct DiscoveryOptions {
    bool mountServiceDiscovery = true;
    std::string serviceId = "logos.mixnet";
};

struct Libp2pMixRlnModuleOptions {
    // libp2p host options — mirror logos-libp2p-module's shape so the two
    // modules use the same vocabulary.
    std::vector<std::string> addrs = {};
    std::vector<std::pair<std::string, std::vector<std::string>>> bootstrapNodes = {};
    TransportKind transport = TransportKind::Tcp;
    int maxConnections = 50;
    int maxInConnections = 25;
    int maxOutConnections = 25;
    int maxConnsPerPeer = 1;
    std::vector<uint8_t> privKey = {};

    MixOptions mix = {};
    RlnOptions rln = {};
    DiscoveryOptions discovery = {};

    static Libp2pMixRlnModuleOptions load();
    static Libp2pMixRlnModuleOptions fromJson(const std::string& raw, bool& ok, std::string* err = nullptr);
};

namespace libp2p_mix_rln_config {

inline std::string readSource() {
    const char* cfg = std::getenv("LIBP2P_MIX_RLN_MODULE_CONFIG");
    if (!cfg || !*cfg) {
        return "";
    }
    std::string value(cfg);
    auto firstChar = value.find_first_not_of(" \t\r\n");
    if (firstChar != std::string::npos && value[firstChar] == '{') {
        return value;
    }
    std::ifstream f(value);
    if (!f) {
        fprintf(stderr, "libp2p_mix_rln_module: cannot read config file %s\n", value.c_str());
        return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::vector<uint8_t> decodeHex(const std::string& in) {
    std::string s = in;
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s.erase(0, 2);
    if (s.size() % 2 != 0) throw std::invalid_argument("hex length not even");
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            throw std::invalid_argument("bad hex char");
        };
        out.push_back(static_cast<uint8_t>((hex(s[i]) << 4) | hex(s[i + 1])));
    }
    return out;
}

inline TransportKind parseTransport(const nlohmann::json& j, TransportKind fallback) {
    auto it = j.find("transport");
    if (it == j.end() || !it->is_string()) return fallback;
    std::string t = it->get<std::string>();
    if (t == "tcp") return TransportKind::Tcp;
    if (t == "quic" || t == "quic-v1") return TransportKind::Quic;
    return fallback;
}

inline void applyMix(const nlohmann::json& j, MixOptions& m) {
    if (auto it = j.find("mixPrivKey"); it != j.end()) {
        if (!it->is_string()) throw std::invalid_argument("mix.mixPrivKey must be a string");
        m.mixPrivKey = decodeHex(it->get<std::string>());
    }
    if (auto it = j.find("cover"); it != j.end() && it->is_object()) {
        m.coverRateFraction = it->value("rateFraction", m.coverRateFraction);
    }
}

inline void applyRln(const nlohmann::json& j, RlnOptions& r) {
    r.keystorePath = j.value("keystorePath", r.keystorePath);
    r.keystorePassword = j.value("keystorePassword", r.keystorePassword);
    r.treePath = j.value("treePath", r.treePath);
    r.rlnResourcesPath = j.value("rlnResourcesPath", r.rlnResourcesPath);
    r.rlnIdentifier = j.value("rlnIdentifier", r.rlnIdentifier);
    r.epochDurationSeconds = j.value("epochDurationSeconds", r.epochDurationSeconds);
    r.period = j.value("period", r.period);
    r.messagingRate = j.value("messagingRate", r.messagingRate);
    r.maxEpochGap = j.value("maxEpochGap", r.maxEpochGap);
    r.userMessageLimit = j.value("userMessageLimit", r.userMessageLimit);
    r.acceptableRootWindowSize = j.value("acceptableRootWindowSize", r.acceptableRootWindowSize);
    r.stakedFund = j.value("stakedFund", r.stakedFund);
    r.membershipContentTopic = j.value("membershipContentTopic", r.membershipContentTopic);
    r.proofMetadataContentTopic = j.value("proofMetadataContentTopic", r.proofMetadataContentTopic);
    r.coordCluster = j.value("coordCluster", r.coordCluster);
}

inline void applyDiscovery(const nlohmann::json& j, DiscoveryOptions& d) {
    d.mountServiceDiscovery = j.value("mountServiceDiscovery", d.mountServiceDiscovery);
    d.serviceId = j.value("serviceId", d.serviceId);
}

inline void apply(const nlohmann::json& j, Libp2pMixRlnModuleOptions& o) {
    if (!j.is_object()) return;

    o.addrs = j.value("addrs", o.addrs);
    if (auto it = j.find("bootstrapNodes"); it != j.end() && it->is_array()) {
        o.bootstrapNodes.clear();
        for (const auto& n : *it) {
            o.bootstrapNodes.emplace_back(n.value("peerId", std::string{}),
                                          n.value("addrs", std::vector<std::string>{}));
        }
    }
    o.transport = parseTransport(j, o.transport);
    if (auto it = j.find("privKey"); it != j.end()) {
        if (!it->is_string()) throw std::invalid_argument("privKey must be a string");
        o.privKey = decodeHex(it->get<std::string>());
    }
    o.maxConnections = j.value("maxConnections", o.maxConnections);
    o.maxInConnections = j.value("maxInConnections", o.maxInConnections);
    o.maxOutConnections = j.value("maxOutConnections", o.maxOutConnections);
    o.maxConnsPerPeer = j.value("maxConnsPerPeer", o.maxConnsPerPeer);

    if (auto it = j.find("mix"); it != j.end() && it->is_object()) applyMix(*it, o.mix);
    if (auto it = j.find("rln"); it != j.end() && it->is_object()) applyRln(*it, o.rln);
    if (auto it = j.find("discovery"); it != j.end() && it->is_object()) applyDiscovery(*it, o.discovery);
}

} // namespace libp2p_mix_rln_config

inline Libp2pMixRlnModuleOptions Libp2pMixRlnModuleOptions::fromJson(const std::string& raw, bool& ok, std::string* err) {
    ok = true;
    if (err) err->clear();
    auto j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_discarded()) {
        ok = false;
        if (err) *err = "malformed JSON";
        return {};
    }
    Libp2pMixRlnModuleOptions opts;
    try {
        libp2p_mix_rln_config::apply(j, opts);
    } catch (const std::exception& e) {
        fprintf(stderr, "libp2p_mix_rln_module: invalid config: %s\n", e.what());
        ok = false;
        if (err) *err = e.what();
        return {};
    }
    return opts;
}

inline Libp2pMixRlnModuleOptions Libp2pMixRlnModuleOptions::load() {
    std::string raw = libp2p_mix_rln_config::readSource();
    if (raw.empty()) return {};
    bool ok = false;
    std::string err;
    auto opts = fromJson(raw, ok, &err);
    if (!ok) {
        fprintf(stderr, "libp2p_mix_rln_module: ignoring invalid LIBP2P_MIX_RLN_MODULE_CONFIG: %s\n", err.c_str());
        return {};
    }
    return opts;
}

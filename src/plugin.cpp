#include "plugin.h"

// Scaffold implementation. Every FFI-backed method returns a "not implemented"
// result until the Nim FFI facade (`libp2p_mix_rln`) is wired in — see
// plugin.h and README.md for the roadmap. The two things that DO work today
// are (1) config parsing (validated by tests/unit_config.cpp) and (2) the
// module-loader plumbing produced by the Logos Core codegen.

namespace {
constexpr char kModuleVersion[] = "0.1.0";
constexpr const char* kNotImplemented =
    "not implemented — Nim FFI facade `libp2p_mix_rln` not yet wired";

StdLogosResult notImplemented() {
    return {false, {}, kNotImplemented};
}
} // namespace

Libp2pMixRlnModuleImpl::Libp2pMixRlnModuleImpl(const Libp2pMixRlnModuleOptions& options) {
    // Options are parsed and held for the day the FFI facade is wired. Until
    // then they are only observable through tests.
    (void)options;
}

Libp2pMixRlnModuleImpl::~Libp2pMixRlnModuleImpl() = default;

bool Libp2pMixRlnModuleImpl::ok() {
    // Loader considers the module OK to load — the individual ops report their
    // own not-implemented status.
    return true;
}

StdLogosResult Libp2pMixRlnModuleImpl::status() {
    nlohmann::json j = {
        {"version", kModuleVersion},
        {"state", "scaffold"},
        {"message", kNotImplemented}
    };
    return {true, j, ""};
}

StdLogosResult Libp2pMixRlnModuleImpl::createNode(const std::string& configJson) {
    bool ok = false;
    std::string err;
    (void)Libp2pMixRlnModuleOptions::fromJson(configJson, ok, &err);
    if (!ok) return {false, {}, "invalid config: " + err};
    return notImplemented();
}

StdLogosResult Libp2pMixRlnModuleImpl::start()  { return notImplemented(); }
StdLogosResult Libp2pMixRlnModuleImpl::stop()   { return notImplemented(); }

StdLogosResult Libp2pMixRlnModuleImpl::getNodeInfo(const std::string& field) {
    if (field == "Version") return {true, std::string(kModuleVersion), ""};
    return notImplemented();
}

StdLogosResult Libp2pMixRlnModuleImpl::registerRlnMembership()      { return notImplemented(); }
StdLogosResult Libp2pMixRlnModuleImpl::hasRlnMembership()           { return notImplemented(); }

StdLogosResult Libp2pMixRlnModuleImpl::sendMixMessage(const std::string&,
                                                     const std::string&,
                                                     const std::vector<uint8_t>&) {
    return notImplemented();
}

StdLogosResult Libp2pMixRlnModuleImpl::sendMixMessageWithSurb(const std::string&,
                                                             const std::string&,
                                                             const std::vector<uint8_t>&) {
    return notImplemented();
}

StdLogosResult Libp2pMixRlnModuleImpl::sendMixSurbReply(const std::vector<uint8_t>&,
                                                       const std::vector<uint8_t>&) {
    return notImplemented();
}

StdLogosResult Libp2pMixRlnModuleImpl::listMixPeers()               { return notImplemented(); }
StdLogosResult Libp2pMixRlnModuleImpl::getCoverTrafficRate()        { return notImplemented(); }
StdLogosResult Libp2pMixRlnModuleImpl::setCoverTrafficRate(double)  { return notImplemented(); }

LogosMap Libp2pMixRlnModuleImpl::collectMetrics() {
    return {};
}

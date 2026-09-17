// Pure config logic (no Libp2pMixRlnModuleImpl, links without libp2p_mix_rln.so).

#include <logos_test.h>

#include "../src/config.h"

#include <cstdlib>

namespace {
struct ScopedModuleConfig {
    explicit ScopedModuleConfig(const std::string& value) {
        setenv("LIBP2P_MIX_RLN_MODULE_CONFIG", value.c_str(), 1);
    }
    ~ScopedModuleConfig() { unsetenv("LIBP2P_MIX_RLN_MODULE_CONFIG"); }
};
}

using json = nlohmann::json;
namespace cfg = libp2p_mix_rln_config;

LOGOS_TEST(defaults_match_lip_mixnet) {
    Libp2pMixRlnModuleOptions o;
    // LIP LOGOS-MIXNET fixes the cover-traffic ratio.
    LOGOS_ASSERT_EQ(o.mix.coverRateFraction, 0.7);
    LOGOS_ASSERT_FALSE(o.mix.allowSend);
    LOGOS_ASSERT_FALSE(o.mix.allowExit);
    LOGOS_ASSERT_EQ(o.maxConnsPerPeer, 2);
    // The RLN Relay coord topics have placeholder defaults until the spec pins them.
    LOGOS_ASSERT_EQ(o.rln.membershipContentTopic, std::string("/mix/rln/membership/v1"));
    LOGOS_ASSERT_EQ(o.rln.proofMetadataContentTopic, std::string("/mix/rln/metadata/v1"));
}

LOGOS_TEST(from_json_overlays_nested_sections) {
    auto raw = R"({
        "addrs": ["/ip4/0.0.0.0/tcp/9100"],
        "transport": "quic",
        "maxConnections": 200,
        "mix": { "cover": { "rateFraction": 0.5 } },
        "rln": {
            "keystorePath": "/tmp/ks.json",
            "epochDurationSeconds": 10,
            "membershipContentTopic": "/mix/rln/membership/v2"
        }
    })";
    bool ok = false;
    std::string err;
    auto o = Libp2pMixRlnModuleOptions::fromJson(raw, ok, &err);
    LOGOS_ASSERT_TRUE(ok);
    LOGOS_ASSERT_EQ(o.addrs.size(), 1u);
    LOGOS_ASSERT_TRUE(o.transport == TransportKind::Quic);
    LOGOS_ASSERT_EQ(o.maxConnections, 200);
    LOGOS_ASSERT_EQ(o.mix.coverRateFraction, 0.5);
    LOGOS_ASSERT_EQ(o.rln.keystorePath, std::string("/tmp/ks.json"));
    LOGOS_ASSERT_EQ(o.rln.epochDurationSeconds, 10);
    LOGOS_ASSERT_EQ(o.rln.membershipContentTopic, std::string("/mix/rln/membership/v2"));
}

LOGOS_TEST(from_json_rejects_malformed) {
    bool ok = true;
    std::string err;
    auto o = Libp2pMixRlnModuleOptions::fromJson("{not json", ok, &err);
    (void)o;
    LOGOS_ASSERT_FALSE(ok);
    LOGOS_ASSERT_FALSE(err.empty());
}

LOGOS_TEST(load_reads_env_inline_json) {
    ScopedModuleConfig s(R"({"maxConnections": 77})");
    auto o = Libp2pMixRlnModuleOptions::load();
    LOGOS_ASSERT_EQ(o.maxConnections, 77);
}

LOGOS_TEST(endpoint_roles_are_independent_opt_ins) {
    bool ok = false;
    auto sender = Libp2pMixRlnModuleOptions::fromJson(
        R"({"mix":{"allowSend":true}})", ok);
    LOGOS_ASSERT_TRUE(ok);
    LOGOS_ASSERT_TRUE(sender.mix.allowSend);
    LOGOS_ASSERT_FALSE(sender.mix.allowExit);
    auto exit = Libp2pMixRlnModuleOptions::fromJson(
        R"({"mix":{"allowExit":true}})", ok);
    LOGOS_ASSERT_TRUE(ok);
    LOGOS_ASSERT_FALSE(exit.mix.allowSend);
    LOGOS_ASSERT_TRUE(exit.mix.allowExit);
    Libp2pMixRlnModuleOptions::fromJson(
        R"({"mix":{"allowSend":"true"}})", ok);
    LOGOS_ASSERT_FALSE(ok);
    Libp2pMixRlnModuleOptions::fromJson(
        R"({"mix":{"allowExit":1}})", ok);
    LOGOS_ASSERT_FALSE(ok);
}

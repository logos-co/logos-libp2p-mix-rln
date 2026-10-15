#include <logos_test.h>
#include "../src/plugin.h"

LOGOS_TEST(status_does_not_wait_for_node_operations) {
    Libp2pMixRlnModuleImpl module{Libp2pMixRlnModuleOptions{}};
    std::unique_lock<std::mutex> operation(module.m_callMutex);
    auto pending = std::async(std::launch::async, [&] { return module.status(); });
    auto completed = pending.wait_for(std::chrono::seconds(1));
    // Release before asserting so a regression fails instead of hanging teardown.
    operation.unlock();
    auto result = pending.get();
    LOGOS_ASSERT_TRUE(completed == std::future_status::ready);
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.at("state").get<std::string>(), "uninitialized");
    LOGOS_ASSERT_FALSE(result.value.contains("initError"));
}

LOGOS_TEST(status_reports_invalid_creation_config) {
    Libp2pMixRlnModuleImpl module{Libp2pMixRlnModuleOptions{}};
    auto creation = module.createNode("{");
    LOGOS_ASSERT_FALSE(creation.success);
    auto result = module.status();
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.at("initError").get<std::string>(), creation.error);
    LOGOS_ASSERT_EQ(result.value.at("state").get<std::string>(), "uninitialized");
    LOGOS_ASSERT_FALSE(module.start().success);
    LOGOS_ASSERT_FALSE(module.stop().success);
    LOGOS_ASSERT_EQ(module.status().value, result.value);
}

LOGOS_TEST(status_reports_ffi_creation_failure) {
    Libp2pMixRlnModuleImpl module{Libp2pMixRlnModuleOptions{}};
    // The shared backend requires a registry ID; this fails inside the FFI.
    auto creation = module.createNode("{}");
    LOGOS_ASSERT_FALSE(creation.success);
    auto result = module.status();
    LOGOS_ASSERT_EQ(result.value.at("state").get<std::string>(), "failed");
    LOGOS_ASSERT_EQ(result.value.at("initError").get<std::string>(), creation.error);
}

LOGOS_TEST(status_exposes_public_config_without_key_material) {
    bool ok = false;
    auto opts = Libp2pMixRlnModuleOptions::fromJson(R"({
        "privKey":"cafebabe", "transport":"quic", "maxConnections":17,
        "mix":{"mixPrivKey":"deadbeef", "cover":{"rateFraction":0.2},
               "reply":{"timeoutMs":2500}},
        "rln":{"registryId":"test", "registrationOptionsJson":"private options"}
    })", ok);
    LOGOS_ASSERT_TRUE(ok);
    Libp2pMixRlnModuleImpl module{opts};
    auto snapshot = module.status().value;
    auto config = snapshot.at("config");
    LOGOS_ASSERT_TRUE(snapshot.at("membership").is_null());
    LOGOS_ASSERT_EQ(snapshot.at("version").get<std::string>(), "0.1.0");
    LOGOS_ASSERT_EQ(config.at("transport").get<std::string>(), "quic");
    LOGOS_ASSERT_EQ(config.at("maxConnections").get<int>(), 17);
    LOGOS_ASSERT_EQ(config.at("mix").at("reply").at("timeoutMs").get<int>(), 2500);
    LOGOS_ASSERT_FALSE(config.contains("privKey"));
    LOGOS_ASSERT_FALSE(config.at("mix").contains("mixPrivKey"));
    LOGOS_ASSERT_FALSE(config.at("rln").contains("registrationOptionsJson"));
    LOGOS_ASSERT_FALSE(module.createNode("{").success);
    LOGOS_ASSERT_EQ(module.status().value.at("config"), config);
}

LOGOS_TEST_MAIN()

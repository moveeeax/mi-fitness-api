/**
 * @file test_redact.cpp
 * @brief Unit tests for the --dump-config credential masking.
 *
 * Pure unit bucket: operates on a plain json tree, no Config bootstrap.
 * The property under test: after mask_secrets, no credential value from the
 * input survives verbatim, while the set-vs-unset signal does.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "utils/Redact.hpp"

using nlohmann::json;

namespace {

TEST(Redact, KeyIsSecretMatchesUsualSuspectsCaseInsensitively) {
    EXPECT_TRUE(Utils::Redact::key_is_secret("password"));
    EXPECT_TRUE(Utils::Redact::key_is_secret("JWT_SECRET"));
    EXPECT_TRUE(Utils::Redact::key_is_secret("apiToken"));
    EXPECT_TRUE(Utils::Redact::key_is_secret("s3_access_key"));
    EXPECT_FALSE(Utils::Redact::key_is_secret("pool_size"));
    EXPECT_FALSE(Utils::Redact::key_is_secret("host"));
}

TEST(Redact, MaskDsnUserinfoHidesOnlyThePassword) {
    EXPECT_EQ(Utils::Redact::mask_dsn_userinfo("postgresql://app:s3cr3t@pg:5432/app"),
              "postgresql://app:***@pg:5432/app");
    EXPECT_EQ(Utils::Redact::mask_dsn_userinfo("tcp://redis:6379"), "tcp://redis:6379");
    EXPECT_EQ(Utils::Redact::mask_dsn_userinfo("postgresql://app@pg/app"), "postgresql://app@pg/app");
    EXPECT_EQ(Utils::Redact::mask_dsn_userinfo("no dsn here"), "no dsn here");
}

TEST(Redact, MaskSecretsRedactsByKeyAndKeepsSetSignal) {
    json cfg = {{"auth", {{"jwt", {{"secret", "super-secret-value-123"}}}}},
                {"database", {{"primary", "postgresql://app:pw@pg:5432/app"}, {"pool_size", 5}}},
                {"mail", {{"smtp_password", ""}}},
                {"app", {{"name", "App"}}}};
    Utils::Redact::mask_secrets(cfg);
    const std::string dumped = cfg.dump();
    EXPECT_EQ(dumped.find("super-secret-value-123"), std::string::npos);
    EXPECT_EQ(cfg["auth"]["jwt"]["secret"], "***set (22 chars)");
    EXPECT_EQ(cfg["database"]["primary"], "postgresql://app:***@pg:5432/app");
    EXPECT_EQ(cfg["database"]["pool_size"], 5);
    EXPECT_EQ(cfg["mail"]["smtp_password"], "") << "empty must stay empty — set-vs-unset is the point of the dump";
    EXPECT_EQ(cfg["app"]["name"], "App");
}

TEST(Redact, MaskSecretsRecursesThroughArrays) {
    json cfg = {{"list", json::array({{{"token", "abc123"}}})}};
    Utils::Redact::mask_secrets(cfg);
    EXPECT_EQ(cfg["list"][0]["token"], "***set (6 chars)");
}

}  // namespace

/**
 * @file test_public_paths.cpp
 * @brief Unit tests for the public-paths merge semantics: `api.public_paths`
 *        is a FULL OVERRIDE of Utils::Strings::kDefaultPublicPathsCsv, while
 *        `api.public_paths_extra` is ADDITIVE — appended to whichever base
 *        won. Guards the twice-hit incident (the content module, then a
 *        downstream fork's PayPal webhook) where a route added only to the
 *        compile-time default silently 401'd under any deployment that set
 *        the override key.
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "security/Auth.hpp"
#include "test_helpers.hpp"
#include "utils/Config.hpp"
#include "utils/Strings.hpp"

class PublicPathsTest : public ::testing::Test {
protected:
    std::string test_config_file = "public_paths_test_config.json";

    void write_config(const std::string& body) {
        std::ofstream file(test_config_file);
        file << body;
    }

    void TearDown() override {
        unsetenv("API_PUBLIC_PATHS");
        unsetenv("API_PUBLIC_PATHS_EXTRA");
        if (std::filesystem::exists(test_config_file)) {
            std::filesystem::remove(test_config_file);
        }
        TestHelpers::reset_all_globals();
    }
};

// ── The pure merge helper ────────────────────────────────────────────────────

TEST_F(PublicPathsTest, MergeCsvSetsAppendsExtraToBase) {
    auto set = Utils::Strings::merge_csv_sets("/a,/b", "/c,/d");
    EXPECT_EQ(set.size(), 4u);
    EXPECT_TRUE(set.count("/a"));
    EXPECT_TRUE(set.count("/d"));
}

TEST_F(PublicPathsTest, MergeCsvSetsEmptyExtraIsBase) {
    auto set = Utils::Strings::merge_csv_sets("/a,/b", "");
    EXPECT_EQ(set, Utils::Strings::split_csv_set("/a,/b"));
}

// ── End-to-end through Config + Auth::load_config_from_global ───────────────

TEST_F(PublicPathsTest, ExtraAppendsToBuiltInDefault) {
    // No api block in the file, no override env — the compile-time default is
    // the base; the extra key must ADD to it, not replace it.
    write_config("{}");
    setenv("API_PUBLIC_PATHS_EXTRA", "/api/v1/billing/paypal/webhook", 1);
    Config::initialize(test_config_file);

    auto cfg = Security::Auth::load_config_from_global();
    EXPECT_TRUE(cfg.public_paths.count("/healthz")) << "default entries must survive";
    EXPECT_TRUE(cfg.public_paths.count("/api/v1/billing/paypal/webhook"));
}

TEST_F(PublicPathsTest, OverrideStillReplacesWholeDefault) {
    // The old key keeps its full-override contract for those who rely on it.
    write_config(R"({"api": {"public_paths": "/only"}})");
    Config::initialize(test_config_file);

    auto cfg = Security::Auth::load_config_from_global();
    EXPECT_TRUE(cfg.public_paths.count("/only"));
    EXPECT_FALSE(cfg.public_paths.count("/healthz")) << "override means override";
}

TEST_F(PublicPathsTest, ExtraAppendsToOverrideToo) {
    write_config(R"({"api": {"public_paths": "/only", "public_paths_extra": "/hook"}})");
    Config::initialize(test_config_file);

    auto cfg = Security::Auth::load_config_from_global();
    EXPECT_TRUE(cfg.public_paths.count("/only"));
    EXPECT_TRUE(cfg.public_paths.count("/hook"));
    EXPECT_FALSE(cfg.public_paths.count("/healthz"));
}

TEST_F(PublicPathsTest, ExtraEntriesMatchLikeAnyPublicPath) {
    // Glob (trailing *) entries added via the extra key behave identically.
    auto set = Utils::Strings::merge_csv_sets("/a", "/webhooks/*");
    EXPECT_TRUE(Utils::Strings::path_is_public(set, "/webhooks/paypal"));
    EXPECT_FALSE(Utils::Strings::path_is_public(set, "/webhook"));
}

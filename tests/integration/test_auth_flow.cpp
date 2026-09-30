/**
 * @file test_auth_flow.cpp
 * @brief Integration tests for the auth flow.
 *
 * Drives the controller methods directly (no real HTTP listener) but
 * with full subsystem init — Postgres for users/roles, Redis for refresh
 * revocation. Skips when those aren't reachable.
 *
 * Coverage:
 *   - login bad password → 401 generic
 *   - login good password → 200 + Set-Cookie
 *   - me with valid principal → 200 + user
 *   - refresh path-only smoke (cookie wiring covered by middleware tests
 *     in stage 5; here we exercise the controller's own logic via the
 *     refresh cookie helper)
 */

#include <filesystem>
#include <fstream>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/AuthController.hpp"
#include "database/Database.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Password.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-auth-flow-padding";

class AuthFlowTest : public TestHelpers::CoreBackedTest {
protected:
    Api::AuthController controller;

    std::string config_file_name() const override { return "auth_flow_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["auth"]["cookies"]["enabled"] = true;
        // Local dev — no https in the test container.
        cfg["auth"]["cookies"]["secure"] = false;
        cfg["database"]["migrations_enabled"] = true;
        // Point at a dedicated migrations dir so we run 001_users_and_roles.
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Wipe users between tests so create() doesn't conflict.
        TestHelpers::truncate_users();
    }

    static HttpRequestPtr make_post(const json& body) { return TestHelpers::post_json(body); }

    // Регистрация из API удалена зачисткой 2026-09-30: пользователи заводятся
    // админом. Тестам достаточно строки в базе с захешированным паролем.
    static void seed_user(const std::string& email, const std::string& password) {
        Repositories::RoleRepository roles;
        auto role = roles.find_default();
        ASSERT_TRUE(role.has_value());
        Repositories::UserRepository repo;
        repo.create(email,
                    Security::Password::hash(password),
                    std::nullopt,
                    std::nullopt,
                    role->id,
                    /*confirmed=*/true);
    }
};

TEST_F(AuthFlowTest, loginWrongPasswordReturns401Generic) {
    seed_user("carol@example.com", "rightpassword");

    // Wrong password.
    auto bad = make_post({{"email", "carol@example.com"}, {"password", "WRONGpassword"}});
    HttpResponsePtr badr;
    controller.login(bad, [&](const HttpResponsePtr& r) { badr = r; });
    ASSERT_EQ(badr->statusCode(), k401Unauthorized);
    auto body = json::parse(std::string(badr->body()));
    // Generic code — no "user_not_found" leakage.
    EXPECT_EQ(body["error"].get<std::string>(), "invalid_credentials");

    // The failed attempt is recorded in the audit trail so brute-force is
    // visible (previously only successful admin actions were audited).
    long audited = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params(
            "SELECT COUNT(*) FROM audit_log WHERE action = 'auth.login_failed' AND details->>'email' = $1",
            "carol@example.com");
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_EQ(audited, 1);
}

TEST_F(AuthFlowTest, loginSucceedsAndSetsCookies) {
    seed_user("dan@example.com", "rightpassword");

    auto good = make_post({{"email", "dan@example.com"}, {"password", "rightpassword"}});
    HttpResponsePtr resp;
    controller.login(good, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);

    // Session cookies are attached via addCookie() (so Drogon serializes one
    // Set-Cookie line per cookie); they live in the response's cookie map,
    // not the header map, until the response is rendered on the wire.
    const auto& cookies = resp->cookies();
    EXPECT_GE(cookies.size(), 2u);  // access + refresh
    for (const auto& [name, c] : cookies) {
        EXPECT_TRUE(c.isHttpOnly()) << "cookie " << name << " must be HttpOnly";
        EXPECT_EQ(c.sameSite(), drogon::Cookie::SameSite::kLax) << "cookie " << name << " must be SameSite=Lax";
    }
}

TEST_F(AuthFlowTest, loginUnknownEmailReturns401Generic) {
    auto req = make_post({{"email", "noone@example.com"}, {"password", "anything"}});
    HttpResponsePtr resp;
    controller.login(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k401Unauthorized);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "invalid_credentials");
}

TEST_F(AuthFlowTest, meReturnsUserForValidPrincipal) {
    // Create a user, then synthesize a principal in req->attributes(),
    // mirroring what the auth middleware would have done after verifying
    // an access cookie.
    seed_user("eve@example.com", "rightpassword");

    Repositories::UserRepository repo;
    auto user = repo.find_by_email("eve@example.com");
    ASSERT_TRUE(user.has_value());

    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    Security::Auth::AuthPrincipal principal;
    principal.subject = user->id;
    req->attributes()->insert(Security::Auth::kPrincipalAttr, principal);

    HttpResponsePtr resp;
    controller.me(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["user"]["email"].get<std::string>(), "eve@example.com");
}

TEST_F(AuthFlowTest, meRefuses401WhenNoPrincipal) {
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    HttpResponsePtr resp;
    controller.me(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k401Unauthorized);
}

}  // namespace

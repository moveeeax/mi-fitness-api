/**
 * @file test_templates.cpp
 * @brief Unit tests for the inja email-template renderer.
 *
 * Pure unit bucket: renders against a temp directory created per test —
 * no Config, no network, no sidecars. The repo's real templates under
 * templates/email are exercised too (substitution smoke), so a broken
 * placeholder fails here instead of as a swallowed warn at send time.
 */

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "email/Templates.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

class TemplatesTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<TestHelpers::ScopedEnv> dir_env_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() / "tpl_test";
        fs::create_directories(dir_);
        dir_env_ = std::make_unique<TestHelpers::ScopedEnv>("MAIL_TEMPLATES_DIR", dir_.string());
    }

    void TearDown() override {
        dir_env_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void write(const std::string& file, const std::string& content) { std::ofstream(dir_ / file) << content; }
};

TEST_F(TemplatesTest, renderSubstitutesContext) {
    write("greet.txt", "Hello {{ user.name }}, welcome to {{ app_name }}!");
    auto out = Email::Templates::render("greet", "txt", {{"user", {{"name", "Ada"}}}, {"app_name", "App"}});
    EXPECT_EQ(out, "Hello Ada, welcome to App!");
}

TEST_F(TemplatesTest, renderPairReturnsBothVariants) {
    write("note.txt", "text: {{ v }}");
    write("note.html", "<b>{{ v }}</b>");
    auto pair = Email::Templates::render_pair("note", {{"v", "x"}});
    EXPECT_EQ(pair.text, "text: x");
    EXPECT_EQ(pair.html, "<b>x</b>");
}

TEST_F(TemplatesTest, missingTemplateThrows) {
    EXPECT_THROW(Email::Templates::render("nope", "txt", json::object()), std::runtime_error);
}

TEST_F(TemplatesTest, missingHtmlVariantThrowsFromRenderPair) {
    write("halfpair.txt", "only text");
    EXPECT_THROW(Email::Templates::render_pair("halfpair", json::object()), std::runtime_error);
}

TEST_F(TemplatesTest, defaultContextWithoutConfigHasAppFields) {
    auto ctx = Email::Templates::default_context();
    EXPECT_EQ(ctx["app_name"], "App");
    EXPECT_TRUE(ctx.contains("base_url"));
}

// inja has no autoescaping: the html render path must escape user-controlled
// context strings, or a self-registered display name becomes attacker HTML
// delivered from our From:/DKIM. The txt path must stay verbatim.
TEST_F(TemplatesTest, escapeHtmlEscapesMarkupSignificantChars) {
    EXPECT_EQ(Email::Templates::escape_html(R"(<b>&"'</b>)"), "&lt;b&gt;&amp;&quot;&#39;&lt;/b&gt;");
    EXPECT_EQ(Email::Templates::escape_html("plain text 123"), "plain text 123");
}

TEST_F(TemplatesTest, htmlRenderEscapesContextValues) {
    write("evil.html", "<p>Hi {{ user.name }}</p>");
    auto out = Email::Templates::render("evil", "html", {{"user", {{"name", "<script>alert('x')</script>"}}}});
    EXPECT_EQ(out.find("<script>"), std::string::npos);
    EXPECT_NE(out.find("&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;"), std::string::npos);
}

TEST_F(TemplatesTest, txtRenderStaysVerbatim) {
    write("evil.txt", "Hi {{ name }}");
    auto out = Email::Templates::render("evil", "txt", {{"name", "Tom & Jerry <cat@mouse>"}});
    EXPECT_EQ(out, "Hi Tom & Jerry <cat@mouse>");
}

TEST_F(TemplatesTest, htmlEscapingCoversNestedObjectsAndArrays) {
    write("deep.html", "{{ a.0 }}|{{ b.c }}");
    auto out = Email::Templates::render("deep", "html", {{"a", {"<x>"}}, {"b", {{"c", "\"q\""}}}});
    EXPECT_EQ(out, "&lt;x&gt;|&quot;q&quot;");
}

// The repo's real templates: every shipped pair must render against the
// context AccountEmails builds, with the link placeholder substituted.
TEST_F(TemplatesTest, shippedTemplatesRenderWithAccountContext) {
    if (!fs::exists("templates/email"))
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    dir_env_ = std::make_unique<TestHelpers::ScopedEnv>("MAIL_TEMPLATES_DIR", "templates/email");

    json ctx = {{"app_name", "App"},
                {"base_url", "http://x"},
                {"user", {{"email", "a@b.c"}, {"full_name", "A B"}, {"first_name", "A"}, {"last_name", "B"}}},
                {"confirm_link", "http://x/account/confirm/T"},
                {"reset_link", "http://x/account/reset-password/T"},
                {"change_email_link", "http://x/account/change-email/T"},
                {"invite_link", "http://x/account/join-from-invite/T"},
                {"new_email", "n@b.c"}};

    const char* links[][2] = {{"confirm", "http://x/account/confirm/T"},
                              {"reset_password", "http://x/account/reset-password/T"},
                              {"change_email", "http://x/account/change-email/T"},
                              {"invite", "http://x/account/join-from-invite/T"}};
    for (const auto& [name, link] : links) {
        SCOPED_TRACE(name);
        auto pair = Email::Templates::render_pair(name, ctx);
        EXPECT_NE(pair.text.find(link), std::string::npos);
        EXPECT_NE(pair.html.find(link), std::string::npos);
        EXPECT_EQ(pair.text.find("{{"), std::string::npos) << "unsubstituted placeholder in " << name << ".txt";
    }
}

}  // namespace

/**
 * @file test_mailer_mime.cpp
 * @brief Unit tests for MIME assembly header hardening.
 *
 * Pure unit bucket: build_mime is a deterministic string builder — no SMTP,
 * no Config. The interesting property is that no config- or user-derived
 * value can smuggle a CRLF (extra header) or break out of the From
 * display-name quoted-string.
 */

#include <string>

#include <gtest/gtest.h>

#include "email/Mailer.hpp"

namespace {

Email::MailerConfig base_config() {
    Email::MailerConfig cfg;
    cfg.from = "noreply@example.com";
    cfg.from_name = "App";
    cfg.subject_prefix = "[App]";
    return cfg;
}

Email::Message base_message() {
    Email::Message msg;
    msg.to = "user@example.com";
    msg.subject = "Hello";
    msg.text_body = "hi";
    msg.html_body = "<p>hi</p>";
    return msg;
}

// Count occurrences of a header prefix at line starts.
int count_headers(const std::string& mime, const std::string& header) {
    int n = 0;
    std::string::size_type pos = 0;
    const std::string needle = "\r\n" + header;
    if (mime.rfind(header, 0) == 0)
        ++n;
    while ((pos = mime.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

TEST(MailerMime, StripQuotedStringDropsCrlfQuoteBackslash) {
    EXPECT_EQ(Email::detail::strip_quoted_string("Ann \"O'War\"\\\r\nBcc: x"), "Ann O'WarBcc: x");
}

TEST(MailerMime, FromNameCannotEscapeQuotedString) {
    auto cfg = base_config();
    cfg.from_name = "Evil\" <spoof@evil.test>, \"X";
    const auto mime = Email::detail::build_mime(cfg, base_message(), "<id@test>");
    EXPECT_EQ(mime.find("\"Evil\" <spoof@evil.test>"), std::string::npos);
    EXPECT_NE(mime.find("From: \"Evil <spoof@evil.test>, X\" <noreply@example.com>"), std::string::npos);
}

TEST(MailerMime, CrlfInFromAndPrefixCannotInjectHeaders) {
    auto cfg = base_config();
    cfg.from = "noreply@example.com\r\nBcc: hidden@evil.test";
    cfg.from_name = "";
    cfg.subject_prefix = "[App]\r\nX-Spoof: 1";
    const auto mime = Email::detail::build_mime(cfg, base_message(), "<id@test>");
    // strip_crlf removes the line breaks, not the text — the "Bcc:"/"X-Spoof:"
    // strings survive INSIDE the header value, which is harmless. What must
    // never exist is a NEW header line, i.e. the token at a line start.
    EXPECT_EQ(mime.find("\r\nBcc:"), std::string::npos);
    EXPECT_EQ(mime.find("\r\nX-Spoof:"), std::string::npos);
    EXPECT_EQ(count_headers(mime, "From: "), 1);
}

TEST(MailerMime, SubjectCrlfAlreadyStripped) {
    auto msg = base_message();
    msg.subject = "Hi\r\nX-Evil: yes";
    const auto mime = Email::detail::build_mime(base_config(), msg, "<id@test>");
    // The "X-Evil:" text stays inline in the Subject value; only a new header
    // line (the token after CRLF) would be an injection.
    EXPECT_EQ(mime.find("\r\nX-Evil:"), std::string::npos);
    EXPECT_NE(mime.find("Subject: [App] Hi"), std::string::npos);
}

}  // namespace

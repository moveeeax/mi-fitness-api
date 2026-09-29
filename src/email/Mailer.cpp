/**
 * @file Mailer.cpp
 * @brief Body for Email::detail::via_jobs() — compiled once into app_core.
 *        Deliberately the only thing de-inlined from Mailer.hpp: moving it
 *        here takes the jobs/Jobs.hpp include out of the header, breaking
 *        the email↔jobs cycle at the header level (the jobs→email half
 *        lives only in BuiltinHandlers.cpp now).
 */

#include "email/Mailer.hpp"

#include "jobs/Jobs.hpp"

namespace Email {
namespace detail {

bool via_jobs() {
    if (!Jobs::is_initialized())
        return false;
    if (Config::is_initialized())
        return Config::get().get<bool>("mail.via_jobs", "MAIL_VIA_JOBS", true);
    return true;
}

}  // namespace detail
}  // namespace Email

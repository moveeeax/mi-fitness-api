/**
 * @file AdminController.cpp
 * @brief Bodies for src/api/AdminController.hpp — compiled once into
 *        app_core. The route list, self-protection rules and helper
 *        contracts are documented on the declarations in the header.
 */

#include "api/AdminController.hpp"

#include <cstdint>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "email/AccountEmails.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Audit.hpp"
#include "security/Auth.hpp"
#include "security/Password.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

void AdminController::listUsers(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_ADMIN(req, callback);
    const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

    with_repo_errors(callback, "admin listUsers", [&] {
        Repositories::UserRepository repo;
        auto users = repo.list(page.limit, page.offset);
        long total = repo.count();
        callback(Response::paginated(to_json_array(users), total, page.limit, page.offset));
    });
}

void AdminController::createUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_ADMIN(req, callback);
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    Validation::require(errs, body, "email");
    Validation::require(errs, body, "password");
    Validation::email(errs, body, "email");
    Validation::string_length(errs, body, "password", Validation::kPasswordMinLen, Validation::kPasswordMaxLen);
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }
    // role_id optional — defaults to "User" role.
    auto role = resolve_role(body, "Role does not exist", callback);
    if (!role)
        return;

    with_repo_errors(callback, "admin createUser", [&] {
        const std::string hash = Security::Password::hash(body["password"].get<std::string>());
        // Admin-created users land already-confirmed by default —
        // matches flask-base where /admin/new-user skips the email
        // confirmation step.
        Repositories::UserRepository users;
        auto created = users.create(body["email"].get<std::string>(),
                                    hash,
                                    Validation::opt_string(body, "first_name"),
                                    Validation::opt_string(body, "last_name"),
                                    role->id,
                                    /*confirmed=*/true);
        // Attach the role we already loaded instead of re-querying.
        created.role = *role;
        Security::Audit::record(actor_of(req), "user.create", "user", created.id, {{"email", created.email}});
        callback(Response::created({{"data", json(created)}}));
    });
}

void AdminController::inviteUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_ADMIN(req, callback);
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    Validation::require(errs, body, "email");
    Validation::email(errs, body, "email");
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }
    auto role = resolve_role(body, /*invalid_message=*/"", callback);
    if (!role)
        return;

    with_repo_errors(callback, "admin inviteUser", [&] {
        Repositories::UserRepository users;
        // No password yet — they'll set one via the invite link.
        auto created = users.create(body["email"].get<std::string>(),
                                    std::nullopt,
                                    Validation::opt_string(body, "first_name"),
                                    Validation::opt_string(body, "last_name"),
                                    role->id,
                                    /*confirmed=*/false);
        // Attach the role we already loaded instead of re-querying.
        created.role = *role;
        Email::AccountEmails::send_invite(created);
        Security::Audit::record(actor_of(req), "user.invite", "user", created.id, {{"email", created.email}});
        callback(Response::created({{"data", json(created)}, {"message", "Invitation sent"}}));
    });
}

void AdminController::getUser(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback,
                              const std::string& id) {
    API_REQUIRE_ADMIN(req, callback);
    if (!require_user_id(id, callback))
        return;
    with_repo_errors(callback, "admin getUser", [&] {
        Repositories::UserRepository repo;
        auto user = repo.find(id);
        if (!user) {
            callback(ErrorResponse::not_found("user"));
            return;
        }
        callback(Response::ok({{"data", *user}}));
    });
}

void AdminController::updateUser(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 const std::string& id) {
    API_REQUIRE_ADMIN(req, callback);
    if (!require_user_id(id, callback))
        return;
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;

    // Self-protection: an admin can't change their own role away from
    // admin (flask-base does the same check). Otherwise the very last
    // admin can lock everyone out by accident.
    auto principal = Security::Auth::principal_of(req);
    const bool changing_self = principal && principal->subject == id;

    Repositories::UserRepository users;

    with_repo_errors(callback, "admin updateUser", [&] {
        std::optional<std::string> new_email;
        if (body.contains("email") && body["email"].is_string()) {
            Validation::Errors e;
            Validation::email(e, body, "email");
            if (e.any()) {
                callback(Validation::response_400(e));
                return;
            }
            new_email = body["email"].get<std::string>();
        }
        std::optional<int> new_role_id;
        if (body.contains("role_id") && body["role_id"].is_number_integer()) {
            if (changing_self) {
                callback(ErrorResponse::bad_request(
                    "self_role_change", "You cannot change the role of your own account; ask another admin"));
                return;
            }
            const int requested_role_id = body["role_id"].get<int>();
            Repositories::RoleRepository roles;
            if (!roles.find(requested_role_id)) {
                callback(ErrorResponse::bad_request("invalid_role"));
                return;
            }
            new_role_id = requested_role_id;
        }
        const auto first_name = Validation::opt_string(body, "first_name");
        const auto last_name = Validation::opt_string(body, "last_name");

        // One repository call → one transaction: a constraint failure
        // (e.g. duplicate email) can't leave the role half-changed the
        // way three sequential mutations could.
        if (new_email || new_role_id || first_name || last_name) {
            users.admin_update(id, new_email, new_role_id, first_name, last_name);
        }
        // Read from the primary so the echoed row reflects the write we
        // just made — a lagging replica would return the pre-update values.
        auto fresh = users.find(id, /*from_primary=*/true);
        if (!fresh) {
            callback(ErrorResponse::not_found("user"));
            return;
        }
        Security::Audit::record(actor_of(req), "user.update", "user", id);
        callback(Response::ok({{"data", *fresh}}));
    });
}

void AdminController::deleteUser(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 const std::string& id) {
    API_REQUIRE_ADMIN(req, callback);
    if (!require_user_id(id, callback))
        return;
    // Self-protection — flask-base parity: app/admin/views.py
    // delete_user explicitly refuses to delete current_user.
    auto principal = Security::Auth::principal_of(req);
    if (principal && principal->subject == id) {
        callback(ErrorResponse::bad_request("self_delete", "You cannot delete your own account; ask another admin"));
        return;
    }
    with_repo_errors(callback, "admin deleteUser", [&] {
        Repositories::UserRepository repo;
        repo.remove(id);
        Security::Audit::record(actor_of(req), "user.delete", "user", id);
        callback(Response::ok({{"message", "User deleted"}}));
    });
}

void AdminController::listRoles(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_ADMIN(req, callback);
    with_repo_errors(callback, "admin listRoles", [&] {
        Repositories::RoleRepository repo;
        // CrudBase::list defaults to LIMIT 100; roles are few but pass a
        // high cap so the list isn't silently truncated (was unbounded
        // before the CrudBase refactor).
        auto roles = repo.list(1000);
        callback(Response::list(to_json_array(roles)));
    });
}

void AdminController::createRole(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_ADMIN(req, callback);
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    Validation::require(errs, body, "name");
    Validation::string_length(errs, body, "name", 1, 64);
    if (!body.contains("permissions") || !body["permissions"].is_number_integer()) {
        errs.add("permissions", "invalid_type", "must be an integer bitmask");
    }
    Validation::boolean(errs, body, "is_default");
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }
    const auto perms = static_cast<std::uint32_t>(body["permissions"].get<long>());
    // Explicit null passes Validation::boolean (absent == null == "default"),
    // but body.value(k, false) would throw type_error.302 on it.
    const bool has_is_default = body.contains("is_default") && body["is_default"].is_boolean();
    const bool is_default = has_is_default && body["is_default"].get<bool>();
    with_repo_errors(callback, "admin createRole", [&] {
        Repositories::RoleRepository repo;
        auto created = repo.create(body["name"].get<std::string>(), perms, is_default);
        Security::Audit::record(
            actor_of(req), "role.create", "role", std::to_string(created.id), {{"name", created.name}});
        callback(Response::created({{"data", json(created)}}));
    });
}

void AdminController::updateRole(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 const std::string& id_str) {
    API_REQUIRE_ADMIN(req, callback);
    const auto role_id = require_role_id(id_str, callback);
    if (!role_id)
        return;
    const int id = *role_id;
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    std::optional<std::string> name;
    std::optional<std::uint32_t> permissions;
    std::optional<bool> is_default;
    if (body.contains("name") && body["name"].is_string())
        name = body["name"].get<std::string>();
    if (body.contains("permissions") && body["permissions"].is_number_integer())
        permissions = static_cast<std::uint32_t>(body["permissions"].get<long>());
    if (body.contains("is_default") && body["is_default"].is_boolean())
        is_default = body["is_default"].get<bool>();
    if (!name && !permissions && !is_default) {
        callback(ErrorResponse::bad_request("empty_patch", "Provide at least one of name / permissions / is_default"));
        return;
    }
    with_repo_errors(callback, "admin updateRole", [&] {
        Repositories::RoleRepository repo;
        auto updated = repo.update(id, name, permissions, is_default);
        Security::Audit::record(actor_of(req), "role.update", "role", std::to_string(id));
        callback(Response::ok({{"data", json(updated)}}));
    });
}

void AdminController::deleteRole(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 const std::string& id_str) {
    API_REQUIRE_ADMIN(req, callback);
    const auto role_id = require_role_id(id_str, callback);
    if (!role_id)
        return;
    const int id = *role_id;
    with_repo_errors(callback, "admin deleteRole", [&] {
        Repositories::RoleRepository repo;
        // Self-protection: refuse to delete the default role —
        // future sign-ups would have nowhere to land.
        auto existing = repo.find(id);
        if (existing && existing->is_default) {
            callback(ErrorResponse::bad_request(
                "default_role_protected", "The default role cannot be deleted; promote another role to default first"));
            return;
        }
        repo.remove(id);
        Security::Audit::record(actor_of(req), "role.delete", "role", std::to_string(id));
        callback(Response::ok({{"message", "Role deleted"}}));
    });
}

std::string AdminController::actor_of(const HttpRequestPtr& req) {
    auto p = Security::Auth::principal_of(req);
    return p ? p->subject : std::string{};
}

std::optional<Domain::Role> AdminController::resolve_role(const json& body,
                                                          const std::string& invalid_message,
                                                          const std::function<void(const HttpResponsePtr&)>& callback) {
    std::optional<int> requested_role_id;
    if (body.contains("role_id") && body["role_id"].is_number_integer())
        requested_role_id = body["role_id"].get<int>();
    Repositories::RoleRepository roles;
    auto role = requested_role_id ? roles.find(*requested_role_id) : roles.find_default();
    if (!role) {
        callback(ErrorResponse::bad_request("invalid_role", invalid_message));
        return std::nullopt;
    }
    return role;
}

bool AdminController::require_user_id(const std::string& id,
                                      const std::function<void(const HttpResponsePtr&)>& callback) {
    if (is_valid_uuid(id))
        return true;
    callback(ErrorResponse::bad_request("invalid_id", "Malformed user id"));
    return false;
}

std::optional<int> AdminController::require_role_id(const std::string& id_str,
                                                    const std::function<void(const HttpResponsePtr&)>& callback) {
    const int id = parse_int(id_str, -1);
    if (id <= 0) {
        callback(ErrorResponse::bad_request("invalid_id"));
        return std::nullopt;
    }
    return id;
}

}  // namespace Api

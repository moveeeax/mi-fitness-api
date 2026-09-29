/**
 * @file test_crud_owned.cpp
 * @brief Compile-time + behavioral pin for CrudBase's ownership-scoped reads
 *        (find_owned / list_owned / count_owned, unlocked by kOwnerColumn).
 *
 *        These are member templates of a class template: they only
 *        type-check when something instantiates them. In the template repo
 *        itself only find_owned is instantiated by production code
 *        (BillingController.cpp via BillingRepository, kOwnerColumn =
 *        "user_id"); list_owned/count_owned are instantiated by the output
 *        of `new-resource.sh <Entity> --owned` (the generated controller
 *        calls all three) — i.e. by FORK code, not by anything this repo
 *        compiles. Without this test, a silently broken signature would
 *        pass CI here and detonate in every fork's first `--owned` resource.
 *
 *        Runs against the Database DI seam (Database::install_for_testing,
 *        same fake shape as test_database_seam.cpp) — no Postgres. The fake
 *        returns EMPTY pqxx::results, which is enough to prove the owner
 *        predicate lands in the SQL template and the empty-result paths
 *        behave (nullopt / empty vector). Pure unit.
 */

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "repositories/CrudBase.hpp"

namespace {

// ---- Database seam fake (mirrors test_database_seam.cpp) -------------------

class RecordingTxn : public Database::detail::ErasedTxn {
public:
    std::vector<std::string> queries;

    pqxx::result exec_params_erased(const std::string& query, const pqxx::params& /*params*/) override {
        queries.push_back(query);
        return pqxx::result{};
    }

    pqxx::result exec_erased(const std::string& query) override {
        queries.push_back(query);
        return pqxx::result{};
    }
};

class FakeDatabase : public Database::DatabaseManager {
public:
    RecordingTxn txn;
    std::map<std::string, int> entered;  // "<op>@<pool>" -> count

    bool is_initialized() const override { return true; }
    bool health_check() override { return true; }

protected:
    Database::detail::ErasedTxn* test_transaction_(const char* op, const char* pool) override {
        ++entered[std::string(op) + "@" + pool];
        return &txn;
    }
};

// ---- Minimal owned repository (the shape new-resource.sh --owned emits) ----

struct OwnedNote {
    std::string id;
    // Templated like every shipped domain struct: pqxx result iteration
    // yields row_ref, not row& — a concrete `const pqxx::row&` signature
    // fails to compile inside list_owned (this test proved it live).
    template <class Row>
    static OwnedNote from_row(const Row& /*row*/) {
        return {};
    }
};

class OwnedNoteRepository : public Repositories::CrudBase<OwnedNoteRepository, OwnedNote, std::string> {
public:
    static constexpr const char* kTable = "owned_notes";
    static constexpr const char* kColumns = "id, owner_id";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "id";
    static constexpr const char* kOwnerColumn = "owner_id";
};

class CrudOwnedTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto fake = std::make_unique<FakeDatabase>();
        fake_ = fake.get();
        Database::install_for_testing(std::move(fake));
    }
    void TearDown() override { Database::reset_for_testing(); }

    FakeDatabase* fake_ = nullptr;
    OwnedNoteRepository repo_;
};

TEST_F(CrudOwnedTest, FindOwnedScopesByIdAndOwner) {
    auto found = repo_.find_owned("note-1", "user-1");
    EXPECT_FALSE(found.has_value());  // empty fake result → nullopt, no from_row
    ASSERT_EQ(fake_->txn.queries.size(), 1u);
    EXPECT_EQ(fake_->txn.queries[0], "SELECT id, owner_id FROM owned_notes WHERE id = $1 AND owner_id = $2");
    EXPECT_EQ(fake_->entered["db.read@replica"], 1);

    // from_primary=true routes through the read-after-write path.
    repo_.find_owned("note-1", "user-1", /*from_primary=*/true);
    EXPECT_EQ(fake_->entered["db.read@primary"], 1);
}

TEST_F(CrudOwnedTest, ListOwnedScopesByOwner) {
    auto rows = repo_.list_owned("user-1", 25, 5);
    EXPECT_TRUE(rows.empty());
    ASSERT_EQ(fake_->txn.queries.size(), 1u);
    EXPECT_EQ(fake_->txn.queries[0],
              "SELECT id, owner_id FROM owned_notes WHERE owner_id = $1 ORDER BY id LIMIT $2 OFFSET $3");
}

TEST_F(CrudOwnedTest, CountOwnedScopesByOwner) {
    // The empty fake result can't carry the COUNT(*) row, so reading it throws
    // (seam limitation — see test_database_seam.cpp header). The template still
    // fully instantiates and the SQL it issued is observable.
    EXPECT_THROW((void)repo_.count_owned("user-1"), std::exception);
    ASSERT_EQ(fake_->txn.queries.size(), 1u);
    EXPECT_EQ(fake_->txn.queries[0], "SELECT COUNT(*) FROM owned_notes WHERE owner_id = $1");
}

}  // namespace

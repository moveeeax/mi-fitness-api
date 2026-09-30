/**
 * @file SyncService.cpp
 * @brief Тело оркестрации синка.
 */

#include "sync/SyncService.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <utility>

#include <spdlog/spdlog.h>

#include "database/Database.hpp"
#include "repositories/ActivityRepository.hpp"
#include "repositories/BodyRepository.hpp"
#include "repositories/SamplesRepository.hpp"
#include "repositories/SleepRepository.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "repositories/WorkoutRepository.hpp"
#include "utils/Config.hpp"
#include "xiaomi/CloudClient.hpp"
#include "xiaomi/Normalize.hpp"
#include "xiaomi/Regions.hpp"

namespace Sync {

namespace {

struct Chunk {
    std::string from;
    std::string to;
};

/// Куски диапазона по chunk_days, включительно с обеих сторон.
std::vector<Chunk> split_range(const std::string& from, const std::string& to, int chunk_days) {
    using namespace std::chrono;
    const sys_days start = Xiaomi::detail::parse_date(from);
    const sys_days end = Xiaomi::detail::parse_date(to);
    std::vector<Chunk> chunks;
    for (sys_days cursor = start; cursor <= end;) {
        sys_days chunk_end = cursor + days{chunk_days - 1};
        if (chunk_end > end) {
            chunk_end = end;
        }
        const auto iso = [](sys_days d) {
            const year_month_day ymd{d};
            char out[16];
            std::snprintf(out,
                          sizeof(out),
                          "%04d-%02d-%02d",
                          static_cast<int>(ymd.year()),
                          static_cast<unsigned>(ymd.month()),
                          static_cast<unsigned>(ymd.day()));
            return std::string(out);
        };
        chunks.push_back({iso(cursor), iso(chunk_end)});
        cursor = chunk_end + days{1};
    }
    return chunks;
}

/// Атомарный переход queued -> running. false, когда другой запуск уже идёт.
///
/// Перед захватом протухшие running помечаются interrupted: воркер, убитый
/// посреди синка (OOM, вытеснение узла), не снимает свою строку сам, и без
/// этого шага частичный индекс one_running_sync_run глушил бы все будущие
/// запуски навсегда (Critical 2 финального обзора). Порог протухания больше
/// максимальной честной длительности: потолок на тип, помноженный на число
/// типов, плюс запас.
bool try_start(long run_id, long stale_seconds) {
    try {
        return Database::get().execute_write([&](auto& txn) {
            txn.exec_params(
                "UPDATE sync_runs SET status = 'interrupted', finished_at = now() "
                "WHERE status = 'running' AND started_at < now() - make_interval(secs => $1::double precision)",
                stale_seconds);
            // Только из queued: повторно доставленное задание не оживляет
            // закрытый запуск и не переписывает его журнал (Important 4).
            auto r = txn.exec_params(
                "UPDATE sync_runs SET status = 'running', started_at = now() "
                "WHERE id = $1 AND status = 'queued' RETURNING id",
                run_id);
            return !r.empty();
        });
    } catch (const std::exception&) {
        // Нарушение частичного уникального индекса one_running_sync_run.
        return false;
    }
}

/// Дата YYYY-MM-DD, сдвинутая на days суток. Формат уже проверен вызывающим.
std::string shift_date(const std::string& date, int days) {
    std::chrono::year_month_day ymd{std::chrono::year(std::stoi(date.substr(0, 4))),
                                    std::chrono::month(static_cast<unsigned>(std::stoi(date.substr(5, 2)))),
                                    std::chrono::day(static_cast<unsigned>(std::stoi(date.substr(8, 2))))};
    const auto shifted = std::chrono::year_month_day(std::chrono::sys_days(ymd) + std::chrono::days(days));
    char out[16];
    std::snprintf(out,
                  sizeof(out),
                  "%04d-%02u-%02u",
                  static_cast<int>(shifted.year()),
                  static_cast<unsigned>(shifted.month()),
                  static_cast<unsigned>(shifted.day()));
    return out;
}

void bump_sync_state(const std::string& data_type, long added) {
    Database::get().execute_write([&](auto& txn) {
        txn.exec_params(
            "INSERT INTO sync_state (data_type, last_sync_at, records_count) "
            "VALUES ($1, now(), $2) "
            "ON CONFLICT (data_type) DO UPDATE SET "
            "last_sync_at = now(), records_count = sync_state.records_count + $2",
            data_type,
            added);
        return true;
    });
}

}  // namespace

SyncService::SyncService(Xiaomi::HttpTransport& transport,
                         Xiaomi::Credentials credentials,
                         std::function<void(const Xiaomi::Credentials&)> on_rotate)
    : transport_(transport), credentials_(std::move(credentials)), on_rotate_(std::move(on_rotate)) {}

nlohmann::json SyncService::run(long run_id,
                                const std::string& from,
                                const std::string& to,
                                const std::vector<std::string>& data_types) {
    Repositories::SyncRunRepository runs;
    nlohmann::json result = nlohmann::json::object();

    int chunk_days = 7;
    long type_timeout = 180;
    if (Config::is_initialized()) {
        chunk_days = Config::get().get<int>("xiaomi.sync_chunk_days", "MI_FITNESS_CHUNK_DAYS", 7);
        type_timeout = Config::get().get<long>("xiaomi.sync_type_timeout_seconds", "MI_FITNESS_SYNC_TYPE_TIMEOUT", 180);
    }

    const long stale_seconds = type_timeout * static_cast<long>(kAllDataTypes.size()) + 600;
    if (!try_start(run_id, stale_seconds)) {
        // Другой запуск держит running: честный skipped, облако не трогаем.
        // Журнал трогается только у стоящего в очереди: завершённый запуск
        // повторная доставка задания не переписывает.
        result["skipped_reason"] = "another sync run is in progress";
        runs.skip_if_queued(run_id, result);
        return result;
    }

    Xiaomi::CloudClient client(transport_, credentials_, on_rotate_);
    bool logged_in = false;
    bool auth_dead = false;
    bool any_failed = false;

    for (const auto& data_type : data_types) {
        auto& entry = result[data_type];
        const auto type_started = std::chrono::steady_clock::now();
        // Отказ авторизации лечится свежим токеном, а не повтором: после
        // первого auth-отказа остальные типы закрываются без сети, иначе
        // один запуск сжигает до восьми входов с мёртвым токеном
        // (Important 5 финального обзора).
        if (auth_dead) {
            any_failed = true;
            entry["error"] = "auth";
            continue;
        }
        try {
            if (!logged_in) {
                client.login();
                logged_in = true;
            }
            Repositories::UpsertCounts counts;
            long skipped = 0;
            long suppressed = 0;
            // daily_activity агрегируется по всему диапазону: границы кусков
            // идут в поясе региона, устройство может жить в другом, и день на
            // стыке кусков размазан по двум выборкам. Upsert по кускам
            // перезатирал полный агрегат огрызком (сверка с мостом, отчёт
            // 2026-09), поэтому минуты копятся здесь и нормализуются один раз
            // после всех кусков.
            std::vector<nlohmann::json> activity_steps;
            std::vector<nlohmann::json> activity_calories;

            for (const auto& chunk : split_range(from, to, chunk_days)) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - type_started)
                        .count();
                if (elapsed > type_timeout) {
                    // Потолок на тип: между кусками, прервать запрос посреди
                    // безопасно нельзя, а каждый ограничен таймаутом транспорта.
                    throw Xiaomi::MiFitnessProtocolError("type exceeded its time budget");
                }
                const std::string& user = client.credentials().user_id;
                if (data_type == "daily_activity") {
                    const auto steps = client.fetch_key("steps", chunk.from, chunk.to, {});
                    const auto calories = client.fetch_key("calories", chunk.from, chunk.to, {});
                    activity_steps.insert(activity_steps.end(), steps.begin(), steps.end());
                    activity_calories.insert(activity_calories.end(), calories.begin(), calories.end());
                } else if (data_type == "sleep") {
                    const auto records = client.fetch_key("sleep", chunk.from, chunk.to, {});
                    auto sessions = Xiaomi::normalize_sleep(records, user, skipped);
                    // Окно отчётов: даты пробуждения кандидатов без оценки,
                    // с днём запаса на разницу поясов.
                    std::string report_from;
                    std::string report_to;
                    for (const auto& s : sessions) {
                        if (s.sleep_score.has_value() || s.is_nap) {
                            continue;
                        }
                        const std::string day = s.end_at.substr(0, 10);
                        if (report_from.empty() || day < report_from) {
                            report_from = day;
                        }
                        if (report_to.empty() || day > report_to) {
                            report_to = day;
                        }
                    }
                    if (!report_from.empty()) {
                        try {
                            // День запаса с обеих сторон, как у эталона: пояс
                            // отчёта может не совпадать с поясом региона, и
                            // отчёт крайнего дня лежит вне узкого окна.
                            const auto reports =
                                client.fetch_daily_sleep_reports(shift_date(report_from, -1), shift_date(report_to, 1));
                            const int offset =
                                credentials_.region.empty() || credentials_.region == "cn" ? 8 * 3600 : 0;
                            Xiaomi::apply_daily_sleep_scores(sessions, reports, offset);
                        } catch (const std::exception&) {
                            // Отчёты необязательны: их провал не трогает сессии.
                            spdlog::warn("sleep: daily score lookup unavailable");
                        }
                    }
                    const auto c = Repositories::SleepRepository().upsert(sessions);
                    counts.added += c.added;
                    counts.updated += c.updated;
                } else if (data_type == "workouts") {
                    const auto records = client.fetch_sport_records(chunk.from, chunk.to);
                    const auto items = Xiaomi::normalize_workouts(records, user, skipped);
                    const auto c = Repositories::WorkoutRepository().upsert(items);
                    counts.added += c.added;
                    counts.updated += c.updated;
                } else if (data_type == "body_measurements") {
                    const auto records = client.fetch_key("weight", chunk.from, chunk.to, {});
                    const auto items = Xiaomi::normalize_body(records, user, skipped);
                    const auto c = Repositories::BodyRepository().upsert(items);
                    counts.added += c.added;
                    counts.updated += c.updated;
                } else if (data_type == "heart_rate") {
                    const auto records = client.fetch_key("heart_rate", chunk.from, chunk.to, {});
                    const auto resting = client.fetch_key("resting_heart_rate", chunk.from, chunk.to, {});
                    const auto items = Xiaomi::normalize_heart_rate(records, resting, user, skipped);
                    const auto c = Repositories::SamplesRepository().upsert_heart_rate(items);
                    counts.added += c.added;
                    counts.updated += c.updated;
                } else if (data_type == "spo2") {
                    const auto records = client.fetch_key("spo2", chunk.from, chunk.to, {});
                    const auto items = Xiaomi::normalize_spo2(records, user, skipped);
                    const auto c = Repositories::SamplesRepository().upsert_spo2(items);
                    counts.added += c.added;
                    counts.updated += c.updated;
                } else if (data_type == "stress") {
                    const auto records = client.fetch_key("stress", chunk.from, chunk.to, {});
                    const auto items = Xiaomi::normalize_stress(records, user, skipped);
                    const auto c = Repositories::SamplesRepository().upsert_stress(items);
                    counts.added += c.added;
                    counts.updated += c.updated;
                } else if (data_type == "abnormal_heart_beat") {
                    const auto records = client.fetch_key("abnormal_heart_beat", chunk.from, chunk.to, {});
                    const auto items = Xiaomi::normalize_abnormal_heart_beat(records, user, skipped);
                    const auto c = Repositories::SamplesRepository().upsert_abnormal(items);
                    counts.added += c.added;
                    counts.updated += c.updated;
                } else {
                    throw Xiaomi::MiFitnessProtocolError("unknown data type requested");
                }
            }

            if (data_type == "daily_activity") {
                const std::string& user = client.credentials().user_id;
                auto normalized = Xiaomi::normalize_daily_activity(activity_steps, activity_calories, user);
                // Окно режется в поясе региона, и минуты соседних локальных
                // суток попадают внутрь. Дни вне [from, to] заведомо
                // частичные: их upsert затёр бы полный день в базе огрызком
                // (Critical 1 финального обзора).
                std::erase_if(normalized.days, [&](const auto& day) { return day.date < from || day.date > to; });
                const auto c = Repositories::ActivityRepository().upsert(normalized.days);
                counts.added += c.added;
                counts.updated += c.updated;
                skipped += normalized.skipped;
                suppressed += normalized.suppressed_steps;
            }

            entry["added"] = counts.added;
            entry["updated"] = counts.updated;
            entry["skipped"] = skipped;
            if (suppressed > 0) {
                entry["suppressed_steps"] = suppressed;
            }
            bump_sync_state(data_type, counts.added);
        } catch (const Xiaomi::MiFitnessAuthError& e) {
            any_failed = true;
            auth_dead = !logged_in;
            entry["error"] = "auth";
            spdlog::warn("sync {}: auth failure: {}", data_type, e.what());
        } catch (const Xiaomi::MiFitnessProtocolError& e) {
            any_failed = true;
            entry["error"] = "protocol";
            spdlog::warn("sync {}: protocol failure: {}", data_type, e.what());
        } catch (const std::exception& e) {
            any_failed = true;
            entry["error"] = "other";
            spdlog::warn("sync {}: failure: {}", data_type, e.what());
        }
    }

    runs.finish(run_id, any_failed ? "failed" : "succeeded", result);
    return result;
}

}  // namespace Sync

/**
 * @file Dispatcher.cpp
 * @brief Bodies for src/jobs/Dispatcher.hpp — compiled once into app_core.
 *        Registration stays inline in the header (JobHandlerRegistrar runs at
 *        static init from handler headers); the lookup/dispatch paths live
 *        here.
 */

#include "jobs/Dispatcher.hpp"

namespace Jobs {

Dispatcher& Dispatcher::get() {
    static Dispatcher instance;
    return instance;
}

std::vector<std::string> Dispatcher::known_types() const {
    std::vector<std::string> out;
    out.reserve(handlers_.size());
    for (const auto& kv : handlers_)
        out.push_back(kv.first);
    return out;
}

std::vector<std::string> Dispatcher::unregistered(const std::vector<std::string>& subscribed) const {
    std::vector<std::string> out;
    for (const auto& t : subscribed)
        if (handlers_.find(t) == handlers_.end())
            out.push_back(t);
    return out;
}

json Dispatcher::dispatch(const Job& job) const {
    auto it = handlers_.find(job.type);
    if (it == handlers_.end())
        throw PermanentJobError("no handler for job type: " + job.type);
    return it->second(job.payload);
}

}  // namespace Jobs

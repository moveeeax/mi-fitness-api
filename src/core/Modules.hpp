/**
 * @file Modules.hpp
 * @brief Feature-module switches + process shutdown flag.
 * @details The tiny slice of Core:: that controllers and guards actually
 *          consult per-request. Kept separate from Core.hpp so including a
 *          module switch costs only utils/Config.hpp — Core.hpp is the
 *          composition root and transitively pulls every subsystem
 *          (database, cache, OTel, ...), which made every
 *          controller recompile whenever any subsystem header changed.
 *          Core.hpp includes this header, so the names below stay usable as
 *          Core::* from either include.
 */

#pragma once

#include <atomic>

#include "utils/Config.hpp"

namespace Core {

/**
 * @brief Shutdown state flag, flipped by main/worker signal handlers before
 *        Core::shutdown() is called. Used by /ready so Kubernetes stops
 *        sending new traffic while in-flight requests complete.
 */
inline std::atomic<bool> shutting_down_flag{false};

inline void begin_shutdown() {
    shutting_down_flag.store(true);
}
inline bool is_shutting_down() {
    return shutting_down_flag.load();
}

}  // namespace Core

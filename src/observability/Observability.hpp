/**
 * @file Observability.hpp
 * @brief Observability module for logging, metrics, and tracing
 * @details Integrates spdlog for logging, prometheus-cpp for metrics,
 *          and opentelemetry-cpp for distributed tracing
 *
 * Non-template bodies (sink/formatter wiring incl. the custom spdlog flags,
 * the prometheus builders, the OTel SDK pipeline setup/teardown and the
 * global-instance lifecycle) live in Observability.cpp (compiled once into
 * app_core; ADR 0003 as amended 2026-08-22) — so the heavy OTel SDK/exporter
 * headers are no longer exposed to including TUs. The prometheus metric
 * headers stay here: create_counter/create_gauge/add_histogram return the
 * metric types by reference and every caller uses them directly.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <opentelemetry/trace/provider.h>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <spdlog/spdlog.h>

namespace Observability {

namespace trace_api = opentelemetry::trace;
namespace prometheus_ns = prometheus;

/**
 * @brief Logging subsystem
 */
class Logger {
private:
    std::shared_ptr<spdlog::logger> logger;

public:
    /**
     * @brief Initialize logger with console and file sinks
     * @param name Logger name
     * @param log_file Path to log file (optional)
     */
    // format: "text" (human-readable, default) or "json" (one JSON object
    // per line — feed-ready for Loki / ELK / Datadog). We hand-roll the
    // JSON pattern so spdlog still emits a single allocation per record.
    void initialize(const std::string& name = "app",
                    const std::string& log_file = "",
                    const std::string& format = "text",
                    const std::string& service_name = "");

    /**
     * @brief Set log level
     * @param level Level string (trace, debug, info, warn, error, critical)
     */
    void set_level(const std::string& level);

    /**
     * @brief Get underlying spdlog logger
     * @return Shared pointer to logger
     */
    std::shared_ptr<spdlog::logger> get() { return logger; }

    /**
     * @brief Flush all logs
     */
    void flush() {
        if (logger) {
            logger->flush();
        }
    }

    /**
     * @brief Shutdown logger
     */
    void shutdown() {
        if (logger) {
            logger->flush();
            spdlog::shutdown();
        }
    }
};

/**
 * @brief Metrics subsystem using Prometheus
 */
class Metrics {
private:
    std::shared_ptr<prometheus_ns::Registry> registry;
    std::unique_ptr<prometheus_ns::Exposer> exposer;

public:
    /**
     * @brief Initialize metrics with Prometheus exposer.
     *        If @p bind_address is empty, the registry is created but no HTTP
     *        exposer is started — useful for CLI commands (--create-admin etc.)
     *        running alongside a live server in the same container.
     */
    void initialize(const std::string& bind_address = "0.0.0.0:9090");

    /**
     * @brief Create a counter metric
     */
    prometheus_ns::Family<prometheus_ns::Counter>& create_counter(
        const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels = {});

    /**
     * @brief Create a gauge metric
     */
    prometheus_ns::Family<prometheus_ns::Gauge>& create_gauge(const std::string& name,
                                                              const std::string& help,
                                                              const std::map<std::string, std::string>& labels = {});

    /**
     * @brief Create a histogram family.
     * @note Bucket boundaries are set per-instance via Family::Add(labels, buckets),
     *       not on the family itself — the prometheus-cpp API does not carry
     *       default buckets at the family level. Use add_histogram() for a
     *       one-shot create-family-and-instance call.
     */
    prometheus_ns::Family<prometheus_ns::Histogram>& create_histogram(
        const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels = {});

    /**
     * @brief Create a histogram family and register a single instance with the given buckets.
     * @return Reference to the registered histogram ready for Observe() calls.
     */
    prometheus_ns::Histogram& add_histogram(const std::string& name,
                                            const std::string& help,
                                            const std::vector<double>& buckets,
                                            const std::map<std::string, std::string>& instance_labels = {},
                                            const std::map<std::string, std::string>& family_labels = {});

    /**
     * @brief Get registry
     */
    std::shared_ptr<prometheus_ns::Registry> get_registry() { return registry; }

    /**
     * @brief Shutdown metrics
     */
    void shutdown() {
        exposer.reset();
        registry.reset();
    }
};

/**
 * @brief Tracing subsystem using OpenTelemetry
 */
class Tracer {
private:
    std::shared_ptr<trace_api::TracerProvider> provider;

public:
    /**
     * @brief Initialize tracing
     * @param service_name Service name for traces
     * @param otlp_endpoint OTLP HTTP endpoint (empty = OStream fallback)
     */
    void initialize(const std::string& service_name = "mi_fitness_api_service", const std::string& otlp_endpoint = "");

    /**
     * @brief Get a tracer instance
     */
    opentelemetry::nostd::shared_ptr<trace_api::Tracer> get_tracer(const std::string& name = "default");

    /**
     * @brief Shutdown tracing. Flush + shut down the SDK provider so buffered
     *        spans are exported, then point the GLOBAL provider back at the
     *        no-op — otherwise Database::AutoSpan (which reads the global
     *        provider, not this member) would keep minting spans into a
     *        half-torn-down pipeline during the rest of Core::shutdown.
     */
    void shutdown();
};

/**
 * @brief Main observability manager
 */
class ObservabilitySystem {
private:
    Logger logger_system;
    Metrics metrics_system;
    Tracer tracer_system;
    bool initialized = false;

public:
    /**
     * @brief Initialize all observability subsystems
     */
    void initialize(const std::string& log_name = "app",
                    const std::string& log_file = "logs/app.log",
                    const std::string& metrics_addr = "0.0.0.0:9090",
                    const std::string& service_name = "mi_fitness_api_service",
                    const std::string& otlp_endpoint = "",
                    const std::string& log_format = "text");

    Logger& logger() { return logger_system; }
    Metrics& metrics() { return metrics_system; }
    Tracer& tracer() { return tracer_system; }

    void shutdown();

    bool is_initialized() const { return initialized; }
};

/**
 * @brief Initialize the global observability instance
 */
void initialize(const std::string& log_name = "app",
                const std::string& log_file = "logs/app.log",
                const std::string& metrics_addr = "0.0.0.0:9090",
                const std::string& service_name = "mi_fitness_api_service",
                const std::string& otlp_endpoint = "",
                const std::string& log_format = "text");

ObservabilitySystem& get();

bool is_initialized();

void shutdown();

}  // namespace Observability

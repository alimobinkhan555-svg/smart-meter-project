/*
 * analytics.h - user-space analytics engine for the smart meter.
 *
 * Pure C++ (no external dependencies): turns raw cumulative pulse counts
 * into energy readings, aggregates them per hour / per day, computes the
 * descriptive statistics required by the project brief and flags anomalies
 * (sudden spikes in the pulse rate).
 */
#ifndef SMART_METER_ANALYTICS_H
#define SMART_METER_ANALYTICS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace smartmeter {

/* Energy unit conversion: 1 pulse == `pulses_per_wh` watts per hour. */
inline constexpr double kDefaultPulsesPerWh = 1.0;

/* A single CSV record / log line. */
struct Reading {
    std::int64_t epoch_seconds{0};    /* wall clock, seconds since 1970    */
    std::uint64_t pulse_count{0};     /* cumulative pulses from the driver */
    std::uint64_t pulse_delta{0};     /* pulses since the previous reading */
    double energy_wh{0.0};            /* delta converted to energy        */
};

/* Descriptive statistics over the whole history (or a window). */
struct Summary {
    std::size_t sample_count{0};
    double total_energy_wh{0.0};
    double average_wh{0.0};
    double max_wh{0.0};
    double min_wh{0.0};
    std::int64_t peak_epoch{0};
    std::int64_t valley_epoch{0};
    double peak_wh{0.0};
    double min_wh_steady{0.0};   /* max() is meaningless on a fresh boot */
    double avg_pulses_per_hour{0.0};
    std::int64_t first_epoch{0};
    std::int64_t last_epoch{0};
    std::int64_t span_seconds{0};
    double energy_kwh{0.0};
};

/* Anomaly record produced by detectAnomalies(). */
struct Anomaly {
    std::int64_t epoch{0};
    double energy_wh{0.0};
    double baseline_wh{0.0};
    double z_score{0.0};
    std::string severity() const; /* CRITICAL | HIGH | MEDIUM | LOW */
};

/* Rolling spike detector state for the live dashboard. */
struct SpikeState {
    bool in_spike{false};
    std::size_t consecutive{0};
    double ratio{0.0}; /* instantaneous / baseline */
};

class AnalyticsEngine {
public:
    explicit AnalyticsEngine(double pulses_per_wh = kDefaultPulsesPerWh,
                             std::size_t history_limit = 0);

    void setPulsesPerWh(double pulses_per_wh);
    double pulsesPerWh() const { return pulses_per_wh_; }

    /* Append a reading and keep the history bounded (0 == unbounded). */
    void addReading(const Reading& reading);

    /* Derive a Reading from a new cumulative pulse count and log it. */
    Reading appendSample(std::int64_t epoch_seconds, std::uint64_t pulse_count);

    Summary summarize() const;
    Summary summarizeWindow(std::size_t window) const;

    /* Energy grouped by hour / by day, oldest first. */
    std::vector<std::pair<std::int64_t, double>> hourlyEnergy() const;
    std::vector<std::pair<std::int64_t, double>> dailyEnergy() const;

    /* Energy in the top-N most recent hours, newest first. */
    std::vector<std::pair<std::int64_t, double>> recentHours(std::size_t count) const;

    std::vector<Anomaly> detectAnomalies(double z_threshold = 3.0) const;

    /* Instantaneous rate of the most recent sample, in pulses per minute.
     * Returns 0 when fewer than two readings are available. */
    double currentRatePerMinute() const;

    /* Compare the instantaneous rate against a rolling baseline. */
    SpikeState updateSpike(std::int64_t epoch_seconds, double window_seconds = 60.0) const;

    std::uint64_t lastPulseCount() const { return last_pulse_count_; }
    const std::vector<Reading>& readings() const { return readings_; }
    std::size_t size() const { return readings_.size(); }

private:
    double pulses_per_wh_;
    std::size_t history_limit_;
    std::vector<Reading> readings_;
    std::uint64_t last_pulse_count_{0};
    std::int64_t last_epoch_{0};
};

}  // namespace smartmeter

#endif /* SMART_METER_ANALYTICS_H */

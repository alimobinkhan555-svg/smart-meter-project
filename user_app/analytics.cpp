/*
 * analytics.cpp - statistics, aggregation and anomaly detection.
 *
 * Everything here is pure computation over the reading history: the class
 * holds no I/O and no global state, which makes it trivially testable.
 */
#include "analytics.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <sstream>

namespace smartmeter {
namespace {

constexpr std::int64_t kSecondsPerHour = 3600;
constexpr std::int64_t kSecondsPerDay = 86400;

double safeStddev(const std::vector<double>& values, double mean) {
    if (values.size() < 2) return 0.0;
    double sum = 0.0;
    for (double v : values) {
        const double d = v - mean;
        sum += d * d;
    }
    return std::sqrt(sum / static_cast<double>(values.size() - 1));
}

}  // namespace

std::string Anomaly::severity() const {
    const double az = std::fabs(z_score);
    if (az >= 6.0) return "CRITICAL";
    if (az >= 4.0) return "HIGH";
    if (az >= 3.0) return "MEDIUM";
    return "LOW";
}

AnalyticsEngine::AnalyticsEngine(double pulses_per_wh, std::size_t history_limit)
    : pulses_per_wh_(pulses_per_wh > 0.0 ? pulses_per_wh : kDefaultPulsesPerWh),
      history_limit_(history_limit) {}

void AnalyticsEngine::setPulsesPerWh(double pulses_per_wh) {
    if (pulses_per_wh > 0.0) pulses_per_wh_ = pulses_per_wh;
}

void AnalyticsEngine::addReading(const Reading& reading) {
    if (reading.pulse_count < last_pulse_count_) {
        /* The counter was reset (driver reload / SM_IOC_RESET): rebase so
         * the delta stays non-negative instead of wrapping to a huge value. */
        last_pulse_count_ = 0;
        last_epoch_ = 0;
    }
    readings_.push_back(reading);
    if (history_limit_ > 0 && readings_.size() > history_limit_) {
        readings_.erase(readings_.begin(),
                        readings_.begin() +
                            static_cast<std::ptrdiff_t>(readings_.size() - history_limit_));
    }
    last_pulse_count_ = reading.pulse_count;
    last_epoch_ = reading.epoch_seconds;
}

Reading AnalyticsEngine::appendSample(std::int64_t epoch_seconds, std::uint64_t pulse_count) {
    Reading reading;
    reading.epoch_seconds = epoch_seconds;
    reading.pulse_count = pulse_count;
    reading.pulse_delta = pulse_count > last_pulse_count_ ? pulse_count - last_pulse_count_ : 0;
    reading.energy_wh = static_cast<double>(reading.pulse_delta) / pulses_per_wh_;

    const bool counter_reset = pulse_count < last_pulse_count_;
    if (counter_reset) {
        /* First reading after a reset: treat the raw count as the delta so
         * the energy total is not lost. */
        reading.pulse_delta = pulse_count;
        reading.energy_wh = static_cast<double>(pulse_count) / pulses_per_wh_;
    }
    addReading(reading);
    return reading;
}

Summary AnalyticsEngine::summarize() const {
    Summary summary;
    summary.sample_count = readings_.size();
    if (readings_.empty()) return summary;

    double total = 0.0;
    double peak = -1.0;
    double valley = -1.0;
    summary.first_epoch = readings_.front().epoch_seconds;
    summary.last_epoch = readings_.back().epoch_seconds;

    for (std::size_t i = 0; i < readings_.size(); ++i) {
        const Reading& r = readings_[i];
        total += r.energy_wh;
        if (r.energy_wh > peak) {
            peak = r.energy_wh;
            summary.peak_epoch = r.epoch_seconds;
        }
        if (valley < 0.0 || r.energy_wh < valley) {
            valley = r.energy_wh;
            summary.valley_epoch = r.epoch_seconds;
        }
    }

    summary.total_energy_wh = total;
    summary.energy_kwh = total / 1000.0;
    summary.average_wh = total / static_cast<double>(readings_.size());
    summary.max_wh = peak;
    summary.min_wh = valley;
    summary.peak_wh = peak;

    /* Ignore single-reading outliers when reporting the steady-state
     * minimum: a 900 s gap with no pulses is not a real minimum. */
    std::vector<double> energies;
    energies.reserve(readings_.size());
    for (const Reading& r : readings_) energies.push_back(r.energy_wh);
    std::sort(energies.begin(), energies.end());
    const std::size_t trim = energies.size() > 10 ? energies.size() / 20 : 0;
    summary.min_wh_steady = energies[trim];

    const std::int64_t span = summary.last_epoch - summary.first_epoch;
    summary.span_seconds = span > 0 ? span : 0;
    if (span > 0) {
        summary.avg_pulses_per_hour =
            (total * pulses_per_wh_) / (static_cast<double>(span) / 3600.0);
    }
    return summary;
}

Summary AnalyticsEngine::summarizeWindow(std::size_t window) const {
    if (window == 0 || window >= readings_.size()) return summarize();

    Summary summary;
    const std::size_t offset = readings_.size() - window;
    summary.sample_count = window;
    double total = 0.0;
    double peak = -1.0;
    double valley = -1.0;
    for (std::size_t i = offset; i < readings_.size(); ++i) {
        const Reading& r = readings_[i];
        total += r.energy_wh;
        if (r.energy_wh > peak) {
            peak = r.energy_wh;
            summary.peak_epoch = r.epoch_seconds;
        }
        if (valley < 0.0 || r.energy_wh < valley) {
            valley = r.energy_wh;
            summary.valley_epoch = r.epoch_seconds;
        }
    }
    summary.total_energy_wh = total;
    summary.energy_kwh = total / 1000.0;
    summary.average_wh = total / static_cast<double>(window);
    summary.max_wh = peak;
    summary.min_wh = valley;
    summary.min_wh_steady = valley;
    summary.peak_wh = peak;
    summary.first_epoch = readings_[offset].epoch_seconds;
    summary.last_epoch = readings_.back().epoch_seconds;
    summary.span_seconds = summary.last_epoch > summary.first_epoch
                               ? summary.last_epoch - summary.first_epoch
                               : 0;
    if (summary.span_seconds > 0) {
        summary.avg_pulses_per_hour =
            (total * pulses_per_wh_) / (static_cast<double>(summary.span_seconds) / 3600.0);
    }
    return summary;
}

std::vector<std::pair<std::int64_t, double>> AnalyticsEngine::hourlyEnergy() const {
    std::map<std::int64_t, double> buckets;
    for (const Reading& r : readings_) {
        const std::int64_t hour = r.epoch_seconds / kSecondsPerHour;
        buckets[hour] += r.energy_wh;
    }
    return {buckets.begin(), buckets.end()};
}

std::vector<std::pair<std::int64_t, double>> AnalyticsEngine::dailyEnergy() const {
    std::map<std::int64_t, double> buckets;
    for (const Reading& r : readings_) {
        const std::int64_t day = r.epoch_seconds / kSecondsPerDay;
        buckets[day] += r.energy_wh;
    }
    return {buckets.begin(), buckets.end()};
}

std::vector<std::pair<std::int64_t, double>> AnalyticsEngine::recentHours(
    std::size_t count) const {
    std::vector<std::pair<std::int64_t, double>> all = hourlyEnergy();
    if (count == 0 || count >= all.size()) return all;
    return std::vector<std::pair<std::int64_t, double>>(all.end() - static_cast<std::ptrdiff_t>(count),
                                                        all.end());
}

std::vector<Anomaly> AnalyticsEngine::detectAnomalies(double z_threshold) const {
    std::vector<Anomaly> anomalies;
    if (readings_.size() < 4) return anomalies;

    /* Rolling window statistics over the preceding samples; the current
     * sample is excluded from its own baseline so a spike cannot mask
     * itself. */
    constexpr std::size_t kWindow = 20;
    std::vector<double> window;
    window.reserve(kWindow);

    for (std::size_t i = 0; i < readings_.size(); ++i) {
        const double value = readings_[i].energy_wh;
        if (!window.empty()) {
            const double mean =
                std::accumulate(window.begin(), window.end(), 0.0) /
                static_cast<double>(window.size());
            const double sd = safeStddev(window, mean);
            /* A flat baseline (sd == 0) must not divide by zero: fall back
             * to a relative test against the mean. */
            const bool spike = sd > 1e-9
                                   ? ((value - mean) / sd >= z_threshold)
                                   : (mean > 1e-9 && value >= mean * 2.0 && value > 0.0);
            if (spike && value > 0.0) {
                Anomaly a;
                a.epoch = readings_[i].epoch_seconds;
                a.energy_wh = value;
                a.baseline_wh = mean;
                a.z_score = sd > 1e-9 ? (value - mean) / sd
                                      : (mean > 1e-9 ? value / mean : 0.0);
                anomalies.push_back(a);
            }
        }
        window.push_back(value);
        if (window.size() > kWindow) window.erase(window.begin());
    }
    return anomalies;
}

double AnalyticsEngine::currentRatePerMinute() const {
    if (readings_.size() < 2) return 0.0;
    const Reading& latest = readings_.back();
    const Reading& previous = readings_[readings_.size() - 2];
    const std::int64_t interval = latest.epoch_seconds - previous.epoch_seconds;
    if (interval <= 0) return 0.0;
    const double pulses = static_cast<double>(latest.pulse_delta);
    return pulses * 60.0 / static_cast<double>(interval);
}

SpikeState AnalyticsEngine::updateSpike(std::int64_t epoch_seconds,
                                        double window_seconds) const {
    SpikeState state;
    const std::int64_t cutoff = epoch_seconds - static_cast<std::int64_t>(window_seconds);

    double window_total = 0.0;
    std::size_t window_count = 0;
    for (const Reading& r : readings_) {
        if (r.epoch_seconds >= cutoff) {
            window_total += r.energy_wh;
            ++window_count;
        }
    }
    if (window_count < 2) return state;

    const double baseline = window_total / static_cast<double>(window_count);
    double latest = 0.0;
    for (auto it = readings_.rbegin(); it != readings_.rend(); ++it) {
        if (it->epoch_seconds >= cutoff) {
            latest = it->energy_wh;
            break;
        }
    }
    if (baseline <= 1e-9) {
        state.ratio = latest > 0.0 ? 1e9 : 0.0;
    } else {
        state.ratio = latest / baseline;
    }
    state.in_spike = state.ratio >= 2.0;

    /* Count how many trailing samples are at or above 2x the baseline so
     * the dashboard can distinguish a one-off glitch from a sustained
     * overload (e.g. an air conditioner kicking in). */
    const double threshold = baseline * 2.0;
    for (auto it = readings_.rbegin(); it != readings_.rend(); ++it) {
        if (it->epoch_seconds < cutoff) break;
        if (it->energy_wh >= threshold) {
            ++state.consecutive;
        } else {
            break;
        }
    }
    return state;
}

}  // namespace smartmeter

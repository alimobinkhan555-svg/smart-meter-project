/*
 * selftest.cpp - dependency-free self test for the user-space modules.
 *
 * Covers the parts that are easy to get subtly wrong:
 *   * pulse -> energy conversion and cumulative deltas
 *   * counter reset handling (must not wrap around)
 *   * hourly / daily bucketing across a UTC midnight boundary
 *   * descriptive statistics
 *   * anomaly detection (an injected spike must be flagged)
 *   * CSV write -> read round trip, including malformed-line skipping
 *   * the shared memory ring (publish -> consume -> overflow accounting)
 *
 * Build: make test && ./build/selftest
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "analytics.h"
#include "file_io.h"
#include "ipc_channel.h"
#include "pulse_source.h"

using namespace smartmeter;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        std::cout << "  PASS  " << what << '\n';
    } else {
        ++g_failures;
        std::cout << "  FAIL  " << what << '\n';
    }
}

void checkNear(double actual, double expected, double tolerance, const std::string& what) {
    check(std::fabs(actual - expected) <= tolerance, what);
}

constexpr std::int64_t kHour = 3600;
constexpr std::int64_t kDay = 86400;

void testEnergyConversion() {
    std::cout << "\n[pulse -> energy conversion]\n";
    AnalyticsEngine engine(1.0);
    engine.appendSample(1000, 10);
    engine.appendSample(1001, 25);
    engine.appendSample(1002, 26);

    check(engine.size() == 3, "three readings recorded");
    checkNear(engine.summarize().total_energy_wh, 26.0, 1e-9,
              "total energy equals the cumulative pulse count");
    checkNear(engine.readings()[1].energy_wh, 15.0, 1e-9, "second sample delta is 15 Wh");
    checkNear(engine.readings()[2].energy_wh, 1.0, 1e-9, "third sample delta is 1 Wh");
}

void testPulsesPerWh() {
    std::cout << "\n[calibration: 1 pulse = 0.5 Wh]\n";
    AnalyticsEngine engine(0.5);
    engine.appendSample(1000, 0);
    engine.appendSample(1001, 10);
    checkNear(engine.summarize().total_energy_wh, 20.0, 1e-9,
              "10 pulses become 20 Wh at 2 pulses/Wh");
}

void testCounterReset() {
    std::cout << "\n[counter reset must not wrap around]\n";
    AnalyticsEngine engine(1.0);
    engine.appendSample(1000, 1000);
    engine.appendSample(1001, 1010);
    engine.appendSample(1002, 5); /* driver reloaded, counter restarted */

    const Summary summary = engine.summarize();
    checkNear(summary.total_energy_wh, 1015.0, 1e-9,
              "energy after a reset counts 1010 + 5 pulses, not a wrapped value");
    check(engine.readings().back().pulse_delta == 5, "post-reset delta is 5");
}

void testAggregation() {
    std::cout << "\n[hourly and daily aggregation]\n";
    AnalyticsEngine engine(1.0);
    /* 23:00, 23:30, 00:00, 00:30 - must split across two hours and two days. */
    const std::int64_t day = 18000 * kDay; /* arbitrary but UTC aligned */
    engine.appendSample(day + 23 * kHour, 10);
    engine.appendSample(day + 23 * kHour + 1800, 30);
    engine.appendSample(day + kDay, 45);
    engine.appendSample(day + kDay + 1800, 60);

    const auto hourly = engine.hourlyEnergy();
    const auto daily = engine.dailyEnergy();

    check(hourly.size() == 2, "two hourly buckets");
    check(daily.size() == 2, "two daily buckets");
    if (hourly.size() == 2 && daily.size() == 2) {
        checkNear(hourly[0].second, 30.0, 1e-9, "first hour holds 30 Wh");
        checkNear(hourly[1].second, 30.0, 1e-9, "second hour holds 30 Wh");
        checkNear(daily[0].second, 30.0, 1e-9, "day one holds 30 Wh");
        checkNear(daily[1].second, 30.0, 1e-9, "day two holds 30 Wh");
    }

    const Summary summary = engine.summarize();
    check(summary.peak_epoch == day + 23 * kHour + 1800, "peak sample is the 20 Wh step");
    checkNear(summary.max_wh, 20.0, 1e-9, "max per-sample energy is 20 Wh");
    checkNear(summary.average_wh, 15.0, 1e-9, "average per-sample energy is 15 Wh");
}

void testAnomalies() {
    std::cout << "\n[anomaly detection]\n";
    AnalyticsEngine engine(1.0);
    std::int64_t epoch = 20000 * kDay;
    std::uint64_t pulses = 0;
    for (int i = 0; i < 40; ++i) {
        pulses += 10;
        engine.appendSample(epoch + i * 60, pulses);
    }
    check(engine.detectAnomalies().empty(), "a flat baseline produces no anomalies");

    pulses += 400; /* sudden overload */
    engine.appendSample(epoch + 40 * 60, pulses);

    const auto anomalies = engine.detectAnomalies(3.0);
    check(!anomalies.empty(), "an injected 400-pulse spike is flagged");
    if (!anomalies.empty()) {
        checkNear(anomalies.front().energy_wh, 400.0, 1e-9, "anomaly magnitude is 400 Wh");
        check(anomalies.front().severity() == "CRITICAL" ||
                  anomalies.front().severity() == "HIGH",
              "anomaly severity is HIGH or CRITICAL");
    }
}

void testCurrentRate() {
    std::cout << "\n[instantaneous rate]\n";
    AnalyticsEngine engine(1.0);
    engine.appendSample(30000, 0);
    engine.appendSample(30060, 30); /* 30 pulses in 60 s = 30 pulses/min */
    checkNear(engine.currentRatePerMinute(), 30.0, 1e-9, "rate is 30 pulses per minute");
}

void testCsvRoundTrip() {
    std::cout << "\n[CSV write -> read round trip]\n";
    const std::string path = "logs/selftest_pulses.csv";
    std::remove(path.c_str());

    {
        CsvLogger logger(path, true);
        std::string error;
        check(logger.open(&error), "logger opens the file");
        logger.writeComment("selftest metadata");
        const std::int64_t base = 17000 * kDay;
        for (int i = 0; i < 5; ++i) {
            Reading reading;
            reading.epoch_seconds = base + i * 60;
            reading.pulse_count = 100 + i * 7;
            reading.pulse_delta = 7;
            reading.energy_wh = 7.0;
            check(logger.append(reading, &error), "row appended");
        }
        logger.close();
    }

    /* Append garbage that the reader has to skip. */
    {
        FILE* file = std::fopen(path.c_str(), "a");
        assert(file != nullptr);
        std::fputs("not-a-timestamp,42,42\n", file);
        std::fputs("# another comment\n", file);
        std::fputs("\n", file);
        std::fclose(file);
    }

    std::vector<Reading> loaded;
    std::size_t skipped = 0;
    std::string error;
    check(CsvReader::load(path, &loaded, &skipped, &error), "log parses back");
    check(loaded.size() == 5, "five data rows recovered");
    check(skipped == 1, "one malformed line skipped");
    if (loaded.size() == 5) {
        check(loaded.front().pulse_count == 100, "first pulse count round trips");
        check(loaded.back().pulse_count == 128, "last pulse count round trips");
        check(loaded.front().epoch_seconds == 17000 * kDay, "timestamp round trips");
    }
    std::remove(path.c_str());
}

void testTimestamps() {
    std::cout << "\n[timestamp helpers]\n";
    const std::int64_t epoch = 1759276800; /* 2025-10-01T00:00:00Z */
    const std::string text = fs_util::formatTimestamp(epoch);
    check(text == "2025-10-01T00:00:00Z", "formatTimestamp produces ISO-8601 UTC");

    std::int64_t parsed = 0;
    check(fs_util::parseTimestamp(text, &parsed) && parsed == epoch,
          "parseTimestamp is the inverse");
    check(fs_util::parseTimestamp("1759276800", &parsed) && parsed == epoch,
          "raw epoch seconds are also accepted");
}

void testIpcRing() {
    std::cout << "\n[shared memory ring]\n";
    char err[256] = {0};
    check(sm_ipc_shm_create("/smart_meter_selftest", 8, err, sizeof(err)) == 0,
          "ring created");

    for (int i = 0; i < 5; ++i) {
        sm_ipc_sample sample;
        sample.sequence = static_cast<uint64_t>(i);
        sample.timestamp_ns = monotonicNowNs();
        sample.epoch_seconds = static_cast<uint64_t>(realtimeNowSeconds());
        sample.pulse_count = static_cast<uint64_t>(100 + i);
        check(sm_ipc_shm_publish(&sample) == 1, "publish succeeds");
    }
    check(sm_ipc_shm_available() == 5, "five samples pending");

    sm_ipc_sample out;
    check(sm_ipc_shm_consume(&out) == 1 && out.sequence == 0, "first sample comes back in order");
    check(sm_ipc_shm_consume(&out) == 1 && out.sequence == 1, "second sample follows");

    /* Fill the ring past capacity: the oldest slot must be recycled and
     * the drop must be accounted for. */
    int overflowed = 0;
    for (int i = 0; i < 10; ++i) {
        sm_ipc_sample sample;
        sample.sequence = static_cast<uint64_t>(100 + i);
        sample.timestamp_ns = 0;
        sample.epoch_seconds = 0;
        sample.pulse_count = static_cast<uint64_t>(i);
        if (sm_ipc_shm_publish(&sample) == 0) ++overflowed;
    }
    check(overflowed > 0, "overflow is reported when the reader falls behind");
    check(sm_ipc_shm_dropped() == static_cast<uint64_t>(overflowed),
          "dropped counter matches the overflow");
    sm_ipc_shm_detach();

    check(sm_ipc_mq_create("/smart_meter_selftest_cmd", 4, 128, err, sizeof(err)) == 0,
          "message queue created");
    check(sm_ipc_mq_send("status") == 0, "command queued");
    char buffer[128];
    check(sm_ipc_mq_receive(buffer, sizeof(buffer), 100) > 0 &&
              std::string(buffer) == "status",
          "command received");
    sm_ipc_mq_close();
}

void testSimulator() {
    std::cout << "\n[in-process simulator]\n";
    SimulatedPulseSource source(36000, 0); /* 10 pulses/second */
    std::string error;
    check(source.open(&error), "simulator starts");

    source.inject(500, &error);
    RawSample sample;
    check(source.read(&sample, 500, &error), "simulator reports a sample");
    check(sample.pulse_count >= 500, "injected pulses are counted");

    SourceStats stats;
    check(source.getStats(&stats, &error), "simulator stats available");
    check(stats.source == SM_SOURCE_SIM, "stats report the simulated source");
    source.close();
}

}  // namespace

int main() {
    std::cout << "smart-meter self test\n===================\n";

    testEnergyConversion();
    testPulsesPerWh();
    testCounterReset();
    testAggregation();
    testAnomalies();
    testCurrentRate();
    testTimestamps();
    testCsvRoundTrip();
    testIpcRing();
    testSimulator();

    std::cout << "\n===================\n"
              << g_checks - g_failures << '/' << g_checks << " checks passed\n";
    if (g_failures > 0) {
        std::cout << g_failures << " FAILURE(S)\n";
        return EXIT_FAILURE;
    }
    std::cout << "all good\n";
    return EXIT_SUCCESS;
}

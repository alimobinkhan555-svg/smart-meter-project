/*
 * pulse_source.h - abstraction over the pulse producer.
 *
 * The dashboard never talks to the character device directly; it consumes
 * RawSample snapshots from a PulseSource.  Three implementations exist:
 *
 *   DevicePulseSource  - reads /dev/smart_meter (the kernel driver)
 *   SimulatedPulseSource - in-process generator, no root required
 *   FilePulseSource    - replays a CSV log, useful for demos and CI
 *
 * All three produce the same struct so the analytics engine is source
 * agnostic: this is the abstraction that lets the whole user-space stack
 * be exercised on a machine where the module is not loaded.
 */
#ifndef SMART_METER_PULSE_SOURCE_H
#define SMART_METER_PULSE_SOURCE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "driver_interface.h"

namespace smartmeter {

/* Cumulative counter snapshot plus the time it was taken. */
struct RawSample {
    std::uint64_t sequence{0};
    std::uint64_t timestamp_ns{0}; /* CLOCK_MONOTONIC */
    std::uint64_t pulse_count{0};
};

/* Factory. `kind` is one of: device, simulated, file, auto. */
enum class SourceKind { kDevice, kSimulated, kFile, kAuto };

SourceKind sourceKindFromString(const std::string& text, bool* ok = nullptr);
const char* sourceKindName(SourceKind kind);

struct SourceStats {
    std::uint64_t pulse_count{0};
    std::uint64_t pulses_delta{0};
    std::uint64_t last_pulse_ns{0};
    std::uint64_t driver_uptime_ns{0};
    std::uint64_t interrupt_count{0};
    std::uint64_t simulated_count{0};
    std::uint64_t injected_count{0};
    std::uint32_t source{SM_SOURCE_NONE};
    std::uint32_t enabled{0};
};

struct SourceConfig {
    std::uint32_t pulses_per_hour{3600};
    std::uint32_t source{SM_SOURCE_SIM};
    std::uint32_t enabled{1};
};

class PulseSource {
public:
    virtual ~PulseSource() = default;

    virtual bool open(std::string* error) = 0;
    virtual void close() = 0;
    /* Blocks until a snapshot is available or the poll timeout expires.
     * Returns true when `out` was filled. */
    virtual bool read(RawSample* out, int timeout_ms, std::string* error) = 0;
    virtual bool inject(std::uint64_t pulses, std::string* error) = 0;
    virtual bool reset(std::string* error) = 0;
    virtual bool getStats(SourceStats* out, std::string* error) const = 0;
    virtual bool getConfig(SourceConfig* out, std::string* error) const = 0;
    virtual bool setConfig(const SourceConfig& config, std::string* error) = 0;
    virtual const char* description() const = 0;
};

/* ------------------------------------------------------------------ */

class DevicePulseSource final : public PulseSource {
public:
    explicit DevicePulseSource(std::string path = SM_DEVICE_PATH);
    ~DevicePulseSource() override;

    bool open(std::string* error) override;
    void close() override;
    bool read(RawSample* out, int timeout_ms, std::string* error) override;
    bool inject(std::uint64_t pulses, std::string* error) override;
    bool reset(std::string* error) override;
    bool getStats(SourceStats* out, std::string* error) const override;
    bool getConfig(SourceConfig* out, std::string* error) const override;
    bool setConfig(const SourceConfig& config, std::string* error) override;
    const char* description() const override { return "linux character device"; }

    static bool deviceExists(const std::string& path);

private:
    std::string path_;
    int fd_{-1};
    std::uint64_t last_sequence_{0};
};

/* ------------------------------------------------------------------ */

/* In-process hrtimer equivalent: a thread emits `pulses_per_hour` pulses
 * per hour, with optional jitter so anomaly detection has something to
 * detect. `jitter_percent` scales each interval randomly. */
class SimulatedPulseSource final : public PulseSource {
public:
    explicit SimulatedPulseSource(std::uint32_t pulses_per_hour = 3600,
                                  std::uint32_t jitter_percent = 0);
    ~SimulatedPulseSource() override;

    bool open(std::string* error) override;
    void close() override;
    bool read(RawSample* out, int timeout_ms, std::string* error) override;
    bool inject(std::uint64_t pulses, std::string* error) override;
    bool reset(std::string* error) override;
    bool getStats(SourceStats* out, std::string* error) const override;
    bool getConfig(SourceConfig* out, std::string* error) const override;
    bool setConfig(const SourceConfig& config, std::string* error) override;
    const char* description() const override { return "in-process simulator"; }

    /* Fires a burst of pulses, mimicking a spike. */
    void simulateSpike(std::uint64_t pulses);

private:
    void emitterLoop();

    std::uint32_t pulses_per_hour_{3600};
    std::uint32_t jitter_percent_{0};
    std::atomic<std::uint64_t> pulse_count_{0};
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> simulated_count_{0};
    std::atomic<std::uint64_t> injected_count_{0};
    std::atomic<std::uint64_t> last_pulse_ns_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> enabled_{true};
    std::thread thread_;
};

/* ------------------------------------------------------------------ */

/* Replays a CSV log at wall-clock speed; used by demos and tests when no
 * meter is available. */
class FilePulseSource final : public PulseSource {
public:
    explicit FilePulseSource(std::string path, double speed = 60.0);
    ~FilePulseSource() override;

    bool open(std::string* error) override;
    void close() override;
    bool read(RawSample* out, int timeout_ms, std::string* error) override;
    bool inject(std::uint64_t pulses, std::string* error) override;
    bool reset(std::string* error) override;
    bool getStats(SourceStats* out, std::string* error) const override;
    bool getConfig(SourceConfig* out, std::string* error) const override;
    bool setConfig(const SourceConfig& config, std::string* error) override;
    const char* description() const override { return "CSV log replay"; }

private:
    std::string path_;
    double speed_;
    std::size_t cursor_{0};
    std::vector<std::uint64_t> pulses_;
    std::vector<std::uint64_t> timestamps_ns_;
    std::uint64_t injected_{0};
};

/* ------------------------------------------------------------------ */

struct SourceSelection {
    std::unique_ptr<PulseSource> source;
    SourceKind kind{SourceKind::kAuto};
    std::string device_error; /* why auto fell back to the simulator */
};

/* `auto` prefers the character device and silently degrades to the
 * simulator so the dashboard is always usable. */
SourceSelection makeSource(SourceKind kind, const std::string& device_path,
                           const std::string& csv_path, std::uint32_t pulses_per_hour);

/* CLOCK_MONOTONIC helper shared by the sources and the collector. */
std::uint64_t monotonicNowNs();
std::int64_t realtimeNowSeconds();

}  // namespace smartmeter

#endif /* SMART_METER_PULSE_SOURCE_H */

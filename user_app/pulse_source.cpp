/*
 * pulse_source.cpp - implementations of the three pulse producers.
 */
#include "pulse_source.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file_io.h"

namespace smartmeter {

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

std::uint64_t monotonicNowNs() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

std::int64_t realtimeNowSeconds() {
    timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return static_cast<std::int64_t>(ts.tv_sec);
}

SourceKind sourceKindFromString(const std::string& text, bool* ok) {
    if (ok) *ok = true;
    if (text == "device" || text == "dev") return SourceKind::kDevice;
    if (text == "sim" || text == "simulated" || text == "virtual") return SourceKind::kSimulated;
    if (text == "file" || text == "csv") return SourceKind::kFile;
    if (text == "auto") return SourceKind::kAuto;
    if (ok) *ok = false;
    return SourceKind::kAuto;
}

const char* sourceKindName(SourceKind kind) {
    switch (kind) {
        case SourceKind::kDevice: return "device";
        case SourceKind::kSimulated: return "simulated";
        case SourceKind::kFile: return "file";
        case SourceKind::kAuto: return "auto";
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */
/* DevicePulseSource                                                   */
/* ------------------------------------------------------------------ */

DevicePulseSource::DevicePulseSource(std::string path) : path_(std::move(path)) {}

DevicePulseSource::~DevicePulseSource() { close(); }

bool DevicePulseSource::deviceExists(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return false;
    return S_ISCHR(info.st_mode);
}

bool DevicePulseSource::open(std::string* error) {
    close();
    /* O_RDWR so that write()-based pulse injection also works. */
    fd_ = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        if (error) {
            *error = "open(" + path_ + ") failed: " + std::strerror(errno);
        }
        return false;
    }
    last_sequence_ = 0;
    return true;
}

void DevicePulseSource::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool DevicePulseSource::read(RawSample* out, int timeout_ms, std::string* error) {
    if (fd_ < 0 || out == nullptr) {
        if (error) *error = "device is not open";
        return false;
    }

    struct pollfd pfd {};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) return false;
        if (error) *error = std::string("poll failed: ") + std::strerror(errno);
        return false;
    }
    if (ready == 0) return false; /* timed out: caller refreshes the view */
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        if (error) *error = "device reported an error condition";
        return false;
    }

    sm_sample sample{};
    const ssize_t bytes = ::read(fd_, &sample, sizeof(sample));
    if (bytes < 0) {
        if (errno == EINTR || errno == EAGAIN) return false;
        if (error) *error = std::string("read failed: ") + std::strerror(errno);
        return false;
    }
    if (static_cast<std::size_t>(bytes) != sizeof(sample)) {
        if (error) *error = "short read from device";
        return false;
    }

    out->sequence = sample.sequence;
    out->timestamp_ns = sample.timestamp_ns;
    out->pulse_count = sample.pulse_count;
    last_sequence_ = sample.sequence;
    return true;
}

bool DevicePulseSource::inject(std::uint64_t pulses, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "device is not open";
        return false;
    }
    if (pulses == 0) pulses = 1;
    const sm_u64 value = static_cast<sm_u64>(pulses);
    const ssize_t written = ::write(fd_, &value, sizeof(value));
    if (written != static_cast<ssize_t>(sizeof(value))) {
        if (error) *error = std::string("write failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool DevicePulseSource::reset(std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "device is not open";
        return false;
    }
    if (::ioctl(fd_, SM_IOC_RESET) != 0) {
        if (error) *error = std::string("SM_IOC_RESET failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool DevicePulseSource::getStats(SourceStats* out, std::string* error) const {
    if (out == nullptr) return false;
    if (fd_ < 0) {
        if (error) *error = "device is not open";
        return false;
    }
    sm_stats stats{};
    if (::ioctl(fd_, SM_IOC_GET_STATS, &stats) != 0) {
        if (error) *error = std::string("SM_IOC_GET_STATS failed: ") + std::strerror(errno);
        return false;
    }
    out->pulse_count = stats.pulse_count;
    out->pulses_delta = stats.pulses_delta;
    out->last_pulse_ns = stats.last_pulse_ns;
    out->driver_uptime_ns = stats.driver_uptime_ns;
    out->interrupt_count = stats.interrupt_count;
    out->simulated_count = stats.simulated_count;
    out->injected_count = stats.injected_count;
    out->source = stats.source;
    out->enabled = stats.enabled;
    return true;
}

bool DevicePulseSource::getConfig(SourceConfig* out, std::string* error) const {
    if (out == nullptr) return false;
    if (fd_ < 0) {
        if (error) *error = "device is not open";
        return false;
    }
    sm_config config{};
    if (::ioctl(fd_, SM_IOC_GET_CONFIG, &config) != 0) {
        if (error) *error = std::string("SM_IOC_GET_CONFIG failed: ") + std::strerror(errno);
        return false;
    }
    out->pulses_per_hour = config.pulses_per_hour;
    out->source = config.source;
    out->enabled = config.enabled;
    return true;
}

bool DevicePulseSource::setConfig(const SourceConfig& config, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "device is not open";
        return false;
    }
    sm_config payload{};
    payload.pulses_per_hour = config.pulses_per_hour == 0 ? 1 : config.pulses_per_hour;
    payload.source = config.source;
    payload.enabled = config.enabled;
    payload.reserved = 0;
    if (::ioctl(fd_, SM_IOC_SET_CONFIG, &payload) != 0) {
        if (error) *error = std::string("SM_IOC_SET_CONFIG failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* SimulatedPulseSource                                                */
/* ------------------------------------------------------------------ */

SimulatedPulseSource::SimulatedPulseSource(std::uint32_t pulses_per_hour,
                                           std::uint32_t jitter_percent)
    : pulses_per_hour_(pulses_per_hour == 0 ? 1 : pulses_per_hour),
      jitter_percent_(jitter_percent > 100 ? 100 : jitter_percent) {}

SimulatedPulseSource::~SimulatedPulseSource() { close(); }

bool SimulatedPulseSource::open(std::string* error) {
    if (running_.load()) return true;
    (void)error;
    pulse_count_.store(0);
    sequence_.store(0);
    simulated_count_.store(0);
    injected_count_.store(0);
    last_pulse_ns_.store(monotonicNowNs());
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread(&SimulatedPulseSource::emitterLoop, this);
    return true;
}

void SimulatedPulseSource::close() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void SimulatedPulseSource::emitterLoop() {
    std::mt19937 rng(static_cast<std::uint32_t>(::getpid()) ^
                     static_cast<std::uint32_t>(monotonicNowNs()));
    std::uniform_int_distribution<int> jitter(100 - static_cast<int>(jitter_percent_),
                                              100 + static_cast<int>(jitter_percent_));

    while (!stop_.load()) {
        /* A real meter emits a fixed number of pulses per hour; the
         * interval between two pulses is 3600/pulses_per_hour seconds. */
        const double base_interval = 3600.0 / static_cast<double>(pulses_per_hour_);
        const double factor = static_cast<double>(jitter(rng)) / 100.0;
        double interval_ms = base_interval * factor * 1000.0;
        if (interval_ms < 1.0) interval_ms = 1.0;

        /* Sleep in small slices so close() stays responsive. */
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<long long>(interval_ms));
        while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (stop_.load()) break;
        if (!enabled_.load()) continue;

        pulse_count_.fetch_add(1);
        sequence_.fetch_add(1);
        simulated_count_.fetch_add(1);
        last_pulse_ns_.store(monotonicNowNs());
    }
}

bool SimulatedPulseSource::read(RawSample* out, int timeout_ms, std::string* error) {
    if (out == nullptr) return false;
    if (!running_.load()) {
        if (error) *error = "simulator is not running";
        return false;
    }

    /* Wait until the sequence changes, or the timeout expires. */
    const std::uint64_t start_sequence = sequence_.load();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    while (sequence_.load() == start_sequence) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    out->sequence = sequence_.load();
    out->timestamp_ns = monotonicNowNs();
    out->pulse_count = pulse_count_.load();
    return true;
}

bool SimulatedPulseSource::inject(std::uint64_t pulses, std::string* error) {
    if (error) error->clear();
    if (pulses == 0) pulses = 1;
    pulse_count_.fetch_add(pulses);
    sequence_.fetch_add(pulses);
    injected_count_.fetch_add(pulses);
    last_pulse_ns_.store(monotonicNowNs());
    return true;
}

void SimulatedPulseSource::simulateSpike(std::uint64_t pulses) { inject(pulses, nullptr); }

bool SimulatedPulseSource::reset(std::string* error) {
    if (error) error->clear();
    pulse_count_.store(0);
    sequence_.store(0);
    simulated_count_.store(0);
    injected_count_.store(0);
    last_pulse_ns_.store(monotonicNowNs());
    return true;
}

bool SimulatedPulseSource::getStats(SourceStats* out, std::string* error) const {
    if (out == nullptr) return false;
    if (error) error->clear();
    out->pulse_count = pulse_count_.load();
    out->pulses_delta = out->pulse_count;
    out->last_pulse_ns = last_pulse_ns_.load();
    out->driver_uptime_ns = 0;
    out->interrupt_count = sequence_.load();
    out->simulated_count = simulated_count_.load();
    out->injected_count = injected_count_.load();
    out->source = SM_SOURCE_SIM;
    out->enabled = enabled_.load() ? 1u : 0u;
    return true;
}

bool SimulatedPulseSource::getConfig(SourceConfig* out, std::string* error) const {
    if (out == nullptr) return false;
    if (error) error->clear();
    out->pulses_per_hour = pulses_per_hour_;
    out->source = SM_SOURCE_SIM;
    out->enabled = enabled_.load() ? 1u : 0u;
    return true;
}

bool SimulatedPulseSource::setConfig(const SourceConfig& config, std::string* error) {
    if (error) error->clear();
    if (config.pulses_per_hour > 0) pulses_per_hour_ = config.pulses_per_hour;
    enabled_.store(config.enabled != 0);
    return true;
}

/* ------------------------------------------------------------------ */
/* FilePulseSource                                                     */
/* ------------------------------------------------------------------ */

FilePulseSource::FilePulseSource(std::string path, double speed)
    : path_(std::move(path)), speed_(speed > 0.0 ? speed : 1.0) {}

FilePulseSource::~FilePulseSource() = default;

bool FilePulseSource::open(std::string* error) {
    std::vector<Reading> readings;
    if (!CsvReader::load(path_, &readings, nullptr, error)) return false;

    pulses_.clear();
    timestamps_ns_.clear();
    pulses_.reserve(readings.size());
    timestamps_ns_.reserve(readings.size());
    const std::int64_t base = readings.front().epoch_seconds;
    for (const Reading& reading : readings) {
        pulses_.push_back(reading.pulse_count);
        timestamps_ns_.push_back(static_cast<std::uint64_t>(
            (reading.epoch_seconds - base) * 1000000000LL / static_cast<std::int64_t>(speed_)));
    }
    cursor_ = 0;
    return true;
}

void FilePulseSource::close() {
    pulses_.clear();
    timestamps_ns_.clear();
    cursor_ = 0;
}

bool FilePulseSource::read(RawSample* out, int timeout_ms, std::string* error) {
    if (out == nullptr) return false;
    if (cursor_ >= pulses_.size()) {
        if (error) error->clear();
        return false; /* end of replay */
    }
    /* Pacing: do not return the next record before it is "due". */
    if (cursor_ > 0) {
        const std::uint64_t due = timestamps_ns_[cursor_];
        while (monotonicNowNs() < due) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    out->sequence = cursor_;
    out->timestamp_ns = monotonicNowNs();
    out->pulse_count = pulses_[cursor_];
    ++cursor_;
    return true;
}

bool FilePulseSource::inject(std::uint64_t pulses, std::string* error) {
    if (error) error->clear();
    injected_ += pulses;
    if (!pulses_.empty()) pulses_.back() += pulses;
    return true;
}

bool FilePulseSource::reset(std::string* error) {
    if (error) error->clear();
    cursor_ = 0;
    injected_ = 0;
    return true;
}

bool FilePulseSource::getStats(SourceStats* out, std::string* error) const {
    if (out == nullptr) return false;
    if (error) error->clear();
    out->pulse_count = pulses_.empty() ? 0 : pulses_.back();
    out->pulses_delta = out->pulse_count;
    out->last_pulse_ns = monotonicNowNs();
    out->driver_uptime_ns = 0;
    out->interrupt_count = cursor_;
    out->simulated_count = 0;
    out->injected_count = injected_;
    out->source = SM_SOURCE_NONE;
    out->enabled = 1;
    return true;
}

bool FilePulseSource::getConfig(SourceConfig* out, std::string* error) const {
    if (out == nullptr) return false;
    if (error) error->clear();
    out->pulses_per_hour = 3600;
    out->source = SM_SOURCE_NONE;
    out->enabled = 1;
    return true;
}

bool FilePulseSource::setConfig(const SourceConfig& config, std::string* error) {
    if (error) error->clear();
    (void)config; /* replay rate is fixed at construction time */
    return true;
}

/* ------------------------------------------------------------------ */
/* factory                                                             */
/* ------------------------------------------------------------------ */

SourceSelection makeSource(SourceKind kind, const std::string& device_path,
                           const std::string& csv_path, std::uint32_t pulses_per_hour) {
    SourceSelection selection;
    selection.kind = kind;

    auto tryDevice = [&]() -> bool {
        if (!DevicePulseSource::deviceExists(device_path)) {
            selection.device_error = device_path + " does not exist";
            return false;
        }
        auto device = std::unique_ptr<DevicePulseSource>(new DevicePulseSource(device_path));
        std::string error;
        if (!device->open(&error)) {
            selection.device_error = error;
            return false;
        }
        selection.kind = SourceKind::kDevice;
        selection.source = std::move(device);
        return true;
    };

    if (kind == SourceKind::kDevice) {
        if (!tryDevice()) {
            /* Explicitly requested but unavailable: still hand back a
             * working simulator so the app never dead-ends. */
            selection.kind = SourceKind::kSimulated;
            selection.source =
                std::unique_ptr<SimulatedPulseSource>(new SimulatedPulseSource(pulses_per_hour));
        }
        return selection;
    }

    if (kind == SourceKind::kFile) {
        auto file = std::unique_ptr<FilePulseSource>(new FilePulseSource(csv_path));
        std::string error;
        if (!file->open(&error)) {
            selection.kind = SourceKind::kSimulated;
            selection.device_error = error;
            selection.source =
                std::unique_ptr<SimulatedPulseSource>(new SimulatedPulseSource(pulses_per_hour));
            return selection;
        }
        selection.kind = SourceKind::kFile;
        selection.source = std::move(file);
        return selection;
    }

    if (kind == SourceKind::kSimulated) {
        selection.kind = SourceKind::kSimulated;
        selection.source =
            std::unique_ptr<SimulatedPulseSource>(new SimulatedPulseSource(pulses_per_hour));
        return selection;
    }

    /* auto */
    if (tryDevice()) return selection;
    selection.kind = SourceKind::kSimulated;
    selection.source =
        std::unique_ptr<SimulatedPulseSource>(new SimulatedPulseSource(pulses_per_hour));
    return selection;
}

}  // namespace smartmeter

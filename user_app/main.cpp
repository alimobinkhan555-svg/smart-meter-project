/*
 * main.cpp - terminal dashboard and menu driven front end.
 *
 * Responsibilities:
 *   * pick a pulse source (character device, in-process simulator, or CSV
 *     replay) and start the acquisition pipeline (two worker threads);
 *   * drain the reading queue into the AnalyticsEngine and render the live
 *     view, statistics, hourly/daily breakdowns and anomaly reports;
 *   * expose controls: inject pulses, change the simulated rate, pause the
 *     counter, export data, reset;
 *   * publish every reading into a POSIX shared memory ring and accept
 *     control commands over a POSIX message queue (IPC).
 *
 * Build:  g++ -std=c++17 -pthread -I../include *.cpp -o app
 * Run  :  ./app --help
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

#include "analytics.h"
#include "collector.h"
#include "csv_export.h"
#include "driver_interface.h"
#include "file_io.h"
#include "ipc_channel.h"
#include "pulse_source.h"

using namespace smartmeter;

namespace {

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;
constexpr std::size_t kDefaultHistoryLimit = 100000;
constexpr std::size_t kTailRows = 15;

std::atomic<bool> g_interrupted{false};

void handleSignal(int) { g_interrupted.store(true); }

/* ------------------------------------------------------------------ */
/* formatting helpers                                                  */
/* ------------------------------------------------------------------ */

/* Numbers are always rendered through these so a manipulator never ends
 * up inside a std::string (which would silently print nothing useful). */
template <typename T>
std::string num(T value, int precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string numInt(long long value) { return std::to_string(value); }

void clearScreen() { std::cout << "\033[2J\033[H" << std::flush; }

std::string rule(char character, int width = 78) {
    return std::string(static_cast<std::size_t>(width), character);
}

std::string bar(double value, double maximum, int width = 30) {
    if (maximum <= 0.0 || value <= 0.0) {
        return std::string(static_cast<std::size_t>(width), '.');
    }
    const double fraction = value / maximum;
    int filled = static_cast<int>(fraction * width + 0.5);
    filled = std::max(0, std::min(width, filled));
    return std::string(static_cast<std::size_t>(filled), '#') +
           std::string(static_cast<std::size_t>(width - filled), '.');
}

std::string humanDuration(std::int64_t seconds) {
    if (seconds <= 0) return "0s";
    const std::int64_t days = seconds / 86400;
    const std::int64_t hours = (seconds % 86400) / 3600;
    const std::int64_t minutes = (seconds % 3600) / 60;
    const std::int64_t secs = seconds % 60;

    std::ostringstream out;
    if (days > 0) out << days << "d ";
    if (days > 0 || hours > 0) out << hours << "h ";
    if (days > 0 || hours > 0 || minutes > 0) out << minutes << "m ";
    out << secs << "s";
    return out.str();
}

/* `hourlyEnergy()`/`dailyEnergy()` return bucket indices (hours/days since
 * the epoch), not raw timestamps, so scale them back before formatting. */
std::string hourLabel(std::int64_t hour_bucket) {
    const std::string full = fs_util::formatTimestamp(hour_bucket * 3600);
    if (full.size() >= 17) return full.substr(5, 11); /* MM-DD HH:MM */
    return full;
}

std::string dayLabel(std::int64_t day_bucket) {
    const std::string full = fs_util::formatTimestamp(day_bucket * 86400);
    if (full.size() >= 11) return full.substr(0, 10); /* YYYY-MM-DD */
    return full;
}

void printKeyValue(const std::string& key, const std::string& value, int width = 22) {
    std::cout << "  " << std::left << std::setw(width) << key << std::right << ": " << value
              << '\n';
}

void printHeader(const std::string& title) {
    std::cout << "\n" << title << '\n';
    std::cout << rule('-') << '\n';
}

/* ------------------------------------------------------------------ */
/* options                                                             */
/* ------------------------------------------------------------------ */

struct Options {
    SourceKind kind{SourceKind::kAuto};
    std::string device_path{SM_DEVICE_PATH};
    std::string csv_path{"samples/sample_history.csv"};
    std::string log_path{"logs/pulses.csv"};
    std::string export_path;
    std::string ipc_shm{SM_IPC_DEFAULT_SHM};
    std::string ipc_mq{SM_IPC_DEFAULT_MQ};
    double pulses_per_wh{1.0};
    std::uint32_t pulses_per_hour{3600};
    int interval_ms{500};
    std::size_t history_limit{kDefaultHistoryLimit};
    bool publish{false};
    bool consume_ipc{false};
    bool demo{false};
    int run_seconds{0};
    bool quiet{false};
    bool help{false};
    bool version{false};
};

void printUsage(const char* program) {
    std::cout << "Smart Meter - pulse counter and analytics agent\n\n"
              << "USAGE\n  " << program << " [options]\n\n"
              << "PULSE SOURCE\n"
              << "  --source <auto|device|sim|file>  Where pulses come from. 'auto' prefers\n"
              << "                                   /dev/smart_meter and falls back to the\n"
              << "                                   in-process simulator (default: auto).\n"
              << "  --device <path>                  Character device (default: /dev/smart_meter)\n"
              << "  --replay <file.csv>              Replay a CSV log at 60x speed\n"
              << "  --pulses-per-hour <n>            Simulated meter rate (default: 3600)\n"
              << "  --pulses-per-wh <n>              Energy per pulse (default: 1.0)\n\n"
              << "ACQUISITION\n"
              << "  --interval-ms <n>                Dashboard refresh / poll interval\n"
              << "  --log <file.csv>                 CSV log (default: logs/pulses.csv)\n"
              << "  --history-limit <n>              Readings kept in RAM (0 = unlimited)\n\n"
              << "OUTPUT AND IPC\n"
              << "  --publish                        Publish every reading into shared memory\n"
              << "  --ipc                            Consume pulses from shared memory\n"
              << "  --shm-name <name>                POSIX shm object (default: "
              << SM_IPC_DEFAULT_SHM << ")\n"
              << "  --mq-name <name>                 POSIX message queue (default: "
              << SM_IPC_DEFAULT_MQ << ")\n"
              << "  --export <base>                  Write <base>_samples.csv,\n"
              << "                                   <base>_summary.json and <base>.gp\n\n"
              << "RUN MODES\n"
              << "  --demo                           Non-interactive 30 second demo\n"
              << "  --run-seconds <n>                Run for n seconds, then exit\n"
              << "  --quiet                          Suppress per-interval output\n"
              << "  --version / --help\n\n"
              << "EXAMPLES\n"
              << "  sudo insmod smart_meter_driver.ko simulate=1 pulses_per_hour=1800\n"
              << "  " << program << " --source auto --interval-ms 500 --log logs/pulses.csv\n"
              << "  " << program << " --demo                     # works without root\n"
              << "  " << program << " --source file --replay samples/sample_history.csv\n"
              << "  " << program << " --publish &               # shared memory producer\n"
              << "  " << program << " --ipc                     # shared memory consumer\n";
}

std::string requireValue(int argc, char** argv, int& index, const char* flag) {
    if (index + 1 >= argc) {
        std::cerr << "error: " << flag << " requires a value\n";
        std::exit(EXIT_FAILURE);
    }
    return std::string(argv[++index]);
}

bool parseOptions(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options->help = true;
        } else if (arg == "--version") {
            options->version = true;
        } else if (arg == "--source") {
            bool ok = false;
            options->kind =
                sourceKindFromString(requireValue(argc, argv, i, "--source"), &ok);
            if (!ok) {
                std::cerr << "error: unknown source kind\n";
                return false;
            }
        } else if (arg == "--device") {
            options->device_path = requireValue(argc, argv, i, "--device");
        } else if (arg == "--replay") {
            options->csv_path = requireValue(argc, argv, i, "--replay");
            options->kind = SourceKind::kFile;
        } else if (arg == "--pulses-per-hour") {
            options->pulses_per_hour = static_cast<std::uint32_t>(
                std::stoul(requireValue(argc, argv, i, "--pulses-per-hour")));
        } else if (arg == "--pulses-per-wh") {
            options->pulses_per_wh =
                std::stod(requireValue(argc, argv, i, "--pulses-per-wh"));
        } else if (arg == "--interval-ms") {
            options->interval_ms = std::stoi(requireValue(argc, argv, i, "--interval-ms"));
        } else if (arg == "--log") {
            options->log_path = requireValue(argc, argv, i, "--log");
        } else if (arg == "--history-limit") {
            options->history_limit = static_cast<std::size_t>(
                std::stoull(requireValue(argc, argv, i, "--history-limit")));
        } else if (arg == "--publish") {
            options->publish = true;
        } else if (arg == "--ipc") {
            options->consume_ipc = true;
        } else if (arg == "--shm-name") {
            options->ipc_shm = requireValue(argc, argv, i, "--shm-name");
        } else if (arg == "--mq-name") {
            options->ipc_mq = requireValue(argc, argv, i, "--mq-name");
        } else if (arg == "--export") {
            options->export_path = requireValue(argc, argv, i, "--export");
        } else if (arg == "--demo") {
            options->demo = true;
        } else if (arg == "--run-seconds") {
            options->run_seconds = std::stoi(requireValue(argc, argv, i, "--run-seconds"));
        } else if (arg == "--quiet") {
            options->quiet = true;
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return false;
        }
    }

    if (options->demo) {
        /* A demo must show visible data quickly: one pulse per second with
         * a 250 ms refresh gives ~120 readings over 30 s. */
        options->kind = SourceKind::kSimulated;
        options->pulses_per_hour = 3600;
        options->interval_ms = 250;
        if (options->run_seconds <= 0) options->run_seconds = 30;
    }
    if (options->interval_ms <= 0) options->interval_ms = 500;
    if (options->pulses_per_hour == 0) options->pulses_per_hour = 3600;
    if (options->pulses_per_wh <= 0.0) options->pulses_per_wh = 1.0;
    return true;
}

/* ------------------------------------------------------------------ */
/* IPC: producer side                                                  */
/* ------------------------------------------------------------------ */

/*
 * Wraps the POSIX shm ring.  When enabled, every reading produced by the
 * pipeline is republished so other processes (dashboards, web servers,
 * billing jobs) can consume the pulse stream without touching
 * /dev/smart_meter themselves.
 */
class IpcPublisher {
public:
    bool start(const std::string& shm_name, const std::string& mq_name, std::string* error) {
        char buffer[256] = {0};
        if (sm_ipc_shm_create(shm_name.c_str(), 1024, buffer, sizeof(buffer)) != 0) {
            if (error) *error = buffer;
            return false;
        }
        if (sm_ipc_mq_create(mq_name.c_str(), 16, 256, buffer, sizeof(buffer)) != 0) {
            /* Shared memory is the important half; the queue is optional. */
            std::cerr << "warning: message queue unavailable: " << buffer << '\n';
        } else {
            mq_name_ = mq_name;
        }
        return true;
    }

    void publish(const Reading& reading, std::uint64_t sequence) {
        sm_ipc_sample sample;
        sample.sequence = sequence;
        sample.timestamp_ns = monotonicNowNs();
        sample.epoch_seconds = static_cast<std::uint64_t>(reading.epoch_seconds);
        sample.pulse_count = reading.pulse_count;
        if (sm_ipc_shm_publish(&sample) == 0) ++dropped_;
        ++published_;
    }

    void pollCommands(const std::function<void(const std::string&)>& handler) {
        if (mq_name_.empty() || !handler) return;
        char buffer[256];
        while (sm_ipc_mq_receive(buffer, sizeof(buffer), 0) > 0) {
            handler(std::string(buffer));
        }
    }

    std::uint64_t published() const { return published_; }
    std::uint64_t dropped() const { return dropped_; }

    void stop() {
        sm_ipc_mq_close();
        sm_ipc_shm_detach();
    }

private:
    std::string mq_name_;
    std::uint64_t published_{0};
    std::uint64_t dropped_{0};
};

/* Consumer side: pulses published by another process through shared memory. */
class IpcConsumer {
public:
    bool start(const std::string& shm_name, std::string* error) {
        char buffer[256] = {0};
        if (sm_ipc_shm_attach(shm_name.c_str(), buffer, sizeof(buffer)) != 0) {
            if (error) *error = buffer;
            return false;
        }
        return true;
    }

    /* Moves everything currently queued into `sink`; returns the count. */
    std::size_t pump(const std::function<bool(const RawSample&)>& sink) {
        std::size_t moved = 0;
        sm_ipc_sample sample;
        while (sm_ipc_shm_consume(&sample) == 1) {
            RawSample raw;
            raw.sequence = sample.sequence;
            raw.timestamp_ns = sample.timestamp_ns;
            raw.pulse_count = sample.pulse_count;
            if (!sink(raw)) break;
            ++moved;
        }
        return moved;
    }

    std::uint64_t produced() const { return sm_ipc_shm_produced(); }
    void stop() { sm_ipc_shm_detach(); }
};

/* ------------------------------------------------------------------ */
/* application state                                                   */
/* ------------------------------------------------------------------ */

struct AppState {
    Options options;
    AnalyticsEngine engine;
    std::unique_ptr<CsvLogger> logger;
    std::unique_ptr<PulseSource> source;
    std::unique_ptr<IpcPublisher> publisher;
    std::unique_ptr<IpcConsumer> consumer;
    std::unique_ptr<Collector> collector;
    std::unique_ptr<LoggerWorker> logger_worker;

    BoundedQueue<RawSample> sample_queue{4096};
    BoundedQueue<Reading> reading_queue{4096};

    SourceStats device_stats{};
    std::uint64_t ipc_sequence{0};
    std::uint64_t baseline_pulses{0}; /* pulses already present in the log */
    std::int64_t started_epoch{0};
    std::size_t loaded_rows{0};
    bool collection_active{false};
    bool ipc_attached{false};

    AppState() : engine(1.0) {}

    std::string sourceLabel() const {
        if (ipc_attached) return "shared memory (IPC consumer)";
        if (source) return source->description();
        return "none";
    }

    double baselineEnergyWh() const {
        return static_cast<double>(baseline_pulses) / engine.pulsesPerWh();
    }

    std::uint64_t sessionPulses() const {
        const std::uint64_t total = device_stats.pulse_count;
        return total > baseline_pulses ? total - baseline_pulses : 0;
    }
};

void refreshDeviceStats(AppState& state) {
    if (!state.source) return;
    SourceStats stats;
    std::string error;
    if (state.source->getStats(&stats, &error)) state.device_stats = stats;
}

/* Pull everything the worker threads produced into the analytics engine. */
std::size_t drainReadings(AppState& state) {
    std::size_t drained = 0;
    Reading reading;
    while (state.reading_queue.tryPop(&reading)) {
        state.engine.addReading(reading);
        ++drained;
        if (state.publisher) state.publisher->publish(reading, state.ipc_sequence++);
    }
    return drained;
}

/* Move any pulses waiting in shared memory into the sample queue. */
std::size_t pumpIpc(AppState& state) {
    if (!state.consumer) return 0;
    return state.consumer->pump([&state](const RawSample& raw) {
        return state.sample_queue.push(raw, &g_interrupted);
    });
}

/* Handle a control command received over the POSIX message queue. */
void handleIpcCommand(AppState& state, const std::string& command) {
    std::cout << "\n  [ipc] command received: " << command << '\n';
    if (command == "reset") {
        if (state.source) {
            std::string error;
            state.source->reset(&error);
            refreshDeviceStats(state);
            std::cout << "  [ipc] counter reset\n";
        }
    } else if (command.rfind("rate=", 0) == 0) {
        if (state.source) {
            const long value = std::strtol(command.c_str() + 5, nullptr, 10);
            SourceConfig config;
            std::string error;
            if (value > 0 && state.source->getConfig(&config, &error)) {
                config.pulses_per_hour = static_cast<std::uint32_t>(value);
                config.enabled = 1;
                config.source = SM_SOURCE_SIM;
                state.source->setConfig(config, &error);
                state.options.pulses_per_hour = static_cast<std::uint32_t>(value);
                std::cout << "  [ipc] rate set to " << value << " pulses/hour\n";
            }
        }
    } else if (command == "pause" || command == "resume") {
        const bool paused = command == "pause";
        if (state.collector) {
            state.collector->setPaused(paused);
            state.collection_active = state.collector->running() && !paused;
        }
        std::cout << "  [ipc] collector " << (paused ? "paused" : "resumed") << '\n';
    } else if (command == "status") {
        const Summary summary = state.engine.summarize();
        std::cout << "  [ipc] pulses=" << state.device_stats.pulse_count
                  << " energy=" << num(summary.total_energy_wh + state.baselineEnergyWh())
                  << " Wh readings=" << state.engine.size() << '\n';
    } else {
        std::cout << "  [ipc] unknown command\n";
    }
}

void pollIpcCommands(AppState& state) {
    if (!state.publisher) return;
    state.publisher->pollCommands([&state](const std::string& command) {
        handleIpcCommand(state, command);
    });
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */
/* ------------------------------------------------------------------ */

void renderLiveView(AppState& state) {
    const Summary summary = state.engine.summarize();
    const std::int64_t now = realtimeNowSeconds();
    const SpikeState spike = state.engine.updateSpike(now, 60.0);
    const double lifetime_wh = summary.total_energy_wh + state.baselineEnergyWh();

    clearScreen();
    std::cout << rule('=') << '\n';
    std::cout << "  SMART METER - PULSE COUNTER & ANALYTICS DASHBOARD   v" << kVersionMajor << '.'
              << kVersionMinor << '\n';
    std::cout << rule('=') << '\n';

    printHeader("[ LIVE ]");
    printKeyValue("time", fs_util::formatTimestamp(now));
    printKeyValue("source", state.sourceLabel());
    printKeyValue("pulse count", numInt(static_cast<long long>(state.device_stats.pulse_count)));
    printKeyValue("cumulative energy",
                  num(lifetime_wh, 2) + " Wh (" + num(lifetime_wh / 1000.0, 4) + " kWh)");
    printKeyValue("this session", numInt(static_cast<long long>(state.sessionPulses())) +
                                       " pulses in " +
                                       humanDuration(now - state.started_epoch));
    printKeyValue("current rate", num(state.engine.currentRatePerMinute(), 2) + " pulses/min");
    printKeyValue("collector", state.collection_active ? "RUNNING" : "STOPPED");
    printKeyValue("spike detector", spike.in_spike
                                       ? "SPIKE x" + num(spike.ratio, 1) + " for " +
                                             numInt(static_cast<long long>(spike.consecutive)) +
                                             " samples"
                                       : "normal");

    printHeader("[ COUNTERS ]");
    printKeyValue("samples acquired",
                  numInt(state.collector ? static_cast<long long>(state.collector->samplesCollected())
                                         : 0));
    printKeyValue("rows in CSV",
                  numInt(state.logger_worker
                             ? static_cast<long long>(state.logger_worker->rowsWritten())
                             : 0));
    printKeyValue("in-memory readings", numInt(static_cast<long long>(state.engine.size())));
    printKeyValue("queue depth", numInt(static_cast<long long>(state.sample_queue.size())) + " / " +
                                      numInt(static_cast<long long>(state.sample_queue.capacity())));
    printKeyValue("historic readings restored", numInt(static_cast<long long>(state.loaded_rows)));
    if (state.device_stats.source == SM_SOURCE_GPIO) {
        printKeyValue("interrupts handled",
                      numInt(static_cast<long long>(state.device_stats.interrupt_count)));
    } else if (state.device_stats.source == SM_SOURCE_SIM) {
        printKeyValue("simulated pulses",
                      numInt(static_cast<long long>(state.device_stats.simulated_count)));
    }
    if (state.device_stats.injected_count > 0) {
        printKeyValue("injected pulses",
                      numInt(static_cast<long long>(state.device_stats.injected_count)));
    }
    if (state.ipc_attached) {
        printKeyValue("ipc produced", numInt(static_cast<long long>(sm_ipc_shm_produced())) +
                                          " (dropped " +
                                          numInt(static_cast<long long>(sm_ipc_shm_dropped())) + ")");
    }

    printHeader("[ RECENT ACTIVITY - last " + numInt(static_cast<long long>(kTailRows)) +
                " readings ]");
    std::cout << "  " << std::left << std::setw(22) << "TIMESTAMP" << std::right << std::setw(12)
              << "PULSES" << std::setw(10) << "DELTA" << std::setw(12) << "ENERGY(Wh)" << '\n';
    const std::vector<Reading>& history = state.engine.readings();
    const std::size_t first = history.size() > kTailRows ? history.size() - kTailRows : 0;
    for (std::size_t i = first; i < history.size(); ++i) {
        const Reading& reading = history[i];
        std::cout << "  " << std::left << std::setw(22)
                  << fs_util::formatTimestamp(reading.epoch_seconds) << std::right << std::setw(12)
                  << reading.pulse_count << std::setw(10) << reading.pulse_delta << std::setw(12)
                  << num(reading.energy_wh) << '\n';
    }

    printHeader("[ MENU ]");
    std::cout << "   1) refresh        2) statistics     3) hourly usage   4) daily usage\n"
              << "   5) peak usage     6) anomalies      7) inject pulses  8) set rate\n"
              << "   9) start/stop     10) export data   11) tail CSV log  12) reset counters\n"
              << "  13) load CSV log   14) send IPC cmd    0) quit\n";
    std::cout << rule('-') << '\n' << "choice: " << std::flush;
}

void renderStatistics(AppState& state) {
    const Summary summary = state.engine.summarize();
    printHeader("=== ENERGY STATISTICS ===");
    printKeyValue("samples analysed", numInt(static_cast<long long>(summary.sample_count)));
    printKeyValue("total energy", num(summary.total_energy_wh) + " Wh (" +
                                       num(summary.energy_kwh, 4) + " kWh)");
    printKeyValue("total pulses",
                  numInt(static_cast<long long>(state.device_stats.pulse_count)));
    printKeyValue("average per sample", num(summary.average_wh) + " Wh");
    printKeyValue("maximum", num(summary.max_wh) + " Wh at " +
                                 fs_util::formatTimestamp(summary.peak_epoch));
    printKeyValue("minimum (steady)", num(summary.min_wh_steady) + " Wh at " +
                                            fs_util::formatTimestamp(summary.valley_epoch));
    printKeyValue("average rate", num(summary.avg_pulses_per_hour, 1) + " pulses/hour");
    printKeyValue("window", fs_util::formatTimestamp(summary.first_epoch) + "  ->  " +
                                fs_util::formatTimestamp(summary.last_epoch) + "  (" +
                                humanDuration(summary.span_seconds) + ")");

    const Summary window = state.engine.summarizeWindow(20);
    std::cout << "\n  last 20 samples: " << num(window.total_energy_wh) << " Wh, avg "
              << num(window.average_wh) << " Wh, max " << num(window.max_wh) << " Wh\n";

    const auto anomalies = state.engine.detectAnomalies();
    std::cout << "  anomalies in history: " << anomalies.size() << " (use menu 6 for details)\n";
}

void renderHourly(AppState& state) {
    const auto hourly = state.engine.recentHours(24);
    printHeader("=== HOURLY USAGE (last " + numInt(static_cast<long long>(hourly.size())) +
                " hours) ===");
    if (hourly.empty()) {
        std::cout << "  no data collected yet\n";
        return;
    }
    double maximum = 0.0;
    for (const auto& bucket : hourly) maximum = std::max(maximum, bucket.second);

    std::cout << "  " << std::left << std::setw(18) << "HOUR" << std::right << std::setw(12)
              << "ENERGY(Wh)" << "   GRAPH\n";
    for (const auto& bucket : hourly) {
        std::cout << "  " << std::left << std::setw(18) << hourLabel(bucket.first) << std::right
                  << std::setw(12) << num(bucket.second) << "   " << bar(bucket.second, maximum)
                  << '\n';
    }
    std::cout << "\n  peak hour: " << hourLabel(hourly.front().first) << " .. "
              << hourLabel(hourly.back().first) << '\n';
}

void renderDaily(AppState& state) {
    const auto daily = state.engine.dailyEnergy();
    printHeader("=== DAILY USAGE (" + numInt(static_cast<long long>(daily.size())) + " days) ===");
    if (daily.empty()) {
        std::cout << "  no data collected yet\n";
        return;
    }
    double maximum = 0.0;
    double cumulative = 0.0;
    for (const auto& bucket : daily) {
        maximum = std::max(maximum, bucket.second);
        cumulative += bucket.second;
    }

    std::cout << "  " << std::left << std::setw(14) << "DAY" << std::right << std::setw(12)
              << "ENERGY(Wh)" << "   GRAPH\n";
    for (const auto& bucket : daily) {
        std::cout << "  " << std::left << std::setw(14) << dayLabel(bucket.first) << std::right
                  << std::setw(12) << num(bucket.second) << "   " << bar(bucket.second, maximum)
                  << '\n';
    }
    std::cout << "\n  cumulative: " << num(cumulative) << " Wh (" << num(cumulative / 1000.0, 4)
              << " kWh)\n";
}

void renderPeak(AppState& state) {
    const Summary summary = state.engine.summarize();
    printHeader("=== PEAK USAGE ===");
    if (summary.sample_count == 0) {
        std::cout << "  no data collected yet\n";
        return;
    }
    printKeyValue("peak sample", num(summary.peak_wh) + " Wh");
    printKeyValue("peak at", fs_util::formatTimestamp(summary.peak_epoch));
    printKeyValue("quietest sample", num(summary.min_wh_steady) + " Wh at " +
                                         fs_util::formatTimestamp(summary.valley_epoch));

    const auto hourly = state.engine.hourlyEnergy();
    if (hourly.empty()) return;

    auto peak_hour = *std::max_element(
        hourly.begin(), hourly.end(),
        [](const std::pair<std::int64_t, double>& a, const std::pair<std::int64_t, double>& b) {
            return a.second < b.second;
        });
    printKeyValue("peak hour", hourLabel(peak_hour.first) + "  (" + num(peak_hour.second) +
                                   " Wh)");

    /* Hour-of-day profile: the usual way to identify a peak usage time. */
    std::vector<double> profile(24, 0.0);
    std::vector<std::size_t> counts(24, 0);
    for (const auto& bucket : hourly) {
        const std::int64_t slot = ((bucket.first % 24) + 24) % 24;
        profile[static_cast<std::size_t>(slot)] += bucket.second;
        counts[static_cast<std::size_t>(slot)] += 1;
    }
    double maximum = 0.0;
    std::size_t busiest = 0;
    for (std::size_t i = 0; i < profile.size(); ++i) {
        maximum = std::max(maximum, profile[i]);
        if (profile[i] > profile[busiest]) busiest = i;
    }

    std::cout << "\n  hour-of-day profile (mean Wh per hour bucket):\n";
    for (std::size_t i = 0; i < profile.size(); ++i) {
        if (counts[i] == 0) continue;
        std::ostringstream label;
        label << std::setfill('0') << std::setw(2) << i << ":00";
        std::cout << "    " << label.str() << "  " << std::setw(9)
                  << num(profile[i] / static_cast<double>(counts[i])) << "  "
                  << bar(profile[i], maximum, 32) << '\n';
    }
    std::cout << "\n  busiest hour of day: " << busiest << ":00\n";
}

void renderAnomalies(AppState& state) {
    const auto anomalies = state.engine.detectAnomalies();
    printHeader("=== ANOMALY REPORT (|z| >= 3.0 over a 20 sample window) ===");
    if (anomalies.empty()) {
        std::cout << "  no anomalies detected - consumption looks stable\n";
        return;
    }
    std::cout << "  " << std::left << std::setw(22) << "TIMESTAMP" << std::right << std::setw(12)
              << "ENERGY" << std::setw(12) << "BASELINE" << std::setw(10) << "Z-SCORE"
              << std::setw(10) << "SEVERITY" << '\n';
    for (const Anomaly& anomaly : anomalies) {
        std::cout << "  " << std::left << std::setw(22)
                  << fs_util::formatTimestamp(anomaly.epoch) << std::right << std::setw(12)
                  << num(anomaly.energy_wh) << std::setw(12) << num(anomaly.baseline_wh)
                  << std::setw(10) << num(anomaly.z_score) << std::setw(10)
                  << anomaly.severity() << '\n';
    }
    std::cout << "\n  " << numInt(static_cast<long long>(anomalies.size()))
              << " anomalous sample(s) detected.\n";
}

void renderLogTail(AppState& state) {
    std::vector<Reading> tail;
    std::string error;
    if (!CsvReader::tail(state.options.log_path, kTailRows, &tail, &error)) {
        std::cout << "\n  " << error << '\n';
        return;
    }
    printHeader("=== CSV LOG: " + state.options.log_path + " (" +
                numInt(static_cast<long long>(fs_util::fileSize(state.options.log_path))) +
                " bytes) ===");
    if (tail.empty()) {
        std::cout << "  log is empty\n";
        return;
    }
    std::cout << "  timestamp,pulse_count,energy_consumed\n";
    for (const Reading& reading : tail) {
        std::cout << "  " << fs_util::formatTimestamp(reading.epoch_seconds) << ','
                  << reading.pulse_count << ',' << num(reading.energy_wh, 4) << '\n';
    }
}

std::string prompt(const std::string& text) {
    std::cout << "  " << text << ": " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::string();
    return line;
}

void doInjectPulses(AppState& state) {
    if (!state.source) {
        std::cout << "  pulse injection needs a device or simulated source\n";
        return;
    }
    const std::string line = prompt("pulses to inject");
    if (line.empty()) return;
    try {
        const unsigned long long count = std::stoull(line);
        std::string error;
        if (state.source->inject(count, &error)) {
            refreshDeviceStats(state);
            std::cout << "  injected " << count << " pulse(s)\n";
        } else {
            std::cout << "  injection failed: " << error << '\n';
        }
    } catch (const std::exception&) {
        std::cout << "  not a number\n";
    }
}

void doSetRate(AppState& state) {
    if (!state.source) {
        std::cout << "  rate control needs a device or simulated source\n";
        return;
    }
    std::cout << "  current rate: " << state.options.pulses_per_hour << " pulses/hour\n";
    const std::string line = prompt("new rate in pulses/hour");
    if (line.empty()) return;
    try {
        const long value = std::stol(line);
        if (value <= 0) {
            std::cout << "  rate must be positive\n";
            return;
        }
        SourceConfig config;
        std::string error;
        if (!state.source->getConfig(&config, &error)) {
            std::cout << "  " << error << '\n';
            return;
        }
        config.pulses_per_hour = static_cast<std::uint32_t>(value);
        config.enabled = 1;
        config.source = SM_SOURCE_SIM;
        if (state.source->setConfig(config, &error)) {
            state.options.pulses_per_hour = static_cast<std::uint32_t>(value);
            std::cout << "  rate updated to " << value << " pulses/hour\n";
        } else {
            std::cout << "  " << error << '\n';
        }
    } catch (const std::exception&) {
        std::cout << "  not a number\n";
    }
}

void doToggleCollector(AppState& state) {
    if (!state.collector || !state.collector->running()) {
        std::cout << "  collector is not running\n";
        return;
    }
    const bool paused = !state.collector->paused();
    state.collector->setPaused(paused);
    state.collection_active = !paused;
    std::cout << "  collector " << (paused ? "paused" : "resumed") << '\n';
}

void doExport(AppState& state) {
    drainReadings(state);
    std::string base = state.options.export_path;
    if (base.empty()) {
        std::string stamp = fs_util::formatTimestamp(realtimeNowSeconds());
        for (char& c : stamp) {
            if (c == ':') c = '-';
        }
        base = "exports/smart_meter_" + stamp;
    }
    std::string error;
    if (exportAll(state.engine, base, &error)) {
        std::cout << "  exported:\n    " << base << "_samples.csv\n    " << base
                  << "_summary.json\n    " << base << ".gp  (gnuplot script)\n";
    } else {
        std::cout << "  export failed: " << error << '\n';
    }
}

void doReset(AppState& state) {
    if (!state.source) {
        std::cout << "  reset needs a device or simulated source\n";
        return;
    }
    std::string error;
    if (state.source->reset(&error)) {
        refreshDeviceStats(state);
        std::cout << "  counters reset\n";
    } else {
        std::cout << "  reset failed: " << error << '\n';
    }
}

void doLoadCsv(AppState& state) {
    std::string path = prompt("CSV file [" + state.options.log_path + "]");
    if (path.empty()) path = state.options.log_path;

    std::vector<Reading> readings;
    std::size_t skipped = 0;
    std::string error;
    if (!CsvReader::load(path, &readings, &skipped, &error)) {
        std::cout << "  " << error << '\n';
        return;
    }
    for (const Reading& reading : readings) state.engine.addReading(reading);
    state.loaded_rows += readings.size();
    std::cout << "  loaded " << readings.size() << " rows from " << path;
    if (skipped > 0) std::cout << " (" << skipped << " malformed lines skipped)";
    std::cout << '\n';
}

void doSendCommand(AppState& state) {
    if (!state.publisher) {
        std::cout << "  message queue unavailable - start with --publish\n";
        return;
    }
    const std::string command = prompt("command (reset | rate=1800 | status | pause | resume)");
    if (command.empty()) return;
    const int rc = sm_ipc_mq_send(command.c_str());
    if (rc == 0) {
        std::cout << "  command sent on " << state.options.ipc_mq << '\n';
    } else if (rc == 1) {
        std::cout << "  queue full, command dropped\n";
    } else {
        std::cout << "  message queue is not open\n";
    }
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool loadExistingLog(AppState& state) {
    if (!fs_util::fileExists(state.options.log_path)) return true;

    std::vector<Reading> history;
    std::size_t skipped = 0;
    std::string error;
    if (!CsvReader::load(state.options.log_path, &history, &skipped, &error)) {
        std::cerr << "warning: " << error << '\n';
        return true;
    }
    if (history.empty()) return true;

    for (const Reading& reading : history) state.engine.addReading(reading);
    state.loaded_rows = history.size();
    state.baseline_pulses = history.back().pulse_count;
    if (!state.options.quiet) {
        std::cout << "restored " << history.size() << " historical readings from "
                  << state.options.log_path << " (baseline " << state.baseline_pulses
                  << " pulses)\n";
    }
    return true;
}

bool startPipeline(AppState& state) {
    if (state.source) {
        state.collector.reset(
            new Collector(*state.source, &state.sample_queue, state.options.log_path));
        std::string error;
        if (!state.collector->start(state.options.interval_ms, &error)) {
            std::cerr << "error: cannot start the collector: " << error << '\n';
            return false;
        }
    }
    state.collection_active = state.collector && state.collector->running();

    state.logger_worker.reset(new LoggerWorker(&state.sample_queue, state.logger.get(),
                                               &state.reading_queue, state.options.pulses_per_wh));
    std::string error;
    if (!state.logger_worker->start(&error)) {
        std::cerr << "error: cannot open the log file: " << error << '\n';
        return false;
    }
    return true;
}

void stopPipeline(AppState& state) {
    /* Stop producing first, then let the logger drain what is already in
     * the queue - the other way round would lose the tail of the run. */
    if (state.collector) state.collector->stop();
    if (state.logger_worker) state.logger_worker->stop();
    state.collection_active = false;
}

int runDemo(AppState& state) {
    std::cout << "=== SMART METER DEMO RUN (" << state.options.run_seconds << " s) ===\n"
              << "source   : " << state.sourceLabel() << '\n'
              << "log file : " << state.options.log_path << '\n'
              << "rate     : " << state.options.pulses_per_hour << " pulses/hour\n"
              << "(1 pulse = " << num(state.options.pulses_per_wh, 2) << " Wh)\n\n";

    const int tick_ms = state.options.interval_ms;
    const int spike_tick = state.options.run_seconds * 1000 / tick_ms * 3 / 4;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(state.options.run_seconds);
    int tick = 0;

    while (std::chrono::steady_clock::now() < deadline && !g_interrupted.load()) {
        pumpIpc(state);
        const std::size_t drained = drainReadings(state);
        pollIpcCommands(state);
        refreshDeviceStats(state);

        if (!state.options.quiet) {
            const Summary summary = state.engine.summarize();
            const std::int64_t elapsed = realtimeNowSeconds() - state.started_epoch;
            std::cout << "t+" << std::setw(3) << elapsed << "s  pulses=" << std::setw(7)
                      << state.device_stats.pulse_count << "  energy=" << std::setw(9)
                      << num(summary.total_energy_wh + state.baselineEnergyWh()) << " Wh"
                      << "  avg=" << num(summary.average_wh) << "  max=" << num(summary.max_wh)
                      << "  q=" << state.sample_queue.size() << "  new=" << drained << '\n';
        }

        /* Deliberate overload late in the run so the anomaly detector has
         * something to report in the sample output. */
        if (tick == spike_tick && state.source) {
            std::string error;
            state.source->inject(600, &error);
            if (!state.options.quiet) {
                std::cout << "  ** injected a 600-pulse spike to exercise anomaly detection **\n";
            }
        }

        ++tick;
        std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
    }

    stopPipeline(state);
    drainReadings(state);

    renderStatistics(state);
    renderHourly(state);
    renderPeak(state);
    renderAnomalies(state);

    if (!state.options.export_path.empty()) {
        std::string error;
        if (exportAll(state.engine, state.options.export_path, &error)) {
            std::cout << "\nexported to " << state.options.export_path << "_samples.csv\n";
        }
    }
    std::cout << "\nlog written to " << state.options.log_path << " ("
              << numInt(static_cast<long long>(fs_util::fileSize(state.options.log_path)))
              << " bytes)\n";
    return 0;
}

int runTimed(AppState& state, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline && !g_interrupted.load()) {
        pumpIpc(state);
        drainReadings(state);
        pollIpcCommands(state);
        refreshDeviceStats(state);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    stopPipeline(state);
    drainReadings(state);

    const Summary summary = state.engine.summarize();
    std::cout << "\ncollected " << state.engine.size() << " readings, "
              << num(summary.total_energy_wh + state.baselineEnergyWh()) << " Wh total\n";
    renderAnomalies(state);
    return 0;
}

int runInteractive(AppState& state) {
    bool running = true;
    while (running && !g_interrupted.load()) {
        pumpIpc(state);
        drainReadings(state);
        pollIpcCommands(state);
        refreshDeviceStats(state);
        renderLiveView(state);

        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        int choice = 0;
        try {
            choice = std::stoi(line);
        } catch (const std::exception&) {
            std::cout << "  please type a number (0 to quit)\n";
            continue;
        }

        switch (choice) {
            case 0: running = false; break;
            case 1: break; /* the loop redraws anyway */
            case 2: renderStatistics(state); break;
            case 3: renderHourly(state); break;
            case 4: renderDaily(state); break;
            case 5: renderPeak(state); break;
            case 6: renderAnomalies(state); break;
            case 7: doInjectPulses(state); break;
            case 8: doSetRate(state); break;
            case 9: doToggleCollector(state); break;
            case 10: doExport(state); break;
            case 11: renderLogTail(state); break;
            case 12: doReset(state); break;
            case 13: doLoadCsv(state); break;
            case 14: doSendCommand(state); break;
            default: std::cout << "  unknown choice " << choice << '\n'; break;
        }

        if (running) {
            std::cout << "\npress Enter to continue..." << std::flush;
            if (!std::getline(std::cin, line)) running = false;
        }
    }

    stopPipeline(state);
    drainReadings(state);
    std::cout << "\ncollector stopped; " << state.logger->rowCount()
              << " rows written this run\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, &options)) return EXIT_FAILURE;
    if (options.help) {
        printUsage(argv[0]);
        return 0;
    }
    if (options.version) {
        std::cout << "smart-meter-app " << kVersionMajor << '.' << kVersionMinor
                  << " (driver ABI " << SM_DRIVER_VERSION << ")\n";
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    AppState state;
    state.options = options;
    state.engine = AnalyticsEngine(options.pulses_per_wh, options.history_limit);
    state.logger.reset(new CsvLogger(options.log_path, true));
    state.started_epoch = realtimeNowSeconds();

    if (options.consume_ipc) {
        state.consumer.reset(new IpcConsumer());
        std::string error;
        if (!state.consumer->start(options.ipc_shm, &error)) {
            std::cerr << "error: cannot attach to shared memory '" << options.ipc_shm
                      << "': " << error << '\n';
            std::cerr << "hint: start a producer first:  " << argv[0] << " --publish\n";
            return EXIT_FAILURE;
        }
        state.ipc_attached = true;
        std::cout << "consuming pulses from shared memory '" << options.ipc_shm << "'\n";
    } else {
        SourceSelection selection = makeSource(options.kind, options.device_path,
                                               options.csv_path, options.pulses_per_hour);
        state.source = std::move(selection.source);
        if (!selection.device_error.empty() && selection.kind != SourceKind::kDevice) {
            std::cerr << "warning: " << selection.device_error
                      << " - using the in-process simulator instead\n";
        }
        if (options.publish) {
            state.publisher.reset(new IpcPublisher());
            std::string error;
            if (!state.publisher->start(options.ipc_shm, options.ipc_mq, &error)) {
                std::cerr << "warning: IPC disabled: " << error << '\n';
                state.publisher.reset();
            } else {
                std::cout << "publishing readings to '" << options.ipc_shm
                          << "', accepting commands on '" << options.ipc_mq << "'\n";
            }
        }
    }

    loadExistingLog(state);

    if (!state.source && !state.ipc_attached) {
        std::cerr << "error: no pulse source available\n";
        return EXIT_FAILURE;
    }

    if (!startPipeline(state)) return EXIT_FAILURE;
    refreshDeviceStats(state);

    int status = 0;
    if (options.demo) {
        status = runDemo(state);
    } else if (options.run_seconds > 0) {
        status = runTimed(state, options.run_seconds);
    } else {
        status = runInteractive(state);
    }

    /* --- shutdown ------------------------------------------------- */
    if (state.publisher) {
        std::cout << "published " << state.publisher->published() << " readings ("
                  << state.publisher->dropped() << " dropped)\n";
        state.publisher->stop();
    }
    if (state.consumer) state.consumer->stop();
    if (state.logger) state.logger->close();

    std::cout << "bye\n";
    return status;
}

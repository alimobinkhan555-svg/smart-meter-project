/*
 * collector.cpp - pthread based acquisition and logging threads.
 */
#include "collector.h"

#include <utility>

namespace smartmeter {

/* ------------------------------------------------------------------ */
/* Collector                                                           */
/* ------------------------------------------------------------------ */

Collector::Collector(PulseSource& source, BoundedQueue<RawSample>* output,
                     std::string log_path)
    : source_(source), output_(output), log_path_(std::move(log_path)) {}

Collector::~Collector() { stop(); }

bool Collector::start(int interval_ms, std::string* error) {
    if (running_.load()) return true;

    stop_.store(false);
    paused_.store(false);

    std::string source_error;
    if (!source_.open(&source_error)) {
        if (error) *error = source_error;
        return false;
    }

    /* Persist the acquisition configuration as a comment in the log so the
     * CSV is self-describing. */
    if (!log_path_.empty()) {
        CsvLogger header(log_path_, true);
        if (header.open(error)) {
            header.writeComment("source=" + std::string(source_.description()) +
                                " interval_ms=" + std::to_string(interval_ms));
        }
    }

    running_.store(true);
    thread_ = std::thread(&Collector::threadMain, this, interval_ms);
    return true;
}

void Collector::stop() {
    stop_.store(true);
    if (output_ != nullptr) output_->wakeAll();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

const std::string& Collector::lastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void Collector::threadMain(int interval_ms) {
    std::string error;
    while (!stop_.load()) {
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        RawSample sample;
        error.clear();
        if (source_.read(&sample, interval_ms, &error)) {
            collected_.fetch_add(1);
            if (!output_->push(sample, &stop_)) break;
        } else if (!error.empty()) {
            fprintf(stderr, "[dbg] read error: %s\n", error.c_str());
        }
        if (!error.empty()) {
            errors_.fetch_add(1);
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = error;
        }
    }
    source_.close();
}

/* ------------------------------------------------------------------ */
/* LoggerWorker                                                        */
/* ------------------------------------------------------------------ */

LoggerWorker::LoggerWorker(BoundedQueue<RawSample>* input, CsvLogger* logger,
                           BoundedQueue<Reading>* output, double pulses_per_wh)
    : input_(input),
      logger_(logger),
      output_(output),
      pulses_per_wh_(pulses_per_wh > 0.0 ? pulses_per_wh : 1.0) {}

LoggerWorker::~LoggerWorker() { stop(); }

bool LoggerWorker::start(std::string* error) {
    if (running_.load()) return true;
    if (logger_ != nullptr && !logger_->isOpen()) {
        if (!logger_->open(error)) return false;
    }

    /* Resume from the last row of an existing log so appending a new run
     * does not produce a bogus "everything at once" first record. */
    previous_pulses_ = 0;
    if (logger_ != nullptr && fs_util::fileExists(logger_->path())) {
        std::vector<Reading> history;
        if (CsvReader::tail(logger_->path(), 1, &history, nullptr) && !history.empty()) {
            previous_pulses_ = history.back().pulse_count;
        }
    }

    stop_.store(false);
    running_.store(true);
    thread_ = std::thread(&LoggerWorker::threadMain, this);
    return true;
}

void LoggerWorker::stop() {
    stop_.store(true);
    if (input_ != nullptr) input_->wakeAll();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    if (logger_ != nullptr) logger_->flush();
}

void LoggerWorker::threadMain() {
    /* Batch the writes: fsync per sample would dominate the runtime at
     * 1 pulse/second sampling rates. */
    constexpr int kBatchMax = 64;
    constexpr int kDrainTimeoutMs = 250;

    Reading batch[kBatchMax];
    int batch_size = 0;
    std::uint64_t previous_pulses = previous_pulses_;
    /* The final flush happens after stop_ is set, so handing the rows to
     * the analytics queue must not be aborted by stop_ - otherwise the tail
     * of every run is silently dropped.  This flag stays false, so a stuck
     * consumer back-pressures the worker instead of losing data. */
    std::atomic<bool> never_abort{false};

    auto flushBatch = [&]() {
        if (batch_size == 0) return;
        std::string error;
        for (int i = 0; i < batch_size; ++i) {
            if (logger_ != nullptr && !logger_->append(batch[i], &error)) {
                errors_.fetch_add(1);
            } else {
                rows_.fetch_add(1);
            }
            if (output_ != nullptr && !output_->push(batch[i], &never_abort)) break;
        }
        batch_size = 0;
    };

    while (!stop_.load()) {
        RawSample sample;
        if (!input_->popFor(&sample, kDrainTimeoutMs, &stop_)) {
            /* Nothing pending: get what is left on disk out to the log. */
            flushBatch();
            continue;
        }

        Reading reading;
        reading.epoch_seconds = realtimeNowSeconds();
        reading.pulse_count = sample.pulse_count;

        /* The counter going backwards means the driver was reloaded, the
         * device was unplugged, or SM_IOC_RESET was issued.  Rebase to zero
         * instead of wrapping the delta, otherwise the delta would stay 0
         * until the counter climbed past its old value. */
        if (sample.pulse_count < previous_pulses) previous_pulses = 0;

        reading.pulse_delta = sample.pulse_count - previous_pulses;
        /* 1 pulse == 1 Wh is the default calibration of the meter. */
        reading.energy_wh = static_cast<double>(reading.pulse_delta) / pulses_per_wh_;
        previous_pulses = sample.pulse_count;
        total_pulses_.store(sample.pulse_count);

        batch[batch_size++] = reading;
        if (batch_size >= kBatchMax) flushBatch();
    }
    flushBatch();
    previous_pulses_ = previous_pulses;
}

}  // namespace smartmeter

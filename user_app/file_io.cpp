/*
 * file_io.cpp - CSV logging and log parsing.
 */
#include "file_io.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <sys/stat.h>
#include <sys/types.h>

namespace smartmeter {
namespace {

/* Parses the "2026-10-01T09:15:00Z" form written by formatTimestamp(). */
bool parseIso8601(const std::string& text, std::int64_t* epoch_seconds) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    char zone = 'Z';
    const int matched = std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &year, &month,
                                    &day, &hour, &minute, &second, &zone);
    if (matched < 6) return false;
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;

    std::tm tm_value{};
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month - 1;
    tm_value.tm_mday = day;
    tm_value.tm_hour = hour;
    tm_value.tm_min = minute;
    tm_value.tm_sec = second;
    const time_t seconds = timegm(&tm_value);
    if (seconds == static_cast<time_t>(-1)) return false;
    *epoch_seconds = static_cast<std::int64_t>(seconds);
    return true;
}

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream stream(line);
    while (std::getline(stream, field, ',')) fields.push_back(trim(field));
    return fields;
}

}  // namespace

/* ------------------------------------------------------------------ */
/* fs_util                                                             */
/* ------------------------------------------------------------------ */

namespace fs_util {

bool ensureDirectory(const std::string& path, std::string* error) {
    if (path.empty() || path == ".") return true;

    /* Create every missing component, including intermediate ones. */
    std::string current;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!current.empty() && current != "." && current != "/") {
                if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    if (error) {
                        *error = "mkdir(" + current + ") failed: " + std::strerror(errno);
                    }
                    return false;
                }
            }
        }
        if (i < path.size()) current.push_back(path[i]);
    }
    return true;
}

bool fileExists(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

std::string formatTimestamp(std::int64_t epoch_seconds) {
    const time_t raw = static_cast<time_t>(epoch_seconds);
    std::tm utc {};
    if (gmtime_r(&raw, &utc) == nullptr) return std::string();
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return std::string();
    }
    return std::string(buffer);
}

bool parseTimestamp(const std::string& text, std::int64_t* epoch_seconds) {
    if (text.empty() || epoch_seconds == nullptr) return false;

    /* Plain integer: UNIX epoch seconds. */
    char* end = nullptr;
    const long long numeric = std::strtoll(text.c_str(), &end, 10);
    if (end != nullptr && end != text.c_str() && *end == '\0') {
        *epoch_seconds = static_cast<std::int64_t>(numeric);
        return true;
    }
    return parseIso8601(text, epoch_seconds);
}

std::string ensureCsvExtension(const std::string& path) {
    if (path.size() >= 4) {
        std::string tail = path.substr(path.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == ".csv") return path;
    }
    return path + ".csv";
}

std::uintmax_t fileSize(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return 0;
    return static_cast<std::uintmax_t>(info.st_size);
}

}  // namespace fs_util

/* ------------------------------------------------------------------ */
/* CsvLogger                                                           */
/* ------------------------------------------------------------------ */

CsvLogger::CsvLogger(std::string path, bool append)
    : path_(std::move(path)), append_(append) {}

CsvLogger::~CsvLogger() { close(); }

std::string CsvLogger::headerLine() {
    return "timestamp,pulse_count,energy_consumed";
}

std::string CsvLogger::formatRecord(const Reading& reading) {
    std::ostringstream out;
    out << fs_util::formatTimestamp(reading.epoch_seconds) << ','
        << reading.pulse_count << ',' << std::fixed << std::setprecision(4)
        << reading.energy_wh;
    return out.str();
}

bool CsvLogger::open(std::string* error) {
    close();

    const std::size_t slash = path_.find_last_of('/');
    if (slash != std::string::npos) {
        std::string directory = path_.substr(0, slash);
        if (!fs_util::ensureDirectory(directory, error)) return false;
    }

    const bool needs_header = append_ ? !fs_util::fileExists(path_)
                                      : true;
    file_.open(path_, std::ios::out | (append_ ? std::ios::app : std::ios::trunc));
    if (!file_.is_open()) {
        if (error) *error = "cannot open " + path_ + ": " + std::strerror(errno);
        return false;
    }
    if (needs_header) {
        file_ << headerLine() << '\n';
        file_.flush();
    }
    rows_written_ = 0;
    return true;
}

void CsvLogger::close() {
    if (!file_.is_open()) return;
    file_.flush();
    file_.close();
}

bool CsvLogger::writeComment(const std::string& text) {
    if (!file_.is_open()) return false;
    file_ << "# " << text << '\n';
    file_.flush();
    return static_cast<bool>(file_);
}

bool CsvLogger::append(const Reading& reading, std::string* error) {
    if (!file_.is_open()) {
        if (error) *error = "CSV logger is not open";
        return false;
    }
    const std::string timestamp = fs_util::formatTimestamp(reading.epoch_seconds);
    if (timestamp.empty()) {
        if (error) *error = "invalid timestamp";
        return false;
    }
    file_ << timestamp << ',' << reading.pulse_count << ',' << std::fixed
          << std::setprecision(4) << reading.energy_wh << '\n';
    if (!file_) {
        if (error) *error = "write failed on " + path_;
        return false;
    }
    ++rows_written_;
    return true;
}

bool CsvLogger::flush() {
    if (!file_.is_open()) return false;
    file_.flush();
    return static_cast<bool>(file_);
}

/* ------------------------------------------------------------------ */
/* CsvReader                                                           */
/* ------------------------------------------------------------------ */

bool CsvReader::load(const std::string& path, std::vector<Reading>* out,
                     std::size_t* skipped, std::string* error) {
    if (out == nullptr) return false;
    std::ifstream file(path);
    if (!file.is_open()) {
        if (error) *error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }

    std::string line;
    std::size_t skipped_lines = 0;
    while (std::getline(file, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        const std::vector<std::string> fields = splitCsv(trimmed);
        if (fields.size() < 2) {
            ++skipped_lines;
            continue;
        }
        if (fields[0] == "timestamp") continue; /* header */

        Reading reading;
        if (!fs_util::parseTimestamp(fields[0], &reading.epoch_seconds)) {
            ++skipped_lines;
            continue;
        }
        try {
            reading.pulse_count = std::stoull(fields[1]);
            if (fields.size() >= 3) {
                reading.energy_wh = std::stod(fields[2]);
            } else {
                reading.energy_wh = static_cast<double>(reading.pulse_count);
            }
        } catch (const std::exception&) {
            ++skipped_lines;
            continue;
        }
        out->push_back(reading);
    }

    if (skipped != nullptr) *skipped = skipped_lines;
    return true;
}

bool CsvReader::tail(const std::string& path, std::size_t count,
                     std::vector<Reading>* out, std::string* error) {
    if (out == nullptr) return false;
    std::vector<Reading> all;
    if (!load(path, &all, nullptr, error)) return false;
    if (all.size() > count) {
        out->assign(all.end() - static_cast<std::ptrdiff_t>(count), all.end());
    } else {
        *out = std::move(all);
    }
    return true;
}

}  // namespace smartmeter

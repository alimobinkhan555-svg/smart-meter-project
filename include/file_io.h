/*
 * file_io.h - CSV persistence for the smart meter application.
 *
 * Log format (RFC 4180 friendly, one record per line):
 *
 *   timestamp,pulse_count,energy_consumed
 *   2026-10-01T09:15:00Z,1200,1200.00
 *
 * timestamps are ISO-8601 UTC so the file can be handed straight to
 * gnuplot / pandas / Excel.  Comments start with '#' and are ignored by the
 * reader, which lets the application annotate the file with configuration
 * metadata without breaking the parser.
 */
#ifndef SMART_METER_FILE_IO_H
#define SMART_METER_FILE_IO_H

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "analytics.h"

namespace smartmeter {

class CsvLogger {
public:
    explicit CsvLogger(std::string path, bool append = true);
    ~CsvLogger();

    CsvLogger(const CsvLogger&) = delete;
    CsvLogger& operator=(const CsvLogger&) = delete;

    /* Creates the parent directory and opens the file, writing the header
     * when the file is new. */
    bool open(std::string* error = nullptr);
    void close();

    /* Insert a '#' comment line (metadata) - ignored by CsvReader. */
    bool writeComment(const std::string& text);

    bool append(const Reading& reading, std::string* error = nullptr);
    bool flush();

    bool isOpen() const { return file_.is_open(); }
    const std::string& path() const { return path_; }
    std::size_t rowCount() const { return rows_written_; }

    static std::string headerLine();
    static std::string formatRecord(const Reading& reading);

private:
    std::string path_;
    bool append_;
    std::ofstream file_;
    std::size_t rows_written_{0};
};

class CsvReader {
public:
    /* Parses the file, skipping comments and malformed lines.
     * Returns false only when the file cannot be opened. */
    static bool load(const std::string& path, std::vector<Reading>* out,
                     std::size_t* skipped = nullptr, std::string* error = nullptr);

    /* Last 'count' records, oldest first. */
    static bool tail(const std::string& path, std::size_t count,
                     std::vector<Reading>* out, std::string* error = nullptr);
};

namespace fs_util {

bool ensureDirectory(const std::string& path, std::string* error = nullptr);
bool fileExists(const std::string& path);

/* "2026-10-01T09:15:00Z" for a UNIX epoch value; empty string on failure. */
std::string formatTimestamp(std::int64_t epoch_seconds);
/* Inverse of formatTimestamp(); returns false on malformed input. */
bool parseTimestamp(const std::string& text, std::int64_t* epoch_seconds);

/* Appends ".csv" when the path has no extension yet. */
std::string ensureCsvExtension(const std::string& path);

/* Number of bytes the file occupies, 0 when missing. */
std::uintmax_t fileSize(const std::string& path);

}  // namespace fs_util

}  // namespace smartmeter

#endif /* SMART_METER_FILE_IO_H */

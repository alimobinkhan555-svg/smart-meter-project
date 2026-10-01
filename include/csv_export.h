/*
 * csv_export.h - export the collected history for external visualisation.
 *
 * Two formats are produced from the same in-memory history:
 *   CSV  -> gnuplot, pandas, Excel, Grafana
 *   JSON -> dashboards, web front ends, MQTT payloads
 *
 * A gnuplot script is emitted alongside the CSV so the exported data can
 * be plotted with a single command.
 */
#ifndef SMART_METER_CSV_EXPORT_H
#define SMART_METER_CSV_EXPORT_H

#include <string>
#include <vector>

#include "analytics.h"

namespace smartmeter {

/* One row of an export: the reading plus its derived rate. */
struct ExportRow {
    std::int64_t epoch_seconds{0};
    std::string timestamp;
    std::uint64_t pulse_count{0};
    std::uint64_t pulse_delta{0};
    double energy_wh{0.0};
    double energy_kwh{0.0};
    double pulses_per_hour{0.0};
};

/* Derives per-row rates from the reading history. */
std::vector<ExportRow> buildExportRows(const AnalyticsEngine& engine);

/* timestamp,epoch,pulse_count,pulse_delta,energy_wh,energy_kwh,pulses_per_hour */
bool writeCsv(const std::string& path, const std::vector<ExportRow>& rows,
              std::string* error = nullptr);

/* {
 *   "generated_at": "...",
 *   "pulses_per_wh": 1.0,
 *   "totals": { ... },
 *   "hourly": [ { "bucket_start": "...", "epoch": 0, "energy_wh": 0.0 } ],
 *   "daily":  [ ... ],
 *   "anomalies": [ ... ],
 *   "samples": [ ... ]
 * } */
bool writeJson(const std::string& path, const AnalyticsEngine& engine,
               std::string* error = nullptr);

/* gnuplot script that plots pulses and energy against the exported CSV. */
bool writeGnuplotScript(const std::string& path, const std::string& csv_path,
                        std::string* error = nullptr);

/* Convenience: CSV + JSON + gnuplot script in one call. */
bool exportAll(const AnalyticsEngine& engine, const std::string& base_path,
               std::string* error = nullptr);

}  // namespace smartmeter

#endif /* SMART_METER_CSV_EXPORT_H */

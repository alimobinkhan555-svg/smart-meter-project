/*
 * csv_export.cpp - export helpers.
 */
#include "csv_export.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "file_io.h"
#include "pulse_source.h"

namespace smartmeter {
namespace {

std::string jsonNumber(double value, int precision = 4) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string jsonString(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace

std::vector<ExportRow> buildExportRows(const AnalyticsEngine& engine) {
    std::vector<ExportRow> rows;
    const std::vector<Reading>& readings = engine.readings();
    rows.reserve(readings.size());

    for (std::size_t i = 0; i < readings.size(); ++i) {
        const Reading& reading = readings[i];
        ExportRow row;
        row.epoch_seconds = reading.epoch_seconds;
        row.timestamp = fs_util::formatTimestamp(reading.epoch_seconds);
        row.pulse_count = reading.pulse_count;
        row.pulse_delta = reading.pulse_delta;
        row.energy_wh = reading.energy_wh;
        row.energy_kwh = reading.energy_wh / 1000.0;

        /* Instantaneous rate in pulses/hour: energy of this sample scaled
         * by the interval that preceded it. */
        std::int64_t interval = 0;
        if (i > 0) {
            interval = reading.epoch_seconds - readings[i - 1].epoch_seconds;
        } else if (readings.size() > 1) {
            interval = readings[1].epoch_seconds - readings[0].epoch_seconds;
        }
        if (interval > 0) {
            row.pulses_per_hour = (reading.energy_wh * engine.pulsesPerWh() * 3600.0) /
                                  static_cast<double>(interval);
        }
        rows.push_back(row);
    }
    return rows;
}

bool writeCsv(const std::string& path, const std::vector<ExportRow>& rows,
              std::string* error) {
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos &&
        !fs_util::ensureDirectory(path.substr(0, slash), error)) {
        return false;
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        if (error) *error = "cannot open " + path + " for writing";
        return false;
    }

    out << "timestamp,epoch,pulse_count,pulse_delta,energy_wh,energy_kwh,pulses_per_hour\n";
    out << std::fixed;
    for (const ExportRow& row : rows) {
        out << row.timestamp << ',' << row.epoch_seconds << ',' << row.pulse_count << ','
            << row.pulse_delta << ',' << std::setprecision(4) << row.energy_wh << ','
            << std::setprecision(6) << row.energy_kwh << ',' << std::setprecision(2)
            << row.pulses_per_hour << '\n';
    }
    out.flush();
    if (!out) {
        if (error) *error = "write failed on " + path;
        return false;
    }
    return true;
}

bool writeJson(const std::string& path, const AnalyticsEngine& engine, std::string* error) {
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos &&
        !fs_util::ensureDirectory(path.substr(0, slash), error)) {
        return false;
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        if (error) *error = "cannot open " + path + " for writing";
        return false;
    }

    const Summary summary = engine.summarize();
    const std::vector<ExportRow> rows = buildExportRows(engine);
    const std::vector<Anomaly> anomalies = engine.detectAnomalies();

    out << "{\n";
    out << "  \"generated_at\": "
        << jsonString(fs_util::formatTimestamp(realtimeNowSeconds())) << ",\n";
    out << "  \"pulses_per_wh\": " << jsonNumber(engine.pulsesPerWh()) << ",\n";

    out << "  \"totals\": {\n";
    out << "    \"sample_count\": " << summary.sample_count << ",\n";
    out << "    \"total_pulses\": " << (rows.empty() ? 0ULL : rows.back().pulse_count) << ",\n";
    out << "    \"total_energy_wh\": " << jsonNumber(summary.total_energy_wh) << ",\n";
    out << "    \"total_energy_kwh\": " << jsonNumber(summary.energy_kwh) << ",\n";
    out << "    \"average_wh\": " << jsonNumber(summary.average_wh) << ",\n";
    out << "    \"max_wh\": " << jsonNumber(summary.max_wh) << ",\n";
    out << "    \"min_wh\": " << jsonNumber(summary.min_wh_steady) << ",\n";
    out << "    \"peak_epoch\": " << summary.peak_epoch << ",\n";
    out << "    \"peak_timestamp\": "
        << jsonString(fs_util::formatTimestamp(summary.peak_epoch)) << ",\n";
    out << "    \"avg_pulses_per_hour\": " << jsonNumber(summary.avg_pulses_per_hour, 2)
        << ",\n";
    out << "    \"first_timestamp\": "
        << jsonString(fs_util::formatTimestamp(summary.first_epoch)) << ",\n";
    out << "    \"last_timestamp\": "
        << jsonString(fs_util::formatTimestamp(summary.last_epoch)) << "\n";
    out << "  },\n";

    out << "  \"hourly\": [\n";
    const auto hourly = engine.hourlyEnergy();
    for (std::size_t i = 0; i < hourly.size(); ++i) {
        out << "    {\"bucket_start\": "
            << jsonString(fs_util::formatTimestamp(hourly[i].first)) << ", \"epoch\": "
            << hourly[i].first << ", \"energy_wh\": " << jsonNumber(hourly[i].second) << "}";
        out << (i + 1 < hourly.size() ? ",\n" : "\n");
    }
    out << "  ],\n";

    out << "  \"daily\": [\n";
    const auto daily = engine.dailyEnergy();
    for (std::size_t i = 0; i < daily.size(); ++i) {
        out << "    {\"bucket_start\": "
            << jsonString(fs_util::formatTimestamp(daily[i].first)) << ", \"epoch\": "
            << daily[i].first << ", \"energy_wh\": " << jsonNumber(daily[i].second) << "}";
        out << (i + 1 < daily.size() ? ",\n" : "\n");
    }
    out << "  ],\n";

    out << "  \"anomalies\": [\n";
    for (std::size_t i = 0; i < anomalies.size(); ++i) {
        out << "    {\"timestamp\": "
            << jsonString(fs_util::formatTimestamp(anomalies[i].epoch)) << ", \"energy_wh\": "
            << jsonNumber(anomalies[i].energy_wh) << ", \"baseline_wh\": "
            << jsonNumber(anomalies[i].baseline_wh) << ", \"z_score\": "
            << jsonNumber(anomalies[i].z_score, 2) << ", \"severity\": "
            << jsonString(anomalies[i].severity()) << "}";
        out << (i + 1 < anomalies.size() ? ",\n" : "\n");
    }
    out << "  ],\n";

    out << "  \"samples\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const ExportRow& row = rows[i];
        out << "    {\"timestamp\": " << jsonString(row.timestamp) << ", \"pulse_count\": "
            << row.pulse_count << ", \"pulse_delta\": " << row.pulse_delta
            << ", \"energy_wh\": " << jsonNumber(row.energy_wh)
            << ", \"pulses_per_hour\": " << jsonNumber(row.pulses_per_hour, 2) << "}";
        out << (i + 1 < rows.size() ? ",\n" : "\n");
    }
    out << "  ]\n";
    out << "}\n";

    out.flush();
    if (!out) {
        if (error) *error = "write failed on " + path;
        return false;
    }
    return true;
}

bool writeGnuplotScript(const std::string& path, const std::string& csv_path,
                        std::string* error) {
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos &&
        !fs_util::ensureDirectory(path.substr(0, slash), error)) {
        return false;
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        if (error) *error = "cannot open " + path + " for writing";
        return false;
    }

    out << "# Smart meter export - render with:  gnuplot " << path << "\n";
    out << "set terminal pngcairo size 1100,420 font 'Sans,10'\n";
    out << "set output '" << csv_path << ".png'\n";
    out << "set datafile separator ','\n";
    out << "set xdata time\n";
    out << "set timefmt '%Y-%m-%dT%H:%M:%SZ'\n";
    out << "set format x '%H:%M'\n";
    out << "set xlabel 'time'\n";
    out << "set ylabel 'energy (Wh)'\n";
    out << "set grid\n";
    out << "set multiplot layout 2,1\n";
    out << "set tics nomirror\n";
    out << "set key left top\n";
    out << "plot '" << csv_path
        << "' every ::1 using 2:5 with lines lw2 title 'energy per sample (Wh)' axes x1y1\n";
    out << "unset key\n";
    out << "set ylabel 'cumulative pulses'\n";
    out << "plot '" << csv_path
        << "' every ::1 using 2:3 with lines lw2 dt 2 title 'pulse count' axes x1y1\n";
    out << "unset multiplot\n";
    out.flush();
    if (!out) {
        if (error) *error = "write failed on " + path;
        return false;
    }
    return true;
}

bool exportAll(const AnalyticsEngine& engine, const std::string& base_path,
               std::string* error) {
    std::string directory = ".";
    std::string stem = base_path;
    const std::size_t slash = base_path.find_last_of('/');
    if (slash != std::string::npos) {
        directory = base_path.substr(0, slash);
        stem = base_path.substr(slash + 1);
    }
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);

    const std::string csv_path = directory + "/" + stem + "_samples.csv";
    const std::string json_path = directory + "/" + stem + "_summary.json";
    const std::string script_path = directory + "/" + stem + ".gp";

    std::string local_error;
    if (!writeCsv(csv_path, buildExportRows(engine), &local_error)) {
        if (error) *error = local_error;
        return false;
    }
    if (!writeJson(json_path, engine, &local_error)) {
        if (error) *error = local_error;
        return false;
    }
    if (!writeGnuplotScript(script_path, csv_path, &local_error)) {
        if (error) *error = local_error;
        return false;
    }
    return true;
}

}  // namespace smartmeter

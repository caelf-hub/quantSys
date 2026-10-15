#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "quant_build_info.hpp"

namespace research {

// 64-bit FNV-1a. Identifies an input file across runs; not a security hash.
constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

inline uint64_t fnv1a64(const uint8_t* data, size_t size, uint64_t hash = kFnvOffset) {
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
    return hash;
}

inline std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", byte);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

struct InputFile {
    bool ok = false;
    std::string path;
    uint64_t bytes = 0;
    uint64_t fnv1a64 = 0;
};

// Streams the file once. An 8 GB tape takes several seconds; that cost is
// paid once per run, outside any replay loop.
inline InputFile describe_input(const std::filesystem::path& path) {
    InputFile file;
    file.path = path.string();
    std::ifstream in(path, std::ios::binary);
    if (!in) return file;
    std::vector<char> buf(1 << 20);
    uint64_t hash = kFnvOffset;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = static_cast<size_t>(in.gcount());
        hash = fnv1a64(reinterpret_cast<const uint8_t*>(buf.data()), got, hash);
        file.bytes += got;
    }
    if (in.bad()) return file;
    file.fnv1a64 = hash;
    file.ok = true;
    return file;
}

inline std::string hex64(uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

struct RunRecord {
    std::string tool;
    std::vector<InputFile> inputs;
    std::vector<std::pair<std::string, std::string>> params;
    // Sample counts and drop counts.
    std::vector<std::pair<std::string, uint64_t>> counts;
    std::vector<std::string> outputs;

    std::string to_json() const {
        const std::time_t now =
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
        gmtime_r(&now, &utc);
        char created[32];
        std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &utc);

        std::string j = "{\n";
        j += "  \"tool\": \"" + json_escape(tool) + "\",\n";
        j += "  \"git_commit\": \"" + json_escape(QUANT_GIT_COMMIT) + "\",\n";
        j += std::string("  \"git_dirty\": ") + (QUANT_GIT_DIRTY ? "true" : "false") + ",\n";
        j += "  \"created_utc\": \"" + std::string(created) + "\",\n";
        j += "  \"inputs\": [";
        for (size_t i = 0; i < inputs.size(); ++i) {
            const InputFile& f = inputs[i];
            j += i ? ",\n    " : "\n    ";
            j += "{\"path\": \"" + json_escape(f.path) + "\", \"ok\": " +
                 (f.ok ? "true" : "false") + ", \"bytes\": " + std::to_string(f.bytes) +
                 ", \"fnv1a64\": \"" + hex64(f.fnv1a64) + "\"}";
        }
        j += inputs.empty() ? "],\n" : "\n  ],\n";
        j += "  \"params\": {";
        for (size_t i = 0; i < params.size(); ++i) {
            j += i ? ",\n    " : "\n    ";
            j += "\"" + json_escape(params[i].first) + "\": \"" + json_escape(params[i].second) +
                 "\"";
        }
        j += params.empty() ? "},\n" : "\n  },\n";
        j += "  \"counts\": {";
        for (size_t i = 0; i < counts.size(); ++i) {
            j += i ? ",\n    " : "\n    ";
            j += "\"" + json_escape(counts[i].first) + "\": " + std::to_string(counts[i].second);
        }
        j += counts.empty() ? "},\n" : "\n  },\n";
        j += "  \"outputs\": [";
        for (size_t i = 0; i < outputs.size(); ++i) {
            j += (i ? ", \"" : "\"") + json_escape(outputs[i]) + "\"";
        }
        j += "]\n}\n";
        return j;
    }
};

// Writes <output>.manifest.json. Returns the manifest path, or an empty path
// if it could not be written.
inline std::filesystem::path write_manifest(const RunRecord& record,
                                            const std::filesystem::path& output) {
    std::filesystem::path manifest = output;
    manifest += ".manifest.json";
    std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
    if (!out) return {};
    out << record.to_json();
    out.close();
    if (!out) return {};
    return manifest;
}

} // namespace research

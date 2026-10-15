#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "research/run_record.hpp"

using namespace research;

namespace {

uint64_t hash_text(const std::string& s) {
    return fnv1a64(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

} // namespace

int main() {
    // Published FNV-1a 64-bit test vectors.
    assert(hash_text("") == 0xcbf29ce484222325ULL);
    assert(hash_text("a") == 0xaf63dc4c8601ec8cULL);
    assert(hash_text("foobar") == 0x85944171f73967e8ULL);
    // Hashing in pieces matches hashing at once.
    const std::string foo = "foo";
    const std::string bar = "bar";
    const uint64_t partial = fnv1a64(reinterpret_cast<const uint8_t*>(foo.data()), foo.size());
    assert(fnv1a64(reinterpret_cast<const uint8_t*>(bar.data()), bar.size(), partial) ==
           0x85944171f73967e8ULL);
    std::cout << "[PASS] FNV-1a 64 matches published vectors, incrementally too\n";

    assert(json_escape("plain") == "plain");
    assert(json_escape("a\"b\\c") == "a\\\"b\\\\c");
    assert(json_escape("line\nnext\ttab\r") == "line\\nnext\\ttab\\r");
    assert(json_escape(std::string("\x01\x1f", 2)) == "\\u0001\\u001f");
    assert(json_escape(std::string("nul\0x", 5)) == "nul\\u0000x");
    assert(json_escape("caf\xc3\xa9") == "caf\xc3\xa9");
    std::cout << "[PASS] JSON escaping covers quotes, backslashes, and control bytes\n";

    const auto dir = std::filesystem::temp_directory_path() / "quant_test_run_record";
    std::filesystem::create_directories(dir);
    const auto input = dir / "input.bin";
    {
        std::ofstream out(input, std::ios::binary);
        out << "foobar";
    }
    const InputFile file = describe_input(input);
    assert(file.ok);
    assert(file.bytes == 6);
    assert(file.fnv1a64 == 0x85944171f73967e8ULL);
    assert(!describe_input(dir / "missing.bin").ok);
    std::cout << "[PASS] describe_input sizes and hashes a file, and flags a missing one\n";

    RunRecord record;
    record.tool = "test_tool";
    record.inputs.push_back(file);
    record.params.emplace_back("ticker", "A\"B");
    record.counts.emplace_back("pool_exhausted", 7);
    record.outputs.push_back((dir / "out.csv").string());
    const auto csv = dir / "out.csv";
    const auto manifest = write_manifest(record, csv);
    assert(manifest == dir / "out.csv.manifest.json");
    const std::string json = slurp(manifest);
    assert(json.find("\"tool\": \"test_tool\"") != std::string::npos);
    assert(json.find("\"git_commit\": \"") != std::string::npos);
    assert(json.find("\"git_dirty\": ") != std::string::npos);
    assert(json.find("\"bytes\": 6") != std::string::npos);
    assert(json.find("\"fnv1a64\": \"85944171f73967e8\"") != std::string::npos);
    assert(json.find("\"ticker\": \"A\\\"B\"") != std::string::npos);
    assert(json.find("\"pool_exhausted\": 7") != std::string::npos);
    std::cout << "[PASS] manifest is written next to the CSV with escaped fields\n";

    std::filesystem::remove_all(dir);
    std::cout << "All run record tests passed.\n";
    return 0;
}

#include "stratum.hpp"

#include <iostream>
#include <stdexcept>

namespace {

bool check(bool value, const char* message) {
    if (!value) std::cerr << message << '\n';
    return value;
}

} // namespace

int main() {
    using namespace unique_rx;
    bool ok = true;
    ok &= check(parse_share_target("ffffffff") == UINT64_MAX,
                "4-byte little-endian target normalization failed");
    ok &= check(parse_share_target("0100000000000000") == 1,
                "8-byte little-endian target parsing failed");

    const nlohmann::json wire_job{
        {"job_id", "job-1"},
        {"blob", std::string(76 * 2, '0')},
        {"seed_hash", std::string(64, '0')},
        {"target", "ffffffff"},
        {"algo", "rx/0"},
    };
    const auto job = parse_job(wire_job);
    ok &= check(job.id == "job-1" && job.blob.size() == 76 && job.target == UINT64_MAX,
                "valid rx/0 job did not parse");
    try {
        auto invalid = wire_job;
        invalid["blob"] = "00";
        parse_job(invalid);
        ok &= check(false, "short job blob was accepted");
    } catch (const std::exception&) {
    }
    try {
        parse_share_target("00000000");
        ok &= check(false, "zero pool target was accepted");
    } catch (const std::exception&) {
    }

    if (!ok) return 1;
    std::cout << "Stratum job and target validation passed.\n";
    return 0;
}
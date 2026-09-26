#include "../../extern/dxvk/src/dxvk/dxvk_external_access.h"
#include <cstdio>
#include <cstdlib>
#include <array>
#include <random>

using namespace dxvk;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "Check failed at %d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); } } while (false)

DxvkExternalAccess Use(uint64_t begin, uint64_t end, uint64_t stages, bool write, uint64_t lane = 0) {
    return {1, lane, begin, end, stages, write ? 2ull : 1ull, write};
}

int main() {
    DxvkExternalAccessLedger ledger;
    ledger.registerResource(1);
    auto seed = ledger.prepare({Use(0, 128, 1, true)});
    CHECK(seed.dependencies.empty());
    ledger.commit(std::move(seed));
    auto abandoned = ledger.prepare({Use(0, 64, 2, false)});
    CHECK(abandoned.dependencies.size() == 1);
    auto read = ledger.prepare({Use(0, 64, 2, false)});
    CHECK(read.dependencies.size() == 1);
    CHECK(read.dependencies[0].begin == 0 && read.dependencies[0].end == 64);
    CHECK(read.dependencies[0].srcStages == 1 && read.dependencies[0].dstStages == 2);
    ledger.commit(std::move(read));
    CHECK(ledger.prepare({Use(0, 64, 2, false)}).dependencies.empty());
    CHECK(ledger.prepare({Use(0, 64, 4, false)}).dependencies.size() == 1);
    CHECK(ledger.prepare({Use(64, 128, 2, false)}).dependencies.size() == 1);
    CHECK(ledger.prepare({Use(128, 256, 2, false)}).dependencies.empty());
    CHECK(ledger.prepare({Use(0, 64, 2, false, 1)}).dependencies.empty());
    bool stale = false;
    try { ledger.commit(std::move(abandoned)); } catch (const std::logic_error&) { stale = true; }
    CHECK(stale);
    auto partial = ledger.prepare({Use(16, 32, 8, true)});
    CHECK(partial.dependencies.size() == 1);
    CHECK(partial.dependencies[0].srcStages == (1 | 2));
    ledger.commit(std::move(partial));
    CHECK(ledger.prepare({Use(0, 16, 2, false)}).dependencies.empty());
    auto overwritten = ledger.prepare({Use(16, 32, 2, false)});
    CHECK(overwritten.dependencies.size() == 1 && overwritten.dependencies[0].srcStages == 8);
    CHECK(ledger.prepare({Use(32, 64, 2, false)}).dependencies.empty());
    ledger.retireResource(1);
    ledger.registerResource(1);
    auto reader = ledger.prepare({Use(0, 128, 2, false), Use(0, 128, 4, false)});
    CHECK(reader.dependencies.empty());
    ledger.commit(std::move(reader));
    auto war = ledger.prepare({Use(0, 128, 8, false), Use(0, 128, 16, true)});
    CHECK(war.dependencies.size() == 1);
    CHECK(war.dependencies[0].srcStages == (2 | 4));
    CHECK(war.dependencies[0].srcAccess == 0 && war.dependencies[0].dstAccess == 0);
    CHECK(war.dependencies[0].dstStages == 16);
    ledger.commit(std::move(war));
    auto waw = ledger.prepare({Use(0, 128, 32, true)});
    CHECK(waw.dependencies.size() == 1);
    CHECK(waw.dependencies[0].srcStages == (8 | 16));
    CHECK(waw.dependencies[0].srcAccess == 2 && waw.dependencies[0].dstAccess == 2);
    ledger.retireResource(1);
    bool unknown = false;
    try { ledger.prepare({Use(0, 128, 2, false)}); } catch (const std::invalid_argument&) { unknown = true; }
    CHECK(unknown);
    // Distinct destination stage/access pairs must survive lowering. Adjacent
    // equal scopes merge; different scopes must never become a Cartesian union.
    ledger.registerResource(1);
    auto producer = ledger.prepare({Use(0, 128, 1, true)});
    ledger.commit(std::move(producer));
    auto scopes = ledger.prepare({
        {1, 0, 0, 64, 2, 4, false},
        {1, 0, 64, 128, 2, 4, false},
        {1, 0, 0, 128, 8, 16, false},
        {1, 0, 0, 64, 2, 4, false}});
    CHECK(scopes.dependencies.size() == 2);
    CHECK(scopes.dependencies[0].begin == 0 && scopes.dependencies[0].end == 128);
    CHECK(scopes.dependencies[0].dstStages == 2 && scopes.dependencies[0].dstAccess == 4);
    CHECK(scopes.dependencies[1].begin == 0 && scopes.dependencies[1].end == 128);
    CHECK(scopes.dependencies[1].dstStages == 8 && scopes.dependencies[1].dstAccess == 16);
    ledger.retireResource(1);
    // Registering another view of an existing backing must preserve pending
    // writer scopes even when its conservative bootstrap overlaps that backing.
    ledger.registerResource(1);
    ledger.commit(ledger.prepare({Use(0, 64, 2, true)}));
    ledger.commit(ledger.prepare({Use(32, 96, 4, true)}, true));
    auto bootstrapped = ledger.prepare({Use(0, 96, 8, false)});
    std::sort(bootstrapped.dependencies.begin(), bootstrapped.dependencies.end(),
        [](const auto& a, const auto& b) { return a.begin < b.begin; });
    CHECK(bootstrapped.dependencies.size() == 3);
    CHECK(bootstrapped.dependencies[0].begin == 0 && bootstrapped.dependencies[0].end == 32);
    CHECK(bootstrapped.dependencies[0].srcStages == 2);
    CHECK(bootstrapped.dependencies[1].begin == 32 && bootstrapped.dependencies[1].end == 64);
    CHECK(bootstrapped.dependencies[1].srcStages == (2 | 4));
    CHECK(bootstrapped.dependencies[2].begin == 64 && bootstrapped.dependencies[2].end == 96);
    CHECK(bootstrapped.dependencies[2].srcStages == 4);
    ledger.retireResource(1);
    // Independent byte-by-byte oracle. Exercise splits, gaps, reader-stage
    // visibility, and abandoned transactions without sharing interval code.
    ledger.registerResource(1);
    struct Byte { uint64_t writer = 0, readers = 0, visible = 0; };
    std::array<std::array<Byte, 32>, 2> oracle{};
    std::mt19937 random(731);
    for (unsigned iteration = 0; iteration < 2000; ++iteration) {
        const uint64_t lane = random() % 2;
        const uint64_t begin = random() % 32;
        const uint64_t end = begin + 1 + random() % (32 - begin);
        const uint64_t stages = uint64_t{1} << (random() % 5);
        const bool write = random() % 3 == 0;
        auto transaction = ledger.prepare({Use(begin, end, stages, write, lane)});
        for (uint64_t byte = 0; byte < 32; ++byte) {
            const auto& old = oracle[lane][byte];
            const bool touched = begin <= byte && byte < end;
            const uint64_t writer = (write || (old.visible & stages) != stages) ? old.writer : 0;
            const uint64_t expected = touched ? writer | (write ? old.readers : 0) : 0;
            uint64_t observed = 0;
            for (const auto& dependency : transaction.dependencies) {
                CHECK(dependency.lane == lane && dependency.begin >= begin && dependency.end <= end);
                if (dependency.begin <= byte && byte < dependency.end) {
                    observed |= dependency.srcStages;
                    CHECK(dependency.dstStages == stages);
                    CHECK(dependency.srcAccess == (writer ? 2 : 0));
                    CHECK(dependency.dstAccess == (writer ? (write ? 2 : 1) : 0));
                }
            }
            CHECK(observed == expected);
        }
        if (random() % 5 == 0) continue;
        ledger.commit(std::move(transaction));
        for (uint64_t byte = begin; byte < end; ++byte) {
            auto& state = oracle[lane][byte];
            if (write) state = {stages, 0, 0};
            else { state.readers |= stages; state.visible |= stages; }
        }
    }
    std::puts("External access ledger tests passed");
}

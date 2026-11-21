#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/OwnerValues.h"
#include <array>
#include <cassert>
#include <random>
#include <set>

namespace
{
    void TestActorValueMembership()
    {
        DCLF::Integrations::AdvancedSkin::OwnerValues index;
        using Value = DCLF::Integrations::AdvancedSkin::OwnerValues::Value;
        std::array<Value, 6> rows{};
        Value current{1, 2, 3, 4};
        unsigned sampled = 0;
        auto sample = [&](auto) { ++sampled; return current; };
        auto apply = [&](auto slot, const auto& value) { rows[slot] = value; };
        index.Set(0, 100, 1);
        index.Set(1, 100, 2);
        auto stats = index.Update(sample, apply);
        assert(sampled == 1 && stats.actors == 1 && stats.visited == 2 && rows[0] == rows[1]);
        stats = index.Update(sample, apply);
        assert(stats.visited == 0 && stats.changed == 0); // No per-mesh iteration for unchanged actors.
        index.Set(2, 100, 3); // Equipment join without output change.
        stats = index.Update(sample, apply);
        assert(stats.visited == 1 && rows[2] == current);
        rows[1] = {};
        index.Set(1, 100, 2, true); // Scene writer reset the row, membership stayed constant.
        assert(index.Update(sample, apply).visited == 1 && rows[1] == current);
        index.Set(0, 0, 0); // Representative detached; another member samples the actor.
        index.Set(3, 100, 4);
        current[0] = 0.5f; // Fade affects all surviving parts in one batch.
        stats = index.Update(sample, apply);
        assert(stats.actors == 1 && stats.members == 3 && stats.visited == 3);
        assert(rows[1] == current && rows[2] == current && rows[3] == current);
        index.Set(2, 200, 5); // Slot reuse / actor replacement must initialize the new incarnation.
        assert(!index.Contains(2, 100, 3) && index.Contains(2, 200, 5));
        stats = index.Update(sample, apply);
        assert(stats.actors == 2 && stats.visited == 1);
        index.Set(1, 0, 0);
        index.Set(3, 0, 0);
        assert(index.Update(sample, apply).actors == 1); // Last member retires the group immediately.
        auto snapshot = index;
        index.Clear();
        assert(index.Update(sample, apply).actors == 0);
        assert(snapshot.Update(sample, apply).actors == 1); // Parity copies have independent ownership.
        index.Set(5, 300, 6);
        current = {}; // Disabled skin / zero delta output is still published once.
        assert(index.Update(sample, apply).visited == 1 && rows[5] == current);
        assert(index.Update(sample, apply).visited == 0);

        // Compare incremental propagation with full derivation from the same
        // captured values under reordered membership/value changes.
        index.Clear();
        std::array<std::uint64_t, 6> groups{}, identities{};
        std::array<Value, 4> inputs{};
        std::mt19937 random(317);
        std::uint64_t incarnation = 1000;
        for (unsigned frame = 0; frame < 1000; ++frame) {
            for (unsigned event = 0; event < 4; ++event) {
                const auto slot = random() % groups.size();
                const auto group = random() % inputs.size();
                groups[slot] = group;
                identities[slot] = ++incarnation;
                index.Set(static_cast<std::uint32_t>(slot), group, identities[slot]);
            }
            if (frame % 3 == 0) inputs[random() % inputs.size()][random() % 4] = float(random() % 16);
            stats = index.Update([&](auto slot) { return inputs[groups[slot]]; }, apply);
            std::set<std::uint64_t> actors;
            unsigned members = 0;
            for (unsigned slot = 0; slot < groups.size(); ++slot) {
                if (!groups[slot]) continue;
                actors.insert(groups[slot]);
                ++members;
                assert(rows[slot] == inputs[groups[slot]]);
                assert(index.Contains(slot, groups[slot], identities[slot]));
            }
            assert(stats.actors == actors.size() && stats.members == members);
            assert(index.Update([&](auto slot) { return inputs[groups[slot]]; }, apply).visited == 0);
        }
    }
}

int main()
{
    TestActorValueMembership();
}

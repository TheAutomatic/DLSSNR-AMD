#pragma once
#include "nr_frame_pair.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace nr::frame_pair {
// Consecutive layers that all request reuse transform the same computed
// anchors. One transport rebuilds their combined edit; there is only one
// network cut and one display delay for this run of layers.
template <class Command, class Frame> struct ChainJobs {
    using Run = std::function<uint32_t(Command&, const Frame&, Command, uint32_t, uint32_t)>;
    using Execute = std::function<uint32_t(Command&, const Frame&)>;
    using Pair = std::function<uint32_t(Command&, Frame*, Frame&, const Execute&)>;
    struct Member {
        uint32_t steps;
        Run run;
        Pair pair;
    };
    std::vector<Member> members;
    const void* device = nullptr;
    uint32_t format = 0;
    bool ready = false;
    uint64_t epoch = 0;
    std::vector<uint32_t> pass_counts;

    static ChainJobs* add(const void* device, uint32_t format, uint32_t steps, uintptr_t owner, Run run,
                          Pair pair = {}) {
        auto* age = PresentAge::active;
        if (!age || !age->gathering())
            return nullptr;
        if (!age->group_index || !age->group_jobs.has_value())
            age->group_jobs.emplace<ChainJobs>();
        auto* jobs = std::any_cast<ChainJobs>(&age->group_jobs);
        if (!jobs)
            return nullptr;
        if (jobs->members.empty()) {
            jobs->device = device;
            jobs->format = format;
            jobs->epoch = age->version;
        }
        if (age->group_index != jobs->members.size() || jobs->device != device || jobs->format != format)
            return jobs;
        jobs->epoch = (jobs->epoch ^ uint64_t(owner)) * 1099511628211ull + steps;
        jobs->members.push_back({steps, std::move(run), std::move(pair)});
        jobs->ready = jobs->members.size() == age->group_size;
        return jobs;
    }
    uint32_t record_pair(Command& cmd, Frame* prior, Frame& current, Command tail, float share) {
        uint64_t total = 0, start = 0;
        for (const auto& m : members)
            total += uint64_t(m.steps) * (m.pair || !prior ? 1u : 2u);
        const uint64_t cut = uint64_t(std::llround(double(total) * share));
        uint32_t calls = 0;
        for (const auto& m : members) {
            const auto execute = [&](Command& cursor, const Frame& f) {
                const uint32_t local = start >= cut ? 0u : uint32_t(std::min<uint64_t>(cut - start, m.steps));
                if (local == 0)
                    cursor = tail;
                const auto split = local && local < m.steps ? tail : Command{};
                start += m.steps;
                return m.run(cursor, f, split, local, m.pair && prior ? 2 : 1);
            };
            uint32_t n = 0;
            if (m.pair)
                n = m.pair(cmd, prior, current, execute);
            else {
                if (prior) {
                    prior->output = prior->colour;
                    n = execute(cmd, *prior);
                    if (n == failed_calls)
                        return n;
                }
                current.output = current.colour;
                const auto second = execute(cmd, current);
                if (second == failed_calls)
                    return second;
                n += second;
            }
            if (n == failed_calls)
                return failed_calls;
            pass_counts.push_back(n);
            calls += n;
        }
        return calls;
    }
    uint32_t record(Command head, const Frame& anchor, Command tail, float share, uint32_t gap) {
        uint64_t total = 0;
        for (const auto& m : members)
            total += m.steps;
        const uint64_t cut = uint64_t(std::llround(double(total) * share));
        uint64_t start = 0;
        uint32_t calls = 0;
        Command cmd = head;
        Frame input = anchor;
        for (const auto& m : members) {
            const uint32_t local_cut = start >= cut ? 0u : uint32_t(std::min<uint64_t>(cut - start, m.steps));
            if (local_cut == 0)
                cmd = tail;
            const Command split = local_cut > 0 && local_cut < m.steps ? tail : Command{};
            const auto n = m.run(cmd, input, split, local_cut, gap);
            if (n == failed_calls)
                return failed_calls;
            pass_counts.push_back(n);
            calls += n;
            input.colour = input.output;
            start += m.steps;
        }
        return calls;
    }
};
} // namespace nr::frame_pair

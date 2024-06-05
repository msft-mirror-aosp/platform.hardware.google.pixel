/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include "aidl/SessionTaskMap.h"

using std::literals::chrono_literals::operator""ms;
using std::literals::chrono_literals::operator""ns;

namespace aidl {
namespace google {
namespace hardware {
namespace power {
namespace impl {
namespace pixel {

SessionValueEntry makeSession(int tg) {
    SessionValueEntry sv;
    sv.tgid = tg;
    sv.uid = tg + 1;
    sv.idString = "Sess" + std::to_string(tg);
    sv.isActive = true;
    sv.isAppSession = false;
    sv.lastUpdatedTime = std::chrono::steady_clock::now();
    sv.votes = std::make_shared<Votes>();
    return sv;
}

// Get all sessions associated with taskId
std::vector<int64_t> getSessions(int taskId, const SessionTaskMap &m) {
    std::vector<int64_t> sessionIds;
    m.forEachSessionInTask(
            taskId, [&](int sessionId, const auto & /*sve*/) { sessionIds.push_back(sessionId); });
    std::sort(sessionIds.begin(), sessionIds.end());
    return sessionIds;
}

// Get all tasks associated with sessionId
std::vector<int> getTasks(int64_t sessionId, const SessionTaskMap &m) {
    std::vector<int> tasks;
    m.forEachSessionValTasks([&](int64_t sessId, const auto & /*sve*/, const auto &linkedTasks) {
        if (sessId != sessionId)
            return;
        tasks.insert(std::end(tasks), std::begin(linkedTasks), std::end(linkedTasks));
    });
    std::sort(tasks.begin(), tasks.end());
    return tasks;
}

// Tests ...
TEST(SessionTaskMapTest, add) {
    SessionTaskMap m;
    EXPECT_TRUE(m.add(1, makeSession(1000), {10, 20, 30}));
    EXPECT_TRUE(m.add(2, makeSession(2000), {40, 50}));
    EXPECT_TRUE(m.add(3, makeSession(2000), {60}));
    EXPECT_FALSE(m.add(3, makeSession(2000), {70}));
}

TEST(SessionTaskMapTest, threeWayMappingSessions) {
    SessionTaskMap m;
    m.add(1, makeSession(1000), {10, 20, 30});
    m.add(2, makeSession(2000), {40, 50, 60});
    m.add(3, makeSession(3000), {50});

    // Check three tasks map properly to sessions
    EXPECT_EQ(std::vector<int64_t>({1}), getSessions(10, m));
    EXPECT_EQ(std::vector<int64_t>({1}), getSessions(20, m));
    EXPECT_EQ(std::vector<int64_t>({1}), getSessions(30, m));
    EXPECT_EQ(std::vector<int64_t>({2}), getSessions(40, m));
    EXPECT_EQ(std::vector<int64_t>({2, 3}), getSessions(50, m));
    EXPECT_EQ(std::vector<int64_t>({2}), getSessions(60, m));
}

TEST(SessionTaskMapTest, threeWayMappingTasks) {
    SessionTaskMap m;
    m.add(1, makeSession(1000), {10, 20, 30});
    m.add(2, makeSession(2000), {40, 50, 60});
    m.add(3, makeSession(3000), {50});

    // Check three sessions map properly to tasks
    EXPECT_EQ(std::vector<int>({10, 20, 30}), getTasks(1, m));
    EXPECT_EQ(std::vector<int>({40, 50, 60}), getTasks(2, m));
    EXPECT_EQ(std::vector<int>({50}), getTasks(3, m));
}

TEST(SessionTaskMapTest, removeNonExisting) {
    SessionTaskMap m;
    EXPECT_FALSE(m.remove(1));
}

TEST(SessionTaskMapTest, removeMappingSessions) {
    SessionTaskMap m;
    m.add(1, makeSession(1000), {10, 20, 30});
    m.add(2, makeSession(2000), {40, 50, 60});
    m.add(3, makeSession(3000), {50});

    // remove
    EXPECT_TRUE(m.remove(2));

    // Check that remaining tasks map correctly to sessions
    EXPECT_EQ(std::vector<int64_t>({1}), getSessions(10, m));
    EXPECT_EQ(std::vector<int64_t>({1}), getSessions(20, m));
    EXPECT_EQ(std::vector<int64_t>({1}), getSessions(30, m));
    EXPECT_EQ(std::vector<int64_t>({}), getSessions(40, m));
    EXPECT_EQ(std::vector<int64_t>({3}), getSessions(50, m));
}

TEST(SessionTaskMapTest, removeMappingTasks) {
    SessionTaskMap m;
    EXPECT_FALSE(m.remove(1));

    m.add(1, makeSession(1000), {10, 20, 30});
    m.add(2, makeSession(2000), {40, 50, 60});
    m.add(3, makeSession(3000), {50});

    // remove
    EXPECT_TRUE(m.remove(2));
    EXPECT_FALSE(m.remove(2));

    // Check that remaining tasks map correctly to sessions
    EXPECT_EQ(std::vector<int>({10, 20, 30}), getTasks(1, m));
    EXPECT_EQ(std::vector<int>({}), getTasks(2, m));
    EXPECT_EQ(std::vector<int>({50}), getTasks(3, m));
}

TEST(SessionTaskMapTest, findEmpty) {
    SessionTaskMap m;
    EXPECT_EQ(nullptr, m.findSession(1));
}

TEST(SessionTaskMapTest, findSessionExists) {
    SessionTaskMap m;
    EXPECT_TRUE(m.add(1, makeSession(1000), {}));
    EXPECT_NE(nullptr, m.findSession(1));
}

TEST(SessionTaskMapTest, findSessionEmptyExistsEmpty) {
    SessionTaskMap m;
    EXPECT_EQ(nullptr, m.findSession(1));
    EXPECT_TRUE(m.add(1, makeSession(1000), {}));
    EXPECT_NE(nullptr, m.findSession(1));
    EXPECT_TRUE(m.remove(1));
    EXPECT_EQ(nullptr, m.findSession(1));
}

TEST(SessionTaskMapTest, sizeTasks) {
    SessionTaskMap m;
    EXPECT_EQ(0, m.sizeTasks());
    EXPECT_TRUE(m.add(1, makeSession(1000), {10, 20, 30}));
    EXPECT_TRUE(m.add(2, makeSession(2000), {40, 50, 60}));
    EXPECT_EQ(6, m.sizeTasks());
}

TEST(SessionTaskMapTest, sizeSessions) {
    SessionTaskMap m;
    EXPECT_EQ(0, m.sizeSessions());
    EXPECT_TRUE(m.add(1, makeSession(1000), {10, 20, 30}));
    EXPECT_TRUE(m.add(2, makeSession(2000), {40, 50, 60}));
    EXPECT_EQ(2, m.sizeSessions());
}

TEST(SessionTaskMapTest, replace) {
    SessionTaskMap m;

    // Add three sessions where sessions 2 and 3 have shared threads
    EXPECT_TRUE(m.add(1, makeSession(1000), {10, 20, 30}));
    EXPECT_TRUE(m.add(2, makeSession(2000), {20}));

    std::vector<pid_t> addedThreads;
    std::vector<pid_t> removedThreads;

    m.replace(1, {10, 40}, &addedThreads, &removedThreads);
    EXPECT_EQ(1, addedThreads.size());
    EXPECT_EQ(40, addedThreads[0]);

    EXPECT_EQ(1, removedThreads.size());
    EXPECT_EQ(30, removedThreads[0]);
}

TEST(SessionTaskMapTest, remove) {
    SessionTaskMap m;
    auto tNow = std::chrono::steady_clock::now();
    const int64_t sessionId = 1;
    SessionValueEntry sve;
    sve.isAppSession = true;
    sve.votes = std::make_shared<Votes>();
    sve.votes->add(sessionId, CpuVote(true, tNow, 400ms, 123, 1024));
    m.add(sessionId, sve, {10, 20, 30});
    EXPECT_TRUE(m.isAnyAppSessionActive(tNow));
    EXPECT_TRUE(m.remove(sessionId));
    EXPECT_FALSE(m.isAnyAppSessionActive(tNow));
}

TEST(SessionTaskMapTest, isAnyAppActive) {
    SessionTaskMap m;
    auto tNow = std::chrono::steady_clock::now();

    EXPECT_FALSE(m.isAnyAppSessionActive(tNow));

    const int sessionId = 1000;
    SessionValueEntry sv;
    sv.isActive = true;
    sv.isAppSession = true;
    sv.lastUpdatedTime = tNow;
    sv.votes = std::make_shared<Votes>();
    sv.votes->add(1, CpuVote(true, tNow, 400ms, 123, 1024));

    EXPECT_TRUE(m.add(sessionId, sv, {10, 20, 30}));
    EXPECT_TRUE(m.isAnyAppSessionActive(tNow));
    EXPECT_FALSE(m.isAnyAppSessionActive(tNow + 500ms));
}

int getVoteMin(const SessionTaskMap &m, int64_t taskId, std::chrono::steady_clock::time_point t) {
    UclampRange range;
    std::optional<int32_t> fakeEfficiencyParam = std::nullopt;
    m.getTaskVoteRange(taskId, t, range, fakeEfficiencyParam, fakeEfficiencyParam);
    return range.uclampMin;
}

TEST(SessionTaskMapTest, votesEdgeCaseOverlap) {
    SessionTaskMap m;

    // Sess 1: 10
    EXPECT_TRUE(m.add(1, makeSession(1000), {10}));
    const auto t0 = std::chrono::steady_clock::now();
    const int voteMax = 1000;

    // Session  Vote  UClamp  [Time start----------------Time End]  Delta
    // 1        1     111     [20----60]                            40
    // 1        2     122              [60-85]                      25
    // 1        3     133              [60--90]                     30
    m.addVote(1, 1, 111, voteMax, t0 + 20ns, 40ns);
    m.addVote(1, 2, 122, voteMax, t0 + 60ns, 25ns);
    m.addVote(1, 3, 133, voteMax, t0 + 60ns, 30ns);

    // Before any votes active
    EXPECT_EQ(0, getVoteMin(m, 10, t0 + 0ns));
    // First vote active
    EXPECT_EQ(111, getVoteMin(m, 10, t0 + 20ns));
    // In middle of first vote
    EXPECT_EQ(111, getVoteMin(m, 10, t0 + 35ns));
    // First, second, and third votes active
    EXPECT_EQ(133, getVoteMin(m, 10, t0 + 60ns));
    // Second and third votes are being used
    EXPECT_EQ(133, getVoteMin(m, 10, t0 + 61ns));
    // Third vote is being used
    EXPECT_EQ(133, getVoteMin(m, 10, t0 + 86ns));
    // No votes active
    EXPECT_EQ(0, getVoteMin(m, 10, t0 + 91ns));
}

TEST(SessionTaskMapTest, votesEdgeCaseNoOverlap) {
    SessionTaskMap m;
    // Sess 2: 30
    EXPECT_TRUE(m.add(2, makeSession(2000), {20}));
    const auto t0 = std::chrono::steady_clock::now();
    const int voteMax = 1000;

    // Session  Vote  UClamp  [Time start----------------Time End]  Delta
    // 2        1     211       [30-55]                             25
    // 2        2     222                       [100-135]           35
    // 2        3     233                                [140-180]  40
    m.addVote(2, 1, 211, voteMax, t0 + 30ns, 25ns);
    m.addVote(2, 2, 222, voteMax, t0 + 100ns, 35ns);
    m.addVote(2, 3, 233, voteMax, t0 + 140ns, 40ns);

    // No votes active yet
    EXPECT_EQ(0, getVoteMin(m, 20, t0 + 0ns));
    // First vote active
    EXPECT_EQ(211, getVoteMin(m, 20, t0 + 30ns));
    // Second vote active
    EXPECT_EQ(222, getVoteMin(m, 20, t0 + 100ns));
    // Third vote active
    EXPECT_EQ(233, getVoteMin(m, 20, t0 + 140ns));
    // No votes active
    EXPECT_EQ(0, getVoteMin(m, 20, t0 + 181ns));
}

TEST(SessionTaskMapTest, TwoSessionsOneInactive) {
    const auto tNow = std::chrono::steady_clock::now();
    SessionTaskMap m;

    {
        SessionValueEntry sv;
        sv.isActive = true;
        sv.isAppSession = true;
        sv.lastUpdatedTime = tNow;
        sv.votes = std::make_shared<Votes>();
        sv.votes->add(11, CpuVote(true, tNow, 400ms, 111, 1024));
        EXPECT_TRUE(m.add(1001, sv, {10, 20, 30}));
    }

    {
        SessionValueEntry sv;
        sv.isActive = true;
        sv.isAppSession = true;
        sv.lastUpdatedTime = tNow;
        sv.votes = std::make_shared<Votes>();
        sv.votes->add(22, CpuVote(true, tNow, 400ms, 222, 1024));
        EXPECT_TRUE(m.add(2001, sv, {10, 20, 30}));
    }

    UclampRange uclampRange;
    std::optional<int32_t> fakeEfficiencyParam;
    m.getTaskVoteRange(10, tNow + 10ns, uclampRange, fakeEfficiencyParam, fakeEfficiencyParam);
    EXPECT_EQ(222, uclampRange.uclampMin);

    auto sessItr = m.findSession(2001);
    EXPECT_NE(nullptr, sessItr);
    sessItr->isActive = false;

    uclampRange.uclampMin = 0;
    uclampRange.uclampMax = 1024;
    m.getTaskVoteRange(10, tNow + 10ns, uclampRange, fakeEfficiencyParam, fakeEfficiencyParam);
    EXPECT_EQ(111, uclampRange.uclampMin);
}

TEST(SessionTaskMapTest, GpuVoteBasic) {
    const auto now = std::chrono::steady_clock::now();
    SessionTaskMap m;
    auto const session_id1 = 1001;
    auto const session_id2 = 1002;
    static auto constexpr gpu_vote_id = static_cast<int>(AdpfVoteType::GPU_CAPACITY);

    auto addSessionWithId = [&](int id) {
        SessionValueEntry sv{.isActive = true,
                             .isAppSession = true,
                             .lastUpdatedTime = now,
                             .votes = std::make_shared<Votes>()};
        EXPECT_TRUE(m.add(id, sv, {10, 20, 30}));
    };
    addSessionWithId(session_id1);
    addSessionWithId(session_id2);

    m.addGpuVote(session_id1, gpu_vote_id, Cycles(222), now, 400ms);
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 1ms), Cycles(222));
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 401ms), Cycles(0));

    m.addGpuVote(session_id1, gpu_vote_id, Cycles(111), now, 100ms);
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 1ms), Cycles(111));
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 101ms), Cycles(0));

    m.addGpuVote(session_id2, gpu_vote_id, Cycles(555), now, 50ms);
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 1ms), Cycles(555));
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 51ms), Cycles(111));
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 101ms), Cycles(0));
}

TEST(SessionTaskMapTest, GpuVoteDifferentHints) {
    const auto now = std::chrono::steady_clock::now();
    SessionTaskMap m;
    auto const session_id1 = 1001;
    auto const session_id2 = 1002;

    auto const capacity_vote_id = static_cast<int>(AdpfVoteType::GPU_CAPACITY);
    auto const load_vote_id = static_cast<int>(AdpfVoteType::GPU_LOAD_UP);

    auto addSessionWithId = [&](int id) {
        SessionValueEntry sv{.isActive = true,
                             .isAppSession = true,
                             .lastUpdatedTime = now,
                             .votes = std::make_shared<Votes>()};
        EXPECT_TRUE(m.add(id, sv, {10, 20, 30}));
    };
    addSessionWithId(session_id1);
    addSessionWithId(session_id2);

    m.addGpuVote(session_id1, capacity_vote_id, Cycles(222), now, 400ms);
    m.addGpuVote(session_id1, load_vote_id, Cycles(111), now, 100ms);
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 1ms), Cycles(333));
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 101ms), Cycles(222));
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 401ms), Cycles(0));

    m.addGpuVote(session_id2, capacity_vote_id, Cycles(321), now, 150ms);
    m.addGpuVote(session_id2, load_vote_id, Cycles(123), now, 50ms);
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 1ms), Cycles(444));
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 51ms), Cycles(333));
    EXPECT_EQ(m.getSessionsGpuCapacity(now + 151ms), Cycles(222));
}

TEST(SessionTaskMapTest, usesPowerEfficiencyValues) {
    SessionTaskMap m;
    SessionValueEntry entry = makeSession(2000);
    m.add(2, entry, {20});
    entry.isPowerEfficient = true;
    m.add(3, entry, {30});

    const auto t0 = std::chrono::steady_clock::now();

    std::optional<int32_t> maxEfficientBase = 400;
    std::optional<int32_t> maxEfficientOffset = 200;

    UclampRange baseVote{.uclampMin = 100, .uclampMax = 1024};

    m.addVote(2, 1, baseVote.uclampMin, baseVote.uclampMax, t0, 400ns);
    m.addVote(3, 1, baseVote.uclampMin, baseVote.uclampMax, t0, 400ns);

    UclampRange range;
    m.getTaskVoteRange(30, t0 + 10ns, range, maxEfficientBase, maxEfficientOffset);
    // During the boost we expect it to equal the dynamic value
    EXPECT_EQ(range.uclampMax, baseVote.uclampMin + *maxEfficientOffset);

    range = UclampRange{};
    m.getTaskVoteRange(30, t0 + 800ns, range, maxEfficientBase, maxEfficientOffset);
    // After the boost expires, we expect it to equal the static value
    EXPECT_EQ(range.uclampMax, *maxEfficientBase);

    range = UclampRange{};
    m.getTaskVoteRange(20, t0 + 10ns, range, maxEfficientBase, maxEfficientOffset);
    // We expect this to be unaffected since this session has no power efficiency set
    EXPECT_EQ(range.uclampMax, baseVote.uclampMax);

    range = UclampRange{};
    m.getTaskVoteRange(20, t0 + 800ns, range, maxEfficientBase, maxEfficientOffset);
    // We expect this to be unaffected since this session has no power efficiency set
    EXPECT_EQ(range.uclampMax, baseVote.uclampMax);
}

}  // namespace pixel
}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace google
}  // namespace aidl

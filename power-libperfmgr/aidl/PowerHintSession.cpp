/*
 * Copyright 2021 The Android Open Source Project
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

#define LOG_TAG "powerhal-libperfmgr"
#define ATRACE_TAG (ATRACE_TAG_POWER | ATRACE_TAG_HAL)

#include "PowerHintSession.h"

#include <android-base/logging.h>
#include <android-base/parsedouble.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <perfmgr/AdpfConfig.h>
#include <private/android_filesystem_config.h>
#include <sys/syscall.h>
#include <time.h>
#include <utils/Trace.h>

#include <atomic>

#include "GpuCalculationHelpers.h"
#include "PowerSessionManager.h"

namespace aidl {
namespace google {
namespace hardware {
namespace power {
namespace impl {
namespace pixel {

using ::android::base::StringPrintf;
using ::android::perfmgr::AdpfConfig;
using ::android::perfmgr::HintManager;
using std::chrono::duration_cast;
using std::chrono::nanoseconds;

using std::operator""ms;

namespace {

static std::atomic<int64_t> sSessionIDCounter{0};

static inline int64_t ns_to_100us(int64_t ns) {
    return ns / 100000;
}

}  // namespace

int64_t PowerHintSession::convertWorkDurationToBoostByPid(
        const std::vector<WorkDuration> &actualDurations) {
    std::shared_ptr<AdpfConfig> adpfConfig = HintManager::GetInstance()->GetAdpfProfile();
    const nanoseconds &targetDuration = mDescriptor->targetNs;
    int64_t &integral_error = mDescriptor->integral_error;
    int64_t &previous_error = mDescriptor->previous_error;
    uint64_t samplingWindowP = adpfConfig->mSamplingWindowP;
    uint64_t samplingWindowI = adpfConfig->mSamplingWindowI;
    uint64_t samplingWindowD = adpfConfig->mSamplingWindowD;
    int64_t targetDurationNanos = (int64_t)targetDuration.count();
    int64_t length = actualDurations.size();
    int64_t p_start =
            samplingWindowP == 0 || samplingWindowP > length ? 0 : length - samplingWindowP;
    int64_t i_start =
            samplingWindowI == 0 || samplingWindowI > length ? 0 : length - samplingWindowI;
    int64_t d_start =
            samplingWindowD == 0 || samplingWindowD > length ? 0 : length - samplingWindowD;
    int64_t dt = ns_to_100us(targetDurationNanos);
    int64_t err_sum = 0;
    int64_t derivative_sum = 0;
    for (int64_t i = std::min({p_start, i_start, d_start}); i < length; i++) {
        int64_t actualDurationNanos = actualDurations[i].durationNanos;
        if (std::abs(actualDurationNanos) > targetDurationNanos * 20) {
            ALOGW("The actual duration is way far from the target (%" PRId64 " >> %" PRId64 ")",
                  actualDurationNanos, targetDurationNanos);
        }
        // PID control algorithm
        int64_t error = ns_to_100us(actualDurationNanos - targetDurationNanos);
        if (i >= d_start) {
            derivative_sum += error - previous_error;
        }
        if (i >= p_start) {
            err_sum += error;
        }
        if (i >= i_start) {
            integral_error += error * dt;
            integral_error = std::min(adpfConfig->getPidIHighDivI(), integral_error);
            integral_error = std::max(adpfConfig->getPidILowDivI(), integral_error);
        }
        previous_error = error;
    }

    auto pid_pu_active = adpfConfig->mPidPu;
    if (adpfConfig->mHeuristicBoostOn.has_value() && adpfConfig->mHeuristicBoostOn.value()) {
        pid_pu_active = mHeuristicBoostActive
                                ? adpfConfig->mPidPu * adpfConfig->mHBoostPidPuFactor.value()
                                : adpfConfig->mPidPu;
    }
    int64_t pOut = static_cast<int64_t>((err_sum > 0 ? adpfConfig->mPidPo : pid_pu_active) *
                                        err_sum / (length - p_start));
    int64_t iOut = static_cast<int64_t>(adpfConfig->mPidI * integral_error);
    int64_t dOut =
            static_cast<int64_t>((derivative_sum > 0 ? adpfConfig->mPidDo : adpfConfig->mPidDu) *
                                 derivative_sum / dt / (length - d_start));

    int64_t output = pOut + iOut + dOut;
    ATRACE_INT(mAppDescriptorTrace.trace_pid_err.c_str(), err_sum / (length - p_start));
    ATRACE_INT(mAppDescriptorTrace.trace_pid_integral.c_str(), integral_error);
    ATRACE_INT(mAppDescriptorTrace.trace_pid_derivative.c_str(),
               derivative_sum / dt / (length - d_start));
    ATRACE_INT(mAppDescriptorTrace.trace_pid_pOut.c_str(), pOut);
    ATRACE_INT(mAppDescriptorTrace.trace_pid_iOut.c_str(), iOut);
    ATRACE_INT(mAppDescriptorTrace.trace_pid_dOut.c_str(), dOut);
    ATRACE_INT(mAppDescriptorTrace.trace_pid_output.c_str(), output);
    return output;
}

AppHintDesc::AppHintDesc(int64_t sessionId, int32_t tgid, int32_t uid,
                         const std::vector<int32_t> &threadIds, SessionTag tag,
                         std::chrono::nanoseconds pTargetNs)
    : sessionId(sessionId),
      tgid(tgid),
      uid(uid),
      targetNs(pTargetNs),
      thread_ids(threadIds),
      tag(tag),
      pidControlVariable(0),
      is_active(true),
      update_count(0),
      integral_error(0),
      previous_error(0) {}

PowerHintSession::PowerHintSession(int32_t tgid, int32_t uid, const std::vector<int32_t> &threadIds,
                                   int64_t durationNs, SessionTag tag)
    : mPSManager(PowerSessionManager::getInstance()),
      mSessionId(++sSessionIDCounter),
      mIdString(StringPrintf("%" PRId32 "-%" PRId32 "-%" PRId64, tgid, uid, mSessionId)),
      mDescriptor(std::make_shared<AppHintDesc>(mSessionId, tgid, uid, threadIds, tag,
                                                std::chrono::nanoseconds(durationNs))),
      mAppDescriptorTrace(mIdString),
      mTag(tag),
      mSessionRecords(HintManager::GetInstance()->GetAdpfProfile()->mHeuristicBoostOn.has_value() &&
                                      HintManager::GetInstance()
                                              ->GetAdpfProfile()
                                              ->mHeuristicBoostOn.value()
                              ? std::make_unique<SessionRecords>(HintManager::GetInstance()
                                                                         ->GetAdpfProfile()
                                                                         ->mMaxRecordsNum.value())
                              : nullptr) {
    ATRACE_CALL();
    ATRACE_INT(mAppDescriptorTrace.trace_target.c_str(), mDescriptor->targetNs.count());
    ATRACE_INT(mAppDescriptorTrace.trace_active.c_str(), mDescriptor->is_active.load());

    mLastUpdatedTime.store(std::chrono::steady_clock::now());
    mPSManager->addPowerSession(mIdString, mDescriptor, threadIds);
    // init boost
    auto adpfConfig = HintManager::GetInstance()->GetAdpfProfile();
    mPSManager->voteSet(
            mSessionId, AdpfHintType::ADPF_CPU_LOAD_RESET, adpfConfig->mUclampMinLoadReset, kUclampMax,
            std::chrono::steady_clock::now(),
            duration_cast<nanoseconds>(mDescriptor->targetNs * adpfConfig->mStaleTimeFactor / 2.0));

    mPSManager->voteSet(mSessionId, AdpfHintType::ADPF_VOTE_DEFAULT, adpfConfig->mUclampMinInit,
                        kUclampMax, std::chrono::steady_clock::now(), mDescriptor->targetNs);
    ALOGV("PowerHintSession created: %s", mDescriptor->toString().c_str());
}

PowerHintSession::~PowerHintSession() {
    ATRACE_CALL();
    close();
    ALOGV("PowerHintSession deleted: %s", mDescriptor->toString().c_str());
    ATRACE_INT(mAppDescriptorTrace.trace_target.c_str(), 0);
    ATRACE_INT(mAppDescriptorTrace.trace_actl_last.c_str(), 0);
    ATRACE_INT(mAppDescriptorTrace.trace_active.c_str(), 0);
}

bool PowerHintSession::isAppSession() {
    // Check if uid is in range reserved for applications
    return mDescriptor->uid >= AID_APP_START;
}

void PowerHintSession::updatePidControlVariable(int pidControlVariable, bool updateVote) {
    mDescriptor->pidControlVariable = pidControlVariable;
    if (updateVote) {
        auto adpfConfig = HintManager::GetInstance()->GetAdpfProfile();
        mPSManager->voteSet(mSessionId, AdpfHintType::ADPF_VOTE_DEFAULT, pidControlVariable,
                            kUclampMax, std::chrono::steady_clock::now(),
                            std::max(duration_cast<nanoseconds>(mDescriptor->targetNs *
                                                                adpfConfig->mStaleTimeFactor),
                                     nanoseconds(adpfConfig->mReportingRateLimitNs) * 2));
    }
    ATRACE_INT(mAppDescriptorTrace.trace_min.c_str(), pidControlVariable);
}

void PowerHintSession::tryToSendPowerHint(std::string hint) {
    if (!mSupportedHints[hint].has_value()) {
        mSupportedHints[hint] = HintManager::GetInstance()->IsHintSupported(hint);
    }
    if (mSupportedHints[hint].value()) {
        HintManager::GetInstance()->DoHint(hint);
    }
}

void PowerHintSession::dumpToStream(std::ostream &stream) {
    stream << "ID.Min.Act.Timeout(" << mIdString;
    stream << ", " << mDescriptor->pidControlVariable;
    stream << ", " << mDescriptor->is_active;
    stream << ", " << isTimeout() << ")";
}

ndk::ScopedAStatus PowerHintSession::pause() {
    if (mSessionClosed) {
        ALOGE("Error: session is dead");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (!mDescriptor->is_active.load())
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    // Reset to default uclamp value.
    mPSManager->setThreadsFromPowerSession(mSessionId, {});
    mDescriptor->is_active.store(false);
    mPSManager->pause(mSessionId);
    ATRACE_INT(mAppDescriptorTrace.trace_active.c_str(), false);
    ATRACE_INT(mAppDescriptorTrace.trace_min.c_str(), 0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSession::resume() {
    if (mSessionClosed) {
        ALOGE("Error: session is dead");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (mDescriptor->is_active.load())
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    mPSManager->setThreadsFromPowerSession(mSessionId, mDescriptor->thread_ids);
    mDescriptor->is_active.store(true);
    // resume boost
    mPSManager->resume(mSessionId);
    ATRACE_INT(mAppDescriptorTrace.trace_active.c_str(), true);
    ATRACE_INT(mAppDescriptorTrace.trace_min.c_str(), mDescriptor->pidControlVariable);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSession::close() {
    bool sessionClosedExpectedToBe = false;
    if (!mSessionClosed.compare_exchange_strong(sessionClosedExpectedToBe, true)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    // Remove the session from PowerSessionManager first to avoid racing.
    mPSManager->removePowerSession(mSessionId);
    mDescriptor->is_active.store(false);
    ATRACE_INT(mAppDescriptorTrace.trace_min.c_str(), 0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSession::updateTargetWorkDuration(int64_t targetDurationNanos) {
    if (mSessionClosed) {
        ALOGE("Error: session is dead");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (targetDurationNanos <= 0) {
        ALOGE("Error: targetDurationNanos(%" PRId64 ") should bigger than 0", targetDurationNanos);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    targetDurationNanos =
            targetDurationNanos * HintManager::GetInstance()->GetAdpfProfile()->mTargetTimeFactor;

    mDescriptor->targetNs = std::chrono::nanoseconds(targetDurationNanos);
    mPSManager->updateTargetWorkDuration(mSessionId, AdpfHintType::ADPF_VOTE_DEFAULT,
                                         mDescriptor->targetNs);
    ATRACE_INT(mAppDescriptorTrace.trace_target.c_str(), targetDurationNanos);

    return ndk::ScopedAStatus::ok();
}

bool PowerHintSession::updateHeuristicBoost() {
    auto maxDurationUs = mSessionRecords->getMaxDuration();  // micro seconds
    auto avgDurationUs = mSessionRecords->getAvgDuration();  // micro seconds
    auto numOfReportedDurations = mSessionRecords->getNumOfRecords();
    auto numOfMissedCycles = mSessionRecords->getNumOfMissedCycles();

    if (!maxDurationUs.has_value() || !avgDurationUs.has_value()) {
        return false;
    }

    double maxToAvgRatio;
    if (numOfReportedDurations <= 0) {
        maxToAvgRatio = maxDurationUs.value() * 1.0 / (mDescriptor->targetNs.count() / 1000);
    } else {
        maxToAvgRatio = maxDurationUs.value() / avgDurationUs.value();
    }

    auto adpfConfig = HintManager::GetInstance()->GetAdpfProfile();

    if (mSessionRecords->isLowFrameRate(adpfConfig->mLowFrameRateThreshold.value())) {
        // Turn off the boost when the FPS drops to a low value,
        // since usually this is because of ui changing to low rate scenarios.
        // Extra boost is not needed in these scenarios.
        mHeuristicBoostActive = false;
    } else if (numOfMissedCycles >= adpfConfig->mHBoostOnMissedCycles.value()) {
        mHeuristicBoostActive = true;
    } else if (numOfMissedCycles <= adpfConfig->mHBoostOffMissedCycles.value() &&
               maxToAvgRatio < adpfConfig->mHBoostOffMaxAvgRatio.value()) {
        mHeuristicBoostActive = false;
    }
    ATRACE_INT(mAppDescriptorTrace.trace_heuristic_boost_active.c_str(), mHeuristicBoostActive);
    ATRACE_INT(mAppDescriptorTrace.trace_missed_cycles.c_str(), numOfMissedCycles);
    ATRACE_INT(mAppDescriptorTrace.trace_avg_duration.c_str(), avgDurationUs.value());
    ATRACE_INT(mAppDescriptorTrace.trace_max_duration.c_str(), maxDurationUs.value());
    ATRACE_INT(mAppDescriptorTrace.trace_low_frame_rate.c_str(),
               mSessionRecords->isLowFrameRate(adpfConfig->mLowFrameRateThreshold.value()));
    return mHeuristicBoostActive;
}

ndk::ScopedAStatus PowerHintSession::reportActualWorkDuration(
        const std::vector<WorkDuration> &actualDurations) {
    if (mSessionClosed) {
        ALOGE("Error: session is dead");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (mDescriptor->targetNs.count() == 0LL) {
        ALOGE("Expect to call updateTargetWorkDuration() first.");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (actualDurations.empty()) {
        ALOGE("Error: durations shouldn't be empty.");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (!mDescriptor->is_active.load()) {
        ALOGE("Error: shouldn't report duration during pause state.");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    auto adpfConfig = HintManager::GetInstance()->GetAdpfProfile();
    mDescriptor->update_count++;
    bool isFirstFrame = isTimeout();
    ATRACE_INT(mAppDescriptorTrace.trace_batch_size.c_str(), actualDurations.size());
    ATRACE_INT(mAppDescriptorTrace.trace_actl_last.c_str(), actualDurations.back().durationNanos);
    ATRACE_INT(mAppDescriptorTrace.trace_target.c_str(), mDescriptor->targetNs.count());
    ATRACE_INT(mAppDescriptorTrace.trace_hint_count.c_str(), mDescriptor->update_count);
    ATRACE_INT(mAppDescriptorTrace.trace_hint_overtime.c_str(),
               actualDurations.back().durationNanos - mDescriptor->targetNs.count() > 0);
    ATRACE_INT(mAppDescriptorTrace.trace_is_first_frame.c_str(), (isFirstFrame) ? (1) : (0));
    ATRACE_INT(mAppDescriptorTrace.trace_cpu_duration.c_str(),
               actualDurations.back().cpuDurationNanos);
    ATRACE_INT(mAppDescriptorTrace.trace_gpu_duration.c_str(),
               actualDurations.back().gpuDurationNanos);

    mLastUpdatedTime.store(std::chrono::steady_clock::now());
    if (isFirstFrame) {
        if (isAppSession()) {
            tryToSendPowerHint("ADPF_FIRST_FRAME");
        }

        mPSManager->updateUniversalBoostMode();
    }

    mPSManager->disableBoosts(mSessionId);

    if (!adpfConfig->mPidOn) {
        updatePidControlVariable(adpfConfig->mUclampMinHigh);
        return ndk::ScopedAStatus::ok();
    }

    if (adpfConfig->mHeuristicBoostOn.has_value() && adpfConfig->mHeuristicBoostOn.value()) {
        mSessionRecords->addReportedDurations(actualDurations, mDescriptor->targetNs.count());
        updateHeuristicBoost();
    }

    int64_t output = convertWorkDurationToBoostByPid(actualDurations);

    // Apply to all the threads in the group
    auto uclampMinCeiling = adpfConfig->mUclampMinHigh;
    if (adpfConfig->mHeuristicBoostOn.has_value() && adpfConfig->mHeuristicBoostOn.value()) {
        uclampMinCeiling = mHeuristicBoostActive ? adpfConfig->mHBoostUclampMin.value()
                                                 : adpfConfig->mUclampMinHigh;
    }

    int next_min = std::min(static_cast<int>(uclampMinCeiling),
                            mDescriptor->pidControlVariable + static_cast<int>(output));
    next_min = std::max(static_cast<int>(adpfConfig->mUclampMinLow), next_min);

    updatePidControlVariable(next_min);

    if (!adpfConfig->mGpuBoostOn.value_or(false) || !adpfConfig->mGpuBoostCapacityMax ||
        !actualDurations.back().gpuDurationNanos) {
        return ndk::ScopedAStatus::ok();
    }

    auto const gpu_freq = mPSManager->gpuFrequency();
    if (!gpu_freq) {
        return ndk::ScopedAStatus::ok();
    }
    auto const additional_gpu_capacity =
            calculate_capacity(actualDurations.back(), mDescriptor->targetNs, *gpu_freq);
    ATRACE_INT(mAppDescriptorTrace.trace_gpu_capacity.c_str(),
               static_cast<int>(additional_gpu_capacity));

    auto const additional_gpu_capacity_clamped = std::clamp(
            additional_gpu_capacity, Cycles(0), Cycles(*adpfConfig->mGpuBoostCapacityMax));

    mPSManager->voteSet(
            mSessionId, AdpfHintType::ADPF_GPU_CAPACITY, additional_gpu_capacity_clamped,
            std::chrono::steady_clock::now(),
            duration_cast<nanoseconds>(mDescriptor->targetNs * adpfConfig->mStaleTimeFactor));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSession::sendHint(SessionHint hint) {
    if (mSessionClosed) {
        ALOGE("Error: session is dead");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (mDescriptor->targetNs.count() == 0LL) {
        ALOGE("Expect to call updateTargetWorkDuration() first.");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    auto adpfConfig = HintManager::GetInstance()->GetAdpfProfile();

    switch (hint) {
        case SessionHint::CPU_LOAD_UP:
            updatePidControlVariable(mDescriptor->pidControlVariable);
            mPSManager->voteSet(mSessionId, AdpfHintType::ADPF_CPU_LOAD_UP,
                                adpfConfig->mUclampMinLoadUp, kUclampMax,
                                std::chrono::steady_clock::now(), mDescriptor->targetNs * 2);
            break;
        case SessionHint::CPU_LOAD_DOWN:
            updatePidControlVariable(adpfConfig->mUclampMinLow);
            break;
        case SessionHint::CPU_LOAD_RESET:
            updatePidControlVariable(
                    std::max(adpfConfig->mUclampMinInit,
                             static_cast<uint32_t>(mDescriptor->pidControlVariable)),
                    false);
            mPSManager->voteSet(mSessionId, AdpfHintType::ADPF_CPU_LOAD_RESET,
                                adpfConfig->mUclampMinLoadReset, kUclampMax,
                                std::chrono::steady_clock::now(),
                                duration_cast<nanoseconds>(mDescriptor->targetNs *
                                                           adpfConfig->mStaleTimeFactor / 2.0));
            break;
        case SessionHint::CPU_LOAD_RESUME:
            mPSManager->voteSet(mSessionId, AdpfHintType::ADPF_CPU_LOAD_RESUME,
                                mDescriptor->pidControlVariable, kUclampMax,
                                std::chrono::steady_clock::now(),
                                duration_cast<nanoseconds>(mDescriptor->targetNs *
                                                           adpfConfig->mStaleTimeFactor / 2.0));
            break;
        case SessionHint::GPU_LOAD_UP:
            mPSManager->voteSet(mSessionId, AdpfHintType::ADPF_GPU_LOAD_UP,
                                Cycles(adpfConfig->mGpuCapacityLoadUpHeadroom),
                                std::chrono::steady_clock::now(), mDescriptor->targetNs);
            break;
        case SessionHint::GPU_LOAD_DOWN:
            // TODO(kevindubois): add impl
            break;
        case SessionHint::GPU_LOAD_RESET:
            // TODO(kevindubois): add impl
            break;
        default:
            ALOGE("Error: hint is invalid");
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    tryToSendPowerHint(toString(hint));
    mLastUpdatedTime.store(std::chrono::steady_clock::now());
    mLastHintSent = static_cast<int>(hint);
    ATRACE_INT(mAppDescriptorTrace.trace_session_hint.c_str(), static_cast<int>(hint));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSession::setMode(SessionMode mode, bool enabled) {
    if (mSessionClosed) {
        ALOGE("Error: session is dead");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    switch (mode) {
        case SessionMode::POWER_EFFICIENCY:
            break;
        default:
            ALOGE("Error: mode is invalid");
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    mModes[static_cast<size_t>(mode)] = enabled;
    ATRACE_INT(mAppDescriptorTrace.trace_modes[static_cast<size_t>(mode)].c_str(), enabled);
    mLastUpdatedTime.store(std::chrono::steady_clock::now());
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSession::setThreads(const std::vector<int32_t> &threadIds) {
    if (mSessionClosed) {
        ALOGE("Error: session is dead");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (threadIds.empty()) {
        ALOGE("Error: threadIds should not be empty");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    mDescriptor->thread_ids = threadIds;
    mPSManager->setThreadsFromPowerSession(mSessionId, threadIds);
    // init boost
    updatePidControlVariable(HintManager::GetInstance()->GetAdpfProfile()->mUclampMinInit);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSession::getSessionConfig(SessionConfig *_aidl_return) {
    _aidl_return->id = mSessionId;
    return ndk::ScopedAStatus::ok();
}

SessionTag PowerHintSession::getSessionTag() const {
    return mTag;
}

std::string AppHintDesc::toString() const {
    std::string out = StringPrintf("session %" PRId64 "\n", sessionId);
    out.append(
            StringPrintf("  duration: %" PRId64 " ns\n", static_cast<int64_t>(targetNs.count())));
    out.append(StringPrintf("  uclamp.min: %d \n", pidControlVariable));
    out.append(StringPrintf("  uid: %d, tgid: %d\n", uid, tgid));
    return out;
}

bool PowerHintSession::isActive() {
    return mDescriptor->is_active.load();
}

bool PowerHintSession::isTimeout() {
    auto now = std::chrono::steady_clock::now();
    time_point<steady_clock> staleTime =
            mLastUpdatedTime.load() +
            nanoseconds(static_cast<int64_t>(
                    mDescriptor->targetNs.count() *
                    HintManager::GetInstance()->GetAdpfProfile()->mStaleTimeFactor));
    return now >= staleTime;
}

}  // namespace pixel
}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace google
}  // namespace aidl

/*
 * Copyright (C) 2017 The Android Open Source Project
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
 * See the License for the specic language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_LIBPERFMGR_HINTMANAGER_H_
#define ANDROID_LIBPERFMGR_HINTMANAGER_H_

#include <android-base/thread_annotations.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfmgr/AdpfConfig.h"
#include "perfmgr/NodeLooperThread.h"

namespace android {
namespace perfmgr {

struct HintStats {
    HintStats() : count(0), duration_ms(0) {}
    uint32_t count;
    uint64_t duration_ms;
};

struct HintStatus {
    const std::chrono::milliseconds max_timeout;
    HintStatus() : max_timeout(std::chrono::milliseconds(0)) {}
    explicit HintStatus(std::chrono::milliseconds max_timeout)
        : max_timeout(max_timeout),
          start_time(std::chrono::steady_clock::time_point::min()),
          end_time(std::chrono::steady_clock::time_point::min()) {}
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    struct HintStatsInternal {
        HintStatsInternal() : count(0), duration_ms(0) {}
        std::atomic<uint32_t> count;
        std::atomic<uint64_t> duration_ms;
    } stats;
};

enum class HintActionType { Node, DoHint, EndHint, MaskHint };

struct HintAction {
    HintAction(HintActionType t, const std::string &v, const std::string &p)
        : type(t), value(v), enable_property(p) {}
    HintActionType type;
    std::string value;
    std::string enable_property;
};

struct Hint {
    Hint() {}
    Hint(const Hint &obj)
        : node_actions(obj.node_actions),
          hint_actions(obj.hint_actions),
          mask_requesters(obj.mask_requesters),
          status(obj.status) {}
    std::vector<NodeAction> node_actions;
    std::vector<HintAction> hint_actions;
    mutable std::mutex hint_lock;
    std::set<std::string> mask_requesters GUARDED_BY(hint_lock);
    std::shared_ptr<HintStatus> status GUARDED_BY(hint_lock);
};

// HintManager is the external interface of the library to be used by PowerHAL
// to do power hints with sysfs nodes. HintManager maintains a representation of
// the actions that are parsed from the configuration file as a mapping from a
// PowerHint to the set of actions that are performed for that PowerHint.
class HintManager {
  public:
    HintManager(sp<NodeLooperThread> nm, const std::unordered_map<std::string, Hint> &actions,
                const std::vector<std::shared_ptr<AdpfConfig>> &adpfs)
        : nm_(std::move(nm)), actions_(actions), adpfs_(adpfs), adpf_index_(0) {}
    ~HintManager() {
        if (nm_.get() != nullptr) nm_->Stop();
    }

    // Return true if the sysfs manager thread is running.
    bool IsRunning() const;

    // Do hint based on hint_type which defined as PowerHint in the actions
    // section of the JSON config. Return true with valid hint_type and also
    // NodeLooperThread::Request succeeds; otherwise return false.
    bool DoHint(const std::string& hint_type);

    // Do hint with the override time for all actions defined for the given
    // hint_type.  Return true with valid hint_type and also
    // NodeLooperThread::Request succeeds; otherwise return false.
    bool DoHint(const std::string& hint_type,
                std::chrono::milliseconds timeout_ms_override);

    // End hint early. Return true with valid hint_type and also
    // NodeLooperThread::Cancel succeeds; otherwise return false.
    bool EndHint(const std::string& hint_type);

    // Query if given hint supported.
    bool IsHintSupported(const std::string& hint_type) const;

    // Query if given hint enabled.
    bool IsHintEnabled(const std::string &hint_type) const;

    // set ADPF config by profile name.
    bool SetAdpfProfile(const std::string &profile_name);

    // get current ADPF.
    std::shared_ptr<AdpfConfig> GetAdpfProfile() const;

    // Query if given AdpfProfile supported.
    bool IsAdpfProfileSupported(const std::string &name) const;

    // Static method to construct HintManager from the JSON config file.
    static std::unique_ptr<HintManager> GetFromJSON(
        const std::string& config_path, bool start = true);

    // Return available hints managed by HintManager
    std::vector<std::string> GetHints() const;

    // Return stats of hints managed by HintManager
    HintStats GetHintStats(const std::string &hint_type) const;

    // Dump internal status to fd
    void DumpToFd(int fd);

    // Start thread loop
    bool Start();

    // Singleton
    static std::shared_ptr<HintManager> GetInstance();
    static std::shared_ptr<HintManager> Reload(bool start);

  protected:
    static std::vector<std::unique_ptr<Node>> ParseNodes(
        const std::string& json_doc);
    static std::unordered_map<std::string, Hint> ParseActions(
            const std::string &json_doc, const std::vector<std::unique_ptr<Node>> &nodes);
    static std::vector<std::shared_ptr<AdpfConfig>> ParseAdpfConfigs(const std::string &json_doc);
    static bool InitHintStatus(const std::unique_ptr<HintManager> &hm);

  private:
    HintManager(HintManager const&) = delete;
    void operator=(HintManager const&) = delete;
    static std::shared_ptr<HintManager> mInstance;

    bool ValidateHint(const std::string& hint_type) const;
    // Helper function to update the HintStatus when DoHint
    void DoHintStatus(const std::string &hint_type, std::chrono::milliseconds timeout_ms);
    // Helper function to update the HintStatus when EndHint
    void EndHintStatus(const std::string &hint_type);
    // Helper function to take hint actions when DoHint
    void DoHintAction(const std::string &hint_type);
    // Helper function to take hint actions when EndHint
    void EndHintAction(const std::string &hint_type);
    sp<NodeLooperThread> nm_;
    std::unordered_map<std::string, Hint> actions_;
    std::vector<std::shared_ptr<AdpfConfig>> adpfs_;
    uint32_t adpf_index_;
};

}  // namespace perfmgr
}  // namespace android

#endif  // ANDROID_LIBPERFMGR_HINTMANAGER_H_

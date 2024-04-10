/*
 * Copyright (C) 2024 The Android Open Source Project
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

 #ifndef HARDWARE_GOOGLE_PIXEL_PIXELSTATS_BATTERYFGREPORTER_H
 #define HARDWARE_GOOGLE_PIXEL_PIXELSTATS_BATTERYFGREPORTER_H

#include <cstdint>
#include <string>

#include <aidl/android/frameworks/stats/IStats.h>

namespace android {
namespace hardware {
namespace google {
namespace pixel {

using aidl::android::frameworks::stats::IStats;

class BatteryFGReporter {
  public:
    BatteryFGReporter();

    void checkAndReportFwUpdate(const std::shared_ptr<IStats> &stats_client, const std::string &path);
    void checkAndReportFGAbnormality(const std::shared_ptr<IStats> &stats_client, const std::string &path);

  private:
    const int kVendorAtomOffset = 2;

    enum FGEventType {
      EvtFWUpdate = 0x4655,
    };

    struct BatteryFGLearningParam {
      enum FGEventType type;
      uint16_t fcnom;
      uint16_t dpacc;
      uint16_t dqacc;
      uint16_t fcrep;
      uint16_t repsoc;
      uint16_t msoc;
      uint16_t vfsoc;
      uint16_t fstat;
      uint16_t rcomp0;
      uint16_t tempco;
    };

    int64_t report_time_ = 0;
    int64_t getTimeSecs();

    unsigned int last_abnl_;

    void reportEvent(const std::shared_ptr<IStats> &stats_client,
                     const struct BatteryFGLearningParam &params);

    const int kNumFwUpdateFields = 3;
    const int kNumHistoryValidationFields = 4;
};

}  // namespace pixel
}  // namespace google
}  // namespace hardware
}  // namespace android

 #endif  // HARDWARE_GOOGLE_PIXEL_PIXELSTATS_BATTERYFGREPORTER_H

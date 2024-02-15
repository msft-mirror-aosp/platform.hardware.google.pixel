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
#pragma once

#include <vector>

#include "virtualtemp_estimator_data.h"

namespace thermal {
namespace vtestimator {

enum VtEstimatorStatus {
    kVtEstimatorOk = 0,
    kVtEstimatorInvalidArgs = 1,
    kVtEstimatorInitFailed = 2,
    kVtEstimatorInvokeFailed = 3,
    kVtEstimatorUnSupported = 4,
};

enum VtEstimationType { kUseMLModel = 0, kUseLinearModel = 1, kInvalidEstimationType = 2 };

struct MLModelInitData {
    MLModelInitData() {
        model_path = "";
        use_prev_samples = false;
        prev_samples_order = 1;
        offset = 0;
    }
    ~MLModelInitData() {}

    std::string model_path;
    bool use_prev_samples;
    size_t prev_samples_order;
    float offset;
};

struct LinearModelInitData {
    LinearModelInitData() {
        use_prev_samples = false;
        prev_samples_order = 1;
        offset = 0;
    }
    ~LinearModelInitData() {}

    bool use_prev_samples;
    size_t prev_samples_order;
    std::vector<float> coefficients;
    float offset;
};

union VtEstimationInitData {
    VtEstimationInitData() {}
    ~VtEstimationInitData() {}

    MLModelInitData ml_model_init_data;
    LinearModelInitData linear_model_init_data;
};

// Class to estimate virtual temperature
class VirtualTempEstimator {
  public:
    // Implicit copy-move headers.
    VirtualTempEstimator(const VirtualTempEstimator &) = delete;
    VirtualTempEstimator(VirtualTempEstimator &&) = default;
    VirtualTempEstimator &operator=(const VirtualTempEstimator &) = delete;
    VirtualTempEstimator &operator=(VirtualTempEstimator &&) = default;

    VirtualTempEstimator(VtEstimationType type, size_t num_linked_sensors);
    ~VirtualTempEstimator();

    // Initializes the estimator based on init_data
    VtEstimatorStatus Initialize(const VtEstimationInitData &init_data);

    // Performs the prediction and returns estimated value in output
    VtEstimatorStatus Estimate(const std::vector<float> &thermistors, float *output);

  private:
    void LoadTFLiteWrapper();
    VtEstimationType type;
    std::unique_ptr<VtEstimatorTFLiteData> tflite_instance_;
    std::unique_ptr<VtEstimatorLinearModelData> linear_model_instance_;

    VtEstimatorStatus LinearModelInitialize(LinearModelInitData data);
    VtEstimatorStatus TFliteInitialize(MLModelInitData data);

    VtEstimatorStatus LinearModelEstimate(const std::vector<float> &thermistors, float *output);
    VtEstimatorStatus TFliteEstimate(const std::vector<float> &thermistors, float *output);
};

}  // namespace vtestimator
}  // namespace thermal

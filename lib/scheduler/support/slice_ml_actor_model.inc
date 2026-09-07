// Copyright 2025-2026 coRAN LABS Private Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
namespace ocudu {
namespace slice_ml {

inline constexpr unsigned ACTOR_SEED_N_IN      = 23;
inline constexpr unsigned ACTOR_SEED_D_HIDDEN  = 16;
inline constexpr unsigned ACTOR_SEED_D_FC      = 32;
inline constexpr unsigned ACTOR_SEED_N_ACTIONS = 19;

inline constexpr double ACTOR_SEED_FEAT_MEAN[23] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                                                    0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                                                    0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
inline constexpr double ACTOR_SEED_FEAT_SCALE[23] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
                                                     1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
                                                     1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

inline constexpr double ACTOR_SEED_B_OUT[19] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                                                0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

}
}

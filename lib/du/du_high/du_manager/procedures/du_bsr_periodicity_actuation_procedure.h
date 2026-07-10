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

#include "../du_ue/du_ue_manager_repository.h"
#include "ocudu/du/du_high/du_manager/du_manager_params.h"
#include "ocudu/support/async/async_task.h"

namespace ocudu {
namespace odu {

async_task<void> start_bsr_periodicity_actuation(du_ue_index_t             ue_index,
                                                 unsigned                  recommended_periodic_bsr_timer_sf,
                                                 du_ue_manager_repository& ue_mng,
                                                 const du_manager_params&  du_params);

}
}

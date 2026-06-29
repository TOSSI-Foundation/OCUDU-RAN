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

#include "du_bsr_periodicity_actuation_procedure.h"
#include "../converters/asn1_rrc_config_helpers.h"
#include "../du_ue/du_ue.h"
#include "ocudu/asn1/rrc_nr/cell_group_config.h"
#include "ocudu/f1ap/du/f1ap_du_ue_context_update.h"
#include "ocudu/mac/bsr_config.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/async/async_task.h"
#include "ocudu/support/async/coroutine.h"
#include <optional>

using namespace ocudu;
using namespace odu;

namespace {

std::optional<f1ap_ue_context_modification_required>
prepare_bsr_actuation_request(du_ue_index_t ue_index, unsigned recommended_sf, du_ue_manager_repository& ue_mng)
{
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("DU-MNG");

  du_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    return std::nullopt;
  }

  const du_ue_resource_config& cur_res = *ue->resources;
  if (not cur_res.cell_group.mcg_cfg.bsr_cfg.has_value()) {
    logger.debug("ue={}: Skipping BSR periodicity actuation. Cause: UE has no BSR-Config.", fmt::underlying(ue_index));
    return std::nullopt;
  }

  const periodic_bsr_timer new_timer = to_periodic_bsr_timer(recommended_sf);
  const periodic_bsr_timer cur_timer = cur_res.cell_group.mcg_cfg.bsr_cfg.value().periodic_timer;
  if (new_timer == cur_timer) {

    return std::nullopt;
  }

  du_ue_resource_config new_res                             = cur_res;
  new_res.cell_group.mcg_cfg.bsr_cfg.value().periodic_timer = new_timer;

  asn1::rrc_nr::cell_group_cfg_s asn1_cell_group;
  calculate_cell_group_config_diff(asn1_cell_group, cur_res, new_res);

  byte_buffer   packed_cell_group;
  asn1::bit_ref bref{packed_cell_group};
  if (asn1_cell_group.pack(bref) != asn1::OCUDUASN_SUCCESS) {
    logger.warning("ue={}: Skipping BSR periodicity actuation. Cause: failed to pack CellGroupConfig.",
                   fmt::underlying(ue_index));
    return std::nullopt;
  }

  logger.info("ue={}: Actuating BSR periodicity change sf{} -> sf{} via F1AP UE Context Modification Required.",
              fmt::underlying(ue_index),
              periodic_bsr_timer_to_value(cur_timer),
              periodic_bsr_timer_to_value(new_timer));

  f1ap_ue_context_modification_required req;
  req.ue_index       = ue_index;
  req.cell_group_cfg = std::move(packed_cell_group);
  return req;
}

}

async_task<void> ocudu::odu::start_bsr_periodicity_actuation(du_ue_index_t             ue_index,
                                                             unsigned                  recommended_periodic_bsr_timer_sf,
                                                             du_ue_manager_repository& ue_mng,
                                                             const du_manager_params&  du_params)
{
  return launch_async(
      [ue_index,
       recommended_periodic_bsr_timer_sf,
       &ue_mng,
       &du_params,
       req     = std::optional<f1ap_ue_context_modification_required>{},
       confirm = f1ap_ue_context_modification_confirm{}](coro_context<async_task<void>>& ctx) mutable {
        CORO_BEGIN(ctx);

        req = prepare_bsr_actuation_request(ue_index, recommended_periodic_bsr_timer_sf, ue_mng);
        if (not req.has_value()) {
          CORO_EARLY_RETURN();
        }

        CORO_AWAIT_VALUE(confirm, du_params.f1ap.ue_mng.handle_ue_context_modification_required(req.value()));

        if (confirm.success) {
          ocudulog::fetch_basic_logger("DU-MNG").debug("ue={}: BSR periodicity actuation accepted by gNB-CU.",
                                                       fmt::underlying(ue_index));
        } else {
          ocudulog::fetch_basic_logger("DU-MNG").info("ue={}: BSR periodicity actuation refused by gNB-CU.",
                                                      fmt::underlying(ue_index));
        }

        CORO_RETURN();
      });
}

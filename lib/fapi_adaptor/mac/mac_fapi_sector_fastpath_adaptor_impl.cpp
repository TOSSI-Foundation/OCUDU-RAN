// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "mac_fapi_sector_fastpath_adaptor_impl.h"
#include "ocudu/mac/phy_cell_operation_controller.h"
#include "ocudu/support/ocudu_assert.h"

using namespace ocudu;
using namespace fapi_adaptor;

mac_fapi_sector_fastpath_adaptor_impl::mac_fapi_sector_fastpath_adaptor_impl(
    std::unique_ptr<mac_fapi_p5_sector_fastpath_adaptor> p5_adaptor_,
    std::unique_ptr<mac_fapi_p7_sector_fastpath_adaptor> p7_adaptor_) :
  p5_adaptor(std::move(p5_adaptor_)), p7_adaptor(std::move(p7_adaptor_)), p5_task_sched(P5_TASK_QUEUE_SIZE)
{
  ocudu_assert(p5_adaptor, "Invalid MAC-FAPI P5 sector adaptor");
  ocudu_assert(p7_adaptor, "Invalid MAC-FAPI P7 sector adaptor");
}

mac_fapi_p5_sector_fastpath_adaptor& mac_fapi_sector_fastpath_adaptor_impl::get_p5_sector_fastpath_adaptor()
{
  return *p5_adaptor;
}

mac_fapi_p7_sector_fastpath_adaptor& mac_fapi_sector_fastpath_adaptor_impl::get_p7_sector_adaptor()
{
  return *p7_adaptor;
}

void mac_fapi_sector_fastpath_adaptor_impl::start()
{
  p7_adaptor->get_operation_controller().start();

  auto& p5_ctrl = p5_adaptor->get_operation_controller();
  p5_task_sched.schedule([&p5_ctrl](coro_context<async_task<void>>& ctx) {
    CORO_BEGIN(ctx);
    CORO_AWAIT(p5_ctrl.start());
    CORO_RETURN();
  });
}

void mac_fapi_sector_fastpath_adaptor_impl::stop()
{
  auto& p5_ctrl = p5_adaptor->get_operation_controller();
  p5_task_sched.schedule([&p5_ctrl](coro_context<async_task<void>>& ctx) {
    CORO_BEGIN(ctx);
    CORO_AWAIT(p5_ctrl.stop());
    CORO_RETURN();
  });

  p7_adaptor->get_operation_controller().stop();
}

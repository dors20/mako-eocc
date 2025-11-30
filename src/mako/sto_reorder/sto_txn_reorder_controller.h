#pragma once

#include "occ_reorder/txn_reorder_controller.h"
#include "occ_reorder/parallel_graph_backend.h"
#include "sto_txn_batch_metadata.h"

namespace mako {
namespace sto {

using StoTxnReorderController =
    occ::GenericTxnReorderController<Transaction,
                                     StoTxnBatchBuilder,
                                     occ::ParallelGraphBackend>;

}  // namespace sto
}  // namespace mako



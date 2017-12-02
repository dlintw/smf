// Copyright (c) 2016 Alexander Gallego. All rights reserved.
//
#include "chain_replication/chain_replication_service.h"

#include <flatbuffers/minireflect.h>
#include <utility>

#include "filesystem/wal_lcore_map.h"
#include "filesystem/wal_requests.h"
#include "hashing/hashing_utils.h"
#include "rpc/futurize_utils.h"
#include "seastar_io/priority_manager.h"

namespace smf {
namespace chains {
using namespace smf;       // NOLINT
using namespace smf::wal;  // NOLINT

std::unordered_map<uint32_t, smf::wal_write_request> core_map(
  const tx_put_request *p) {
  std::unordered_map<uint32_t, smf::wal_write_request> retval;

  if (SMF_UNLIKELY(p->data() == nullptr || p->data()->Length() == 0)) {
    LOG_ERROR_IF(p->data() == nullptr, "null pointer to transactions");
    LOG_ERROR_IF(p->data()->Length() == 0, "There are no partitions available");
    return retval;
  }

  std::for_each(
    p->data()->begin(), p->data()->end(),
    [p, &retval](const smf::wal::tx_put_partition_pair *it) mutable {
      // TODO(agallego) - validate
      // f->op         = smf::wal::tx_put_operation::tx_put_operation_full;
      // f->epoch_ms   = lowres_time_now_millis();
      // f->type       =
      // f->invalidation = nullptr;
      auto core = smf::put_to_lcore(p->topic()->c_str(), it);
      if (retval.find(core) == retval.end()) {
        smf::wal_write_request req(
          p,
          smf::priority_manager::thread_local_instance()
            .streaming_write_priority(),
          {it->partition()});
        retval.insert({core, std::move(req)});
      } else {
        retval.at(core).partition_view.insert(it->partition());
      }
    });
  return retval;
}

seastar::future<rpc_typed_envelope<chain_put_reply>>
chain_replication_service::put(
  rpc_recv_typed_context<chain_put_request> &&record) {
  if (!record) { return futurize_status_for_type<chain_put_reply>(400); }

  // * Create a core to put map for all partition pairs
  // * Send to all cores and reduce
  return seastar::do_with(std::move(record), [this](auto &r) {
    auto cmap = core_map(r->put());
    return seastar::map_reduce(
             // for-all
             cmap.begin(), cmap.end(),

             // Mapper function
             [this](auto it) {
               return seastar::smp::submit_to(it.first, [this, r = it.second] {
                 return wal_->local().append(r);
               });
             },

             // Initial State
             tx_put_replyT{},

             // Reducer function
             [this](auto acc, auto next) {
               // TODO(agallego)
               // missing partition information
               acc.start_offset =
                 std::min(acc.start_offset, next.data.start_offset);
               acc.end_offset = std::max(acc.end_offset, next.data.end_offset);
               return acc;
             })
      .then([](auto reduced) {
        rpc_typed_envelope<chain_put_reply> data;
        data.data->put               = std::make_unique<tx_put_replyT>();
        data.data->put->start_offset = reduced.start_offset;
        data.data->put->end_offset   = reduced.end_offset;
        data.envelope.set_status(200);
        return seastar::make_ready_future<rpc_typed_envelope<chain_put_reply>>(
          std::move(data));
      })
      .handle_exception([](auto eptr) {
        LOG_ERROR("Error saving chains::put(): {}", eptr);
        return futurize_status_for_type<chain_put_reply>(501);
      });
  });
}


seastar::future<rpc_typed_envelope<chain_get_reply>>
chain_replication_service::get(
  rpc_recv_typed_context<chain_get_request> &&record) {
  if (!record) { return futurize_status_for_type<chain_get_reply>(400); }
  return futurize_status_for_type<chain_get_reply>(500);

  /*


  auto core_to_handle = get_to_lcore(record.get());
  return seastar::smp::submit_to(
           core_to_handle, [this, r = std::move(record)]() mutable {
             // TODO(agallego) - incorporate topics
             // across the wal's and here as well
             auto &read_priority = priority_manager::thread_local_instance()
                                     .streaming_read_priority();
             wal_read_request read(r->offset(), r->max_bytes(), 0,
                                   read_priority);
             return wal_->local()
               .get(std::move(read))
               .then([old_offset = r->offset()](auto maybe_read) {
                 rpc_typed_envelope<tx_get_reply> reply;
                 reply.envelope.set_status(200);

                 if (!maybe_read || maybe_read.empty()) {
                   reply.data->set_next_offset(old_offset);
                 } else {
                   // update offset to max & set fragments here
                   auto read_data = std::move(maybe_read.value());
                   reply.data->set_next_offset(old_offset + read_data.size());

                   for (auto it  = read_data.fragments.begin(),
                             end = read_data.fragments.end();
                        it != end; ++it) {
                     const auto flags = it->hdr.wal_entry_flags();
                     if (flags & wal::wal_entry_flags::
                                   wal_entry_flags_partial_fagment) {}


                     // THE PLAN
                     // 1) reconstruct the original payload
                     // 2) inflate the zstd buffer
                     // 3) return the txns - already formatted correctly on
                     // input :)
                     // 4) ensure that we do this PER full tx_op otherwise we
                     // have to read
                     //    more data until we hit this limit no?
                     // return the max bytes... and go home.


                     if (flags & wal::wal_entry_flags::
                                   wal_entry_flags_full_fagment) {}
                     // convert wal::fragment -> chain::fragment
                     // TODO(agallego) - make this a util and add a unit tests
                     //
                     auto frag = std::make_unique<chains::tx_fragmentT>();
                     frag->op  = chains::tx_operation::tx_operation_full;
                     frag->id  = static_cast<uint32_t>(random().next());
                     frag->time_micros = time_now_micros();

                     frag->key.reserve(key.size());
                     frag->value.reserve(value.size());
                   }
                 }
                 return seastar::make_ready_future<decltype(reply)>(
                   std::move(reply));
               });
           })
    .handle_exception([](auto eptr) {
      LOG_ERROR("Error getting data chains::get(): {}", eptr);
      return futurize_status_for_type<tx_get_reply>(501);
    });




  */
}
}  // end namespace chains
}  // end namespace smf

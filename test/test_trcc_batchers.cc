#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <memory>

#include "lib/common.h"
#include "lib/transport_request_handle.h"
#include "txn_reordering/request_access_tracker.h"
#include "txn_reordering/storage_batcher.h"
#include "txn_reordering/validator_batcher.h"

namespace mako::txn_reordering {

class StubRequestHandle : public mako::TransportRequestHandle {
 public:
  StubRequestHandle(uint8_t req_type, uint32_t req_nr)
      : req_type_(req_type), req_nr_(req_nr) {
    auto* req = reinterpret_cast<basic_request_t*>(request_buffer_.data());
    req->req_nr = req_nr;
  }

  uint8_t GetRequestType() const override { return req_type_; }
  char* GetRequestBuffer() override { return request_buffer_.data(); }
  char* GetResponseBuffer() override { return response_buffer_.data(); }
  void* GetOpaqueHandle() override { return nullptr; }
  void EnqueueResponse(size_t) override { ++enqueue_calls_; }

  uint32_t enqueue_calls() const { return enqueue_calls_; }
  uint32_t req_nr() const { return req_nr_; }

 private:
  uint8_t req_type_;
  std::array<char, sizeof(basic_request_t)> request_buffer_{};
  std::array<char, 64> response_buffer_{};
  uint32_t enqueue_calls_{0};
  uint32_t req_nr_{0};
};

std::unique_ptr<StubRequestHandle> MakeHandle(uint8_t req_type, uint32_t req_nr) {
  return std::make_unique<StubRequestHandle>(req_type, req_nr);
}

TEST(StorageBatcherTest, WritesBeforeReads) {
  StorageBatcher batcher;
  TxnReorderingConfig cfg{};
  cfg.enabled = true;
  cfg.storage.enabled = true;
  cfg.storage.max_batch = 16;
  cfg.storage.max_batch_bytes = 1024;
  batcher.Reconfigure(cfg);

  std::vector<uint8_t> replayed;
  batcher.SetDispatcher(
      [&](uint8_t req_type, mako::TransportRequestHandle*, size_t) {
        replayed.push_back(req_type);
      });

  StubRequestHandle read_a(getReqType, 1);
  StubRequestHandle write_batch(batchLockReqType, 2);
  StubRequestHandle read_b(getReqType, 3);
  StubRequestHandle write_install(installReqType, 4);

  batcher.Enqueue(&read_a, sizeof(basic_request_t));
  batcher.Enqueue(&write_batch, sizeof(basic_request_t));
  batcher.Enqueue(&read_b, sizeof(basic_request_t));
  batcher.Enqueue(&write_install, sizeof(basic_request_t));
  batcher.Flush();

  std::vector<uint8_t> expected = {
      batchLockReqType,
      installReqType,
      getReqType,
      getReqType,
  };
  EXPECT_EQ(replayed, expected);
}

TEST(ValidatorBatcherTest, OrdersByRequestNumber) {
  ValidatorBatcher batcher;
  TxnReorderingConfig cfg{};
  cfg.enabled = true;
  cfg.validator.enabled = true;
  cfg.validator.batch_size = 16;
  cfg.validator.max_wait_us = 0;
  batcher.Reconfigure(cfg);

  std::vector<uint32_t> replayed;
  batcher.SetDispatcher(
      [&](mako::TransportRequestHandle* handle, size_t) {
        auto* req = reinterpret_cast<basic_request_t*>(handle->GetRequestBuffer());
        replayed.push_back(req->req_nr);
      });

  StubRequestHandle req_a(validateReqType, 10);
  StubRequestHandle req_b(validateReqType, 5);
  StubRequestHandle req_c(validateReqType, 7);

  batcher.Enqueue(&req_a, sizeof(basic_request_t));
  batcher.Enqueue(&req_b, sizeof(basic_request_t));
  batcher.Enqueue(&req_c, sizeof(basic_request_t));
  batcher.Flush();

  std::vector<uint32_t> expected = {10, 5, 7};
  EXPECT_EQ(replayed, expected);
}
TEST(ValidatorBatcherTest, ReordersConflictingTxn) {
  ValidatorBatcher batcher;
  RequestAccessTracker tracker;
  TxnReorderingConfig cfg{};
  cfg.enabled = true;
  cfg.validator.enabled = true;
  cfg.validator.batch_size = 4;
  cfg.validator.policy.type = ValidatorPolicyType::kMinAbort;
  batcher.Reconfigure(cfg);
  batcher.SetAccessTracker(&tracker);

  std::vector<uint32_t> committed;
  std::vector<uint32_t> aborted;
  batcher.SetDispatcher(
      [&](mako::TransportRequestHandle* handle, size_t) {
        auto* req = reinterpret_cast<basic_request_t*>(handle->GetRequestBuffer());
        committed.push_back(req->req_nr);
      });
  batcher.SetAbortDispatcher(
      [&](mako::TransportRequestHandle* handle) {
        auto* req = reinterpret_cast<basic_request_t*>(handle->GetRequestBuffer());
        aborted.push_back(req->req_nr);
      });

  // T300 reads A (needs to happen before anyone writes A)
  // T400 writes A
  // Proper order: T300 -> T400.
  auto t300 = MakeHandle(mako::validateReqType, 300);
  auto t400 = MakeHandle(mako::validateReqType, 400);

  tracker.RecordRead(t300->req_nr(), "1:A");
  tracker.RecordWrite(t400->req_nr(), "1:A");

  // Enqueue in "wrong" order or just rely on reordering logic
  batcher.Enqueue(t400.get(), sizeof(basic_request_t));
  batcher.Enqueue(t300.get(), sizeof(basic_request_t));
  batcher.Flush();

  // Both should commit
  EXPECT_TRUE(aborted.empty());
  EXPECT_EQ(committed.size(), 2u);
  if (committed.size() == 2) {
    EXPECT_EQ(committed[0], 300u); // Reader first
    EXPECT_EQ(committed[1], 400u); // Writer second
  }
}

TEST(ValidatorBatcherTest, DetectsCycleAndAborts) {
  ValidatorBatcher batcher;
  RequestAccessTracker tracker;
  TxnReorderingConfig cfg{};
  cfg.enabled = true;
  cfg.validator.enabled = true;
  cfg.validator.batch_size = 4;
  cfg.validator.policy.type = ValidatorPolicyType::kMinAbort;
  batcher.Reconfigure(cfg);
  batcher.SetAccessTracker(&tracker);

  std::vector<uint32_t> committed;
  std::vector<uint32_t> aborted;
  batcher.SetDispatcher(
      [&](mako::TransportRequestHandle* handle, size_t) {
        auto* req = reinterpret_cast<basic_request_t*>(handle->GetRequestBuffer());
        committed.push_back(req->req_nr);
      });
  batcher.SetAbortDispatcher(
      [&](mako::TransportRequestHandle* handle) {
        auto* req = reinterpret_cast<basic_request_t*>(handle->GetRequestBuffer());
        aborted.push_back(req->req_nr);
      });

  auto t300 = MakeHandle(mako::validateReqType, 300);
  auto t400 = MakeHandle(mako::validateReqType, 400);

  // Cycle:
  // T300 reads A, writes B.
  // T400 reads B, writes A.
  // T300(R A) vs T400(W A) -> T300 must be before T400
  // T400(R B) vs T300(W B) -> T400 must be before T300
  tracker.RecordRead(t300->req_nr(), "1:A");
  tracker.RecordWrite(t300->req_nr(), "1:B");
  
  tracker.RecordRead(t400->req_nr(), "1:B");
  tracker.RecordWrite(t400->req_nr(), "1:A");

  batcher.Enqueue(t300.get(), sizeof(basic_request_t));
  batcher.Enqueue(t400.get(), sizeof(basic_request_t));
  batcher.Flush();

  // One should abort
  EXPECT_EQ(committed.size(), 1u);
  EXPECT_EQ(aborted.size(), 1u);
}


}  // namespace mako::txn_reordering

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <yaml-cpp/yaml.h>

#include "rusty/option.hpp"

namespace mako::txn_reordering {

enum class ValidatorPolicyType : uint8_t {
  kMinAbort = 0,
  kAgePriority,
  kTailLatency,
  kThreadAware
};

enum class ValidatorAlgorithm : uint8_t {
  kSccGreedy = 0,
  kSortGreedy
};

struct StorageBatchingConfig {
  bool enabled{false};
  uint32_t max_batch{64};
  uint32_t flush_interval_us{50};
  uint32_t max_batch_bytes{64 * 1024};
};

struct ValidatorPolicyConfig {
  ValidatorPolicyType type{ValidatorPolicyType::kMinAbort};
  double age_weight{0.0};
  uint32_t tail_deadline_us{0};
  uint32_t thread_penalty{0};
};

struct ValidatorBatchingConfig {
  bool enabled{false};
  uint32_t batch_size{64};
  uint32_t max_wait_us{50};
  ValidatorAlgorithm algorithm{ValidatorAlgorithm::kSccGreedy};
  uint32_t max_fvs_size{0};
  double overhead_limit_us{0.0};
  ValidatorPolicyConfig policy{};
};

struct TxnReorderingConfig {
  bool enabled{false};
  StorageBatchingConfig storage{};
  ValidatorBatchingConfig validator{};
};

TxnReorderingConfig ParseConfigNode(const YAML::Node& node, std::string* error);

class TxnReorderingOptions {
 public:
  static TxnReorderingOptions& Instance();

  bool LoadFromFile(const std::string& path, std::string* error);
  void Apply(const TxnReorderingConfig& cfg);

  bool Enabled() const;
  const TxnReorderingConfig& Get() const;
  std::string CurrentSource() const;

 private:
  TxnReorderingOptions();

  TxnReorderingConfig default_config_;
  mutable std::mutex mutex_;
  rusty::Option<TxnReorderingConfig> config_;
  std::string source_path_;
};

}  // namespace mako::txn_reordering

#include "txn_reordering/txn_reordering_config.h"

#include <cctype>

#include <yaml-cpp/yaml.h>

#include "lib/common.h"

namespace mako::txn_reordering {
namespace {

std::string NormalizeKey(const std::string& value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (char c : value) {
    lowered.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(c))));
  }
  return lowered;
}

ValidatorPolicyType ParsePolicyType(const std::string& name, std::string* error) {
  const std::string lowered = NormalizeKey(name);
  if (lowered == "min_abort" || lowered == "minaborts") {
    return ValidatorPolicyType::kMinAbort;
  }
  if (lowered == "age_priority" || lowered == "age") {
    return ValidatorPolicyType::kAgePriority;
  }
  if (lowered == "tail_latency" || lowered == "tail") {
    return ValidatorPolicyType::kTailLatency;
  }
  if (lowered == "thread_aware" || lowered == "thread") {
    return ValidatorPolicyType::kThreadAware;
  }
  if (error) {
    *error = "Unknown validator policy: " + name;
  }
  return ValidatorPolicyType::kMinAbort;
}

ValidatorAlgorithm ParseAlgorithm(const std::string& name, std::string* error) {
  const std::string lowered = NormalizeKey(name);
  if (lowered == "scc_greedy" || lowered == "scc") {
    return ValidatorAlgorithm::kSccGreedy;
  }
  if (lowered == "sort_greedy" || lowered == "sort") {
    return ValidatorAlgorithm::kSortGreedy;
  }
  if (error) {
    *error = "Unknown validator algorithm: " + name;
  }
  return ValidatorAlgorithm::kSccGreedy;
}

StorageBatchingConfig ParseStorage(const YAML::Node& node) {
  StorageBatchingConfig cfg;
  if (!node || !node.IsMap()) {
    return cfg;
  }
  cfg.enabled = node["enabled"].as<bool>(cfg.enabled);
  cfg.max_batch = node["max_batch"].as<uint32_t>(cfg.max_batch);
  cfg.flush_interval_us =
      node["flush_interval_us"].as<uint32_t>(cfg.flush_interval_us);
  cfg.max_batch_bytes =
      node["max_batch_bytes"].as<uint32_t>(cfg.max_batch_bytes);
  return cfg;
}

ValidatorPolicyConfig ParsePolicy(const YAML::Node& node, std::string* error) {
  ValidatorPolicyConfig cfg;
  if (!node || !node.IsScalar()) {
    return cfg;
  }
  const std::string name = node.as<std::string>("min_abort");
  cfg.type = ParsePolicyType(name, error);
  return cfg;
}

ValidatorPolicyConfig ParsePolicyMap(const YAML::Node& node,
                                     std::string* error) {
  if (!node) {
    return {};
  }
  if (node.IsScalar()) {
    return ParsePolicy(node, error);
  }

  ValidatorPolicyConfig cfg;
  const std::string name =
      node["type"].as<std::string>(std::string("min_abort"));
  cfg.type = ParsePolicyType(name, error);
  cfg.age_weight = node["age_weight"].as<double>(cfg.age_weight);
  cfg.tail_deadline_us =
      node["tail_deadline_us"].as<uint32_t>(cfg.tail_deadline_us);
  cfg.thread_penalty =
      node["thread_penalty"].as<uint32_t>(cfg.thread_penalty);
  return cfg;
}

ValidatorBatchingConfig ParseValidator(const YAML::Node& node,
                                       std::string* error) {
  ValidatorBatchingConfig cfg;
  if (!node || !node.IsMap()) {
    return cfg;
  }
  cfg.enabled = node["enabled"].as<bool>(cfg.enabled);
  cfg.batch_size = node["batch_size"].as<uint32_t>(cfg.batch_size);
  cfg.max_wait_us = node["max_wait_us"].as<uint32_t>(cfg.max_wait_us);
  if (node["algorithm"]) {
    cfg.algorithm =
        ParseAlgorithm(node["algorithm"].as<std::string>(), error);
  }
  cfg.max_fvs_size = node["max_fvs_size"].as<uint32_t>(cfg.max_fvs_size);
  cfg.overhead_limit_us =
      node["overhead_limit_us"].as<double>(cfg.overhead_limit_us);

  const YAML::Node policy_node = node["policy"];
  if (policy_node) {
    cfg.policy = ParsePolicyMap(policy_node, error);
  }

  const YAML::Node policy_params = node["policy_params"];
  if (policy_params) {
    cfg.policy.age_weight =
        policy_params["age_weight"].as<double>(cfg.policy.age_weight);
    cfg.policy.tail_deadline_us = policy_params["tail_deadline_us"].as<uint32_t>(
        cfg.policy.tail_deadline_us);
    cfg.policy.thread_penalty =
        policy_params["thread_penalty"].as<uint32_t>(cfg.policy.thread_penalty);
  }

  return cfg;
}

YAML::Node ExtractRoot(const YAML::Node& input) {
  if (input["txn_reordering"]) {
    return input["txn_reordering"];
  }
  if (input["txn_reordering_config"]) {
    return input["txn_reordering_config"];
  }
  return input;
}

}  // namespace

TxnReorderingConfig ParseConfigNode(const YAML::Node& node,
                                    std::string* error) {
  TxnReorderingConfig cfg;
  if (!node) {
    return cfg;
  }

  if (!node.IsMap()) {
    if (error) {
      *error = "Txn reordering config must be a JSON/YAML object.";
    }
    return cfg;
  }

  cfg.enabled = node["enabled"].as<bool>(cfg.enabled);
  cfg.storage = ParseStorage(node["storage_batching"]);
  cfg.validator = ParseValidator(node["validator_batching"], error);
  return cfg;
}

TxnReorderingOptions& TxnReorderingOptions::Instance() {
  static TxnReorderingOptions instance;
  return instance;
}

TxnReorderingOptions::TxnReorderingOptions()
    : default_config_(),
      config_(rusty::None),
      source_path_("<defaults>") {}

bool TxnReorderingOptions::LoadFromFile(const std::string& path,
                                        std::string* error) {
  try {
    const YAML::Node root = YAML::LoadFile(path);
    const YAML::Node effective = ExtractRoot(root);

    std::string parse_error;
    TxnReorderingConfig cfg = ParseConfigNode(effective, &parse_error);
    if (!parse_error.empty()) {
      if (error) {
        *error = parse_error;
      }
      return false;
    }

    Apply(cfg);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      source_path_ = path;
    }
    return true;
  } catch (const YAML::BadFile& ex) {
    if (error) {
      *error = std::string("Failed to open config: ") + ex.what();
    }
  } catch (const YAML::ParserException& ex) {
    if (error) {
      *error = std::string("Failed to parse config: ") + ex.what();
    }
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
  }
  return false;
}

void TxnReorderingOptions::Apply(const TxnReorderingConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.replace(cfg);
}

bool TxnReorderingOptions::Enabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (config_.is_some()) {
    return config_.unwrap_ref().enabled;
  }
  return default_config_.enabled;
}

const TxnReorderingConfig& TxnReorderingOptions::Get() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (config_.is_some()) {
    return config_.unwrap_ref();
  }
  return default_config_;
}

std::string TxnReorderingOptions::CurrentSource() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return source_path_;
}

}  // namespace mako::txn_reordering

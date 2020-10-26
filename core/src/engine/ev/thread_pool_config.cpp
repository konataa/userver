#include "thread_pool_config.hpp"

namespace engine::ev {

ThreadPoolConfig Parse(const yaml_config::YamlConfig& value,
                       formats::parse::To<ThreadPoolConfig>) {
  ThreadPoolConfig config;
  config.threads = value["threads"].As<size_t>(config.threads);
  config.thread_name = value["thread_name"].As<std::string>(config.thread_name);
  return config;
}

}  // namespace engine::ev

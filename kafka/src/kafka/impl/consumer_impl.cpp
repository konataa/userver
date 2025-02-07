#include <kafka/impl/consumer_impl.hpp>

#include <chrono>

#include <fmt/format.h>

#include <userver/logging/log.hpp>
#include <userver/testsuite/testpoint.hpp>
#include <userver/tracing/span.hpp>
#include <userver/utils/span.hpp>

#include <kafka/impl/configuration.hpp>
#include <kafka/impl/error_buffer.hpp>
#include <kafka/impl/stats.hpp>

USERVER_NAMESPACE_BEGIN

namespace kafka {

namespace {

std::optional<std::chrono::milliseconds> RetrieveTimestamp(
    const impl::MessageHolder& message) {
  rd_kafka_timestamp_type_t type = RD_KAFKA_TIMESTAMP_NOT_AVAILABLE;
  const std::int64_t timestamp =
      rd_kafka_message_timestamp(message.GetHandle(), &type);
  if (timestamp == -1 || type == RD_KAFKA_TIMESTAMP_NOT_AVAILABLE) {
    return std::nullopt;
  }

  return std::chrono::milliseconds{timestamp};
}

}  // namespace

struct Message::Data final {
  Data(impl::MessageHolder message_holder)
      : message(std::move(message_holder)),
        topic(rd_kafka_topic_name(message->rkt)),
        timestamp(RetrieveTimestamp(message)) {}

  Data(Data&& other) = default;

  impl::MessageHolder message;

  const std::string topic;
  std::optional<std::chrono::milliseconds> timestamp;
};

Message::~Message() = default;

const std::string& Message::GetTopic() const { return data_->topic; }

std::string_view Message::GetKey() const {
  const auto* key = data_->message->key;

  if (!key) {
    return std::string_view{};
  }

  return std::string_view{static_cast<const char*>(key),
                          data_->message->key_len};
}

std::string_view Message::GetPayload() const {
  return std::string_view{static_cast<const char*>(data_->message->payload),
                          data_->message->len};
}

std::optional<std::chrono::milliseconds> Message::GetTimestamp() const {
  return data_->timestamp;
}

int Message::GetPartition() const { return data_->message->partition; }

std::int64_t Message::GetOffset() const { return data_->message->offset; }

Message::Message(Message::DataStorage data) : data_(std::move(data)) {}

namespace impl {

namespace {

void ErrorCallbackProxy(rd_kafka_t* consumer, int error_code,
                        const char* reason, void* opaque_ptr) {
  UASSERT(consumer);
  UASSERT(opaque_ptr);

  static_cast<ConsumerImpl*>(opaque_ptr)->ErrorCallback(error_code, reason);
}

void RebalanceCallbackProxy(rd_kafka_t* consumer, rd_kafka_resp_err_t err,
                            rd_kafka_topic_partition_list_t* partitions,
                            void* opaque_ptr) {
  UASSERT(consumer);
  UASSERT(opaque_ptr);

  static_cast<ConsumerImpl*>(opaque_ptr)->RebalanceCallback(err, partitions);
}

void OffsetCommitCallback(rd_kafka_t* consumer, rd_kafka_resp_err_t err,
                          rd_kafka_topic_partition_list_t* committed_offsets,
                          void* opaque_ptr) {
  UASSERT(consumer);
  UASSERT(opaque_ptr);

  static_cast<ConsumerImpl*>(opaque_ptr)
      ->OffsetCommitCallbackProxy(err, committed_offsets);
}

void PrintTopicPartitionsList(
    const rd_kafka_topic_partition_list_t* list,
    std::function<std::string(const rd_kafka_topic_partition_t&)> log,
    bool skip_invalid_offsets = false) {
  if (list == nullptr || list->cnt <= 0) {
    return;
  }

  utils::span<const rd_kafka_topic_partition_t> topic_partitions{
      list->elems, list->elems + static_cast<std::size_t>(list->cnt)};
  for (const auto& topic_partition : topic_partitions) {
    if (skip_invalid_offsets &&
        topic_partition.offset == RD_KAFKA_OFFSET_INVALID) {
      /// @note `librdkafka` does not sets offsets for partitions that were
      /// not committed in the current in the current commit
      LOG_INFO() << "Skipping partition " << topic_partition.partition;
      continue;
    }

    LOG_INFO() << log(topic_partition);
  }
}

void CallTestpoints(const rd_kafka_topic_partition_list_t* list,
                    const std::string& testpoint_name) {
  if (list == nullptr || list->cnt == 0 ||
      !testsuite::AreTestpointsAvailable()) {
    return;
  }

  for (int i = 0; i < list->cnt; ++i) {
    TESTPOINT(testpoint_name, {});
  }
}

}  // namespace

void ConsumerImpl::AssignPartitions(
    const rd_kafka_topic_partition_list_t* partitions) {
  LOG_INFO() << "Assigning new partitions to consumer";
  PrintTopicPartitionsList(
      partitions, [](const rd_kafka_topic_partition_t& partition) {
        return fmt::format("Partition {} for topic '{}' assigning",
                           partition.partition, partition.topic);
      });

  const auto assign_err = rd_kafka_assign(consumer_->GetHandle(), partitions);
  if (assign_err != RD_KAFKA_RESP_ERR_NO_ERROR) {
    LOG_ERROR() << fmt::format("Failed to assign partitions: {}",
                               rd_kafka_err2str(assign_err));
    return;
  }

  LOG_INFO() << "Successfully assigned partitions";
}

void ConsumerImpl::RevokePartitions(
    const rd_kafka_topic_partition_list_t* partitions) {
  LOG_INFO() << "Revoking existing partitions from consumer";

  PrintTopicPartitionsList(
      partitions, [](const rd_kafka_topic_partition_t& partition) {
        return fmt::format("Partition {} of '{}' topic revoking",
                           partition.partition, partition.topic);
      });

  const auto revokation_err = rd_kafka_assign(consumer_->GetHandle(), nullptr);
  if (revokation_err != RD_KAFKA_RESP_ERR_NO_ERROR) {
    LOG_ERROR() << fmt::format("Failed to revoke partitions: {}",
                               rd_kafka_err2str(revokation_err));
    return;
  }

  LOG_INFO() << "Successfully revoked partitions";
}

void ConsumerImpl::ErrorCallback(int error_code, const char* reason) {
  tracing::Span span{"error_callback"};
  span.AddTag("kafka_callback", "error_callback");

  LOG_ERROR() << fmt::format(
      "Error {} occurred because of '{}': {}", error_code, reason,
      rd_kafka_err2str(static_cast<rd_kafka_resp_err_t>(error_code)));

  if (error_code == RD_KAFKA_RESP_ERR__RESOLVE ||
      error_code == RD_KAFKA_RESP_ERR__TRANSPORT ||
      error_code == RD_KAFKA_RESP_ERR__AUTHENTICATION ||
      error_code == RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN) {
    ++stats_.connections_error;
  }
}

void ConsumerImpl::RebalanceCallback(
    rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t* partitions) {
  tracing::Span span{"rebalance_callback"};
  span.AddTag("kafka_callback", "rebalance_callback");

  LOG_INFO() << fmt::format(
      "Consumer group rebalanced ('{}' protocol)",
      rd_kafka_rebalance_protocol(consumer_->GetHandle()));

  switch (err) {
    case RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS:
      AssignPartitions(partitions);
      CallTestpoints(partitions,
                     fmt::format("tp_{}_subscribed", component_name_));
      break;
    case RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS:
      RevokePartitions(partitions);
      CallTestpoints(partitions, fmt::format("tp_{}_revoked", component_name_));
      break;
    default:
      LOG_ERROR() << fmt::format("Failed when rebalancing: {}",
                                 rd_kafka_err2str(err));
      break;
  }
}

void ConsumerImpl::OffsetCommitCallbackProxy(
    rd_kafka_resp_err_t err,
    rd_kafka_topic_partition_list_t* committed_offsets) {
  tracing::Span span{"offset_commit_callback"};
  span.AddTag("kafka_callback", "offset_commit_callback");

  if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
    LOG_ERROR() << fmt::format("Failed to commit offsets: {}",
                               rd_kafka_err2str(err));
    return;
  }

  LOG_INFO() << "Successfully committed offsets";
  PrintTopicPartitionsList(
      committed_offsets,
      [](const rd_kafka_topic_partition_t& offset) {
        return fmt::format(
            "Offset {} committed for topic '{}' within partition "
            "{}",
            offset.offset, offset.topic, offset.partition);
      },
      /*skip_invalid_offsets=*/true);
}

ConsumerImpl::ConsumerImpl(Configuration&& configuration)
    : component_name_(configuration.GetName()),
      conf_([this, configuration = std::move(configuration)]() mutable {
        ConfHolder conf = std::move(configuration).Release();
        rd_kafka_conf_set_opaque(conf.GetHandle(), this);
        rd_kafka_conf_set_error_cb(conf.GetHandle(), &ErrorCallbackProxy);
        rd_kafka_conf_set_rebalance_cb(conf.GetHandle(),
                                       &RebalanceCallbackProxy);
        rd_kafka_conf_set_offset_commit_cb(conf.GetHandle(),
                                           &OffsetCommitCallback);

        return conf;
      }()) {}

ConsumerImpl::~ConsumerImpl() = default;

void ConsumerImpl::Subscribe(const std::vector<std::string>& topics) {
  consumer_.emplace(conf_, RD_KAFKA_CONSUMER);
  /// @note makes available to call `rd_kafka_consumer_poll`
  rd_kafka_poll_set_consumer(consumer_->GetHandle());

  TopicPartitionsListHolder topic_partitions_list{
      rd_kafka_topic_partition_list_new(topics.size())};
  for (const auto& topic : topics) {
    rd_kafka_topic_partition_list_add(topic_partitions_list.GetHandle(),
                                      topic.c_str(), RD_KAFKA_PARTITION_UA);
  }

  LOG_INFO() << fmt::format("Consumer is subscribing to topics: [{}]",
                            fmt::join(topics, ", "));

  rd_kafka_subscribe(consumer_->GetHandle(), topic_partitions_list.GetHandle());
}

void ConsumerImpl::LeaveGroup() {
  const rd_kafka_resp_err_t err =
      rd_kafka_consumer_close(consumer_->GetHandle());
  if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
    LOG_ERROR() << fmt::format("Failed to properly close consumer: {}",
                               rd_kafka_err2str(err));
  }
  consumer_.reset();
}

void ConsumerImpl::Resubscribe(const std::vector<std::string>& topics) {
  LeaveGroup();
  LOG_INFO() << "Leaved group";
  Subscribe(topics);
  LOG_INFO() << "Joined group";
}

void ConsumerImpl::Commit() {
  rd_kafka_commit(consumer_->GetHandle(), nullptr, /*async=*/0);
}

void ConsumerImpl::AsyncCommit() {
  rd_kafka_commit(consumer_->GetHandle(), nullptr, /*async=*/1);
}

std::optional<Message> ConsumerImpl::PollMessage(engine::Deadline deadline) {
  if (deadline.IsReached()) {
    return std::nullopt;
  }

  const auto poll_timeout{std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline.TimeLeft())};

  LOG_DEBUG() << fmt::format("Polling message for {}ms", poll_timeout.count());

  MessageHolder message{rd_kafka_consumer_poll(
      consumer_->GetHandle(), static_cast<int>(poll_timeout.count()))};

  if (!message) {
    return std::nullopt;
  }
  if (message->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
    LOG_WARNING() << fmt::format("Consumed message with error: {}",
                                 rd_kafka_err2str(message->err));
    return std::nullopt;
  }

  Message polled_message{Message::DataStorage{std::move(message)}};

  AccountPolledMessageStat(polled_message);

  LOG_INFO() << fmt::format(
      "Message from kafka topic '{}' received by key '{}' with "
      "partition {} by offset {}",
      polled_message.GetTopic(), polled_message.GetKey(),
      polled_message.GetPartition(), polled_message.GetOffset());

  return polled_message;
}

ConsumerImpl::MessageBatch ConsumerImpl::PollBatch(std::size_t max_batch_size,
                                                   engine::Deadline deadline) {
  MessageBatch batch;

  while (batch.size() < max_batch_size) {
    auto message = PollMessage(deadline);
    if (!message.has_value()) {
      break;
    }
    batch.push_back(std::move(*message));
  }

  if (!batch.empty()) {
    LOG_INFO() << fmt::format("Polled batch of {} messages", batch.size());
  }

  return batch;
}

const Stats& ConsumerImpl::GetStats() const { return stats_; }

std::shared_ptr<TopicStats> ConsumerImpl::GetTopicStats(
    const std::string& topic) {
  return stats_.topics_stats[topic];
}

void ConsumerImpl::AccountPolledMessageStat(const Message& polled_message) {
  auto topic_stats = GetTopicStats(polled_message.GetTopic());
  ++topic_stats->messages_counts.messages_total;

  const auto message_timestamp = polled_message.GetTimestamp();
  if (message_timestamp) {
    const auto take_time = std::chrono::system_clock::now().time_since_epoch();
    const auto ms_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            take_time - message_timestamp.value())
            .count();
    topic_stats->avg_ms_spent_time.GetCurrentCounter().Account(ms_duration);
  } else {
    LOG_WARNING() << fmt::format(
        "No timestamp in messages to topic '{}' by key '{}'",
        polled_message.GetTopic(), polled_message.GetKey());
  }
}
void ConsumerImpl::AccountMessageProcessingSucceeded(const Message& message) {
  ++GetTopicStats(message.GetTopic())->messages_counts.messages_success;
}

void ConsumerImpl::AccountMessageBatchProcessingSucceeded(
    const MessageBatch& batch) {
  for (const auto& message : batch) {
    AccountMessageProcessingSucceeded(message);
  }
}
void ConsumerImpl::AccountMessageProcessingFailed(const Message& message) {
  ++GetTopicStats(message.GetTopic())->messages_counts.messages_error;
}

void ConsumerImpl::AccountMessageBatchProcessingFailed(
    const MessageBatch& batch) {
  for (const auto& message : batch) {
    AccountMessageProcessingFailed(message);
  }
}

}  // namespace impl

}  // namespace kafka

USERVER_NAMESPACE_END

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <iostream>

#include "common/common/macros.h"
#include "envoy/extensions/filters/network/sip_proxy/tra/v3/tra.pb.h"
#include "extensions/filters/network/sip_proxy/filters/factory_base.h"
#include "envoy/extensions/filters/network/sip_proxy/v3/route.pb.h"
#include "envoy/router/router.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/tcp/conn_pool.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/upstream/load_balancer.h"
#include "envoy/upstream/thread_local_cluster.h"

#include "common/common/logger.h"
#include "common/http/header_utility.h"
#include "common/upstream/load_balancer_impl.h"

#include "extensions/filters/network/sip_proxy/conn_manager.h"
#include "extensions/filters/network/sip_proxy/decoder_events.h"
#include "extensions/filters/network/sip_proxy/filters/filter.h"
#include "extensions/filters/network/sip_proxy/router/router.h"
#include "extensions/filters/network/sip_proxy/tra/tra_impl.h"

#include "absl/types/optional.h"
#include "absl/types/any.h"
#include "common/tracing/http_tracer_impl.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SipProxy {
namespace Router {

class RouteEntryImplBase : public RouteEntry,
                           public Route,
                           public std::enable_shared_from_this<RouteEntryImplBase> {
public:
  RouteEntryImplBase(const envoy::extensions::filters::network::sip_proxy::v3::Route& route);

  // Router::RouteEntry
  const std::string& clusterName() const override;
  const Envoy::Router::MetadataMatchCriteria* metadataMatchCriteria() const override {
    return metadata_match_criteria_.get();
  }

  // Router::Route
  const RouteEntry* routeEntry() const override;

  virtual RouteConstSharedPtr matches(MessageMetadata& metadata) const PURE;

protected:
  RouteConstSharedPtr clusterEntry(const MessageMetadata& metadata) const;
  bool headersMatch(const Http::HeaderMap& headers) const;

private:
  /* Not used
  class DynamicRouteEntry : public RouteEntry, public Route {
  public:
    DynamicRouteEntry(const RouteEntryImplBase& parent, absl::string_view cluster_name)
        : parent_(parent), cluster_name_(std::string(cluster_name)) {}

    // Router::RouteEntry
    const std::string& clusterName() const override { return cluster_name_; }
    const Envoy::Router::MetadataMatchCriteria* metadataMatchCriteria() const override {
      return parent_.metadataMatchCriteria();
    }

    // Router::Route
    const RouteEntry* routeEntry() const override { return this; }

  private:
    const RouteEntryImplBase& parent_;
    const std::string cluster_name_;
  }; */

  const std::string cluster_name_;
  Envoy::Router::MetadataMatchCriteriaConstPtr metadata_match_criteria_;
};

using RouteEntryImplBaseConstSharedPtr = std::shared_ptr<const RouteEntryImplBase>;

// match domain from route header or request_uri, this is the more general way
class GeneralRouteEntryImpl : public RouteEntryImplBase {
public:
  GeneralRouteEntryImpl(const envoy::extensions::filters::network::sip_proxy::v3::Route& route);

  // RouteEntryImplBase
  RouteConstSharedPtr matches(MessageMetadata& metadata) const override;

private:
  const std::string domain_;
};

class RouteMatcher {
public:
  RouteMatcher(const envoy::extensions::filters::network::sip_proxy::v3::RouteConfiguration&);

  RouteConstSharedPtr route(MessageMetadata& metadata) const;

private:
  std::vector<RouteEntryImplBaseConstSharedPtr> routes_;
};

#define ALL_SIP_ROUTER_STATS(COUNTER, GAUGE, HISTOGRAM)                                            \
  COUNTER(route_missing)                                                                           \
  COUNTER(unknown_cluster)                                                                         \
  COUNTER(upstream_rq_maintenance_mode)                                                            \
  COUNTER(no_healthy_upstream)

struct RouterStats {
  ALL_SIP_ROUTER_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT, GENERATE_HISTOGRAM_STRUCT)
};

class UpstreamRequest;
class TransactionInfoItem : public Logger::Loggable<Logger::Id::sip> {
public:
  TransactionInfoItem(SipFilters::DecoderFilterCallbacks* active_trans,
                      std::shared_ptr<UpstreamRequest> upstream_request)
      : active_trans_(active_trans), upstream_request_(upstream_request) {}

  ~TransactionInfoItem() = default;

  void resetTrans() { active_trans_->onReset(); }

  void appendMessageList(std::shared_ptr<MessageMetadata> message) { messages_.push_back(message); }

  SipFilters::DecoderFilterCallbacks* activeTrans() const { return active_trans_; }
  std::shared_ptr<UpstreamRequest> upstreamRequest() const { return upstream_request_; }

  SystemTime timestamp() const { return this->active_trans_->streamInfo().startTime(); }
  void toDelete() { deleted_ = true; }
  bool deleted() { return deleted_; }

private:
  std::list<std::shared_ptr<MessageMetadata>> messages_;
  SipFilters::DecoderFilterCallbacks* active_trans_;
  std::shared_ptr<UpstreamRequest> upstream_request_;
  std::chrono::system_clock::time_point timestamp_;
  bool deleted_{false};
};

struct ThreadLocalTransactionInfo : public ThreadLocal::ThreadLocalObject,
                                    public Logger::Loggable<Logger::Id::sip> {
  ThreadLocalTransactionInfo(std::shared_ptr<TransactionInfo> parent, Event::Dispatcher& dispatcher,
                             std::chrono::milliseconds transaction_timeout, std::string own_domain,
                             std::string domain_match_parameter_name)
      : parent_(parent), dispatcher_(dispatcher), transaction_timeout_(transaction_timeout),
        own_domain_(own_domain), domain_match_parameter_name_(domain_match_parameter_name) {
    audit_timer_ = dispatcher.createTimer([this]() -> void { auditTimerAction(); });
    audit_timer_->enableTimer(std::chrono::seconds(2));
  }
  absl::flat_hash_map<std::string, std::shared_ptr<TransactionInfoItem>> transaction_info_map_{};
  absl::flat_hash_map<std::string, std::shared_ptr<UpstreamRequest>> upstream_request_map_{};

  std::shared_ptr<TransactionInfo> parent_;
  Event::Dispatcher& dispatcher_;
  Event::TimerPtr audit_timer_;
  std::chrono::milliseconds transaction_timeout_;
  std::string own_domain_;
  std::string domain_match_parameter_name_;

  void auditTimerAction() {
    const auto p1 = dispatcher_.timeSource().systemTime();
    for (auto it = transaction_info_map_.cbegin(); it != transaction_info_map_.cend();) {
      if (it->second->deleted()) {
        transaction_info_map_.erase(it++);
        continue;
      }

      auto diff =
          std::chrono::duration_cast<std::chrono::milliseconds>(p1 - it->second->timestamp());
      if (diff.count() >= transaction_timeout_.count()) {
        it->second->resetTrans();
        // transaction_info_map_.erase(it++);
      }

      ++it;
      // In single thread, this condition should be cover in line 160
      // And Envoy should be single thread
      // if (it->second->deleted()) {
      //   transaction_info_map_.erase(it++);
      // } else {
      //   ++it;
      // }
    }
    audit_timer_->enableTimer(std::chrono::seconds(2));
  }
};

class TransactionInfo : public std::enable_shared_from_this<TransactionInfo>,
                        Logger::Loggable<Logger::Id::sip> {
public:
  TransactionInfo(const std::string& cluster_name, ThreadLocal::SlotAllocator& tls,
                  std::chrono::milliseconds transaction_timeout, std::string own_domain,
                  std::string domain_match_parameter_name)
      : cluster_name_(cluster_name), tls_(tls.allocateSlot()),
        transaction_timeout_(transaction_timeout), own_domain_(own_domain),
        domain_match_parameter_name_(domain_match_parameter_name) {}

  void init() {
    // Note: `this` and `cluster_name` have a a lifetime of the filter.
    // That may be shorter than the tls callback if the listener is torn down shortly after it is
    // created. We use a weak pointer to make sure this object outlives the tls callbacks.
    std::weak_ptr<TransactionInfo> this_weak_ptr = this->shared_from_this();
    tls_->set(
        [this_weak_ptr](Event::Dispatcher& dispatcher) -> ThreadLocal::ThreadLocalObjectSharedPtr {
          if (auto this_shared_ptr = this_weak_ptr.lock()) {
            return std::make_shared<ThreadLocalTransactionInfo>(
                this_shared_ptr, dispatcher, this_shared_ptr->transaction_timeout_,
                this_shared_ptr->own_domain_, this_shared_ptr->domain_match_parameter_name_);
          }
          return nullptr;
        });

    (void)cluster_name_;
  }
  ~TransactionInfo() = default;

  void insertTransaction(std::string&& transaction_id,
                         SipFilters::DecoderFilterCallbacks* active_trans,
                         std::shared_ptr<UpstreamRequest> upstream_request) {
    tls_->getTyped<ThreadLocalTransactionInfo>().transaction_info_map_.emplace(std::make_pair(
        transaction_id, std::make_shared<TransactionInfoItem>(active_trans, upstream_request)));
  }

  void deleteTransaction(std::string&& transaction_id) {
    tls_->getTyped<ThreadLocalTransactionInfo>()
        .transaction_info_map_.at(transaction_id)
        ->toDelete();
  }

  TransactionInfoItem& getTransaction(std::string&& transaction_id) {
    return *(tls_->getTyped<ThreadLocalTransactionInfo>().transaction_info_map_.at(transaction_id));
  }

  void insertUpstreamRequest(const std::string& host,
                             std::shared_ptr<UpstreamRequest> upstream_request) {
    tls_->getTyped<ThreadLocalTransactionInfo>().upstream_request_map_.emplace(
        std::make_pair(host, upstream_request));
  }

  std::shared_ptr<UpstreamRequest> getUpstreamRequest(const std::string& host) {
    try {
      return tls_->getTyped<ThreadLocalTransactionInfo>().upstream_request_map_.at(host);
    } catch (std::out_of_range) {
      return nullptr;
    }
  }

  void deleteUpstreamRequest(const std::string& host) {
    tls_->getTyped<ThreadLocalTransactionInfo>().upstream_request_map_.erase(host);
  }

  std::string getOwnDomain() { return own_domain_; }

  std::string getDomainMatchParamName() { return domain_match_parameter_name_; }

private:
  const std::string cluster_name_;
  ThreadLocal::SlotPtr tls_;
  std::chrono::milliseconds transaction_timeout_;
  std::string own_domain_;
  std::string domain_match_parameter_name_;
};

class Router : public Upstream::LoadBalancerContextBase,
               public virtual DecoderEventHandler,
               public SipFilters::DecoderFilter,
               Logger::Loggable<Logger::Id::sip> {
public:
  Router(Upstream::ClusterManager& cluster_manager, const std::string& stat_prefix,
         Stats::Scope& scope, Server::Configuration::FactoryContext& context)
      : cluster_manager_(cluster_manager), stats_(generateStats(stat_prefix, scope)),
        context_(context) {}

  // SipFilters::DecoderFilter
  void onDestroy() override;
  void setDecoderFilterCallbacks(SipFilters::DecoderFilterCallbacks& callbacks) override;

  // DecoderEventHandler
  FilterStatus transportBegin(MessageMetadataSharedPtr metadata) override;
  FilterStatus transportEnd() override;
  FilterStatus messageBegin(MessageMetadataSharedPtr metadata) override;
  FilterStatus messageEnd() override;

  // Upstream::LoadBalancerContext
  const Network::Connection* downstreamConnection() const override;
  const Envoy::Router::MetadataMatchCriteria* metadataMatchCriteria() override {
    if (route_entry_) {
      return route_entry_->metadataMatchCriteria();
    }
    return nullptr;
  }

  bool shouldSelectAnotherHost(const Upstream::Host& host) override {
    if (!metadata_->destination().empty()) {
      return false;
    }
    return host.address()->ip()->addressAsString() != metadata_->destination();
  }

private:
  void cleanup();
  RouterStats generateStats(const std::string& prefix, Stats::Scope& scope) {
    return RouterStats{ALL_SIP_ROUTER_STATS(POOL_COUNTER_PREFIX(scope, prefix),
                                            POOL_GAUGE_PREFIX(scope, prefix),
                                            POOL_HISTOGRAM_PREFIX(scope, prefix))};
  }

  FilterStatus handleAffinity();
  FilterStatus messageHandlerWithLoadbalancer(std::shared_ptr<TransactionInfo> transaction_info,
                                              MessageMetadataSharedPtr metadata, std::string dest,
                                              bool& lb_ret);

  QueryStatus handleCustomizedAffinity(std::string type, std::string key,
                                       MessageMetadataSharedPtr metadata);

  Upstream::ClusterManager& cluster_manager_;
  RouterStats stats_;

  RouteConstSharedPtr route_{};
  const RouteEntry* route_entry_{};
  MessageMetadataSharedPtr metadata_{};

  std::shared_ptr<UpstreamRequest> upstream_request_;
  SipFilters::DecoderFilterCallbacks* callbacks_{};
  Upstream::ClusterInfoConstSharedPtr cluster_;
  Upstream::ThreadLocalCluster* thread_local_cluster_;
  std::shared_ptr<TransactionInfos> transaction_infos_{};
  std::shared_ptr<SipSettings> settings_;
  Server::Configuration::FactoryContext& context_;
  bool continue_handling_;
};

class ThreadLocalActiveConn;
class ResponseDecoder : public DecoderCallbacks,
                        public DecoderEventHandler,
                        public Logger::Loggable<Logger::Id::sip> {
public:
  ResponseDecoder(UpstreamRequest& parent)
      : parent_(parent), decoder_(std::make_unique<Decoder>(*this)) {}
  ~ResponseDecoder() override = default;
  bool onData(Buffer::Instance& data);

  // DecoderEventHandler
  FilterStatus messageBegin(MessageMetadataSharedPtr metadata) override {
    UNREFERENCED_PARAMETER(metadata);
    return FilterStatus::Continue;
  }
  FilterStatus messageEnd() override { return FilterStatus::Continue; };
  FilterStatus transportBegin(MessageMetadataSharedPtr metadata) override;
  FilterStatus transportEnd() override { return FilterStatus::Continue; }

  // DecoderCallbacks
  DecoderEventHandler& newDecoderEventHandler(MessageMetadataSharedPtr metadata) override {
    UNREFERENCED_PARAMETER(metadata);
    return *this;
  }
  absl::string_view getLocalIp() override;
  std::string getOwnDomain() override;
  std::string getDomainMatchParamName() override;

private:
  UpstreamRequest& parent_;
  DecoderPtr decoder_;
};

using ResponseDecoderPtr = std::unique_ptr<ResponseDecoder>;

class UpstreamRequest : public Tcp::ConnectionPool::Callbacks,
                        public Tcp::ConnectionPool::UpstreamCallbacks,
                        public std::enable_shared_from_this<UpstreamRequest>,
                        public Logger::Loggable<Logger::Id::sip> {
public:
  UpstreamRequest(Tcp::ConnectionPool::Instance& pool,
                  std::shared_ptr<TransactionInfo> transaction_info);
  ~UpstreamRequest() override;
  FilterStatus start();
  void resetStream();
  void releaseConnection(bool close);

  SipFilters::DecoderFilterCallbacks* getTransaction(std::string&& transaction_id);
  std::shared_ptr<Tcp::ConnectionPool::ConnectionData> connData() { return conn_data_; }

  // Tcp::ConnectionPool::Callbacks
  void onPoolFailure(ConnectionPool::PoolFailureReason reason,
                     Upstream::HostDescriptionConstSharedPtr host) override;
  void onPoolReady(Tcp::ConnectionPool::ConnectionDataPtr&& conn,
                   Upstream::HostDescriptionConstSharedPtr host) override;

  void onRequestStart(bool continue_handling = false);
  void onRequestComplete();
  void onResponseComplete();
  void onUpstreamHostSelected(Upstream::HostDescriptionConstSharedPtr host);
  void onResetStream(ConnectionPool::PoolFailureReason reason);

  // Tcp::ConnectionPool::UpstreamCallbacks
  void onUpstreamData(Buffer::Instance& data, bool end_stream) override;
  void onEvent(Network::ConnectionEvent event) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

  void setDecoderFilterCallbacks(SipFilters::DecoderFilterCallbacks& callbacks);

  ConnectionState connectionState() { return conn_state_; }
  void write(Buffer::Instance& data, bool end_stream) {
    return conn_data_->connection().write(data, end_stream);
  }

  absl::string_view localAddress() {
    return conn_data_->connection().addressProvider().localAddress()->ip()->addressAsString();
  }

  std::shared_ptr<TransactionInfo> transactionInfo() { return transaction_info_; }

private:
  Tcp::ConnectionPool::Instance& conn_pool_;

  Tcp::ConnectionPool::Cancellable* conn_pool_handle_{};
  std::shared_ptr<Tcp::ConnectionPool::ConnectionData> conn_data_;
  Upstream::HostDescriptionConstSharedPtr upstream_host_;
  ConnectionState conn_state_{ConnectionState::NotConnected};

  std::shared_ptr<TransactionInfo> transaction_info_;
  SipFilters::DecoderFilterCallbacks* callbacks_{};
  std::list<MessageMetadataSharedPtr> pending_request_;
  Buffer::OwnedImpl upstream_buffer_;

  bool request_complete_ : 1;
  bool response_complete_ : 1;
};

} // namespace Router
} // namespace SipProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy

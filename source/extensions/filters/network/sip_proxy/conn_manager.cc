#include "extensions/filters/network/sip_proxy/conn_manager.h"

#include "envoy/common/exception.h"
#include "envoy/event/dispatcher.h"
#include "common/tracing/http_tracer_impl.h"

#include "extensions/filters/network/sip_proxy/app_exception_impl.h"
#include "extensions/filters/network/sip_proxy/encoder.h"
#include "extensions/filters/network/sip_proxy/protocol.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SipProxy {

ConnectionManager::ConnectionManager(Config& config, Random::RandomGenerator& random_generator,
                                     TimeSource& time_source,
                                     Server::Configuration::FactoryContext& context,
                                     std::shared_ptr<Router::TransactionInfos> transaction_infos)
    : config_(config), stats_(config_.stats()), decoder_(std::make_unique<Decoder>(*this)),
      random_generator_(random_generator), time_source_(time_source), context_(context),
      transaction_infos_(transaction_infos), p_cookie_ip_map_(std::make_shared<PCookieIPMap>()) {

  if (config.settings()->traServiceConfig().has_grpc_service()) {
    const std::chrono::milliseconds timeout = std::chrono::milliseconds(
        PROTOBUF_GET_MS_OR_DEFAULT(config.settings()->traServiceConfig(), timeout, 2000));
    tra_client_ = TrafficRoutingAssistant::traClient(
        this->context_, config.settings()->traServiceConfig().grpc_service(), timeout,
        config.settings()->traServiceConfig().transport_api_version());
  }
}

ConnectionManager::~ConnectionManager() {
	tra_client_->closeStream();
};

Network::FilterStatus ConnectionManager::onData(Buffer::Instance& data, bool end_stream) {
  ENVOY_CONN_LOG(debug, "sip proxy received data {} --> {}", read_callbacks_->connection(),
                 read_callbacks_->connection().addressProvider().remoteAddress()->asStringView(),
                 read_callbacks_->connection().addressProvider().localAddress()->asStringView());
  request_buffer_.move(data);
  dispatch();

  if (end_stream) {
    ENVOY_CONN_LOG(info, "downstream half-closed", read_callbacks_->connection());

    resetAllTrans(false);
    read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
  }

  return Network::FilterStatus::StopIteration;
}

void ConnectionManager::continueHanding() { decoder_->onData(request_buffer_, true); }

void ConnectionManager::dispatch() { decoder_->onData(request_buffer_); }

void ConnectionManager::sendLocalReply(MessageMetadata& metadata, const DirectResponse& response,
                                       bool end_stream) {
  if (read_callbacks_->connection().state() == Network::Connection::State::Closed) {
    return;
  }

  Buffer::OwnedImpl buffer;
  const DirectResponse::ResponseType result = response.encode(metadata, buffer);

  Buffer::OwnedImpl response_buffer;

  metadata.setEP(getLocalIp());
  std::shared_ptr<Encoder> encoder = std::make_shared<EncoderImpl>();
  encoder->encode(std::make_shared<MessageMetadata>(metadata), response_buffer);

  read_callbacks_->connection().write(response_buffer, end_stream);
  if (end_stream) {
    read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
  }

  switch (result) {
  case DirectResponse::ResponseType::SuccessReply:
    stats_.response_success_.inc();
    break;
  case DirectResponse::ResponseType::ErrorReply:
    stats_.response_error_.inc();
    break;
  case DirectResponse::ResponseType::Exception:
    stats_.response_exception_.inc();
    break;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

void ConnectionManager::doDeferredTransDestroy(ConnectionManager::ActiveTrans& trans) {
  read_callbacks_->connection().dispatcher().deferredDelete(
      std::move(transactions_.at(trans.transactionId())));
  transactions_.erase(trans.transactionId());
}

void ConnectionManager::resetAllTrans(bool local_reset) {
  ENVOY_LOG(info, "active_trans to be deleted {}", transactions_.size());
  for (auto it = transactions_.cbegin(); it != transactions_.cend();) {
    if (local_reset) {
      ENVOY_CONN_LOG(debug, "local close with active request", read_callbacks_->connection());
      stats_.cx_destroy_local_with_active_rq_.inc();
    } else {
      ENVOY_CONN_LOG(debug, "remote close with active request", read_callbacks_->connection());
      stats_.cx_destroy_remote_with_active_rq_.inc();
    }

    (it++)->second->onReset();
  }
}

void ConnectionManager::initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) {
  read_callbacks_ = &callbacks;

  read_callbacks_->connection().addConnectionCallbacks(*this);
  read_callbacks_->connection().enableHalfClose(true);

  auto stream_info = StreamInfo::StreamInfoImpl(time_source_, read_callbacks_->connection().addressProviderSharedPtr());
  tra_client_->setRequestCallbacks(*this);
  tra_client_->subscribeLskpmc("", Tracing::NullSpan::instance(), stream_info);

}

void ConnectionManager::onEvent(Network::ConnectionEvent event) {
  ENVOY_CONN_LOG(info, "received event {}", read_callbacks_->connection(), event);
  resetAllTrans(event == Network::ConnectionEvent::LocalClose);
}

DecoderEventHandler& ConnectionManager::newDecoderEventHandler(MessageMetadataSharedPtr metadata) {
  ENVOY_LOG(trace, "new decoder filter");
  std::string&& k = std::string(metadata->transactionId().value());
  if (metadata->methodType() == MethodType::Ack) {
    if (transactions_.find(k) != transactions_.end()) {
      // ACK_4XX
      return *transactions_.at(k);
    }
  }

  ActiveTransPtr new_trans = std::make_unique<ActiveTrans>(*this, metadata);
  new_trans->createFilterChain();
  transactions_.emplace(k, std::move(new_trans));

  return *transactions_.at(k);
}

void ConnectionManager::complete(TrafficRoutingAssistant::ResponseType type, absl::any resp) {
    switch (type) {
    case TrafficRoutingAssistant::ResponseType::CreateLskpmcResp: {
      ENVOY_LOG(trace, "=== CreateLskpmcResp");
      break;
    }
    case TrafficRoutingAssistant::ResponseType::UpdateLskpmcResp: {
      ENVOY_LOG(trace, "=== UpdateLskpmcResp");
      break;
    }
    case TrafficRoutingAssistant::ResponseType::RetrieveLskpmcResp: {
      auto lskpmcs = absl::any_cast<envoy::extensions::filters::network::sip_proxy::tra::v3::RetrieveLskpmcResponse>(resp).lskpmcs();
      for (auto & item : lskpmcs) {
        if (!item.second.empty()) {
          decoder_->metadata()->setDestination(item.second);
          p_cookie_ip_map_->emplace(item);
	}
        ENVOY_LOG(trace, "=== RetrieveLskpmcResp {}={}", item.first, item.second);
      }

      this->continueHanding();
      break;
    }
    case TrafficRoutingAssistant::ResponseType::DeleteLskpmcResp: {
      ENVOY_LOG(trace, "=== DeleteLskpmcResp");
      break;
    }
    case TrafficRoutingAssistant::ResponseType::SubscribeLskpmcResp: {
      ENVOY_LOG(trace, "=== SubscribeLskpmcResp");
      auto lskpmcs = absl::any_cast<envoy::extensions::filters::network::sip_proxy::tra::v3::SubscribeLskpmcResponse>(resp).lskpmcs();
      for (auto& item : lskpmcs) {
        ENVOY_LOG(debug, "tra update {}={}", item.first, item.second);
        p_cookie_ip_map_->emplace(item);
      }
    }
    default:
      break;
    }
  }


bool ConnectionManager::ResponseDecoder::onData(MessageMetadataSharedPtr metadata) {
  metadata_ = metadata;
  if (auto status = transportBegin(metadata_); status == FilterStatus::StopIteration) {
    return true;
  }

  if (auto status = messageBegin(metadata_); status == FilterStatus::StopIteration) {
    return true;
  }

  if (auto status = messageEnd(); status == FilterStatus::StopIteration) {
    return true;
  }

  if (auto status = transportEnd(); status == FilterStatus::StopIteration) {
    return true;
  }

  return true;
}

FilterStatus ConnectionManager::ResponseDecoder::messageBegin(MessageMetadataSharedPtr metadata) {
  UNREFERENCED_PARAMETER(metadata);
  return FilterStatus::Continue;
}

FilterStatus ConnectionManager::ResponseDecoder::messageEnd() { return FilterStatus::Continue; }

FilterStatus ConnectionManager::ResponseDecoder::transportEnd() {
  ASSERT(metadata_ != nullptr);

  ConnectionManager& cm = parent_.parent_;

  if (cm.read_callbacks_->connection().state() == Network::Connection::State::Closed) {
    throw EnvoyException("downstream connection is closed");
  }

  Buffer::OwnedImpl buffer;

  metadata_->setEP(getLocalIp());
  std::shared_ptr<Encoder> encoder = std::make_shared<EncoderImpl>();
  encoder->encode(metadata_, buffer);

  ENVOY_STREAM_LOG(info, "send response {}\n{}", parent_, buffer.length(), buffer.toString());
  cm.read_callbacks_->connection().write(buffer, false);

  cm.stats_.response_.inc();

  return FilterStatus::Continue;
}

FilterStatus ConnectionManager::ActiveTrans::applyDecoderFilters(ActiveTransDecoderFilter* filter) {
  ASSERT(filter_action_ != nullptr);

  if (!local_response_sent_) {
    std::list<ActiveTransDecoderFilterPtr>::iterator entry;
    if (!filter) {
      entry = decoder_filters_.begin();
    } else {
      entry = std::next(filter->entry());
    }

    for (; entry != decoder_filters_.end(); entry++) {
      const FilterStatus status = filter_action_((*entry)->handle_.get());
      if (local_response_sent_) {
        // The filter called sendLocalReply: stop processing filters and return
        // FilterStatus::Continue irrespective of the current result.
        break;
      }

      if (status != FilterStatus::Continue) {
        return status;
      }
    }
  }

  filter_action_ = nullptr;
  filter_context_.reset();

  return FilterStatus::Continue;
}

FilterStatus ConnectionManager::ActiveTrans::transportBegin(MessageMetadataSharedPtr metadata) {
  filter_context_ = metadata;
  filter_action_ = [this](DecoderEventHandler* filter) -> FilterStatus {
    MessageMetadataSharedPtr metadata = absl::any_cast<MessageMetadataSharedPtr>(filter_context_);
    return filter->transportBegin(metadata);
  };

  return applyDecoderFilters(nullptr);
}

FilterStatus ConnectionManager::ActiveTrans::transportEnd() {
  ASSERT(metadata_ != nullptr);
  parent_.stats_.request_.inc();

  FilterStatus status;
  filter_action_ = [](DecoderEventHandler* filter) -> FilterStatus {
    return filter->transportEnd();
  };

  status = applyDecoderFilters(nullptr);
  if (status == FilterStatus::StopIteration) {
    return status;
  }

  finalizeRequest();

  return status;
}

void ConnectionManager::ActiveTrans::finalizeRequest() {}

FilterStatus ConnectionManager::ActiveTrans::messageBegin(MessageMetadataSharedPtr metadata) {
  metadata_ = metadata;
  filter_context_ = metadata;
  filter_action_ = [this](DecoderEventHandler* filter) -> FilterStatus {
    MessageMetadataSharedPtr metadata = absl::any_cast<MessageMetadataSharedPtr>(filter_context_);
    return filter->messageBegin(metadata);
  };

  return applyDecoderFilters(nullptr);
}

FilterStatus ConnectionManager::ActiveTrans::messageEnd() {
  filter_action_ = [](DecoderEventHandler* filter) -> FilterStatus { return filter->messageEnd(); };
  return applyDecoderFilters(nullptr);
}

void ConnectionManager::ActiveTrans::createFilterChain() {
  parent_.config_.filterFactory().createFilterChain(*this);
}

void ConnectionManager::ActiveTrans::onReset() { parent_.doDeferredTransDestroy(*this); }

void ConnectionManager::ActiveTrans::onError(const std::string& what) {
  if (metadata_) {
    sendLocalReply(AppException(AppExceptionType::ProtocolError, what), true);
    return;
  }

  parent_.doDeferredTransDestroy(*this);
  parent_.read_callbacks_->connection().close(Network::ConnectionCloseType::NoFlush);
}

const Network::Connection* ConnectionManager::ActiveTrans::connection() const {
  return &parent_.read_callbacks_->connection();
}

Router::RouteConstSharedPtr ConnectionManager::ActiveTrans::route() {
  if (!cached_route_) {
    if (metadata_ != nullptr) {
      Router::RouteConstSharedPtr route = parent_.config_.routerConfig().route(*metadata_);
      cached_route_ = std::move(route);
    } else {
      cached_route_ = nullptr;
    }
  }

  return cached_route_.value();
}

void ConnectionManager::ActiveTrans::sendLocalReply(const DirectResponse& response,
                                                    bool end_stream) {
  parent_.sendLocalReply(*metadata_, response, end_stream);

  if (end_stream) {
    return;
  }

  // Consume any remaining request data from the downstream.
  local_response_sent_ = true;
}

void ConnectionManager::ActiveTrans::startUpstreamResponse() {
  response_decoder_ = std::make_unique<ResponseDecoder>(*this);
}

SipFilters::ResponseStatus
ConnectionManager::ActiveTrans::upstreamData(MessageMetadataSharedPtr metadata) {
  ASSERT(response_decoder_ != nullptr);

  try {
    if (response_decoder_->onData(metadata)) {
      // Completed upstream response.
      // parent_.doDeferredRpcDestroy(*this);
      return SipFilters::ResponseStatus::Complete;
    }
    return SipFilters::ResponseStatus::MoreData;
  } catch (const AppException& ex) {
    ENVOY_LOG(error, "sip response application error: {}", ex.what());
    // parent_.stats_.response_decoding_error_.inc();

    sendLocalReply(ex, true);
    return SipFilters::ResponseStatus::Reset;
  } catch (const EnvoyException& ex) {
    ENVOY_CONN_LOG(error, "sip response error: {}", parent_.read_callbacks_->connection(),
                   ex.what());
    // parent_.stats_.response_decoding_error_.inc();

    onError(ex.what());
    return SipFilters::ResponseStatus::Reset;
  }
}

void ConnectionManager::ActiveTrans::resetDownstreamConnection() {
  parent_.read_callbacks_->connection().close(Network::ConnectionCloseType::NoFlush);
}

} // namespace SipProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy

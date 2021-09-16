#include "test/extensions/filters/network/sip_proxy/mocks.h"

#include <memory>

#include "source/common/protobuf/protobuf.h"

#include "gtest/gtest.h"

using testing::_;
using testing::Invoke;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {

// Provide a specialization for ProtobufWkt::Struct (for MockFilterConfigFactory)
template <>
void MessageUtil::validate(const ProtobufWkt::Struct&, ProtobufMessage::ValidationVisitor&) {}

namespace Extensions {
namespace NetworkFilters {
namespace SipProxy {

MockConfig::MockConfig() = default;
MockConfig::~MockConfig() = default;

// MockTransport::MockTransport() {
//  ON_CALL(*this, name()).WillByDefault(ReturnRef(name_));
//  ON_CALL(*this, type()).WillByDefault(Return(type_));
//}
// MockTransport::~MockTransport() = default;
//
// MockProtocol::MockProtocol() {
//  ON_CALL(*this, name()).WillByDefault(ReturnRef(name_));
//  ON_CALL(*this, type()).WillByDefault(Return(type_));
//  ON_CALL(*this, setType(_)).WillByDefault(Invoke([&](ProtocolType type) -> void {
//    type_ = type;
//  }));
//  ON_CALL(*this, supportsUpgrade()).WillByDefault(Return(false));
//}
// MockProtocol::~MockProtocol() = default;

MockDecoderCallbacks::MockDecoderCallbacks() {
  ON_CALL(*this, getLocalIp()).WillByDefault(Return("127.0.0.1"));
  ON_CALL(*this, getOwnDomain()).WillByDefault(Return(""));
  ON_CALL(*this, getDomainMatchParamName()).WillByDefault(Return(""));
}
MockDecoderCallbacks::~MockDecoderCallbacks() = default;

MockDecoderEventHandler::MockDecoderEventHandler() {
  ON_CALL(*this, transportBegin(_)).WillByDefault(Return(FilterStatus::Continue));
  ON_CALL(*this, transportEnd()).WillByDefault(Return(FilterStatus::Continue));
  ON_CALL(*this, messageBegin(_)).WillByDefault(Return(FilterStatus::Continue));
  ON_CALL(*this, messageEnd()).WillByDefault(Return(FilterStatus::Continue));
}
MockDecoderEventHandler::~MockDecoderEventHandler() = default;
//
MockDirectResponse::MockDirectResponse() = default;
MockDirectResponse::~MockDirectResponse() = default;
//
// MockSipObject::MockSipObject() = default;
// MockSipObject::~MockSipObject() = default;

namespace SipFilters {

// MockFilterChainFactoryCallbacks::MockFilterChainFactoryCallbacks() = default;
// MockFilterChainFactoryCallbacks::~MockFilterChainFactoryCallbacks() = default;

MockDecoderFilter::MockDecoderFilter() {
  ON_CALL(*this, transportBegin(_)).WillByDefault(Return(FilterStatus::Continue));
  ON_CALL(*this, transportEnd()).WillByDefault(Return(FilterStatus::Continue));
  ON_CALL(*this, messageBegin(_)).WillByDefault(Return(FilterStatus::Continue));
  ON_CALL(*this, messageEnd()).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, structBegin(_)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, structEnd()).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, fieldBegin(_, _, _)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, fieldEnd()).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, boolValue(_)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, byteValue(_)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, int16Value(_)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, int32Value(_)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, int64Value(_)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, doubleValue(_)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, stringValue(_)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, mapBegin(_, _, _)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, mapEnd()).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, listBegin(_, _)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, listEnd()).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, setBegin(_, _)).WillByDefault(Return(FilterStatus::Continue));
  // ON_CALL(*this, setEnd()).WillByDefault(Return(FilterStatus::Continue));
}
MockDecoderFilter::~MockDecoderFilter() = default;

MockDecoderFilterCallbacks::MockDecoderFilterCallbacks() {

  ON_CALL(*this, streamId()).WillByDefault(Return(stream_id_));
  ON_CALL(*this, transactionInfos()).WillByDefault(Return(transaction_infos_));
  ON_CALL(*this, streamInfo()).WillByDefault(ReturnRef(stream_info_));
}
MockDecoderFilterCallbacks::~MockDecoderFilterCallbacks() = default;

MockFilterConfigFactory::MockFilterConfigFactory() : name_("envoy.filters.sip.mock_filter") {
  mock_filter_ = std::make_shared<NiceMock<MockDecoderFilter>>();
}

MockFilterConfigFactory::~MockFilterConfigFactory() = default;

FilterFactoryCb MockFilterConfigFactory::createFilterFactoryFromProto(
    const Protobuf::Message& proto_config, const std::string& stats_prefix,
    Server::Configuration::FactoryContext& context) {
  UNREFERENCED_PARAMETER(context);

  config_struct_ = dynamic_cast<const ProtobufWkt::Struct&>(proto_config);
  config_stat_prefix_ = stats_prefix;

  return [this](FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addDecoderFilter(mock_filter_);
  };
}

} // namespace SipFilters
//
namespace Router {
//
// MockRateLimitPolicyEntry::MockRateLimitPolicyEntry() {
//  ON_CALL(*this, disableKey()).WillByDefault(ReturnRef(disable_key_));
//}
// MockRateLimitPolicyEntry::~MockRateLimitPolicyEntry() = default;
//
// MockRateLimitPolicy::MockRateLimitPolicy() {
//  ON_CALL(*this, empty()).WillByDefault(Return(true));
//  ON_CALL(*this, getApplicableRateLimit(_)).WillByDefault(ReturnRef(rate_limit_policy_entry_));
//}
// MockRateLimitPolicy::~MockRateLimitPolicy() = default;
//
MockRouteEntry::MockRouteEntry() {
  ON_CALL(*this, clusterName()).WillByDefault(ReturnRef(cluster_name_));
}
MockRouteEntry::~MockRouteEntry() = default;

MockRoute::MockRoute() { ON_CALL(*this, routeEntry()).WillByDefault(Return(&route_entry_)); }
MockRoute::~MockRoute() = default;

} // namespace Router
} // namespace SipProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy

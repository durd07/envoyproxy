#pragma once

#include <chrono>
#include <iostream>
#include <list>
#include <memory>

#include "source/extensions/filters/network/sip_proxy/operation.h"
#include "source/extensions/filters/network/sip_proxy/sip.h"

#include "absl/strings/string_view.h"
#include "absl/types/optional.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SipProxy {

/**
 * MessageMetadata encapsulates metadata about Sip messages. The various fields are considered
 * optional since they may come from either the transport or protocol in some cases. Unless
 * otherwise noted, accessor methods throw absl::bad_optional_access if the corresponding value has
 * not been set.
 */
class MessageMetadata {
public:
  MessageMetadata(){};
  MessageMetadata(std::string&& raw_msg) : raw_msg_(std::move(raw_msg)) {}

  MsgType msgType() { return msg_type_; }
  MethodType methodType() { return method_type_; }
  MethodType respMethodType() { return resp_method_type_; }
  absl::optional<absl::string_view> EP() { return ep_; }
  std::vector<Operation>& operationList() { return operation_list_; }
  absl::optional<absl::string_view> routeEP() { return route_ep_; }
  absl::optional<absl::string_view> routeOpaque() { return route_opaque_; }

  absl::optional<absl::string_view> requestURI() { return request_uri_; }
  absl::optional<absl::string_view> topRoute() { return top_route_; }
  absl::optional<absl::string_view> domain() { return domain_; }
  absl::optional<absl::string_view> transactionId() { return transaction_id_; }
  absl::optional<absl::string_view> destination() { return destination_; }

  std::string& rawMsg() { return raw_msg_; }

  void setMsgType(MsgType data) { msg_type_ = data; }
  void setMethodType(MethodType data) { method_type_ = data; }
  void setRespMethodType(MethodType data) { resp_method_type_ = data; }
  void setOperation(Operation op) { operation_list_.emplace_back(op); }
  void setEP(absl::string_view data) { ep_ = data; }
  void setRouteEP(absl::string_view data) { route_ep_ = data; }
  void setRouteOpaque(absl::string_view data) { route_opaque_ = data; }

  void setRequestURI(absl::string_view data) { request_uri_ = data; }
  void setTopRoute(absl::string_view data) { top_route_ = data; }
  void setDomain(absl::string_view data) { domain_ = data; }

  // input is the full SIP header
  void setTransactionId(absl::string_view data) {
    auto start_index = data.find("branch=");
    if (start_index == absl::string_view::npos) {
      return;
    }
    start_index += strlen("branch=");

    auto end_index = data.find_first_of(";>", start_index);
    if (end_index == absl::string_view::npos) {
      end_index = data.size();
    }
    transaction_id_ = data.substr(start_index, end_index - start_index);
  }

  void setDestination(absl::string_view destination) { destination_ = destination; }
  /*only used in UT*/
  void resetTransactionId() { transaction_id_.reset(); }

private:
  MsgType msg_type_;
  MethodType method_type_;
  MethodType resp_method_type_;
  std::vector<Operation> operation_list_;
  // std::list<absl::optional<size_t>> insert_ep_location_{};
  // absl::optional<size_t> insert_opaque_location_{};
  absl::optional<absl::string_view> ep_{};
  absl::optional<absl::string_view> pep_{};
  absl::optional<absl::string_view> route_ep_{};
  absl::optional<absl::string_view> route_opaque_{};

  absl::optional<absl::string_view> request_uri_{};
  absl::optional<absl::string_view> top_route_{};
  absl::optional<absl::string_view> domain_{};
  absl::optional<absl::string_view> transaction_id_{};
  absl::optional<absl::string_view> destination_{};

  std::string raw_msg_{};
};

using MessageMetadataSharedPtr = std::shared_ptr<MessageMetadata>;

} // namespace SipProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy

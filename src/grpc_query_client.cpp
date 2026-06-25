#include "redis_pvxs_ioc/grpc_query_client.h"

#include <grpcpp/grpcpp.h>

#include "bpm_query.grpc.pb.h"
#include "bpm_query.pb.h"

namespace bq = bpm::query::v1;

namespace redis_pvxs_ioc {
namespace {

void fillSource(bq::Source* dst, const SourceArgs& src) {
  dst->set_digitizer(src.digitizer);
  dst->set_subkey(src.subkey);
}

void fillWindow(bq::Window* dst, int64_t startNs, int64_t endNs, int64_t lengthNs) {
  dst->set_start_ns(startNs);
  dst->set_end_ns(endNs);
  dst->set_length_ns(lengthNs);
}

QueryStatus toStatus(const bq::Status& status) {
  return QueryStatus{status.ok(), status.message()};
}

QueryResult toResult(const bq::QueryReply& reply) {
  QueryResult out;
  out.status = toStatus(reply.status());
  out.values.assign(reply.values().begin(), reply.values().end());
  out.timesNs.assign(reply.times_ns().begin(), reply.times_ns().end());
  out.eventTimeNs = reply.event_time_ns();
  out.units = reply.units();
  return out;
}

QueryResult transportError(const grpc::Status& status) {
  QueryResult out;
  out.status.ok = false;
  out.status.message = "gRPC transport error: " + status.error_message() +
                       " (code " + std::to_string(static_cast<int>(status.error_code())) + ")";
  return out;
}

}  // namespace

struct GrpcQueryClient::Impl {
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<bq::BpmQuery::Stub> stub;
};

GrpcQueryClient::GrpcQueryClient(const std::string& endpoint)
    : endpoint_(endpoint), impl_(std::make_unique<Impl>()) {
  impl_->channel = grpc::CreateChannel(endpoint_, grpc::InsecureChannelCredentials());
  impl_->stub = bq::BpmQuery::NewStub(impl_->channel);
}

GrpcQueryClient::~GrpcQueryClient() = default;

QueryResult GrpcQueryClient::average(const AverageArgs& args) {
  bq::AverageRequest request;
  fillSource(request.mutable_source(), args.source);
  fillWindow(request.mutable_window(), args.startNs, args.endNs, args.lengthNs);
  request.set_per_entry_mean(args.perEntryMean);

  bq::QueryReply reply;
  grpc::ClientContext context;
  const auto status = impl_->stub->Average(&context, request, &reply);
  if (!status.ok()) {
    return transportError(status);
  }
  return toResult(reply);
}

QueryResult GrpcQueryClient::onEvent(const OnEventArgs& args) {
  bq::OnEventRequest request;
  fillSource(request.mutable_source(), args.source);
  request.set_event(args.event);
  request.set_delta_ns(args.deltaNs);

  bq::QueryReply reply;
  grpc::ClientContext context;
  const auto status = impl_->stub->OnEvent(&context, request, &reply);
  if (!status.ok()) {
    return transportError(status);
  }
  return toResult(reply);
}

QueryResult GrpcQueryClient::onEventTime(const OnEventTimeArgs& args) {
  bq::OnEventTimeRequest request;
  fillSource(request.mutable_source(), args.source);
  request.set_event_time_ns(args.eventTimeNs);
  request.set_start_ns(args.startNs);
  request.set_length_ns(args.lengthNs);

  bq::QueryReply reply;
  grpc::ClientContext context;
  const auto status = impl_->stub->OnEventTime(&context, request, &reply);
  if (!status.ok()) {
    return transportError(status);
  }
  return toResult(reply);
}

QueryResult GrpcQueryClient::slice(const SliceArgs& args) {
  bq::SliceRequest request;
  fillSource(request.mutable_source(), args.source);
  fillWindow(request.mutable_window(), 0, args.endNs, 0);
  request.set_start_index(args.startIndex);
  request.set_length(args.length);
  request.set_stride(args.stride);

  bq::QueryReply reply;
  grpc::ClientContext context;
  const auto status = impl_->stub->Slice(&context, request, &reply);
  if (!status.ok()) {
    return transportError(status);
  }
  return toResult(reply);
}

QueryResult GrpcQueryClient::decimate(const DecimateArgs& args) {
  bq::DecimateRequest request;
  fillSource(request.mutable_source(), args.source);
  fillWindow(request.mutable_window(), 0, args.endNs, 0);
  request.set_factor(args.factor);
  request.set_max_points(args.maxPoints);

  bq::QueryReply reply;
  grpc::ClientContext context;
  const auto status = impl_->stub->Decimate(&context, request, &reply);
  if (!status.ok()) {
    return transportError(status);
  }
  return toResult(reply);
}

OrbitResult GrpcQueryClient::orbit(const OrbitArgs& args) {
  bq::OrbitRequest request;
  request.set_machine(args.machine);
  request.set_section(args.section);
  fillWindow(request.mutable_window(), args.startNs, args.endNs, args.lengthNs);
  request.set_start_index(args.startIndex);
  request.set_end_index(args.endIndex);

  bq::OrbitReply reply;
  grpc::ClientContext context;
  const auto status = impl_->stub->Orbit(&context, request, &reply);

  OrbitResult out;
  if (!status.ok()) {
    out.status.ok = false;
    out.status.message = "gRPC transport error: " + status.error_message() +
                         " (code " + std::to_string(static_cast<int>(status.error_code())) + ")";
    return out;
  }
  out.status = toStatus(reply.status());
  out.timeNs = reply.time_ns();
  out.hNames.assign(reply.h_names().begin(), reply.h_names().end());
  out.hOrbit.assign(reply.h_orbit().begin(), reply.h_orbit().end());
  out.hIntensity.assign(reply.h_intensity().begin(), reply.h_intensity().end());
  out.vNames.assign(reply.v_names().begin(), reply.v_names().end());
  out.vOrbit.assign(reply.v_orbit().begin(), reply.v_orbit().end());
  out.vIntensity.assign(reply.v_intensity().begin(), reply.v_intensity().end());
  return out;
}

}  // namespace redis_pvxs_ioc

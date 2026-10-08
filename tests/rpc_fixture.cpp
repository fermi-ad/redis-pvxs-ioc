#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "rpc_fixture.grpc.pb.h"

namespace {
grpc::Status echo(const redis_pvxs_test::Request* request, redis_pvxs_test::Reply* reply) {
  reply->set_number(request->number());
  reply->set_text(request->text());
  return grpc::Status::OK;
}
class First final : public redis_pvxs_test::First::Service {
  grpc::Status Value(grpc::ServerContext*, const redis_pvxs_test::Request* request,
                     redis_pvxs_test::Reply* reply) override { return echo(request, reply); }
  grpc::Status Sys(grpc::ServerContext*, const redis_pvxs_test::Request* request,
                   redis_pvxs_test::Reply* reply) override { return echo(request, reply); }
};
class Second final : public redis_pvxs_test::Second::Service {
  grpc::Status Value(grpc::ServerContext*, const redis_pvxs_test::Request* request,
                     redis_pvxs_test::Reply* reply) override { return echo(request, reply); }
};
class Strict final : public redis_pvxs_test::Strict::Service {
  std::atomic<unsigned> calls_{0};
  grpc::Status Echo(grpc::ServerContext*, const redis_pvxs_test::StrictRequest* request,
                    redis_pvxs_test::StrictReply* reply) override {
    reply->mutable_request()->CopyFrom(*request);
    reply->set_calls(++calls_);
    if (request->text() == "large")
      for (int i = 0; i < 2048; ++i) reply->mutable_request()->add_numbers(1);
    return grpc::Status::OK;
  }
  grpc::Status Other(grpc::ServerContext*, const redis_pvxs_test::Label* request,
                     redis_pvxs_test::Label* reply) override {
    reply->CopyFrom(*request);
    return grpc::Status::OK;
  }
};
class Unsupported final : public redis_pvxs_test::Unsupported::Service {};
class Control final : public redis_pvxs_test::Control::Service {
  std::atomic<uint64_t> started_{0}, completed_{0}, cancelled_{0}, failed_{0};
  grpc::Status Wait(grpc::ServerContext* context, const redis_pvxs_test::ControlRequest* request,
                    redis_pvxs_test::ControlReply* reply) override {
    ++started_;
    if (request->fail()) {
      ++failed_;
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "accepted command failed; do not replay");
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(request->delay_ms());
    while (std::chrono::steady_clock::now() < until) {
      if (context->IsCancelled()) { ++cancelled_; return grpc::Status::CANCELLED; }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ++completed_;
    reply->set_token(request->token());
    return grpc::Status::OK;
  }
  grpc::Status Inspect(grpc::ServerContext*, const redis_pvxs_test::Empty*, redis_pvxs_test::Counts* reply) override {
    reply->set_started(started_); reply->set_completed(completed_);
    reply->set_cancelled(cancelled_); reply->set_failed(failed_);
    return grpc::Status::OK;
  }
};
}
int main(int argc, char** argv) {
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;
  First first;
  Second second;
  Strict strict;
  Unsupported unsupported;
  Control control;
  builder.RegisterService(&first);
  builder.RegisterService(&second);
  builder.RegisterService(&strict);
  builder.RegisterService(&unsupported);
  builder.RegisterService(&control);
  int port = 0;
  builder.AddListeningPort("127.0.0.1:" + std::string(argc == 2 ? argv[1] : "0"), grpc::InsecureServerCredentials(), &port);
  auto server = builder.BuildAndStart();
  if (!server || port == 0) return 1;
  std::cout << port << std::endl;
  server->Wait();
}

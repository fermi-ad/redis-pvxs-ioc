#include <iostream>
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
}
int main() {
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;
  First first;
  Second second;
  builder.RegisterService(&first);
  builder.RegisterService(&second);
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  auto server = builder.BuildAndStart();
  if (!server || port == 0) return 1;
  std::cout << port << std::endl;
  server->Wait();
}

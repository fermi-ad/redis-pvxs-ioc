#include "redis_pvxs_ioc/grpc_bridge.h"

#include <cstdlib>
#include <stdexcept>

#include <grpcpp/grpcpp.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/support/byte_buffer.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>

#include "reflection.grpc.pb.h"

namespace gpb = google::protobuf;
namespace refl = grpc::reflection::v1alpha;

namespace redis_pvxs_ioc {
namespace {

// ---- string -> proto field ----

void setScalar(gpb::Message* msg, const gpb::FieldDescriptor* f, const std::string& v) {
  const auto* r = msg->GetReflection();
  switch (f->cpp_type()) {
    case gpb::FieldDescriptor::CPPTYPE_INT32:  r->SetInt32(msg, f, static_cast<int32_t>(std::strtoll(v.c_str(), nullptr, 0))); break;
    case gpb::FieldDescriptor::CPPTYPE_INT64:  r->SetInt64(msg, f, static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 0))); break;
    case gpb::FieldDescriptor::CPPTYPE_UINT32: r->SetUInt32(msg, f, static_cast<uint32_t>(std::strtoull(v.c_str(), nullptr, 0))); break;
    case gpb::FieldDescriptor::CPPTYPE_UINT64: r->SetUInt64(msg, f, static_cast<uint64_t>(std::strtoull(v.c_str(), nullptr, 0))); break;
    case gpb::FieldDescriptor::CPPTYPE_DOUBLE: r->SetDouble(msg, f, std::strtod(v.c_str(), nullptr)); break;
    case gpb::FieldDescriptor::CPPTYPE_FLOAT:  r->SetFloat(msg, f, std::strtof(v.c_str(), nullptr)); break;
    case gpb::FieldDescriptor::CPPTYPE_BOOL:   r->SetBool(msg, f, v == "1" || v == "true" || v == "yes" || v == "True"); break;
    case gpb::FieldDescriptor::CPPTYPE_STRING: r->SetString(msg, f, v); break;
    case gpb::FieldDescriptor::CPPTYPE_ENUM: {
      char* end = nullptr;
      long n = std::strtol(v.c_str(), &end, 0);
      if (end && *end == '\0') r->SetEnumValue(msg, f, static_cast<int>(n));
      else if (const auto* ev = f->enum_type()->FindValueByName(v)) r->SetEnum(msg, f, ev);
      break;
    }
    default: throw std::runtime_error("cannot set field " + f->name());
  }
}

// Search the descriptor tree for a singular leaf field named `leaf`; if exactly
// one exists, write its dotted path to `out`.
void findLeafPath(const gpb::Descriptor* d, const std::string& leaf,
                  const std::string& prefix, std::string& out, int& count) {
  for (int i = 0; i < d->field_count(); ++i) {
    const auto* f = d->field(i);
    if (f->is_repeated()) continue;
    if (f->cpp_type() == gpb::FieldDescriptor::CPPTYPE_MESSAGE) {
      findLeafPath(f->message_type(), leaf, prefix + f->name() + ".", out, count);
    } else if (f->name() == leaf) {
      out = prefix + f->name();
      ++count;
    }
  }
}

// Set request field `path` (a proto field name, dotted path, or unique leaf).
void setField(gpb::Message* msg, const std::string& path, const std::string& val) {
  const auto* desc = msg->GetDescriptor();
  auto dot = path.find('.');
  if (dot == std::string::npos) {
    if (const auto* f = desc->FindFieldByName(path)) {
      if (f->cpp_type() != gpb::FieldDescriptor::CPPTYPE_MESSAGE && !f->is_repeated()) {
        setScalar(msg, f, val);
        return;
      }
    }
    // Bare name not a top-level scalar: try a unique nested leaf.
    std::string found;
    int count = 0;
    findLeafPath(desc, path, "", found, count);
    if (count == 1) { setField(msg, found, val); return; }
    return;  // unknown / ambiguous: ignore (caller may log)
  }
  std::string head = path.substr(0, dot), rest = path.substr(dot + 1);
  const auto* f = desc->FindFieldByName(head);
  if (!f || f->cpp_type() != gpb::FieldDescriptor::CPPTYPE_MESSAGE || f->is_repeated()) return;
  setField(msg->GetReflection()->MutableMessage(msg, f), rest, val);
}

// ---- proto field -> pvxs TypeCode ----

pvxs::TypeCode scalarTC(const gpb::FieldDescriptor* f) {
  using TC = pvxs::TypeCode;
  switch (f->cpp_type()) {
    case gpb::FieldDescriptor::CPPTYPE_INT32:  return TC::Int32;
    case gpb::FieldDescriptor::CPPTYPE_INT64:  return TC::Int64;
    case gpb::FieldDescriptor::CPPTYPE_UINT32: return TC::UInt32;
    case gpb::FieldDescriptor::CPPTYPE_UINT64: return TC::UInt64;
    case gpb::FieldDescriptor::CPPTYPE_DOUBLE: return TC::Float64;
    case gpb::FieldDescriptor::CPPTYPE_FLOAT:  return TC::Float32;
    case gpb::FieldDescriptor::CPPTYPE_BOOL:   return TC::Bool;
    case gpb::FieldDescriptor::CPPTYPE_ENUM:   return TC::Int32;
    case gpb::FieldDescriptor::CPPTYPE_STRING: return TC::String;
    default: throw std::runtime_error("unsupported scalar field " + f->name());
  }
}

pvxs::TypeCode arrayTC(pvxs::TypeCode s) {
  using TC = pvxs::TypeCode;
  switch (s.code) {
    case TC::Int32:   return TC::Int32A;
    case TC::Int64:   return TC::Int64A;
    case TC::UInt32:  return TC::UInt32A;
    case TC::UInt64:  return TC::UInt64A;
    case TC::Float32: return TC::Float32A;
    case TC::Float64: return TC::Float64A;
    case TC::Bool:    return TC::BoolA;
    case TC::String:  return TC::StringA;
    default: throw std::runtime_error("unsupported array element type");
  }
}

// Build pvxs struct members mirroring a protobuf message descriptor.
std::vector<pvxs::Member> buildMembers(const gpb::Descriptor* d) {
  std::vector<pvxs::Member> out;
  for (int i = 0; i < d->field_count(); ++i) {
    const auto* f = d->field(i);
    if (f->cpp_type() == gpb::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (f->is_repeated())
        throw std::runtime_error("repeated message field unsupported: " + f->name());
      out.emplace_back(pvxs::TypeCode::Struct, f->name(), buildMembers(f->message_type()));
    } else {
      pvxs::TypeCode tc = scalarTC(f);
      out.emplace_back(f->is_repeated() ? arrayTC(tc) : tc, f->name());
    }
  }
  return out;
}

// dst is taken by value: a pvxs::Value is a handle into the tree, so assigning
// into the copy mutates the same field (and lets us pass the v[name] temporary).
template <typename T, typename Get>
void fillArray(pvxs::Value dst, const gpb::Message& msg, const gpb::FieldDescriptor* f,
               const gpb::Reflection* r, int n, Get get) {
  pvxs::shared_array<T> arr(n);
  for (int k = 0; k < n; ++k) arr[k] = static_cast<T>((r->*get)(msg, f, k));
  dst = arr.freeze();
}

// Populate a pvxs Value (built from the same descriptor) from a protobuf message.
void populate(pvxs::Value v, const gpb::Message& msg) {
  const auto* d = msg.GetDescriptor();
  const auto* r = msg.GetReflection();
  for (int i = 0; i < d->field_count(); ++i) {
    const auto* f = d->field(i);
    const std::string& nm = f->name();
    if (f->cpp_type() == gpb::FieldDescriptor::CPPTYPE_MESSAGE) {
      populate(v[nm], r->GetMessage(msg, f));
      continue;
    }
    if (f->is_repeated()) {
      int n = r->FieldSize(msg, f);
      switch (f->cpp_type()) {
        case gpb::FieldDescriptor::CPPTYPE_DOUBLE: fillArray<double>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedDouble); break;
        case gpb::FieldDescriptor::CPPTYPE_FLOAT:  fillArray<float>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedFloat); break;
        case gpb::FieldDescriptor::CPPTYPE_INT64:  fillArray<int64_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedInt64); break;
        case gpb::FieldDescriptor::CPPTYPE_UINT64: fillArray<uint64_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedUInt64); break;
        case gpb::FieldDescriptor::CPPTYPE_INT32:  fillArray<int32_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedInt32); break;
        case gpb::FieldDescriptor::CPPTYPE_UINT32: fillArray<uint32_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedUInt32); break;
        case gpb::FieldDescriptor::CPPTYPE_ENUM:   fillArray<int32_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedEnumValue); break;
        case gpb::FieldDescriptor::CPPTYPE_BOOL:   fillArray<bool>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedBool); break;
        case gpb::FieldDescriptor::CPPTYPE_STRING: {
          pvxs::shared_array<std::string> arr(n);
          for (int k = 0; k < n; ++k) arr[k] = r->GetRepeatedString(msg, f, k);
          v[nm] = arr.freeze();
          break;
        }
        default: break;
      }
      continue;
    }
    switch (f->cpp_type()) {
      case gpb::FieldDescriptor::CPPTYPE_DOUBLE: v[nm] = r->GetDouble(msg, f); break;
      case gpb::FieldDescriptor::CPPTYPE_FLOAT:  v[nm] = r->GetFloat(msg, f); break;
      case gpb::FieldDescriptor::CPPTYPE_INT64:  v[nm] = static_cast<int64_t>(r->GetInt64(msg, f)); break;
      case gpb::FieldDescriptor::CPPTYPE_UINT64: v[nm] = static_cast<uint64_t>(r->GetUInt64(msg, f)); break;
      case gpb::FieldDescriptor::CPPTYPE_INT32:  v[nm] = static_cast<int32_t>(r->GetInt32(msg, f)); break;
      case gpb::FieldDescriptor::CPPTYPE_UINT32: v[nm] = static_cast<uint32_t>(r->GetUInt32(msg, f)); break;
      case gpb::FieldDescriptor::CPPTYPE_ENUM:   v[nm] = r->GetEnumValue(msg, f); break;
      case gpb::FieldDescriptor::CPPTYPE_BOOL:   v[nm] = r->GetBool(msg, f); break;
      case gpb::FieldDescriptor::CPPTYPE_STRING: v[nm] = r->GetString(msg, f); break;
      default: break;
    }
  }
}

// ---- ByteBuffer <-> Message ----

grpc::ByteBuffer toByteBuffer(const gpb::Message& m) {
  std::string s;
  m.SerializeToString(&s);
  grpc::Slice slice(s);
  return grpc::ByteBuffer(&slice, 1);
}

bool fromByteBuffer(const grpc::ByteBuffer& buf, gpb::Message& m) {
  std::vector<grpc::Slice> slices;
  if (!buf.Dump(&slices).ok()) return false;
  std::string s;
  for (const auto& sl : slices) s.append(reinterpret_cast<const char*>(sl.begin()), sl.size());
  return m.ParseFromString(s);
}

}  // namespace

// ---- Impl ----

struct GrpcBridge::Impl {
  struct ServiceCtx {
    std::unique_ptr<gpb::SimpleDescriptorDatabase> db;
    std::unique_ptr<gpb::DescriptorPool> pool;
    std::unique_ptr<gpb::DynamicMessageFactory> factory;
    const gpb::ServiceDescriptor* service = nullptr;
    std::map<std::string, const gpb::MethodDescriptor*> methods;
  };

  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<refl::ServerReflection::Stub> reflStub;
  std::unique_ptr<grpc::GenericStub> genericStub;
  std::map<std::string, ServiceCtx> services;  // by fully-qualified service name
};

GrpcBridge::GrpcBridge(const std::string& endpoint)
    : impl_(std::make_unique<Impl>()), endpoint_(endpoint) {
  impl_->channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  impl_->reflStub = refl::ServerReflection::NewStub(impl_->channel);
  impl_->genericStub = std::make_unique<grpc::GenericStub>(impl_->channel);
}

GrpcBridge::~GrpcBridge() = default;

std::vector<BridgeMethod> GrpcBridge::discover(const std::string& service) {
  grpc::ClientContext ctx;
  auto stream = impl_->reflStub->ServerReflectionInfo(&ctx);

  refl::ServerReflectionRequest req;
  req.set_file_containing_symbol(service);
  if (!stream->Write(req)) throw std::runtime_error("reflection write failed for " + endpoint_);

  refl::ServerReflectionResponse resp;
  if (!stream->Read(&resp))
    throw std::runtime_error("reflection read failed for " + endpoint_);
  if (resp.has_error_response())
    throw std::runtime_error("reflection error for " + service + ": " +
                             resp.error_response().error_message());
  if (!resp.has_file_descriptor_response())
    throw std::runtime_error("reflection returned no descriptors for " + service);

  Impl::ServiceCtx sc;
  sc.db = std::make_unique<gpb::SimpleDescriptorDatabase>();
  for (const auto& bytes : resp.file_descriptor_response().file_descriptor_proto()) {
    gpb::FileDescriptorProto fdp;
    if (!fdp.ParseFromString(bytes)) throw std::runtime_error("bad FileDescriptorProto");
    sc.db->Add(fdp);
  }
  stream->WritesDone();
  ctx.TryCancel();  // single round-trip; no more reads needed

  sc.pool = std::make_unique<gpb::DescriptorPool>(sc.db.get());
  sc.factory = std::make_unique<gpb::DynamicMessageFactory>();
  sc.service = sc.pool->FindServiceByName(service);
  if (!sc.service) throw std::runtime_error("service not found via reflection: " + service);

  std::vector<BridgeMethod> out;
  for (int i = 0; i < sc.service->method_count(); ++i) {
    const auto* m = sc.service->method(i);
    sc.methods[m->name()] = m;
    out.push_back(BridgeMethod{service, m->name()});
  }
  impl_->services[service] = std::move(sc);
  return out;
}

pvxs::Value GrpcBridge::call(const BridgeMethod& method,
                             const std::map<std::string, std::string>& fields) {
  auto sit = impl_->services.find(method.service);
  if (sit == impl_->services.end()) throw std::runtime_error("service not discovered: " + method.service);
  auto& sc = sit->second;
  auto mit = sc.methods.find(method.method);
  if (mit == sc.methods.end()) throw std::runtime_error("method not found: " + method.method);
  const auto* md = mit->second;

  // Build request message and apply fields.
  std::unique_ptr<gpb::Message> reqMsg(sc.factory->GetPrototype(md->input_type())->New());
  for (const auto& [name, val] : fields) setField(reqMsg.get(), name, val);

  // Generic unary call: /<service>/<method>
  const std::string path = "/" + method.service + "/" + method.method;
  grpc::ByteBuffer reqBuf = toByteBuffer(*reqMsg);
  grpc::ClientContext ctx;
  grpc::CompletionQueue cq;
  grpc::ByteBuffer repBuf;
  grpc::Status status;

  std::unique_ptr<grpc::GenericClientAsyncResponseReader> rpc(
      impl_->genericStub->PrepareUnaryCall(&ctx, path, reqBuf, &cq));
  rpc->StartCall();
  rpc->Finish(&repBuf, &status, reinterpret_cast<void*>(1));
  void* tag = nullptr;
  bool ok = false;
  cq.Next(&tag, &ok);

  if (!status.ok())
    throw std::runtime_error("gRPC " + method.method + " failed: " + status.error_message() +
                             " (code " + std::to_string(static_cast<int>(status.error_code())) + ")");

  std::unique_ptr<gpb::Message> repMsg(sc.factory->GetPrototype(md->output_type())->New());
  if (!fromByteBuffer(repBuf, *repMsg)) throw std::runtime_error("failed to parse gRPC reply");

  pvxs::TypeDef def(pvxs::TypeCode::Struct, md->output_type()->full_name(),
                    buildMembers(md->output_type()));
  pvxs::Value v = def.create();
  populate(v, *repMsg);
  return v;
}

}  // namespace redis_pvxs_ioc

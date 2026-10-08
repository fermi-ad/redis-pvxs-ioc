#include "redis_pvxs_ioc/grpc_bridge.h"

#include <chrono>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

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

constexpr size_t kMaxDepth = 32, kMaxFields = 4096, kMaxArguments = 1024;
constexpr size_t kMaxDescriptorBytes = 4u * 1024u * 1024u;

[[noreturn]] void badField(const gpb::FieldDescriptor* field, const std::string& reason) {
  throw std::runtime_error(reason + " for field " + field->full_name());
}

template<class T> T parseInteger(const std::string& text, const gpb::FieldDescriptor* field) {
  if (text.empty() || std::isspace(static_cast<unsigned char>(text.front()))) badField(field, "invalid integer");
  errno = 0;
  char* end = nullptr;
  if constexpr (std::is_unsigned_v<T>) {
    if (text.front() == '-') badField(field, "negative unsigned integer");
    const auto value = std::strtoull(text.c_str(), &end, 0);
    if (errno == ERANGE || end != text.c_str() + text.size() || value > std::numeric_limits<T>::max())
      badField(field, "invalid or out-of-range integer");
    return static_cast<T>(value);
  } else {
    const auto value = std::strtoll(text.c_str(), &end, 0);
    if (errno == ERANGE || end != text.c_str() + text.size() ||
        value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max())
      badField(field, "invalid or out-of-range integer");
    return static_cast<T>(value);
  }
}

template<class T> T parseReal(const std::string& text, const gpb::FieldDescriptor* field) {
  if (text.empty() || std::isspace(static_cast<unsigned char>(text.front()))) badField(field, "invalid real number");
  errno = 0;
  char* end = nullptr;
  T value;
  if constexpr (std::is_same_v<T, float>) value = std::strtof(text.c_str(), &end);
  else value = std::strtod(text.c_str(), &end);
  if (errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(value))
    badField(field, "invalid or out-of-range real number");
  return value;
}

// ---- string -> proto field ----

void setScalar(gpb::Message* msg, const gpb::FieldDescriptor* f, const std::string& v) {
  const auto* r = msg->GetReflection();
  switch (f->cpp_type()) {
    case gpb::FieldDescriptor::CPPTYPE_INT32:  r->SetInt32(msg, f, parseInteger<int32_t>(v, f)); break;
    case gpb::FieldDescriptor::CPPTYPE_INT64:  r->SetInt64(msg, f, parseInteger<int64_t>(v, f)); break;
    case gpb::FieldDescriptor::CPPTYPE_UINT32: r->SetUInt32(msg, f, parseInteger<uint32_t>(v, f)); break;
    case gpb::FieldDescriptor::CPPTYPE_UINT64: r->SetUInt64(msg, f, parseInteger<uint64_t>(v, f)); break;
    case gpb::FieldDescriptor::CPPTYPE_DOUBLE: r->SetDouble(msg, f, parseReal<double>(v, f)); break;
    case gpb::FieldDescriptor::CPPTYPE_FLOAT:  r->SetFloat(msg, f, parseReal<float>(v, f)); break;
    case gpb::FieldDescriptor::CPPTYPE_BOOL: {
      std::string text;
      for (unsigned char c : v) text.push_back(static_cast<char>(std::tolower(c)));
      if (text == "1" || text == "true" || text == "yes") r->SetBool(msg, f, true);
      else if (text == "0" || text == "false" || text == "no") r->SetBool(msg, f, false);
      else badField(f, "invalid boolean");
      break;
    }
    case gpb::FieldDescriptor::CPPTYPE_STRING: r->SetString(msg, f, v); break;
    case gpb::FieldDescriptor::CPPTYPE_ENUM: {
      const auto* value = f->enum_type()->FindValueByName(v);
      if (!value && !v.empty() && (std::isdigit(static_cast<unsigned char>(v.front())) || v.front() == '-' || v.front() == '+'))
        value = f->enum_type()->FindValueByNumber(parseInteger<int32_t>(v, f));
      if (!value) badField(f, "unknown enum value");
      r->SetEnum(msg, f, value);
      break;
    }
    default: throw std::runtime_error("cannot set field " + f->name());
  }
}

// Search the descriptor tree for a singular leaf field named `leaf`; if exactly
// one exists, write its dotted path to `out`.
void findLeafPath(const gpb::Descriptor* d, const std::string& leaf,
                  const std::string& prefix, std::string& out, int& count,
                  size_t& remaining, std::set<const gpb::Descriptor*> ancestors = {}) {
  if (ancestors.size() >= kMaxDepth || !ancestors.insert(d).second)
    throw std::runtime_error("recursive or too deeply nested request schema: " + d->full_name());
  for (int i = 0; i < d->field_count(); ++i) {
    if (remaining == 0) throw std::runtime_error("request schema exceeds field search limit");
    --remaining;
    const auto* f = d->field(i);
    if (f->is_repeated()) continue;
    if (f->cpp_type() == gpb::FieldDescriptor::CPPTYPE_MESSAGE) {
      findLeafPath(f->message_type(), leaf, prefix + f->name() + ".", out, count, remaining, ancestors);
    } else if (f->name() == leaf) {
      out = prefix + f->name();
      ++count;
    }
  }
}

// Resolve aliases before merging call arguments over defaults. Empty means
// absent; a known unsupported field or ambiguous alias is always an error.
std::string canonicalPath(const gpb::Descriptor* desc, const std::string& path) {
  if (path.empty() || path.size() > 4096) throw std::runtime_error("invalid RPC field path");
  auto dot = path.find('.');
  if (dot == std::string::npos) {
    if (const auto* f = desc->FindFieldByName(path)) {
      if (f->cpp_type() == gpb::FieldDescriptor::CPPTYPE_MESSAGE || f->is_repeated())
        badField(f, "RPC arguments require singular scalar fields");
      return path;
    }
    // Bare name not a top-level scalar: try a unique nested leaf.
    std::string found;
    int count = 0;
    size_t remaining = kMaxFields;
    findLeafPath(desc, path, "", found, count, remaining);
    if (count > 1) throw std::runtime_error("ambiguous RPC field: " + path);
    return count == 1 ? found : std::string{};
  }
  size_t begin = 0, depth = 0;
  for (;;) {
    if (++depth > kMaxDepth) throw std::runtime_error("RPC field path exceeds nesting limit");
    const auto end = path.find('.', begin);
    const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
    if (part.empty()) throw std::runtime_error("invalid RPC field path: " + path);
    const auto* field = desc->FindFieldByName(part);
    if (!field) return {};
    if (field->is_repeated()) badField(field, "repeated request fields are unsupported");
    if (end == std::string::npos) {
      if (field->cpp_type() == gpb::FieldDescriptor::CPPTYPE_MESSAGE)
        badField(field, "RPC arguments require scalar leaves");
      return path;
    }
    if (field->cpp_type() != gpb::FieldDescriptor::CPPTYPE_MESSAGE) return {};
    desc = field->message_type(); begin = end + 1;
  }
}

void setField(gpb::Message* msg, const std::string& path, const std::string& value) {
  size_t begin = 0;
  for (;;) {
    const auto end = path.find('.', begin);
    const auto* field = msg->GetDescriptor()->FindFieldByName(path.substr(begin, end == std::string::npos ? end : end - begin));
    if (!field) throw std::runtime_error("unknown RPC field: " + path);
    const auto* reflection = msg->GetReflection();
    if (const auto* group = field->containing_oneof()) {
      const auto* prior = reflection->GetOneofFieldDescriptor(*msg, group);
      if (prior && prior != field) badField(field, "conflicting oneof arguments");
    }
    if (end == std::string::npos) { setScalar(msg, field, value); return; }
    msg = reflection->MutableMessage(msg, field); begin = end + 1;
  }
}

GrpcBridge::Fields canonicalFields(const gpb::Descriptor* descriptor, const GrpcBridge::Fields& input) {
  if (input.size() > kMaxArguments) throw std::runtime_error("too many RPC arguments");
  GrpcBridge::Fields result;
  for (const auto& field : input) {
    const auto path = canonicalPath(descriptor, field.first);
    if (path.empty()) throw std::runtime_error("unknown RPC field: " + field.first);
    if (!result.emplace(path, field.second).second) throw std::runtime_error("RPC field supplied twice: " + path);
  }
  return result;
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
std::vector<pvxs::Member> buildMembers(const gpb::Descriptor* d, size_t& remaining,
                                      std::set<const gpb::Descriptor*> ancestors = {}) {
  if (ancestors.size() >= kMaxDepth || !ancestors.insert(d).second)
    throw std::runtime_error("recursive or too deeply nested reply schema: " + d->full_name());
  std::vector<pvxs::Member> out;
  for (int i = 0; i < d->field_count(); ++i) {
    if (remaining == 0) throw std::runtime_error("reply schema exceeds field limit");
    --remaining;
    const auto* f = d->field(i);
    if (f->cpp_type() == gpb::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (f->is_repeated())
        throw std::runtime_error("repeated message field unsupported: " + f->name());
      out.emplace_back(pvxs::TypeCode::Struct, f->name(), buildMembers(f->message_type(), remaining, ancestors));
    } else {
      pvxs::TypeCode tc = scalarTC(f);
      out.emplace_back(f->is_repeated() ? arrayTC(tc) : tc, f->name());
    }
  }
  return out;
}

// dst is taken by value: a pvxs::Value is a handle into the tree, so assigning
// into the copy mutates the same field (and lets us pass the v[name] temporary).
void chargePayload(size_t& remaining, size_t count, size_t width = 1) {
  if (count > remaining / width) throw std::runtime_error("decoded RPC reply exceeds payload limit");
  remaining -= count * width;
}

template <typename T, typename Get>
void fillArray(pvxs::Value dst, const gpb::Message& msg, const gpb::FieldDescriptor* f,
               const gpb::Reflection* r, int n, Get get, size_t& remaining) {
  chargePayload(remaining, n, sizeof(T));
  pvxs::shared_array<T> arr(n);
  for (int k = 0; k < n; ++k) arr[k] = static_cast<T>((r->*get)(msg, f, k));
  dst = arr.freeze();
}

// Populate a pvxs Value (built from the same descriptor) from a protobuf message.
void populate(pvxs::Value v, const gpb::Message& msg, size_t& remaining) {
  const auto* d = msg.GetDescriptor();
  const auto* r = msg.GetReflection();
  for (int i = 0; i < d->field_count(); ++i) {
    const auto* f = d->field(i);
    const std::string& nm = f->name();
    if (f->cpp_type() == gpb::FieldDescriptor::CPPTYPE_MESSAGE) {
      populate(v[nm], r->GetMessage(msg, f), remaining);
      continue;
    }
    if (f->is_repeated()) {
      int n = r->FieldSize(msg, f);
      switch (f->cpp_type()) {
        case gpb::FieldDescriptor::CPPTYPE_DOUBLE: fillArray<double>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedDouble, remaining); break;
        case gpb::FieldDescriptor::CPPTYPE_FLOAT:  fillArray<float>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedFloat, remaining); break;
        case gpb::FieldDescriptor::CPPTYPE_INT64:  fillArray<int64_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedInt64, remaining); break;
        case gpb::FieldDescriptor::CPPTYPE_UINT64: fillArray<uint64_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedUInt64, remaining); break;
        case gpb::FieldDescriptor::CPPTYPE_INT32:  fillArray<int32_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedInt32, remaining); break;
        case gpb::FieldDescriptor::CPPTYPE_UINT32: fillArray<uint32_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedUInt32, remaining); break;
        case gpb::FieldDescriptor::CPPTYPE_ENUM:   fillArray<int32_t>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedEnumValue, remaining); break;
        case gpb::FieldDescriptor::CPPTYPE_BOOL:   fillArray<bool>(v[nm], msg, f, r, n, &gpb::Reflection::GetRepeatedBool, remaining); break;
        case gpb::FieldDescriptor::CPPTYPE_STRING: {
          chargePayload(remaining, n, sizeof(std::string));
          pvxs::shared_array<std::string> arr(n);
          for (int k = 0; k < n; ++k) {
            auto item = r->GetRepeatedString(msg, f, k);
            chargePayload(remaining, item.size()); arr[k] = std::move(item);
          }
          v[nm] = arr.freeze();
          break;
        }
        default: break;
      }
      continue;
    }
    size_t width = 8;
    switch (f->cpp_type()) {
    case gpb::FieldDescriptor::CPPTYPE_BOOL: width = 1; break;
    case gpb::FieldDescriptor::CPPTYPE_INT32:
    case gpb::FieldDescriptor::CPPTYPE_UINT32:
    case gpb::FieldDescriptor::CPPTYPE_ENUM:
    case gpb::FieldDescriptor::CPPTYPE_FLOAT: width = 4; break;
    case gpb::FieldDescriptor::CPPTYPE_STRING: width = r->GetString(msg, f).size(); break;
    default: break;
    }
    chargePayload(remaining, width);
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

grpc::ByteBuffer toByteBuffer(const gpb::Message& m, size_t maximum) {
  if (!m.IsInitialized()) throw std::runtime_error("missing required RPC fields: " + m.InitializationErrorString());
  if (m.ByteSizeLong() > maximum) throw std::runtime_error("RPC request exceeds payload limit");
  std::string s;
  if (!m.SerializeToString(&s)) throw std::runtime_error("failed to serialize RPC request");
  grpc::Slice slice(s);
  return grpc::ByteBuffer(&slice, 1);
}

bool fromByteBuffer(const grpc::ByteBuffer& buf, gpb::Message& m, size_t maximum) {
  if (buf.Length() > maximum) throw std::runtime_error("RPC reply exceeds payload limit");
  grpc::Slice slice;
  if (!buf.DumpToSingleSlice(&slice).ok()) return false;
  return m.ParseFromArray(slice.begin(), static_cast<int>(slice.size()));
}

}  // namespace

// ---- Impl ----

struct GrpcCallControl::Impl {
  grpc::ClientContext context;
  std::atomic<bool> cancelled{false};
};
GrpcCallControl::GrpcCallControl() : impl_(std::make_unique<Impl>()) {}
GrpcCallControl::~GrpcCallControl() = default;
void GrpcCallControl::cancel() {
  impl_->cancelled = true;
  impl_->context.TryCancel();
}

struct GrpcBridge::Impl {
  struct ServiceCtx {
    std::unique_ptr<gpb::SimpleDescriptorDatabase> db;
    std::unique_ptr<gpb::DescriptorPool> pool;
    std::unique_ptr<gpb::DynamicMessageFactory> factory;
    const gpb::ServiceDescriptor* service = nullptr;
    std::map<std::string, const gpb::MethodDescriptor*> methods;
    std::map<std::string, pvxs::TypeDef> replies;
  };

  std::shared_ptr<grpc::Channel> channel;
  std::shared_ptr<grpc::Channel> reflectionChannel;
  std::unique_ptr<refl::ServerReflection::Stub> reflStub;
  std::unique_ptr<grpc::GenericStub> genericStub;
  std::map<std::string, ServiceCtx> services;  // by fully-qualified service name
  uint32_t discoveryTimeoutMs = 3000, timeoutMs = 10000;
  size_t maxMessageBytes = 0;
};

GrpcBridge::GrpcBridge(const std::string& endpoint, uint32_t discoveryTimeoutMs,
                       uint32_t timeoutMs, size_t maxMessageBytes)
    : impl_(std::make_unique<Impl>()), endpoint_(endpoint) {
  if (!discoveryTimeoutMs || !timeoutMs || !maxMessageBytes || maxMessageBytes > static_cast<size_t>(std::numeric_limits<int>::max()))
    throw std::invalid_argument("RPC deadlines and payload limit must be positive and supported");
  impl_->discoveryTimeoutMs = discoveryTimeoutMs; impl_->timeoutMs = timeoutMs;
  impl_->maxMessageBytes = maxMessageBytes;
  grpc::ChannelArguments arguments;
  arguments.SetMaxReceiveMessageSize(static_cast<int>(maxMessageBytes));
  arguments.SetMaxSendMessageSize(static_cast<int>(maxMessageBytes));
  arguments.SetInt("grpc.enable_retries", 0);
  impl_->channel = grpc::CreateCustomChannel(endpoint, grpc::InsecureChannelCredentials(), arguments);
  arguments.SetMaxReceiveMessageSize(static_cast<int>(kMaxDescriptorBytes));
  arguments.SetMaxSendMessageSize(static_cast<int>(kMaxDescriptorBytes));
  impl_->reflectionChannel = grpc::CreateCustomChannel(endpoint, grpc::InsecureChannelCredentials(), arguments);
  impl_->reflStub = refl::ServerReflection::NewStub(impl_->reflectionChannel);
  impl_->genericStub = std::make_unique<grpc::GenericStub>(impl_->channel);
}

GrpcBridge::~GrpcBridge() = default;
size_t GrpcBridge::maxMessageBytes() const { return impl_->maxMessageBytes; }

std::vector<BridgeMethod> GrpcBridge::discover(const std::string& service,
                                              std::shared_ptr<GrpcCallControl> control) {
  if (!control) control = std::make_shared<GrpcCallControl>();
  auto& ctx = control->impl_->context;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(impl_->discoveryTimeoutMs));
  ctx.set_wait_for_ready(true);
  auto stream = impl_->reflStub->ServerReflectionInfo(&ctx);
  struct FinishReflection {
    grpc::ClientContext& context;
    decltype(stream)& call;
    ~FinishReflection() { context.TryCancel(); call->Finish(); }
  } finish{ctx, stream};

  refl::ServerReflectionRequest req;
  req.set_file_containing_symbol(service);
  if (!stream->Write(req)) throw RpcDiscoveryUnavailable("reflection write failed for " + endpoint_);

  refl::ServerReflectionResponse resp;
  if (!stream->Read(&resp))
    throw RpcDiscoveryUnavailable("reflection read failed for " + endpoint_);
  if (resp.has_error_response())
    throw RpcDiscoveryUnavailable("reflection error for " + service + ": " +
                             resp.error_response().error_message());
  if (!resp.has_file_descriptor_response())
    throw std::runtime_error("reflection returned no descriptors for " + service);
  if (resp.ByteSizeLong() > kMaxDescriptorBytes ||
      resp.file_descriptor_response().file_descriptor_proto_size() > 256)
    throw std::runtime_error("reflection descriptor response exceeds limits for " + service);

  Impl::ServiceCtx sc;
  sc.db = std::make_unique<gpb::SimpleDescriptorDatabase>();
  for (const auto& bytes : resp.file_descriptor_response().file_descriptor_proto()) {
    gpb::FileDescriptorProto fdp;
    if (!fdp.ParseFromString(bytes)) throw std::runtime_error("bad FileDescriptorProto");
    if (!sc.db->Add(fdp))
      throw std::runtime_error("failed to add reflected descriptor " + fdp.name() +
                               " for " + service);
  }
  stream->WritesDone();
  ctx.TryCancel();  // single round-trip; no more reads needed

  sc.pool = std::make_unique<gpb::DescriptorPool>(sc.db.get());
  // Bind the factory to the same pool the descriptors were reflected into, so
  // GetPrototype()/New() resolve nested and extension types from that pool.
  sc.factory = std::make_unique<gpb::DynamicMessageFactory>(sc.pool.get());
  sc.service = sc.pool->FindServiceByName(service);
  if (!sc.service) throw RpcDiscoveryUnavailable("service not found via reflection: " + service);

  std::vector<BridgeMethod> out;
  for (int i = 0; i < sc.service->method_count(); ++i) {
    if (i >= 256) throw std::runtime_error("reflected service exceeds method limit");
    const auto* m = sc.service->method(i);
    if (m->client_streaming() || m->server_streaming())
      throw std::runtime_error("streaming RPC method is unsupported: " + m->full_name());
    size_t remaining = kMaxFields;
    sc.replies.emplace(m->name(), pvxs::TypeDef(pvxs::TypeCode::Struct, m->output_type()->full_name(),
                                               buildMembers(m->output_type(), remaining)));
    sc.methods[m->name()] = m;
    out.push_back(BridgeMethod{service, m->name()});
  }
  impl_->services[service] = std::move(sc);
  return out;
}

GrpcBridge::MethodDefaults GrpcBridge::prepareDefaults(const std::string& service,
    const Fields& shared, const MethodDefaults& methods) {
  auto& context = impl_->services.at(service);
  if (shared.size() > kMaxArguments) throw std::runtime_error("too many shared RPC defaults");
  for (const auto& method : methods)
    if (!context.methods.count(method.first)) throw std::runtime_error("unknown RPC method in method_defaults: " + method.first);
  MethodDefaults result;
  std::set<std::string> used;
  for (const auto& method : context.methods) {
    const auto* descriptor = method.second->input_type();
    Fields values;
    for (const auto& field : shared) {
      const auto path = canonicalPath(descriptor, field.first);
      if (path.empty()) continue;
      if (!values.emplace(path, field.second).second) throw std::runtime_error("shared RPC default supplied twice: " + path);
      used.insert(field.first);
    }
    const auto specific = methods.find(method.first);
    if (specific != methods.end())
      for (const auto& field : canonicalFields(descriptor, specific->second)) values[field.first] = field.second;
    if (values.size() > kMaxArguments) throw std::runtime_error("too many combined RPC defaults");
    std::unique_ptr<gpb::Message> message(context.factory->GetPrototype(descriptor)->New());
    for (const auto& field : values) setField(message.get(), field.first, field.second);
    if (message->ByteSizeLong() > impl_->maxMessageBytes) throw std::runtime_error("RPC defaults exceed payload limit");
    result.emplace(method.first, std::move(values));
  }
  for (const auto& field : shared)
    if (!used.count(field.first)) throw std::runtime_error("unknown shared RPC default: " + field.first);
  return result;
}

pvxs::Value GrpcBridge::call(const BridgeMethod& method,
                             const Fields& fields, const Fields& defaults,
                             std::chrono::steady_clock::time_point deadline,
                             const std::shared_ptr<GrpcCallControl>& control,
                             const std::function<bool()>& beforeDispatch) {
  auto sit = impl_->services.find(method.service);
  if (sit == impl_->services.end()) throw std::runtime_error("service not discovered: " + method.service);
  auto& sc = sit->second;
  auto mit = sc.methods.find(method.method);
  if (mit == sc.methods.end()) throw std::runtime_error("method not found: " + method.method);
  const auto* md = mit->second;

  // Build request message and apply fields.
  std::unique_ptr<gpb::Message> reqMsg(sc.factory->GetPrototype(md->input_type())->New());
  auto merged = canonicalFields(md->input_type(), defaults);
  for (const auto& field : canonicalFields(md->input_type(), fields)) merged[field.first] = field.second;
  if (merged.size() > kMaxArguments) throw std::runtime_error("too many combined RPC arguments");
  for (const auto& field : merged) setField(reqMsg.get(), field.first, field.second);

  // Generic unary call: /<service>/<method>
  const std::string path = "/" + method.service + "/" + method.method;
  grpc::ByteBuffer reqBuf = toByteBuffer(*reqMsg, impl_->maxMessageBytes);
  auto& ctx = control->impl_->context;
  const auto now = std::chrono::steady_clock::now();
  deadline = std::min(deadline, now + std::chrono::milliseconds(impl_->timeoutMs));
  const auto timeRemaining = deadline - now;
  if (timeRemaining <= std::chrono::steady_clock::duration::zero())
    throw std::runtime_error("total RPC deadline exceeded before dispatch");
  ctx.set_deadline(std::chrono::system_clock::now() + timeRemaining);
  if (control->impl_->cancelled || !beforeDispatch() || control->impl_->cancelled)
    throw std::runtime_error("RPC cancelled or no longer authorized at dispatch; not retried");
  grpc::CompletionQueue cq;
  grpc::ByteBuffer repBuf;
  grpc::Status status;

  void* const kFinishTag = reinterpret_cast<void*>(1);
  std::unique_ptr<grpc::GenericClientAsyncResponseReader> rpc(
      impl_->genericStub->PrepareUnaryCall(&ctx, path, reqBuf, &cq));
  rpc->StartCall();
  rpc->Finish(&repBuf, &status, kFinishTag);

  // Wait for the single Finish completion, then shut the queue down and drain it
  // so cq destructs cleanly. The deadline above guarantees Next() returns even if
  // the backend stalls (the op then completes with DEADLINE_EXCEEDED).
  void* tag = nullptr;
  bool ok = false;
  const bool gotEvent = cq.Next(&tag, &ok);
  const bool finished = gotEvent && tag == kFinishTag && ok;
  cq.Shutdown();
  while (cq.Next(&tag, &ok)) {}

  if (!finished)
    throw std::runtime_error("gRPC " + method.method +
                             " did not complete cleanly for " + endpoint_);

  if (!status.ok())
    throw std::runtime_error("gRPC " + method.method + " failed: " + status.error_message() +
                             " (code " + std::to_string(static_cast<int>(status.error_code())) + ")");

  std::unique_ptr<gpb::Message> repMsg(sc.factory->GetPrototype(md->output_type())->New());
  if (!fromByteBuffer(repBuf, *repMsg, impl_->maxMessageBytes)) throw std::runtime_error("failed to parse gRPC reply");

  pvxs::Value v = sc.replies.at(method.method).create();
  auto remaining = impl_->maxMessageBytes;
  populate(v, *repMsg, remaining);
  return v;
}

}  // namespace redis_pvxs_ioc

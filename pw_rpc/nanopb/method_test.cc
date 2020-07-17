// Copyright 2020 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_rpc/internal/method.h"

#include <array>

#include "gtest/gtest.h"
#include "pb_encode.h"
#include "pw_rpc/internal/service.h"
#include "pw_rpc/server_context.h"
#include "pw_rpc_private/internal_test_utils.h"
#include "pw_rpc_test_protos/test.pb.h"

namespace pw::rpc::internal {
namespace {

using std::byte;

#define ENCODE_PB(proto, init, result) \
  _ENCODE_PB_EXPAND(proto, init, result, __LINE__)

#define _ENCODE_PB_EXPAND(proto, init, result, unique) \
  _ENCODE_PB_IMPL(proto, init, result, unique)

#define _ENCODE_PB_IMPL(proto, init, result, unique)              \
  std::array<pb_byte_t, 2 * sizeof(proto)> _pb_buffer_##unique{}; \
  const std::span result =                                        \
      EncodeProtobuf<proto, proto##_fields>(proto init, _pb_buffer_##unique)

template <typename T, auto fields>
std::span<const byte> EncodeProtobuf(const T& protobuf,
                                     std::span<pb_byte_t> buffer) {
  auto output = pb_ostream_from_buffer(buffer.data(), buffer.size());
  EXPECT_TRUE(pb_encode(&output, fields, &protobuf));
  return std::as_bytes(buffer.first(output.bytes_written));
}

class FakeGeneratedService : public Service {
 public:
  constexpr FakeGeneratedService(uint32_t id) : Service(id, kMethods) {}

  static Status DoNothing(ServerContext&,
                          const pw_rpc_test_Empty&,
                          pw_rpc_test_Empty&);

  static Status AddFive(ServerContext&,
                        const pw_rpc_test_TestRequest&,
                        pw_rpc_test_TestResponse&);

  static void StartStream(ServerContext&,
                          const pw_rpc_test_TestRequest&,
                          ServerWriter<pw_rpc_test_TestResponse>&);

  static constexpr std::array<Method, 3> kMethods = {
      Method::Unary<DoNothing>(
          10u, pw_rpc_test_Empty_fields, pw_rpc_test_Empty_fields),
      Method::Unary<AddFive>(
          11u, pw_rpc_test_TestRequest_fields, pw_rpc_test_TestResponse_fields),
      Method::ServerStreaming<StartStream>(
          12u, pw_rpc_test_TestRequest_fields, pw_rpc_test_TestResponse_fields),
  };
};

pw_rpc_test_TestRequest last_request;
ServerWriter<pw_rpc_test_TestResponse> last_writer;

Status FakeGeneratedService::AddFive(ServerContext&,
                                     const pw_rpc_test_TestRequest& request,
                                     pw_rpc_test_TestResponse& response) {
  last_request = request;
  response.value = request.integer + 5;
  return Status::UNAUTHENTICATED;
}

Status FakeGeneratedService::DoNothing(ServerContext&,
                                       const pw_rpc_test_Empty&,
                                       pw_rpc_test_Empty&) {
  return Status::UNKNOWN;
}

void FakeGeneratedService::StartStream(
    ServerContext&,
    const pw_rpc_test_TestRequest& request,
    ServerWriter<pw_rpc_test_TestResponse>& writer) {
  last_request = request;

  last_writer = std::move(writer);
}

TEST(Method, UnaryRpc_SendsResponse) {
  ENCODE_PB(pw_rpc_test_TestRequest, {.integer = 123}, request);

  const Method& method = std::get<1>(FakeGeneratedService::kMethods);
  ServerContextForTest<FakeGeneratedService> context(method);
  method.Invoke(context.get(), context.packet(request));

  const Packet& response = context.output().sent_packet();
  EXPECT_EQ(response.status(), Status::UNAUTHENTICATED);

  // Field 1 (encoded as 1 << 3) with 128 as the value.
  constexpr std::byte expected[]{
      std::byte{0x08}, std::byte{0x80}, std::byte{0x01}};

  EXPECT_EQ(sizeof(expected), response.payload().size());
  EXPECT_EQ(0,
            std::memcmp(expected, response.payload().data(), sizeof(expected)));

  EXPECT_EQ(123, last_request.integer);
}

TEST(Method, UnaryRpc_InvalidPayload_SendsError) {
  std::array<byte, 8> bad_payload{byte{0xFF}, byte{0xAA}, byte{0xDD}};

  const Method& method = std::get<0>(FakeGeneratedService::kMethods);
  ServerContextForTest<FakeGeneratedService> context(method);
  method.Invoke(context.get(), context.packet(bad_payload));

  const Packet& packet = context.output().sent_packet();
  EXPECT_EQ(PacketType::ERROR, packet.type());
  EXPECT_EQ(Status::DATA_LOSS, packet.status());
  EXPECT_EQ(context.kServiceId, packet.service_id());
  EXPECT_EQ(method.id(), packet.method_id());
}

TEST(Method, UnaryRpc_BufferTooSmallForResponse_SendsInternalError) {
  constexpr int64_t value = 0x7FFFFFFF'FFFFFF00ll;
  ENCODE_PB(pw_rpc_test_TestRequest, {.integer = value}, request);

  const Method& method = std::get<1>(FakeGeneratedService::kMethods);
  // Output buffer is too small for the response, but can fit an error packet.
  ServerContextForTest<FakeGeneratedService, 22> context(method);
  ASSERT_LT(context.output().buffer_size(),
            context.packet(request).MinEncodedSizeBytes() + request.size() + 1);

  method.Invoke(context.get(), context.packet(request));

  const Packet& packet = context.output().sent_packet();
  EXPECT_EQ(PacketType::ERROR, packet.type());
  EXPECT_EQ(Status::INTERNAL, packet.status());
  EXPECT_EQ(context.kServiceId, packet.service_id());
  EXPECT_EQ(method.id(), packet.method_id());

  EXPECT_EQ(value, last_request.integer);
}

TEST(Method, ServerStreamingRpc_SendsNothingWhenInitiallyCalled) {
  ENCODE_PB(pw_rpc_test_TestRequest, {.integer = 555}, request);

  const Method& method = std::get<2>(FakeGeneratedService::kMethods);
  ServerContextForTest<FakeGeneratedService> context(method);

  method.Invoke(context.get(), context.packet(request));

  EXPECT_EQ(0u, context.output().packet_count());
  EXPECT_EQ(555, last_request.integer);
}

TEST(Method, ServerWriter_SendsResponse) {
  const Method& method = std::get<2>(FakeGeneratedService::kMethods);
  ServerContextForTest<FakeGeneratedService> context(method);

  method.Invoke(context.get(), context.packet({}));

  EXPECT_EQ(Status::OK, last_writer.Write({.value = 100}));

  ENCODE_PB(pw_rpc_test_TestResponse, {.value = 100}, payload);
  std::array<byte, 128> encoded_response = {};
  auto encoded = context.packet(payload).Encode(encoded_response);
  ASSERT_EQ(Status::OK, encoded.status());

  ASSERT_EQ(encoded.size(), context.output().sent_data().size());
  EXPECT_EQ(0,
            std::memcmp(encoded_response.data(),
                        context.output().sent_data().data(),
                        encoded.size()));
}

TEST(Method, ServerStreamingRpc_ServerWriterBufferTooSmall_InternalError) {
  const Method& method = std::get<2>(FakeGeneratedService::kMethods);

  constexpr size_t kNoPayloadPacketSize = 2 /* type */ + 2 /* channel */ +
                                          5 /* service */ + 5 /* method */ +
                                          2 /* payload */ + 2 /* status */;

  // Make the buffer barely fit a packet with no payload.
  ServerContextForTest<FakeGeneratedService, kNoPayloadPacketSize> context(
      method);

  // Verify that the encoded size of a packet with an empty payload is correct.
  std::array<byte, 128> encoded_response = {};
  auto encoded = context.packet({}).Encode(encoded_response);
  ASSERT_EQ(Status::OK, encoded.status());
  ASSERT_EQ(kNoPayloadPacketSize, encoded.size());

  method.Invoke(context.get(), context.packet({}));

  EXPECT_EQ(Status::OK, last_writer.Write({}));                  // Barely fits
  EXPECT_EQ(Status::INTERNAL, last_writer.Write({.value = 1}));  // Too big
}

}  // namespace
}  // namespace pw::rpc::internal

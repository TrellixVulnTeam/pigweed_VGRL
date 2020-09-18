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

#include "pw_hdlc_lite/hdlc_channel.h"

#include <algorithm>
#include <array>
#include <cstddef>

#include "gtest/gtest.h"
#include "pw_bytes/array.h"
#include "pw_stream/memory_stream.h"

using std::byte;

namespace pw::rpc {
namespace {

constexpr byte kFlag = byte{0x7E};
constexpr uint8_t kAddress = 0x7b;  // 123
constexpr byte kControl = byte{0};

// Size of the in-memory buffer to use for this test.
constexpr size_t kSinkBufferSize = 15;

TEST(HdlcChannelOutput, 1BytePayload) {
  std::array<byte, kSinkBufferSize> memory_buffer;
  std::array<byte, kSinkBufferSize> channel_output_buffer;
  stream::MemoryWriter memory_writer(memory_buffer);

  HdlcChannelOutput output(
      memory_writer, channel_output_buffer, kAddress, "HdlcChannelOutput");

  constexpr byte test_data = byte{'A'};
  std::memcpy(output.AcquireBuffer().data(), &test_data, sizeof(test_data));

  constexpr auto expected = bytes::Concat(
      kFlag, kAddress, kControl, 'A', uint32_t{0xA63E2FA5}, kFlag);

  output.SendAndReleaseBuffer(sizeof(test_data));

  EXPECT_STREQ("HdlcChannelOutput", output.name());
  ASSERT_EQ(memory_writer.bytes_written(), expected.size());
  EXPECT_EQ(
      std::memcmp(
          memory_writer.data(), expected.data(), memory_writer.bytes_written()),
      0);
}

TEST(HdlcChannelOutput, EscapingPayloadTest) {
  std::array<byte, kSinkBufferSize> memory_buffer;
  std::array<byte, kSinkBufferSize> channel_output_buffer;
  stream::MemoryWriter memory_writer(memory_buffer);

  HdlcChannelOutput output(
      memory_writer, channel_output_buffer, kAddress, "HdlcChannelOutput");

  constexpr auto test_data = bytes::Array<0x7D>();
  std::memcpy(
      output.AcquireBuffer().data(), test_data.data(), test_data.size());

  constexpr auto expected = bytes::Concat(kFlag,
                                          kAddress,
                                          kControl,
                                          byte{0x7d},
                                          byte{0x7d} ^ byte{0x20},
                                          uint32_t{0x89515322},
                                          kFlag);
  output.SendAndReleaseBuffer(test_data.size());

  EXPECT_STREQ("HdlcChannelOutput", output.name());
  ASSERT_EQ(memory_writer.bytes_written(), 10u);
  EXPECT_EQ(
      std::memcmp(
          memory_writer.data(), expected.data(), memory_writer.bytes_written()),
      0);
}

}  // namespace
}  // namespace pw::rpc
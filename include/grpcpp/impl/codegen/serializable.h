/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef GRPCPP_IMPL_CODEGEN_SERIALIZABLE_H
#define GRPCPP_IMPL_CODEGEN_SERIALIZABLE_H

#include <grpcpp/impl/codegen/byte_buffer.h>
#include <grpcpp/impl/codegen/proto_utils.h>
#include <grpcpp/impl/codegen/serialization_traits.h>

namespace grpc {

/// Define new classes that are meant to type-erase a message
/// and only convey the fact that something can go through
/// serialization. These are used by the interception API. Code should only
/// use the interception API if SerializationTraits for all message types
/// is idempotent and actually gives the caller an owned buffer on Serialize
/// (both of which are true for gRPC's protobuf and ByteBuffer definitions)

class Serializable {
 public:
  template <class M>
  Serializable(const M* msg) {
    serialize_ = [msg](ByteBuffer* bbuf) { bool dummy; return SerializationTraits<M, void>::Serialize(*msg, bbuf, &dummy); };
    SetLength<M, void>(msg);
    if (is_base_of
  }

  Status Serialize(ByteBuffer* buffer);

  size_t Length() const;
 private:
  template <class M, class UnusedButHereForSpecialization = void>
  SetLength(const M* msg);
  std::function<Status(ByteBuffer*)> serialize_;
  std::function<size_t()> length_;
};

template <class M>
Serializable::SetLength<M, typename std::enable_if<std::is_base_of<
                                                     grpc::protobuf::Message, M>::value>::type>(const M* msg) {
  length_ = [msg]() { return msg->ByteSizeLong(); }
}

template <>
Serializable::SetLength<ByteBuffer, void>(const M* msg) {
  length_ = [msg]() { return msg->Length(); }
}

template <class M>
Serializable::SetLength<M, typename std::enable_if<!std::is_base_of<
                                                     grpc::protobuf::Message, M>::value && >::type>(const M* msg) {
}



}  // namespace grpc

#endif  // GRPCPP_IMPL_CODEGEN_SERIALIZABLE_H

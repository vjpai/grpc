/*
 *
 * Copyright 2019 gRPC authors.
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
 */

#ifndef GRPCPP_IMPL_CODEGEN_SERVER_CALLBACK_IMPL_H
#define GRPCPP_IMPL_CODEGEN_SERVER_CALLBACK_IMPL_H

#include <atomic>
#include <functional>
#include <type_traits>

#include <grpcpp/impl/codegen/call.h>
#include <grpcpp/impl/codegen/call_op_set.h>
#include <grpcpp/impl/codegen/callback_common.h>
#include <grpcpp/impl/codegen/config.h>
#include <grpcpp/impl/codegen/core_codegen_interface.h>
#include <grpcpp/impl/codegen/message_allocator.h>
#include <grpcpp/impl/codegen/server_context_impl.h>
#include <grpcpp/impl/codegen/server_interface.h>
#include <grpcpp/impl/codegen/status.h>

namespace grpc_impl {

// Declare base class of all reactors as internal
namespace internal {

// Forward declarations
template <class Request, class Response>
class CallbackUnaryHandler;
template <class Request, class Response>
class CallbackClientStreamingHandler;
template <class Request, class Response>
class CallbackServerStreamingHandler;
template <class Request, class Response>
class CallbackBidiHandler;

class ServerReactor {
 public:
  virtual ~ServerReactor() = default;
  virtual void OnDone() = 0;
  virtual void OnCancel() = 0;

 private:
  friend class ::grpc_impl::ServerContext;
  template <class Request, class Response>
  friend class CallbackUnaryHandler;
  template <class Request, class Response>
  friend class CallbackClientStreamingHandler;
  template <class Request, class Response>
  friend class CallbackServerStreamingHandler;
  template <class Request, class Response>
  friend class CallbackBidiHandler;

  // The ServerReactor is responsible for tracking when it is safe to call
  // OnCancel. This function should not be called until after the method handler
  // is done and the RPC has completed with a cancellation. This is tracked by
  // counting how many of these conditions have been met and calling OnCancel
  // when none remain unmet.

  void MaybeCallOnCancel() {
    if (GPR_UNLIKELY(on_cancel_conditions_remaining_.fetch_sub(
                         1, std::memory_order_acq_rel) == 1)) {
      OnCancel();
    }
  }

  std::atomic<intptr_t> on_cancel_conditions_remaining_{2};
};

template <class Request, class Response>
class DefaultMessageHolder
    : public ::grpc::experimental::MessageHolder<Request, Response> {
 public:
  DefaultMessageHolder() {
    this->set_request(&request_obj_);
    this->set_response(&response_obj_);
  }
  void Release() override {
    // the object is allocated in the call arena.
    this->~DefaultMessageHolder<Request, Response>();
  }

 private:
  Request request_obj_;
  Response response_obj_;
};

}  // namespace internal

namespace experimental {

// Forward declarations
class ServerUnaryReactor;
template <class Request>
class ServerReadReactor;
template <class Response>
class ServerWriteReactor;
template <class Request, class Response>
class ServerBidiReactor;

// NOTE: The actual call/stream object classes are provided as API only to
// support mocking. There are no implementations of these class interfaces in
// the API.
class ServerCallbackUnary {
 public:
  virtual ~ServerCallbackUnary() {}
  virtual void Finish(::grpc::Status s) = 0;
  virtual void SendInitialMetadata() = 0;

 protected:
  // Use a template rather than explicitly specifying ServerUnaryReactor to
  // delay binding and avoid a circular forward declaration issue
  template <class Reactor>
  void BindReactor(Reactor* reactor) {
    reactor->InternalBindCall(this);
  }
};

template <class Request>
class ServerCallbackReader {
 public:
  virtual ~ServerCallbackReader() {}
  virtual void Finish(::grpc::Status s) = 0;
  virtual void SendInitialMetadata() = 0;
  virtual void Read(Request* msg) = 0;

 protected:
  void BindReactor(ServerReadReactor<Request>* reactor) {
    reactor->InternalBindReader(this);
  }
};

template <class Response>
class ServerCallbackWriter {
 public:
  virtual ~ServerCallbackWriter() {}

  virtual void Finish(::grpc::Status s) = 0;
  virtual void SendInitialMetadata() = 0;
  virtual void Write(const Response* msg, ::grpc::WriteOptions options) = 0;
  virtual void WriteAndFinish(const Response* msg, ::grpc::WriteOptions options,
                              ::grpc::Status s) = 0;

 protected:
  void BindReactor(ServerWriteReactor<Response>* reactor) {
    reactor->InternalBindWriter(this);
  }
};

template <class Request, class Response>
class ServerCallbackReaderWriter {
 public:
  virtual ~ServerCallbackReaderWriter() {}

  virtual void Finish(::grpc::Status s) = 0;
  virtual void SendInitialMetadata() = 0;
  virtual void Read(Request* msg) = 0;
  virtual void Write(const Response* msg, ::grpc::WriteOptions options) = 0;
  virtual void WriteAndFinish(const Response* msg, ::grpc::WriteOptions options,
                              ::grpc::Status s) = 0;

 protected:
  void BindReactor(ServerBidiReactor<Request, Response>* reactor) {
    reactor->InternalBindStream(this);
  }
};

// The following classes are the reactor interfaces that are to be implemented
// by the user, returned as the output parameter of the method handler for a
// callback method. Note that none of the classes are pure; all reactions have a
// default empty reaction so that the user class only needs to override those
// classes that it cares about.

/// \a ServerBidiReactor is the interface for a bidirectional streaming RPC.
template <class Request, class Response>
class ServerBidiReactor : public internal::ServerReactor {
 public:
  // NOTE: Initializing stream_ as a constructor initializer rather than a
  //       default initializer because gcc-4.x requires a copy constructor for
  //       default initializing a templated member, which isn't ok for atomic.
  // TODO(vjpai): Switch to default constructor and default initializer when
  //              gcc-4.x is no longer supported
  ServerBidiReactor() : stream_(nullptr) {}
  ~ServerBidiReactor() = default;

  /// Send any initial metadata stored in the RPC context. If not invoked,
  /// any initial metadata will be passed along with the first Write or the
  /// Finish (if there are no writes).
  void StartSendInitialMetadata() {
    bool ready = true;
    auto* stream = stream_.load(std::memory_order_relaxed);
    if (stream == nullptr) {
      grpc::internal::MutexLock l(&stream_mu_);
      stream = stream_.load(std::memory_order_relaxed);
      if (stream == nullptr) {
        ready = false;
        send_initial_metadata_wanted_ = true;
      }
    }
    if (ready) {
      stream->SendInitialMetadata();
    }
  }

  /// Initiate a read operation.
  ///
  /// \param[out] req Where to eventually store the read message. Valid when
  ///                 the library calls OnReadDone
  void StartRead(Request* req) {
    bool ready = true;
    auto* stream = stream_.load(std::memory_order_relaxed);
    if (stream == nullptr) {
      grpc::internal::MutexLock l(&stream_mu_);
      stream = stream_.load(std::memory_order_relaxed);
      if (stream == nullptr) {
        ready = false;
        read_wanted_ = req;
      }
    }
    if (ready) {
      stream->Read(req);
    }
  }

  /// Initiate a write operation.
  ///
  /// \param[in] resp The message to be written. The library takes temporary
  ///                 ownership until OnWriteDone, at which point the
  ///                 application regains ownership of resp.
  void StartWrite(const Response* resp) {
    StartWrite(resp, ::grpc::WriteOptions());
  }

  /// Initiate a write operation with specified options.
  ///
  /// \param[in] resp The message to be written. The library takes temporary
  ///                 ownership until OnWriteDone, at which point the
  ///                 application regains ownership of resp.
  /// \param[in] options The WriteOptions to use for writing this message
  void StartWrite(const Response* resp, ::grpc::WriteOptions options) {
    bool ready = true;
    auto* stream = stream_.load(std::memory_order_relaxed);
    if (stream == nullptr) {
      grpc::internal::MutexLock l(&stream_mu_);
      stream = stream_.load(std::memory_order_relaxed);
      if (stream == nullptr) {
        ready = false;
        write_wanted_ = resp;
        write_options_wanted_ = std::move(options);
      }
    }
    if (ready) {
      stream->Write(resp, std::move(options));
    }
  }

  /// Initiate a write operation with specified options and final RPC Status,
  /// which also causes any trailing metadata for this RPC to be sent out.
  /// StartWriteAndFinish is like merging StartWriteLast and Finish into a
  /// single step. A key difference, though, is that this operation doesn't have
  /// an OnWriteDone reaction - it is considered complete only when OnDone is
  /// available. An RPC can either have StartWriteAndFinish or Finish, but not
  /// both.
  ///
  /// \param[in] resp The message to be written. The library takes temporary
  ///                 ownership until OnWriteDone, at which point the
  ///                 application regains ownership of resp.
  /// \param[in] options The WriteOptions to use for writing this message
  /// \param[in] s The status outcome of this RPC
  void StartWriteAndFinish(const Response* resp, ::grpc::WriteOptions options,
                           ::grpc::Status s) {
    bool ready = true;
    auto* stream = stream_.load(std::memory_order_relaxed);
    if (stream == nullptr) {
      grpc::internal::MutexLock l(&stream_mu_);
      stream = stream_.load(std::memory_order_relaxed);
      if (stream == nullptr) {
        ready = false;
        write_and_finish_wanted_ = true;
        write_wanted_ = resp;
        write_options_wanted_ = std::move(options);
        status_wanted_ = std::move(s);
      }
    }
    if (ready) {
      stream->WriteAndFinish(resp, std::move(options), std::move(s));
    }
  }

  /// Inform system of a planned write operation with specified options, but
  /// allow the library to schedule the actual write coalesced with the writing
  /// of trailing metadata (which takes place on a Finish call).
  ///
  /// \param[in] resp The message to be written. The library takes temporary
  ///                 ownership until OnWriteDone, at which point the
  ///                 application regains ownership of resp.
  /// \param[in] options The WriteOptions to use for writing this message
  void StartWriteLast(const Response* resp, ::grpc::WriteOptions options) {
    StartWrite(resp, std::move(options.set_last_message()));
  }

  /// Indicate that the stream is to be finished and the trailing metadata and
  /// RPC status are to be sent. Every RPC MUST be finished using either Finish
  /// or StartWriteAndFinish (but not both), even if the RPC is already
  /// cancelled.
  ///
  /// \param[in] s The status outcome of this RPC
  void Finish(::grpc::Status s) {
    bool ready = true;
    auto* stream = stream_.load(std::memory_order_relaxed);
    if (stream == nullptr) {
      grpc::internal::MutexLock l(&stream_mu_);
      stream = stream_.load(std::memory_order_relaxed);
      if (stream == nullptr) {
        ready = false;
        finish_wanted_ = true;
        status_wanted_ = std::move(s);
      }
    }
    if (ready) {
      stream->Finish(std::move(s));
    }
  }

  /// Notifies the application that an explicit StartSendInitialMetadata
  /// operation completed. Not used when the sending of initial metadata
  /// piggybacks onto the first write.
  ///
  /// \param[in] ok Was it successful? If false, no further write-side operation
  ///               will succeed.
  virtual void OnSendInitialMetadataDone(bool /*ok*/) {}

  /// Notifies the application that a StartRead operation completed.
  ///
  /// \param[in] ok Was it successful? If false, no further read-side operation
  ///               will succeed.
  virtual void OnReadDone(bool /*ok*/) {}

  /// Notifies the application that a StartWrite (or StartWriteLast) operation
  /// completed.
  ///
  /// \param[in] ok Was it successful? If false, no further write-side operation
  ///               will succeed.
  virtual void OnWriteDone(bool /*ok*/) {}

  /// Notifies the application that all operations associated with this RPC
  /// have completed. This is an override (from the internal base class) but
  /// still abstract, so derived classes MUST override it to be instantiated.
  void OnDone() override = 0;

  /// Notifies the application that this RPC has been cancelled. This is an
  /// override (from the internal base class) but not final, so derived classes
  /// should override it if they want to take action.
  void OnCancel() override {}

 private:
  friend class ServerCallbackReaderWriter<Request, Response>;
  // May be overridden by internal implementation details. This is not a public
  // customization point.
  virtual void InternalBindStream(
      ServerCallbackReaderWriter<Request, Response>* stream) {
    grpc::internal::MutexLock l(&stream_mu_);
    stream_.store(stream, std::memory_order_relaxed);
    if (send_initial_metadata_wanted_) {
      stream->SendInitialMetadata();
    }
    if (read_wanted_ != nullptr) {
      stream->Read(read_wanted_);
    }
    if (write_and_finish_wanted_) {
      stream->WriteAndFinish(write_wanted_, std::move(write_options_wanted_),
                             std::move(status_wanted_));
    } else {
      if (write_wanted_ != nullptr) {
        stream->Write(write_wanted_, std::move(write_options_wanted_));
      }
      if (finish_wanted_) {
        stream->Finish(std::move(status_wanted_));
      }
    }
  }

  grpc::internal::Mutex stream_mu_;
  std::atomic<ServerCallbackReaderWriter<Request, Response>*> stream_;
  bool send_initial_metadata_wanted_ = false;
  bool write_and_finish_wanted_ = false;
  bool finish_wanted_ = false;
  Request* read_wanted_ = nullptr;
  const Response* write_wanted_ = nullptr;
  ::grpc::WriteOptions write_options_wanted_;
  ::grpc::Status status_wanted_;
};

/// \a ServerReadReactor is the interface for a client-streaming RPC.
template <class Request>
class ServerReadReactor : public internal::ServerReactor {
 public:
  ServerReadReactor() : reader_(nullptr) {}
  ~ServerReadReactor() = default;

  /// The following operation initiations are exactly like ServerBidiReactor.
  void StartSendInitialMetadata() {
    bool ready = true;
    auto* reader = reader_.load(std::memory_order_relaxed);
    if (reader == nullptr) {
      grpc::internal::MutexLock l(&reader_mu_);
      reader = reader_.load(std::memory_order_relaxed);
      if (reader == nullptr) {
        ready = false;
        send_initial_metadata_wanted_ = true;
      }
    }
    if (ready) {
      reader->SendInitialMetadata();
    }
  }
  void StartRead(Request* req) {
    bool ready = true;
    auto* reader = reader_.load(std::memory_order_relaxed);
    if (reader == nullptr) {
      grpc::internal::MutexLock l(&reader_mu_);
      reader = reader_.load(std::memory_order_relaxed);
      if (reader == nullptr) {
        ready = false;
        read_wanted_ = req;
      }
    }
    if (ready) {
      reader->Read(req);
    }
  }
  void Finish(::grpc::Status s) {
    bool ready = true;
    auto* reader = reader_.load(std::memory_order_relaxed);
    if (reader == nullptr) {
      grpc::internal::MutexLock l(&reader_mu_);
      reader = reader_.load(std::memory_order_relaxed);
      if (reader == nullptr) {
        ready = false;
        finish_wanted_ = true;
        status_wanted_ = std::move(s);
      }
    }
    if (ready) {
      reader->Finish(std::move(s));
    }
  }

  /// The following notifications are exactly like ServerBidiReactor.
  virtual void OnSendInitialMetadataDone(bool /*ok*/) {}
  virtual void OnReadDone(bool /*ok*/) {}
  void OnDone() override = 0;
  void OnCancel() override {}

 private:
  friend class ServerCallbackReader<Request>;

  // May be overridden by internal implementation details. This is not a public
  // customization point.
  virtual void InternalBindReader(ServerCallbackReader<Request>* reader) {
    grpc::internal::MutexLock l(&reader_mu_);
    reader_.store(reader, std::memory_order_relaxed);
    if (send_initial_metadata_wanted_) {
      reader->SendInitialMetadata();
    }
    if (read_wanted_ != nullptr) {
      reader->Read(read_wanted_);
    }
    if (finish_wanted_) {
      reader->Finish(std::move(status_wanted_));
    }
  }

  grpc::internal::Mutex reader_mu_;
  std::atomic<ServerCallbackReader<Request>*> reader_;
  bool send_initial_metadata_wanted_ = false;
  bool finish_wanted_ = false;
  Request* read_wanted_ = nullptr;
  ::grpc::Status status_wanted_;
};

/// \a ServerWriteReactor is the interface for a server-streaming RPC.
template <class Response>
class ServerWriteReactor : public internal::ServerReactor {
 public:
  ServerWriteReactor() : writer_(nullptr) {}
  ~ServerWriteReactor() = default;

  /// The following operation initiations are exactly like ServerBidiReactor.
  void StartSendInitialMetadata() {
    bool ready = true;
    auto* writer = writer_.load(std::memory_order_relaxed);
    if (writer == nullptr) {
      grpc::internal::MutexLock l(&writer_mu_);
      writer = writer_.load(std::memory_order_relaxed);
      if (writer == nullptr) {
        ready = false;
        send_initial_metadata_wanted_ = true;
      }
    }
    if (ready) {
      writer->SendInitialMetadata();
    }
  }
  void StartWrite(const Response* resp) {
    StartWrite(resp, ::grpc::WriteOptions());
  }
  void StartWrite(const Response* resp, ::grpc::WriteOptions options) {
    bool ready = true;
    auto* writer = writer_.load(std::memory_order_relaxed);
    if (writer == nullptr) {
      grpc::internal::MutexLock l(&writer_mu_);
      writer = writer_.load(std::memory_order_relaxed);
      if (writer == nullptr) {
        ready = false;
        write_wanted_ = resp;
        write_options_wanted_ = std::move(options);
      }
    }
    if (ready) {
      writer->Write(resp, std::move(options));
    }
  }
  void StartWriteAndFinish(const Response* resp, ::grpc::WriteOptions options,
                           ::grpc::Status s) {
    bool ready = true;
    auto* writer = writer_.load(std::memory_order_relaxed);
    if (writer == nullptr) {
      grpc::internal::MutexLock l(&writer_mu_);
      writer = writer_.load(std::memory_order_relaxed);
      if (writer == nullptr) {
        ready = false;
        write_and_finish_wanted_ = true;
        write_wanted_ = resp;
        write_options_wanted_ = std::move(options);
        status_wanted_ = std::move(s);
      }
    }
    if (ready) {
      writer->WriteAndFinish(resp, std::move(options), std::move(s));
    }
  }
  void StartWriteLast(const Response* resp, ::grpc::WriteOptions options) {
    StartWrite(resp, std::move(options.set_last_message()));
  }
  void Finish(::grpc::Status s) {
    bool ready = true;
    auto* writer = writer_.load(std::memory_order_relaxed);
    if (writer == nullptr) {
      grpc::internal::MutexLock l(&writer_mu_);
      writer = writer_.load(std::memory_order_relaxed);
      if (writer == nullptr) {
        ready = false;
        finish_wanted_ = true;
        status_wanted_ = std::move(s);
      }
    }
    if (ready) {
      writer->Finish(std::move(s));
    }
  }

  /// The following notifications are exactly like ServerBidiReactor.
  virtual void OnSendInitialMetadataDone(bool /*ok*/) {}
  virtual void OnWriteDone(bool /*ok*/) {}
  void OnDone() override = 0;
  void OnCancel() override {}

 private:
  friend class ServerCallbackWriter<Response>;
  // May be overridden by internal implementation details. This is not a public
  // customization point.
  virtual void InternalBindWriter(ServerCallbackWriter<Response>* writer) {
    grpc::internal::MutexLock l(&writer_mu_);
    writer_.store(writer, std::memory_order_relaxed);
    if (send_initial_metadata_wanted_) {
      writer->SendInitialMetadata();
    }
    if (write_and_finish_wanted_) {
      writer->WriteAndFinish(write_wanted_, std::move(write_options_wanted_),
                             std::move(status_wanted_));
    } else {
      if (write_wanted_ != nullptr) {
        writer->Write(write_wanted_, std::move(write_options_wanted_));
      }
      if (finish_wanted_) {
        writer->Finish(std::move(status_wanted_));
      }
    }
  }

  grpc::internal::Mutex writer_mu_;
  std::atomic<ServerCallbackWriter<Response>*> writer_;
  bool send_initial_metadata_wanted_ = false;
  bool write_and_finish_wanted_ = false;
  bool finish_wanted_ = false;
  const Response* write_wanted_ = nullptr;
  ::grpc::WriteOptions write_options_wanted_;
  ::grpc::Status status_wanted_;
};

class ServerUnaryReactor : public internal::ServerReactor {
 public:
  ServerUnaryReactor() : call_(nullptr) {}
  ~ServerUnaryReactor() = default;

  /// The following operation initiations are exactly like ServerBidiReactor.
  void StartSendInitialMetadata() {
    bool ready = true;
    auto* call = call_.load(std::memory_order_relaxed);
    if (call == nullptr) {
      grpc::internal::MutexLock l(&call_mu_);
      call = call_.load(std::memory_order_relaxed);
      if (call == nullptr) {
        ready = false;
        send_initial_metadata_wanted_ = true;
      }
    }
    if (ready) {
      call->SendInitialMetadata();
    }
  }
  void Finish(::grpc::Status s) {
    bool ready = true;
    auto* call = call_.load(std::memory_order_relaxed);
    if (call == nullptr) {
      grpc::internal::MutexLock l(&call_mu_);
      call = call_.load(std::memory_order_relaxed);
      if (call == nullptr) {
        ready = false;
        finish_wanted_ = true;
        status_wanted_ = std::move(s);
      }
    }
    if (ready) {
      call->Finish(std::move(s));
    }
  }

  /// The following notifications are exactly like ServerBidiReactor.
  virtual void OnSendInitialMetadataDone(bool ok) {}
  void OnDone() override = 0;
  void OnCancel() override {}

 private:
  friend class ServerCallbackUnary;
  // May be overridden by internal implementation details. This is not a public
  // customization point.
  virtual void InternalBindCall(ServerCallbackUnary* call) {
    grpc::internal::MutexLock l(&call_mu_);
    call_.store(call, std::memory_order_relaxed);
    if (send_initial_metadata_wanted_) {
      call->SendInitialMetadata();
    }
    if (finish_wanted_) {
      call->Finish(std::move(status_wanted_));
    }
  }

  grpc::internal::Mutex call_mu_;
  std::atomic<ServerCallbackUnary*> call_;
  bool send_initial_metadata_wanted_ = false;
  bool finish_wanted_ = false;
  ::grpc::Status status_wanted_;
};

}  // namespace experimental

namespace internal {

template <class Base>
class FinishOnlyReactor : public Base {
 public:
  explicit FinishOnlyReactor(::grpc_impl::ServerContext* ctx) {
    ctx->FinishWithFastStatus(this);
  }
  void OnDone() override { this->~FinishOnlyReactor(); }
};

using UnimplementedUnaryReactor =
    FinishOnlyReactor<experimental::ServerUnaryReactor>;
template <class Request>
using UnimplementedReadReactor =
    FinishOnlyReactor<experimental::ServerReadReactor<Request>>;
template <class Response>
using UnimplementedWriteReactor =
    FinishOnlyReactor<experimental::ServerWriteReactor<Response>>;
template <class Request, class Response>
using UnimplementedBidiReactor =
    FinishOnlyReactor<experimental::ServerBidiReactor<Request, Response>>;

template <class RequestType, class ResponseType>
class CallbackUnaryHandler : public ::grpc::internal::MethodHandler {
 public:
  explicit CallbackUnaryHandler(
      std::function<void(::grpc_impl::ServerContext*, const RequestType*,
                         ResponseType*, experimental::ServerUnaryReactor**)>
          get_reactor)
      : get_reactor_(std::move(get_reactor)) {}

  void SetMessageAllocator(
      ::grpc::experimental::MessageAllocator<RequestType, ResponseType>*
          allocator) {
    allocator_ = allocator;
  }

  void RunHandler(const HandlerParameter& param) final {
    // Arena allocate a controller structure (that includes request/response)
    ::grpc::g_core_codegen_interface->grpc_call_ref(param.call->call());
    auto* allocator_state = static_cast<
        ::grpc::experimental::MessageHolder<RequestType, ResponseType>*>(
        param.internal_data);

    auto* call = new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
        param.call->call(), sizeof(ServerCallbackUnaryImpl)))
        ServerCallbackUnaryImpl(param.server_context, param.call,
                                allocator_state,
                                std::move(param.call_requester));

    experimental::ServerUnaryReactor* reactor = nullptr;
    if (param.status.ok()) {
      ::grpc::internal::CatchingReactorGetter(
          &reactor, get_reactor_, param.server_context, call->request(),
          call->response());
    }

    if (reactor == nullptr) {
      // if deserialization or reactor creator failed, we need to fail the call
      reactor = new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
          param.call->call(), sizeof(UnimplementedUnaryReactor)))
          UnimplementedUnaryReactor(param.server_context);
    }

    call->SetupReactor(reactor);
    // The earliest that OnCancel can be called is after setup is done
    reactor->MaybeCallOnCancel();
    call->MaybeDone();
  }

  void* Deserialize(grpc_call* call, grpc_byte_buffer* req,
                    ::grpc::Status* status, void** handler_data) final {
    ::grpc::ByteBuffer buf;
    buf.set_buffer(req);
    RequestType* request = nullptr;
    ::grpc::experimental::MessageHolder<RequestType, ResponseType>*
        allocator_state = nullptr;
    if (allocator_ != nullptr) {
      allocator_state = allocator_->AllocateMessages();
    } else {
      allocator_state =
          new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
              call, sizeof(DefaultMessageHolder<RequestType, ResponseType>)))
              DefaultMessageHolder<RequestType, ResponseType>();
    }
    *handler_data = allocator_state;
    request = allocator_state->request();
    *status =
        ::grpc::SerializationTraits<RequestType>::Deserialize(&buf, request);
    buf.Release();
    if (status->ok()) {
      return request;
    }
    // Clean up on deserialization failure.
    allocator_state->Release();
    return nullptr;
  }

 private:
  std::function<void(::grpc_impl::ServerContext*, const RequestType*,
                     ResponseType*, experimental::ServerUnaryReactor**)>
      get_reactor_;
  ::grpc::experimental::MessageAllocator<RequestType, ResponseType>*
      allocator_ = nullptr;

  class ServerCallbackUnaryImpl : public experimental::ServerCallbackUnary {
   public:
    void Finish(::grpc::Status s) override {
      finish_tag_.Set(call_.call(), [this](bool) { MaybeDone(); },
                      &finish_ops_);
      finish_ops_.set_core_cq_tag(&finish_tag_);

      if (!ctx_->sent_initial_metadata_) {
        finish_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                        ctx_->initial_metadata_flags());
        if (ctx_->compression_level_set()) {
          finish_ops_.set_compression_level(ctx_->compression_level());
        }
        ctx_->sent_initial_metadata_ = true;
      }
      // The response is dropped if the status is not OK.
      if (s.ok()) {
        finish_ops_.ServerSendStatus(&ctx_->trailing_metadata_,
                                     finish_ops_.SendMessagePtr(response()));
      } else {
        finish_ops_.ServerSendStatus(&ctx_->trailing_metadata_, s);
      }
      finish_ops_.set_core_cq_tag(&finish_tag_);
      call_.PerformOps(&finish_ops_);
    }

    void SendInitialMetadata() override {
      GPR_CODEGEN_ASSERT(!ctx_->sent_initial_metadata_);
      callbacks_outstanding_.fetch_add(1, std::memory_order_relaxed);
      meta_tag_.Set(call_.call(),
                    [this](bool ok) {
                      reactor_->OnSendInitialMetadataDone(ok);
                      MaybeDone();
                    },
                    &meta_ops_);
      meta_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                    ctx_->initial_metadata_flags());
      if (ctx_->compression_level_set()) {
        meta_ops_.set_compression_level(ctx_->compression_level());
      }
      ctx_->sent_initial_metadata_ = true;
      meta_ops_.set_core_cq_tag(&meta_tag_);
      call_.PerformOps(&meta_ops_);
    }

   private:
    friend class CallbackUnaryHandler<RequestType, ResponseType>;

    ServerCallbackUnaryImpl(
        ::grpc_impl::ServerContext* ctx, ::grpc::internal::Call* call,
        ::grpc::experimental::MessageHolder<RequestType, ResponseType>*
            allocator_state,
        std::function<void()> call_requester)
        : ctx_(ctx),
          call_(*call),
          allocator_state_(allocator_state),
          call_requester_(std::move(call_requester)) {
      ctx_->set_message_allocator_state(allocator_state);
    }

    void SetupReactor(experimental::ServerUnaryReactor* reactor) {
      reactor_ = reactor;
      ctx_->BeginCompletionOp(&call_, [this](bool) { MaybeDone(); }, reactor);
      this->BindReactor(reactor);
    }

    const RequestType* request() { return allocator_state_->request(); }
    ResponseType* response() { return allocator_state_->response(); }

    void MaybeDone() {
      if (GPR_UNLIKELY(callbacks_outstanding_.fetch_sub(
                           1, std::memory_order_acq_rel) == 1)) {
        reactor_->OnDone();
        grpc_call* call = call_.call();
        auto call_requester = std::move(call_requester_);
        allocator_state_->Release();
        this->~ServerCallbackUnaryImpl();  // explicitly call destructor
        ::grpc::g_core_codegen_interface->grpc_call_unref(call);
        call_requester();
      }
    }

    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata>
        meta_ops_;
    ::grpc::internal::CallbackWithSuccessTag meta_tag_;
    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata,
                                ::grpc::internal::CallOpSendMessage,
                                ::grpc::internal::CallOpServerSendStatus>
        finish_ops_;
    ::grpc::internal::CallbackWithSuccessTag finish_tag_;

    ::grpc_impl::ServerContext* ctx_;
    ::grpc::internal::Call call_;
    ::grpc::experimental::MessageHolder<RequestType, ResponseType>* const
        allocator_state_;
    std::function<void()> call_requester_;
    experimental::ServerUnaryReactor* reactor_;
    std::atomic<intptr_t> callbacks_outstanding_{
        3};  // reserve for start, Finish, and CompletionOp
  };
};

template <class RequestType, class ResponseType>
class CallbackClientStreamingHandler : public ::grpc::internal::MethodHandler {
 public:
  explicit CallbackClientStreamingHandler(
      std::function<void(::grpc_impl::ServerContext*, ResponseType*,
                         experimental::ServerReadReactor<RequestType>**)>
          get_reactor)
      : get_reactor_(std::move(get_reactor)) {}
  void RunHandler(const HandlerParameter& param) final {
    // Arena allocate a reader structure (that includes response)
    ::grpc::g_core_codegen_interface->grpc_call_ref(param.call->call());

    auto* reader = new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
        param.call->call(), sizeof(ServerCallbackReaderImpl)))
        ServerCallbackReaderImpl(param.server_context, param.call,
                                 std::move(param.call_requester));

    experimental::ServerReadReactor<RequestType>* reactor = nullptr;
    if (param.status.ok()) {
      ::grpc::internal::CatchingReactorGetter(
          &reactor, get_reactor_, param.server_context, reader->response());
    }

    if (reactor == nullptr) {
      // if deserialization or reactor creator failed, we need to fail the call
      reactor = new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
          param.call->call(), sizeof(UnimplementedReadReactor<RequestType>)))
          UnimplementedReadReactor<RequestType>(param.server_context);
    }

    reader->SetupReactor(reactor);
    // The earliest that OnCancel can be called is after setup is done.
    reactor->MaybeCallOnCancel();
    reader->MaybeDone();
  }

 private:
  std::function<void(::grpc_impl::ServerContext*, ResponseType*,
                     experimental::ServerReadReactor<RequestType>**)>
      get_reactor_;

  class ServerCallbackReaderImpl
      : public experimental::ServerCallbackReader<RequestType> {
   public:
    void Finish(::grpc::Status s) override {
      finish_tag_.Set(call_.call(), [this](bool) { MaybeDone(); },
                      &finish_ops_);
      if (!ctx_->sent_initial_metadata_) {
        finish_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                        ctx_->initial_metadata_flags());
        if (ctx_->compression_level_set()) {
          finish_ops_.set_compression_level(ctx_->compression_level());
        }
        ctx_->sent_initial_metadata_ = true;
      }
      // The response is dropped if the status is not OK.
      if (s.ok()) {
        finish_ops_.ServerSendStatus(&ctx_->trailing_metadata_,
                                     finish_ops_.SendMessagePtr(&resp_));
      } else {
        finish_ops_.ServerSendStatus(&ctx_->trailing_metadata_, s);
      }
      finish_ops_.set_core_cq_tag(&finish_tag_);
      call_.PerformOps(&finish_ops_);
    }

    void SendInitialMetadata() override {
      GPR_CODEGEN_ASSERT(!ctx_->sent_initial_metadata_);
      callbacks_outstanding_.fetch_add(1, std::memory_order_relaxed);
      meta_tag_.Set(call_.call(),
                    [this](bool ok) {
                      reactor_->OnSendInitialMetadataDone(ok);
                      MaybeDone();
                    },
                    &meta_ops_);
      meta_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                    ctx_->initial_metadata_flags());
      if (ctx_->compression_level_set()) {
        meta_ops_.set_compression_level(ctx_->compression_level());
      }
      ctx_->sent_initial_metadata_ = true;
      meta_ops_.set_core_cq_tag(&meta_tag_);
      call_.PerformOps(&meta_ops_);
    }

    void Read(RequestType* req) override {
      callbacks_outstanding_.fetch_add(1, std::memory_order_relaxed);
      read_ops_.RecvMessage(req);
      call_.PerformOps(&read_ops_);
    }

   private:
    friend class CallbackClientStreamingHandler<RequestType, ResponseType>;

    ServerCallbackReaderImpl(::grpc_impl::ServerContext* ctx,
                             ::grpc::internal::Call* call,
                             std::function<void()> call_requester)
        : ctx_(ctx), call_(*call), call_requester_(std::move(call_requester)) {}

    void SetupReactor(experimental::ServerReadReactor<RequestType>* reactor) {
      reactor_ = reactor;
      read_tag_.Set(call_.call(),
                    [this](bool ok) {
                      reactor_->OnReadDone(ok);
                      MaybeDone();
                    },
                    &read_ops_);
      read_ops_.set_core_cq_tag(&read_tag_);
      ctx_->BeginCompletionOp(&call_, [this](bool) { MaybeDone(); }, reactor);
      this->BindReactor(reactor);
    }

    ~ServerCallbackReaderImpl() {}

    ResponseType* response() { return &resp_; }

    void MaybeDone() {
      if (GPR_UNLIKELY(callbacks_outstanding_.fetch_sub(
                           1, std::memory_order_acq_rel) == 1)) {
        reactor_->OnDone();
        grpc_call* call = call_.call();
        auto call_requester = std::move(call_requester_);
        this->~ServerCallbackReaderImpl();  // explicitly call destructor
        ::grpc::g_core_codegen_interface->grpc_call_unref(call);
        call_requester();
      }
    }

    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata>
        meta_ops_;
    ::grpc::internal::CallbackWithSuccessTag meta_tag_;
    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata,
                                ::grpc::internal::CallOpSendMessage,
                                ::grpc::internal::CallOpServerSendStatus>
        finish_ops_;
    ::grpc::internal::CallbackWithSuccessTag finish_tag_;
    ::grpc::internal::CallOpSet<
        ::grpc::internal::CallOpRecvMessage<RequestType>>
        read_ops_;
    ::grpc::internal::CallbackWithSuccessTag read_tag_;

    ::grpc_impl::ServerContext* ctx_;
    ::grpc::internal::Call call_;
    ResponseType resp_;
    std::function<void()> call_requester_;
    experimental::ServerReadReactor<RequestType>* reactor_;
    std::atomic<intptr_t> callbacks_outstanding_{
        3};  // reserve for OnStarted, Finish, and CompletionOp
  };
};

template <class RequestType, class ResponseType>
class CallbackServerStreamingHandler : public ::grpc::internal::MethodHandler {
 public:
  explicit CallbackServerStreamingHandler(
      std::function<void(::grpc_impl::ServerContext*, const RequestType*,
                         experimental::ServerWriteReactor<ResponseType>**)>
          get_reactor)
      : get_reactor_(std::move(get_reactor)) {}
  void RunHandler(const HandlerParameter& param) final {
    // Arena allocate a writer structure
    ::grpc::g_core_codegen_interface->grpc_call_ref(param.call->call());

    auto* writer = new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
        param.call->call(), sizeof(ServerCallbackWriterImpl)))
        ServerCallbackWriterImpl(param.server_context, param.call,
                                 static_cast<RequestType*>(param.request),
                                 std::move(param.call_requester));

    experimental::ServerWriteReactor<ResponseType>* reactor = nullptr;
    if (param.status.ok()) {
      ::grpc::internal::CatchingReactorGetter(
          &reactor, get_reactor_, param.server_context, writer->request());
    }
    if (reactor == nullptr) {
      // if deserialization or reactor creator failed, we need to fail the call
      reactor = new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
          param.call->call(), sizeof(UnimplementedWriteReactor<ResponseType>)))
          UnimplementedWriteReactor<ResponseType>(param.server_context);
    }

    writer->SetupReactor(reactor);
    // The earliest that OnCancel can be called is after setup is done
    reactor->MaybeCallOnCancel();
    writer->MaybeDone();
  }

  void* Deserialize(grpc_call* call, grpc_byte_buffer* req,
                    ::grpc::Status* status, void** /*handler_data*/) final {
    ::grpc::ByteBuffer buf;
    buf.set_buffer(req);
    auto* request =
        new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
            call, sizeof(RequestType))) RequestType();
    *status =
        ::grpc::SerializationTraits<RequestType>::Deserialize(&buf, request);
    buf.Release();
    if (status->ok()) {
      return request;
    }
    request->~RequestType();
    return nullptr;
  }

 private:
  std::function<void(::grpc_impl::ServerContext*, const RequestType*,
                     experimental::ServerWriteReactor<ResponseType>**)>
      get_reactor_;

  class ServerCallbackWriterImpl
      : public experimental::ServerCallbackWriter<ResponseType> {
   public:
    void Finish(::grpc::Status s) override {
      finish_tag_.Set(call_.call(), [this](bool) { MaybeDone(); },
                      &finish_ops_);
      finish_ops_.set_core_cq_tag(&finish_tag_);

      if (!ctx_->sent_initial_metadata_) {
        finish_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                        ctx_->initial_metadata_flags());
        if (ctx_->compression_level_set()) {
          finish_ops_.set_compression_level(ctx_->compression_level());
        }
        ctx_->sent_initial_metadata_ = true;
      }
      finish_ops_.ServerSendStatus(&ctx_->trailing_metadata_, s);
      call_.PerformOps(&finish_ops_);
    }

    void SendInitialMetadata() override {
      GPR_CODEGEN_ASSERT(!ctx_->sent_initial_metadata_);
      callbacks_outstanding_.fetch_add(1, std::memory_order_relaxed);
      meta_tag_.Set(call_.call(),
                    [this](bool ok) {
                      reactor_->OnSendInitialMetadataDone(ok);
                      MaybeDone();
                    },
                    &meta_ops_);
      meta_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                    ctx_->initial_metadata_flags());
      if (ctx_->compression_level_set()) {
        meta_ops_.set_compression_level(ctx_->compression_level());
      }
      ctx_->sent_initial_metadata_ = true;
      meta_ops_.set_core_cq_tag(&meta_tag_);
      call_.PerformOps(&meta_ops_);
    }

    void Write(const ResponseType* resp,
               ::grpc::WriteOptions options) override {
      callbacks_outstanding_.fetch_add(1, std::memory_order_relaxed);
      if (options.is_last_message()) {
        options.set_buffer_hint();
      }
      if (!ctx_->sent_initial_metadata_) {
        write_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                       ctx_->initial_metadata_flags());
        if (ctx_->compression_level_set()) {
          write_ops_.set_compression_level(ctx_->compression_level());
        }
        ctx_->sent_initial_metadata_ = true;
      }
      // TODO(vjpai): don't assert
      GPR_CODEGEN_ASSERT(write_ops_.SendMessagePtr(resp, options).ok());
      call_.PerformOps(&write_ops_);
    }

    void WriteAndFinish(const ResponseType* resp, ::grpc::WriteOptions options,
                        ::grpc::Status s) override {
      // This combines the write into the finish callback
      // Don't send any message if the status is bad
      if (s.ok()) {
        // TODO(vjpai): don't assert
        GPR_CODEGEN_ASSERT(finish_ops_.SendMessagePtr(resp, options).ok());
      }
      Finish(std::move(s));
    }

   private:
    friend class CallbackServerStreamingHandler<RequestType, ResponseType>;

    ServerCallbackWriterImpl(::grpc_impl::ServerContext* ctx,
                             ::grpc::internal::Call* call,
                             const RequestType* req,
                             std::function<void()> call_requester)
        : ctx_(ctx),
          call_(*call),
          req_(req),
          call_requester_(std::move(call_requester)) {}

    void SetupReactor(experimental::ServerWriteReactor<ResponseType>* reactor) {
      reactor_ = reactor;
      write_tag_.Set(call_.call(),
                     [this](bool ok) {
                       reactor_->OnWriteDone(ok);
                       MaybeDone();
                     },
                     &write_ops_);
      write_ops_.set_core_cq_tag(&write_tag_);
      ctx_->BeginCompletionOp(&call_, [this](bool) { MaybeDone(); }, reactor);
      this->BindReactor(reactor);
    }
    ~ServerCallbackWriterImpl() { req_->~RequestType(); }

    const RequestType* request() { return req_; }

    void MaybeDone() {
      if (GPR_UNLIKELY(callbacks_outstanding_.fetch_sub(
                           1, std::memory_order_acq_rel) == 1)) {
        reactor_->OnDone();
        grpc_call* call = call_.call();
        auto call_requester = std::move(call_requester_);
        this->~ServerCallbackWriterImpl();  // explicitly call destructor
        ::grpc::g_core_codegen_interface->grpc_call_unref(call);
        call_requester();
      }
    }

    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata>
        meta_ops_;
    ::grpc::internal::CallbackWithSuccessTag meta_tag_;
    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata,
                                ::grpc::internal::CallOpSendMessage,
                                ::grpc::internal::CallOpServerSendStatus>
        finish_ops_;
    ::grpc::internal::CallbackWithSuccessTag finish_tag_;
    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata,
                                ::grpc::internal::CallOpSendMessage>
        write_ops_;
    ::grpc::internal::CallbackWithSuccessTag write_tag_;

    ::grpc_impl::ServerContext* ctx_;
    ::grpc::internal::Call call_;
    const RequestType* req_;
    std::function<void()> call_requester_;
    experimental::ServerWriteReactor<ResponseType>* reactor_;
    std::atomic<intptr_t> callbacks_outstanding_{
        3};  // reserve for OnStarted, Finish, and CompletionOp
  };
};

template <class RequestType, class ResponseType>
class CallbackBidiHandler : public ::grpc::internal::MethodHandler {
 public:
  explicit CallbackBidiHandler(
      std::function<
          void(::grpc_impl::ServerContext*,
               experimental::ServerBidiReactor<RequestType, ResponseType>**)>
          get_reactor)
      : get_reactor_(std::move(get_reactor)) {}
  void RunHandler(const HandlerParameter& param) final {
    ::grpc::g_core_codegen_interface->grpc_call_ref(param.call->call());

    auto* stream = new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
        param.call->call(), sizeof(ServerCallbackReaderWriterImpl)))
        ServerCallbackReaderWriterImpl(param.server_context, param.call,
                                       std::move(param.call_requester));

    experimental::ServerBidiReactor<RequestType, ResponseType>* reactor =
        nullptr;
    if (param.status.ok()) {
      ::grpc::internal::CatchingReactorGetter(&reactor, get_reactor_,
                                              param.server_context);
    }

    if (reactor == nullptr) {
      // if deserialization or reactor creator failed, we need to fail the call
      reactor = new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
          param.call->call(),
          sizeof(UnimplementedBidiReactor<RequestType, ResponseType>)))
          UnimplementedBidiReactor<RequestType, ResponseType>(
              param.server_context);
    }

    stream->SetupReactor(reactor);
    // The earliest that OnCancel can be called is after setup is done.
    reactor->MaybeCallOnCancel();
    stream->MaybeDone();
  }

 private:
  std::function<void(
      ::grpc_impl::ServerContext*,
      experimental::ServerBidiReactor<RequestType, ResponseType>**)>
      get_reactor_;

  class ServerCallbackReaderWriterImpl
      : public experimental::ServerCallbackReaderWriter<RequestType,
                                                        ResponseType> {
   public:
    void Finish(::grpc::Status s) override {
      finish_tag_.Set(call_.call(), [this](bool) { MaybeDone(); },
                      &finish_ops_);
      finish_ops_.set_core_cq_tag(&finish_tag_);

      if (!ctx_->sent_initial_metadata_) {
        finish_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                        ctx_->initial_metadata_flags());
        if (ctx_->compression_level_set()) {
          finish_ops_.set_compression_level(ctx_->compression_level());
        }
        ctx_->sent_initial_metadata_ = true;
      }
      finish_ops_.ServerSendStatus(&ctx_->trailing_metadata_, s);
      call_.PerformOps(&finish_ops_);
    }

    void SendInitialMetadata() override {
      GPR_CODEGEN_ASSERT(!ctx_->sent_initial_metadata_);
      callbacks_outstanding_.fetch_add(1, std::memory_order_relaxed);
      meta_tag_.Set(call_.call(),
                    [this](bool ok) {
                      reactor_->OnSendInitialMetadataDone(ok);
                      MaybeDone();
                    },
                    &meta_ops_);
      meta_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                    ctx_->initial_metadata_flags());
      if (ctx_->compression_level_set()) {
        meta_ops_.set_compression_level(ctx_->compression_level());
      }
      ctx_->sent_initial_metadata_ = true;
      meta_ops_.set_core_cq_tag(&meta_tag_);
      call_.PerformOps(&meta_ops_);
    }

    void Write(const ResponseType* resp,
               ::grpc::WriteOptions options) override {
      callbacks_outstanding_.fetch_add(1, std::memory_order_relaxed);
      if (options.is_last_message()) {
        options.set_buffer_hint();
      }
      if (!ctx_->sent_initial_metadata_) {
        write_ops_.SendInitialMetadata(&ctx_->initial_metadata_,
                                       ctx_->initial_metadata_flags());
        if (ctx_->compression_level_set()) {
          write_ops_.set_compression_level(ctx_->compression_level());
        }
        ctx_->sent_initial_metadata_ = true;
      }
      // TODO(vjpai): don't assert
      GPR_CODEGEN_ASSERT(write_ops_.SendMessagePtr(resp, options).ok());
      call_.PerformOps(&write_ops_);
    }

    void WriteAndFinish(const ResponseType* resp, ::grpc::WriteOptions options,
                        ::grpc::Status s) override {
      // Don't send any message if the status is bad
      if (s.ok()) {
        // TODO(vjpai): don't assert
        GPR_CODEGEN_ASSERT(finish_ops_.SendMessagePtr(resp, options).ok());
      }
      Finish(std::move(s));
    }

    void Read(RequestType* req) override {
      callbacks_outstanding_.fetch_add(1, std::memory_order_relaxed);
      read_ops_.RecvMessage(req);
      call_.PerformOps(&read_ops_);
    }

   private:
    friend class CallbackBidiHandler<RequestType, ResponseType>;

    ServerCallbackReaderWriterImpl(::grpc_impl::ServerContext* ctx,
                                   ::grpc::internal::Call* call,
                                   std::function<void()> call_requester)
        : ctx_(ctx), call_(*call), call_requester_(std::move(call_requester)) {}

    void SetupReactor(
        experimental::ServerBidiReactor<RequestType, ResponseType>* reactor) {
      reactor_ = reactor;
      write_tag_.Set(call_.call(),
                     [this](bool ok) {
                       reactor_->OnWriteDone(ok);
                       MaybeDone();
                     },
                     &write_ops_);
      write_ops_.set_core_cq_tag(&write_tag_);
      read_tag_.Set(call_.call(),
                    [this](bool ok) {
                      reactor_->OnReadDone(ok);
                      MaybeDone();
                    },
                    &read_ops_);
      read_ops_.set_core_cq_tag(&read_tag_);
      ctx_->BeginCompletionOp(&call_, [this](bool) { MaybeDone(); }, reactor);
      this->BindReactor(reactor);
    }

    void MaybeDone() {
      if (GPR_UNLIKELY(callbacks_outstanding_.fetch_sub(
                           1, std::memory_order_acq_rel) == 1)) {
        reactor_->OnDone();
        grpc_call* call = call_.call();
        auto call_requester = std::move(call_requester_);
        this->~ServerCallbackReaderWriterImpl();  // explicitly call destructor
        ::grpc::g_core_codegen_interface->grpc_call_unref(call);
        call_requester();
      }
    }

    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata>
        meta_ops_;
    ::grpc::internal::CallbackWithSuccessTag meta_tag_;
    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata,
                                ::grpc::internal::CallOpSendMessage,
                                ::grpc::internal::CallOpServerSendStatus>
        finish_ops_;
    ::grpc::internal::CallbackWithSuccessTag finish_tag_;
    ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata,
                                ::grpc::internal::CallOpSendMessage>
        write_ops_;
    ::grpc::internal::CallbackWithSuccessTag write_tag_;
    ::grpc::internal::CallOpSet<
        ::grpc::internal::CallOpRecvMessage<RequestType>>
        read_ops_;
    ::grpc::internal::CallbackWithSuccessTag read_tag_;

    ::grpc_impl::ServerContext* ctx_;
    ::grpc::internal::Call call_;
    std::function<void()> call_requester_;
    experimental::ServerBidiReactor<RequestType, ResponseType>* reactor_;
    std::atomic<intptr_t> callbacks_outstanding_{
        3};  // reserve for OnStarted, Finish, and CompletionOp
  };
};

}  // namespace internal
}  // namespace grpc_impl

#endif  // GRPCPP_IMPL_CODEGEN_SERVER_CALLBACK_IMPL_H

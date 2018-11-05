/*
 *
 * Copyright 2018 gRPC authors.
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

#ifndef GRPCPP_IMPL_CODEGEN_SERVER_CALLBACK_H
#define GRPCPP_IMPL_CODEGEN_SERVER_CALLBACK_H

#include <atomic>
#include <functional>

#include <grpcpp/impl/codegen/call.h>
#include <grpcpp/impl/codegen/call_op_set.h>
#include <grpcpp/impl/codegen/callback_common.h>
#include <grpcpp/impl/codegen/config.h>
#include <grpcpp/impl/codegen/core_codegen_interface.h>
#include <grpcpp/impl/codegen/server_context.h>
#include <grpcpp/impl/codegen/server_interface.h>
#include <grpcpp/impl/codegen/status.h>

namespace grpc {

// forward declarations
namespace internal {
template <class RequestType, class ResponseType>
class CallbackUnaryHandler;
}  // namespace internal

namespace experimental {

// For unary RPCs, the exposed controller class is only an interface
// and the actual implementation is an internal class.
class ServerCallbackRpcController {
 public:
  virtual ~ServerCallbackRpcController() {}

  // The method handler must call this function when it is done so that
  // the library knows to free its resources
  virtual void Finish(Status s) = 0;

  // Allow the method handler to push out the initial metadata before
  // the response and status are ready
  virtual void SendInitialMetadata(std::function<void(bool)>) = 0;
};

class ServerBidiReactor {
 public:
  virtual ~ServerBidiReactor() {}
  virtual void OnSendInitialMetadataDone(bool ok) {}
  virtual void OnReadDone(bool ok) {}
  virtual void OnWriteDone(bool ok) {}
};

class ServerReadReactor {
 public:
  virtual ~ServerReadReactor() {}
  virtual void OnSendInitialMetadataDone(bool ok) {}
  virtual void OnReadDone(bool ok) {}
};

class ServerWriteReactor {
 public:
  virtual ~ServerWriteReactor() {}
  virtual void OnSendInitialMetadataDone(bool ok) {}
  virtual void OnWriteDone(bool ok) {}
};

template <class Request>
class ServerCallbackReader {
 public:
  virtual ~ServerCallbackReader() {}

  virtual void BindReactor(ServerReadReactor* reactor) = 0;
  virtual void Finish(Status s) = 0;
  virtual void SendInitialMetadata() = 0;
  virtual void Read(Request* msg) = 0;
};

template <class Response>
class ServerCallbackWriter {
 public:
  virtual ~ServerCallbackWriter() {}

  virtual void BindReactor(ServerWriteReactor* reactor) = 0;
  virtual void Finish(Status s) = 0;
  virtual void SendInitialMetadata() = 0;
  void Write(const Response* msg) { Write(msg, WriteOptions()); }
  virtual void Write(const Response* msg, WriteOptions options) = 0;
  virtual void WriteAndFinish(const Response* msg, WriteOptions options,
                              Status s) {
    // Default implementation that can/should be overridden
    Write(msg, std::move(options));
    Finish(std::move(s));
  };
  void WriteLast(const Response* msg, WriteOptions options) {
    Write(msg, options.set_last_message());
  }
};

template <class Request, class Response>
class ServerCallbackReaderWriter {
 public:
  virtual ~ServerCallbackReaderWriter() {}

  virtual void BindReactor(ServerBidiReactor* reactor) = 0;
  virtual void Finish(Status s) = 0;
  virtual void SendInitialMetadata() = 0;
  virtual void Read(Request* msg) = 0;
  void Write(const Response* msg) { Write(msg, WriteOptions()); }
  virtual void Write(const Response* msg, WriteOptions options) = 0;
  virtual void WriteAndFinish(const Response* msg, WriteOptions options,
                              Status s) {
    Write(msg, std::move(options));
    Finish(std::move(s));
  };
  void WriteLast(const Response* msg, WriteOptions options) {
    Write(msg, options.set_last_message());
  }
};

}  // namespace experimental

namespace internal {

template <class RequestType, class ResponseType>
class CallbackUnaryHandler : public MethodHandler {
 public:
  CallbackUnaryHandler(
      std::function<void(ServerContext*, const RequestType*, ResponseType*,
                         experimental::ServerCallbackRpcController*)>
          func)
      : func_(func) {}
  void RunHandler(const HandlerParameter& param) final {
    // Arena allocate a controller structure (that includes request/response)
    g_core_codegen_interface->grpc_call_ref(param.call->call());
    auto* controller = new (g_core_codegen_interface->grpc_call_arena_alloc(
        param.call->call(), sizeof(ServerCallbackRpcControllerImpl)))
        ServerCallbackRpcControllerImpl(
            param.server_context, param.call,
            static_cast<RequestType*>(param.request),
            std::move(param.call_requester));
    Status status = param.status;

    if (status.ok()) {
      // Call the actual function handler and expect the user to call finish
      CatchingCallback(func_, param.server_context, controller->request(),
                       controller->response(), controller);
    } else {
      // if deserialization failed, we need to fail the call
      controller->Finish(status);
    }
  }

  void* Deserialize(grpc_call* call, grpc_byte_buffer* req,
                    Status* status) final {
    ByteBuffer buf;
    buf.set_buffer(req);
    auto* request = new (g_core_codegen_interface->grpc_call_arena_alloc(
        call, sizeof(RequestType))) RequestType();
    *status = SerializationTraits<RequestType>::Deserialize(&buf, request);
    buf.Release();
    if (status->ok()) {
      return request;
    }
    request->~RequestType();
    return nullptr;
  }

 private:
  std::function<void(ServerContext*, const RequestType*, ResponseType*,
                     experimental::ServerCallbackRpcController*)>
      func_;

  // The implementation class of ServerCallbackRpcController is a private member
  // of CallbackUnaryHandler since it is never exposed anywhere, and this allows
  // it to take advantage of CallbackUnaryHandler's friendships.
  class ServerCallbackRpcControllerImpl
      : public experimental::ServerCallbackRpcController {
   public:
    void Finish(Status s) override {
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
                                     finish_ops_.SendMessage(resp_));
      } else {
        finish_ops_.ServerSendStatus(&ctx_->trailing_metadata_, s);
      }
      finish_ops_.set_core_cq_tag(&finish_tag_);
      call_.PerformOps(&finish_ops_);
    }

    void SendInitialMetadata(std::function<void(bool)> f) override {
      GPR_CODEGEN_ASSERT(!ctx_->sent_initial_metadata_);
      callbacks_outstanding_++;
      // TODO(vjpai): Consider taking f as a move-capture if we adopt C++14
      //              and if performance of this operation matters
      meta_tag_.Set(call_.call(),
                    [this, f](bool ok) {
                      f(ok);
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

    ServerCallbackRpcControllerImpl(ServerContext* ctx, Call* call,
                                    const RequestType* req,
                                    std::function<void()> call_requester)
        : ctx_(ctx),
          call_(*call),
          req_(req),
          call_requester_(std::move(call_requester)) {
      ctx_->BeginCompletionOp(call, [this](bool) { MaybeDone(); });
    }

    ~ServerCallbackRpcControllerImpl() { req_->~RequestType(); }

    const RequestType* request() { return req_; }
    ResponseType* response() { return &resp_; }

    void MaybeDone() {
      if (--callbacks_outstanding_ == 0) {
        grpc_call* call = call_.call();
        auto call_requester = std::move(call_requester_);
        this->~ServerCallbackRpcControllerImpl();  // explicitly call destructor
        g_core_codegen_interface->grpc_call_unref(call);
        call_requester();
      }
    }

    CallOpSet<CallOpSendInitialMetadata> meta_ops_;
    CallbackWithSuccessTag meta_tag_;
    CallOpSet<CallOpSendInitialMetadata, CallOpSendMessage,
              CallOpServerSendStatus>
        finish_ops_;
    CallbackWithSuccessTag finish_tag_;

    ServerContext* ctx_;
    Call call_;
    const RequestType* req_;
    ResponseType resp_;
    std::function<void()> call_requester_;
    std::atomic_int callbacks_outstanding_{
        2};  // reserve for Finish and CompletionOp
  };
};

template <class RequestType, class ResponseType>
class CallbackClientStreamingHandler : public MethodHandler {
 public:
  CallbackClientStreamingHandler(
      std::function<void(ServerContext*, ResponseType*,
                         experimental::ServerCallbackReader<RequestType>*)>
          func)
      : func_(std::move(func)) {}
  void RunHandler(const HandlerParameter& param) final {
    // Arena allocate a reader structure (that includes response)
    g_core_codegen_interface->grpc_call_ref(param.call->call());
    auto* reader = new (g_core_codegen_interface->grpc_call_arena_alloc(
        param.call->call(), sizeof(ServerCallbackReaderImpl)))
        ServerCallbackReaderImpl(param.server_context, param.call,
                                 std::move(param.call_requester));
    Status status = param.status;

    if (status.ok()) {
      // Call the actual function handler and expect the user to call finish
      CatchingCallback(func_, param.server_context, reader->response(), reader);
    } else {
      // if deserialization failed, we need to fail the call
      reader->Finish(status);
    }
  }

 private:
  std::function<void(ServerContext*, ResponseType*,
                     experimental::ServerCallbackReader<RequestType>*)>
      func_;

  class ServerCallbackReaderImpl
      : public experimental::ServerCallbackReader<RequestType> {
   public:
    void BindReactor(experimental::ServerReadReactor* reactor) override {
      GPR_CODEGEN_ASSERT(reactor_ == nullptr);
      reactor_ = reactor;
    }
    void Finish(Status s) override {
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
                                     finish_ops_.SendMessage(resp_));
      } else {
        finish_ops_.ServerSendStatus(&ctx_->trailing_metadata_, s);
      }
      finish_ops_.set_core_cq_tag(&finish_tag_);
      call_.PerformOps(&finish_ops_);
    }

    void SendInitialMetadata() override {
      GPR_CODEGEN_ASSERT(!ctx_->sent_initial_metadata_);
      callbacks_outstanding_++;
      meta_tag_.Set(call_.call(),
                    [this](bool ok) {
                      if (reactor_ != nullptr) {
                        reactor_->OnSendInitialMetadataDone(ok);
                      }
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
      callbacks_outstanding_++;
      read_ops_.RecvMessage(req);
      call_.PerformOps(&read_ops_);
    }

   private:
    friend class CallbackClientStreamingHandler<RequestType, ResponseType>;

    ServerCallbackReaderImpl(ServerContext* ctx, Call* call,
                             std::function<void()> call_requester)
        : ctx_(ctx),
          call_(*call),
          call_requester_(std::move(call_requester)),
          reactor_(nullptr) {
      ctx_->BeginCompletionOp(call, [this](bool) { MaybeDone(); });
      read_tag_.Set(call_.call(),
                    [this](bool ok) {
                      if (reactor_ != nullptr) {
                        reactor_->OnReadDone(ok);
                      }
                      MaybeDone();
                    },
                    &read_ops_);
      read_ops_.set_core_cq_tag(&read_tag_);
    }

    ~ServerCallbackReaderImpl() {}

    ResponseType* response() { return &resp_; }

    void MaybeDone() {
      if (--callbacks_outstanding_ == 0) {
        grpc_call* call = call_.call();
        auto call_requester = std::move(call_requester_);
        this->~ServerCallbackReaderImpl();  // explicitly call destructor
        g_core_codegen_interface->grpc_call_unref(call);
        call_requester();
      }
    }

    CallOpSet<CallOpSendInitialMetadata> meta_ops_;
    CallbackWithSuccessTag meta_tag_;
    CallOpSet<CallOpSendInitialMetadata, CallOpSendMessage,
              CallOpServerSendStatus>
        finish_ops_;
    CallbackWithSuccessTag finish_tag_;
    CallOpSet<CallOpRecvMessage<RequestType>> read_ops_;
    CallbackWithSuccessTag read_tag_;

    ServerContext* ctx_;
    Call call_;
    ResponseType resp_;
    std::function<void()> call_requester_;
    experimental::ServerReadReactor* reactor_;
    std::atomic_int callbacks_outstanding_{
        2};  // reserve for Finish and CompletionOp
  };
};

template <class RequestType, class ResponseType>
class CallbackServerStreamingHandler : public MethodHandler {
 public:
  CallbackServerStreamingHandler(
      std::function<void(ServerContext*, const RequestType*,
                         experimental::ServerCallbackWriter<ResponseType>*)>
          func)
      : func_(std::move(func)) {}
  void RunHandler(const HandlerParameter& param) final {
    // Arena allocate a writer structure
    g_core_codegen_interface->grpc_call_ref(param.call->call());
    auto* writer = new (g_core_codegen_interface->grpc_call_arena_alloc(
        param.call->call(), sizeof(ServerCallbackWriterImpl)))
        ServerCallbackWriterImpl(param.server_context, param.call,
                                 static_cast<RequestType*>(param.request),
                                 std::move(param.call_requester));
    Status status = param.status;

    if (status.ok()) {
      // Call the actual function handler and expect the user to call finish
      CatchingCallback(func_, param.server_context, writer->request(), writer);
    } else {
      // if deserialization failed, we need to fail the call
      writer->Finish(status);
    }
  }

  void* Deserialize(grpc_call* call, grpc_byte_buffer* req,
                    Status* status) final {
    ByteBuffer buf;
    buf.set_buffer(req);
    auto* request = new (g_core_codegen_interface->grpc_call_arena_alloc(
        call, sizeof(RequestType))) RequestType();
    *status = SerializationTraits<RequestType>::Deserialize(&buf, request);
    buf.Release();
    if (status->ok()) {
      return request;
    }
    request->~RequestType();
    return nullptr;
  }

 private:
  std::function<void(ServerContext*, const RequestType*,
                     experimental::ServerCallbackWriter<ResponseType>*)>
      func_;

  class ServerCallbackWriterImpl
      : public experimental::ServerCallbackWriter<ResponseType> {
   public:
    void BindReactor(experimental::ServerWriteReactor* reactor) override {
      GPR_CODEGEN_ASSERT(reactor_ == nullptr);
      reactor_ = reactor;
    }
    void Finish(Status s) override {
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
      callbacks_outstanding_++;
      meta_tag_.Set(call_.call(),
                    [this](bool ok) {
                      if (reactor_ != nullptr) {
                        reactor_->OnSendInitialMetadataDone(ok);
                      }
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

    void Write(const ResponseType* resp, WriteOptions options) override {
      callbacks_outstanding_++;
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
      GPR_CODEGEN_ASSERT(write_ops_.SendMessage(*resp, options).ok());
      call_.PerformOps(&write_ops_);
    }

    void WriteAndFinish(const ResponseType* resp, WriteOptions options,
                        Status s) override {
      // This combines the write into the finish callback
      // Don't send any message if the status is bad
      if (s.ok()) {
        // TODO(vjpai): don't assert
        GPR_CODEGEN_ASSERT(finish_ops_.SendMessage(*resp, options).ok());
      }
      Finish(std::move(s));
    }

   private:
    friend class CallbackServerStreamingHandler<RequestType, ResponseType>;

    ServerCallbackWriterImpl(ServerContext* ctx, Call* call,
                             const RequestType* req,
                             std::function<void()> call_requester)
        : ctx_(ctx),
          call_(*call),
          req_(req),
          call_requester_(std::move(call_requester)),
          reactor_(nullptr) {
      ctx_->BeginCompletionOp(call, [this](bool) { MaybeDone(); });
      write_tag_.Set(call_.call(),
                     [this](bool ok) {
                       if (reactor_ != nullptr) {
                         reactor_->OnWriteDone(ok);
                       }
                       MaybeDone();
                     },
                     &write_ops_);
      write_ops_.set_core_cq_tag(&write_tag_);
    }
    ~ServerCallbackWriterImpl() { req_->~RequestType(); }

    const RequestType* request() { return req_; }

    void MaybeDone() {
      if (--callbacks_outstanding_ == 0) {
        grpc_call* call = call_.call();
        auto call_requester = std::move(call_requester_);
        this->~ServerCallbackWriterImpl();  // explicitly call destructor
        g_core_codegen_interface->grpc_call_unref(call);
        call_requester();
      }
    }

    CallOpSet<CallOpSendInitialMetadata> meta_ops_;
    CallbackWithSuccessTag meta_tag_;
    CallOpSet<CallOpSendInitialMetadata, CallOpSendMessage,
              CallOpServerSendStatus>
        finish_ops_;
    CallbackWithSuccessTag finish_tag_;
    CallOpSet<CallOpSendInitialMetadata, CallOpSendMessage> write_ops_;
    CallbackWithSuccessTag write_tag_;

    ServerContext* ctx_;
    Call call_;
    const RequestType* req_;
    std::function<void()> call_requester_;
    experimental::ServerWriteReactor* reactor_;
    std::atomic_int callbacks_outstanding_{
        2};  // reserve for Finish and CompletionOp
  };
};

template <class RequestType, class ResponseType>
class CallbackBidiHandler : public MethodHandler {
 public:
  CallbackBidiHandler(
      std::function<void(
          ServerContext*,
          experimental::ServerCallbackReaderWriter<RequestType, ResponseType>*)>
          func)
      : func_(std::move(func)) {}
  void RunHandler(const HandlerParameter& param) final {
    g_core_codegen_interface->grpc_call_ref(param.call->call());
    auto* stream = new (g_core_codegen_interface->grpc_call_arena_alloc(
        param.call->call(), sizeof(ServerCallbackReaderWriterImpl)))
        ServerCallbackReaderWriterImpl(param.server_context, param.call,
                                       std::move(param.call_requester));
    Status status = param.status;

    if (status.ok()) {
      // Call the actual function handler and expect the user to call finish
      CatchingCallback(func_, param.server_context, stream);
    } else {
      // if deserialization failed, we need to fail the call
      stream->Finish(status);
    }
  }

 private:
  std::function<void(
      ServerContext*,
      experimental::ServerCallbackReaderWriter<RequestType, ResponseType>*)>
      func_;

  class ServerCallbackReaderWriterImpl
      : public experimental::ServerCallbackReaderWriter<RequestType,
                                                        ResponseType> {
   public:
    void BindReactor(experimental::ServerBidiReactor* reactor) override {
      GPR_CODEGEN_ASSERT(reactor_ == nullptr);
      reactor_ = reactor;
    }
    void Finish(Status s) override {
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
      callbacks_outstanding_++;
      meta_tag_.Set(call_.call(),
                    [this](bool ok) {
                      if (reactor_ != nullptr) {
                        reactor_->OnSendInitialMetadataDone(ok);
                      }
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

    void Write(const ResponseType* resp, WriteOptions options) override {
      callbacks_outstanding_++;
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
      GPR_CODEGEN_ASSERT(write_ops_.SendMessage(*resp, options).ok());
      call_.PerformOps(&write_ops_);
    }

    void WriteAndFinish(const ResponseType* resp, WriteOptions options,
                        Status s) override {
      // Don't send any message if the status is bad
      if (s.ok()) {
        // TODO(vjpai): don't assert
        GPR_CODEGEN_ASSERT(finish_ops_.SendMessage(*resp, options).ok());
      }
      Finish(std::move(s));
    }

    void Read(RequestType* req) override {
      callbacks_outstanding_++;
      read_ops_.RecvMessage(req);
      call_.PerformOps(&read_ops_);
    }

   private:
    friend class CallbackBidiHandler<RequestType, ResponseType>;

    ServerCallbackReaderWriterImpl(ServerContext* ctx, Call* call,
                                   std::function<void()> call_requester)
        : ctx_(ctx),
          call_(*call),
          call_requester_(std::move(call_requester)),
          reactor_(nullptr) {
      ctx_->BeginCompletionOp(call, [this](bool) { MaybeDone(); });
      write_tag_.Set(call_.call(),
                     [this](bool ok) {
                       if (reactor_ != nullptr) {
                         reactor_->OnWriteDone(ok);
                       }
                       MaybeDone();
                     },
                     &write_ops_);
      write_ops_.set_core_cq_tag(&write_tag_);
      read_tag_.Set(call_.call(),
                    [this](bool ok) {
                      if (reactor_ != nullptr) {
                        reactor_->OnReadDone(ok);
                      }
                      MaybeDone();
                    },
                    &read_ops_);
      read_ops_.set_core_cq_tag(&read_tag_);
    }
    ~ServerCallbackReaderWriterImpl() {}

    void MaybeDone() {
      if (--callbacks_outstanding_ == 0) {
        grpc_call* call = call_.call();
        auto call_requester = std::move(call_requester_);
        this->~ServerCallbackReaderWriterImpl();  // explicitly call destructor
        g_core_codegen_interface->grpc_call_unref(call);
        call_requester();
      }
    }

    CallOpSet<CallOpSendInitialMetadata> meta_ops_;
    CallbackWithSuccessTag meta_tag_;
    CallOpSet<CallOpSendInitialMetadata, CallOpSendMessage,
              CallOpServerSendStatus>
        finish_ops_;
    CallbackWithSuccessTag finish_tag_;
    CallOpSet<CallOpSendInitialMetadata, CallOpSendMessage> write_ops_;
    CallbackWithSuccessTag write_tag_;
    CallOpSet<CallOpRecvMessage<RequestType>> read_ops_;
    CallbackWithSuccessTag read_tag_;

    ServerContext* ctx_;
    Call call_;
    std::function<void()> call_requester_;
    experimental::ServerBidiReactor* reactor_;
    std::atomic_int callbacks_outstanding_{
        2};  // reserve for Finish and CompletionOp
  };
};

}  // namespace internal

}  // namespace grpc

#endif  // GRPCPP_IMPL_CODEGEN_SERVER_CALLBACK_H

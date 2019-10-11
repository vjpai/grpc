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

#ifndef GRPCPP_IMPL_CODEGEN_SERVER_CALLBACK_HANDLERS_H
#define GRPCPP_IMPL_CODEGEN_SERVER_CALLBACK_HANDLERS_H

#include <grpcpp/impl/codegen/message_allocator.h>
#include <grpcpp/impl/codegen/rpc_service_method.h>
#include <grpcpp/impl/codegen/server_callback.h>
#include <grpcpp/impl/codegen/server_context.h>
#include <grpcpp/impl/codegen/status.h>

namespace grpc_impl {
namespace internal {

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
          UnimplementedUnaryReactor(
              ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, ""));
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
      finish_tag_.Set(call_.call(), [this](bool) { MaybeDone(); }, &finish_ops_,
                      reactor_->InternalInlineable());
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
                    &meta_ops_, false);
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
          UnimplementedReadReactor<RequestType>(
              ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, ""));
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
      finish_tag_.Set(call_.call(), [this](bool) { MaybeDone(); }, &finish_ops_,
                      false);
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
                    &meta_ops_, false);
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
                    &read_ops_, false);
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
          UnimplementedWriteReactor<ResponseType>(
              ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, ""));
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
      finish_tag_.Set(call_.call(), [this](bool) { MaybeDone(); }, &finish_ops_,
                      false);
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
                    &meta_ops_, false);
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
                     &write_ops_, false);
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
              ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, ""));
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
      finish_tag_.Set(call_.call(), [this](bool) { MaybeDone(); }, &finish_ops_,
                      false);
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
                    &meta_ops_, false);
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
                     &write_ops_, false);
      write_ops_.set_core_cq_tag(&write_tag_);
      read_tag_.Set(call_.call(),
                    [this](bool ok) {
                      reactor_->OnReadDone(ok);
                      MaybeDone();
                    },
                    &read_ops_, false);
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

#endif  // GRPCPP_IMPL_CODEGEN_SERVER_CALLBACK_HANDLERS_H

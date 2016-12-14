/*
 *
 * Copyright 2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/ext/client_channel/http_connect_handshaker.h"

#include <string.h>

#include <grpc/slice_buffer.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/ext/client_channel/uri_parser.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/http/format_request.h"
#include "src/core/lib/http/parser.h"
#include "src/core/lib/support/env.h"

typedef struct http_connect_handshaker {
  // Base class.  Must be first.
  grpc_handshaker base;

  char* proxy_server;
  char* server_name;

  gpr_refcount refcount;
  gpr_mu mu;

  bool shutdown;
  // Endpoint and read buffer to destroy after a shutdown.
  grpc_endpoint* endpoint_to_destroy;
  grpc_slice_buffer* read_buffer_to_destroy;

  // State saved while performing the handshake.
  grpc_handshaker_args* args;
  grpc_closure* on_handshake_done;

  // Objects for processing the HTTP CONNECT request and response.
  grpc_slice_buffer write_buffer;
  grpc_closure request_done_closure;
  grpc_closure response_read_closure;
  grpc_http_parser http_parser;
  grpc_http_response http_response;
} http_connect_handshaker;

// Unref and clean up handshaker.
static void http_connect_handshaker_unref(grpc_exec_ctx* exec_ctx,
                                          http_connect_handshaker* handshaker) {
  if (gpr_unref(&handshaker->refcount)) {
    gpr_mu_destroy(&handshaker->mu);
    if (handshaker->endpoint_to_destroy != NULL) {
      grpc_endpoint_destroy(exec_ctx, handshaker->endpoint_to_destroy);
    }
    if (handshaker->read_buffer_to_destroy != NULL) {
      grpc_slice_buffer_destroy(handshaker->read_buffer_to_destroy);
      gpr_free(handshaker->read_buffer_to_destroy);
    }
    gpr_free(handshaker->proxy_server);
    gpr_free(handshaker->server_name);
    grpc_slice_buffer_destroy(&handshaker->write_buffer);
    grpc_http_parser_destroy(&handshaker->http_parser);
    grpc_http_response_destroy(&handshaker->http_response);
    gpr_free(handshaker);
  }
}

// Set args fields to NULL, saving the endpoint and read buffer for
// later destruction.
static void cleanup_args_for_failure_locked(
    http_connect_handshaker* handshaker) {
  handshaker->endpoint_to_destroy = handshaker->args->endpoint;
  handshaker->args->endpoint = NULL;
  handshaker->read_buffer_to_destroy = handshaker->args->read_buffer;
  handshaker->args->read_buffer = NULL;
  grpc_channel_args_destroy(handshaker->args->args);
  handshaker->args->args = NULL;
}

// If the handshake failed or we're shutting down, clean up and invoke the
// callback with the error.
static void handshake_failed_locked(grpc_exec_ctx* exec_ctx,
                                    http_connect_handshaker* handshaker,
                                    grpc_error* error) {
  if (error == GRPC_ERROR_NONE) {
    // If we were shut down after an endpoint operation succeeded but
    // before the endpoint callback was invoked, we need to generate our
    // own error.
    error = GRPC_ERROR_CREATE("Handshaker shutdown");
  }
  if (!handshaker->shutdown) {
    // TODO(ctiller): It is currently necessary to shutdown endpoints
    // before destroying them, even if we know that there are no
    // pending read/write callbacks.  This should be fixed, at which
    // point this can be removed.
    grpc_endpoint_shutdown(exec_ctx, handshaker->args->endpoint);
    // Not shutting down, so the handshake failed.  Clean up before
    // invoking the callback.
    cleanup_args_for_failure_locked(handshaker);
    // Set shutdown to true so that subsequent calls to
    // http_connect_handshaker_shutdown() do nothing.
    handshaker->shutdown = true;
  }
  // Invoke callback.
  grpc_exec_ctx_sched(exec_ctx, handshaker->on_handshake_done, error, NULL);
}

// Callback invoked when finished writing HTTP CONNECT request.
static void on_write_done(grpc_exec_ctx* exec_ctx, void* arg,
                          grpc_error* error) {
  http_connect_handshaker* handshaker = arg;
  gpr_mu_lock(&handshaker->mu);
  if (error != GRPC_ERROR_NONE || handshaker->shutdown) {
    // If the write failed or we're shutting down, clean up and invoke the
    // callback with the error.
    handshake_failed_locked(exec_ctx, handshaker, GRPC_ERROR_REF(error));
    gpr_mu_unlock(&handshaker->mu);
    http_connect_handshaker_unref(exec_ctx, handshaker);
  } else {
    // Otherwise, read the response.
    // The read callback inherits our ref to the handshaker.
    grpc_endpoint_read(exec_ctx, handshaker->args->endpoint,
                       handshaker->args->read_buffer,
                       &handshaker->response_read_closure);
    gpr_mu_unlock(&handshaker->mu);
  }
}

// Callback invoked for reading HTTP CONNECT response.
static void on_read_done(grpc_exec_ctx* exec_ctx, void* arg,
                         grpc_error* error) {
  http_connect_handshaker* handshaker = arg;
  gpr_mu_lock(&handshaker->mu);
  if (error != GRPC_ERROR_NONE || handshaker->shutdown) {
    // If the read failed or we're shutting down, clean up and invoke the
    // callback with the error.
    handshake_failed_locked(exec_ctx, handshaker, GRPC_ERROR_REF(error));
    goto done;
  }
  // Add buffer to parser.
  for (size_t i = 0; i < handshaker->args->read_buffer->count; ++i) {
    if (GRPC_SLICE_LENGTH(handshaker->args->read_buffer->slices[i]) > 0) {
      size_t body_start_offset = 0;
      error = grpc_http_parser_parse(&handshaker->http_parser,
                                     handshaker->args->read_buffer->slices[i],
                                     &body_start_offset);
      if (error != GRPC_ERROR_NONE) {
        handshake_failed_locked(exec_ctx, handshaker, error);
        goto done;
      }
      if (handshaker->http_parser.state == GRPC_HTTP_BODY) {
        // Remove the data we've already read from the read buffer,
        // leaving only the leftover bytes (if any).
        grpc_slice_buffer tmp_buffer;
        grpc_slice_buffer_init(&tmp_buffer);
        if (body_start_offset <
            GRPC_SLICE_LENGTH(handshaker->args->read_buffer->slices[i])) {
          grpc_slice_buffer_add(
              &tmp_buffer,
              grpc_slice_split_tail(&handshaker->args->read_buffer->slices[i],
                                    body_start_offset));
        }
        grpc_slice_buffer_addn(&tmp_buffer,
                               &handshaker->args->read_buffer->slices[i + 1],
                               handshaker->args->read_buffer->count - i - 1);
        grpc_slice_buffer_swap(handshaker->args->read_buffer, &tmp_buffer);
        grpc_slice_buffer_destroy(&tmp_buffer);
        break;
      }
    }
  }
  // If we're not done reading the response, read more data.
  // TODO(roth): In practice, I suspect that the response to a CONNECT
  // request will never include a body, in which case this check is
  // sufficient.  However, the language of RFC-2817 doesn't explicitly
  // forbid the response from including a body.  If there is a body,
  // it's possible that we might have parsed part but not all of the
  // body, in which case this check will cause us to fail to parse the
  // remainder of the body.  If that ever becomes an issue, we may
  // need to fix the HTTP parser to understand when the body is
  // complete (e.g., handling chunked transfer encoding or looking
  // at the Content-Length: header).
  if (handshaker->http_parser.state != GRPC_HTTP_BODY) {
    grpc_slice_buffer_reset_and_unref(handshaker->args->read_buffer);
    grpc_endpoint_read(exec_ctx, handshaker->args->endpoint,
                       handshaker->args->read_buffer,
                       &handshaker->response_read_closure);
    gpr_mu_unlock(&handshaker->mu);
    return;
  }
  // Make sure we got a 2xx response.
  if (handshaker->http_response.status < 200 ||
      handshaker->http_response.status >= 300) {
    char* msg;
    gpr_asprintf(&msg, "HTTP proxy returned response code %d",
                 handshaker->http_response.status);
    error = GRPC_ERROR_CREATE(msg);
    gpr_free(msg);
    handshake_failed_locked(exec_ctx, handshaker, error);
    goto done;
  }
  // Success.  Invoke handshake-done callback.
  grpc_exec_ctx_sched(exec_ctx, handshaker->on_handshake_done, error, NULL);
done:
  // Set shutdown to true so that subsequent calls to
  // http_connect_handshaker_shutdown() do nothing.
  handshaker->shutdown = true;
  gpr_mu_unlock(&handshaker->mu);
  http_connect_handshaker_unref(exec_ctx, handshaker);
}

//
// Public handshaker methods
//

static void http_connect_handshaker_destroy(grpc_exec_ctx* exec_ctx,
                                            grpc_handshaker* handshaker_in) {
  http_connect_handshaker* handshaker = (http_connect_handshaker*)handshaker_in;
  http_connect_handshaker_unref(exec_ctx, handshaker);
}

static void http_connect_handshaker_shutdown(grpc_exec_ctx* exec_ctx,
                                             grpc_handshaker* handshaker_in) {
  http_connect_handshaker* handshaker = (http_connect_handshaker*)handshaker_in;
  gpr_mu_lock(&handshaker->mu);
  if (!handshaker->shutdown) {
    handshaker->shutdown = true;
    grpc_endpoint_shutdown(exec_ctx, handshaker->args->endpoint);
    cleanup_args_for_failure_locked(handshaker);
  }
  gpr_mu_unlock(&handshaker->mu);
}

static void http_connect_handshaker_do_handshake(
    grpc_exec_ctx* exec_ctx, grpc_handshaker* handshaker_in,
    grpc_tcp_server_acceptor* acceptor, grpc_closure* on_handshake_done,
    grpc_handshaker_args* args) {
  http_connect_handshaker* handshaker = (http_connect_handshaker*)handshaker_in;
  gpr_mu_lock(&handshaker->mu);
  // Save state in the handshaker object.
  handshaker->args = args;
  handshaker->on_handshake_done = on_handshake_done;
  // Send HTTP CONNECT request.
  gpr_log(GPR_INFO, "Connecting to server %s via HTTP proxy %s",
          handshaker->server_name, handshaker->proxy_server);
  grpc_httpcli_request request;
  memset(&request, 0, sizeof(request));
  request.host = handshaker->proxy_server;
  request.http.method = "CONNECT";
  request.http.path = handshaker->server_name;
  request.handshaker = &grpc_httpcli_plaintext;
  grpc_slice request_slice = grpc_httpcli_format_connect_request(&request);
  grpc_slice_buffer_add(&handshaker->write_buffer, request_slice);
  // Take a new ref to be held by the write callback.
  gpr_ref(&handshaker->refcount);
  grpc_endpoint_write(exec_ctx, args->endpoint, &handshaker->write_buffer,
                      &handshaker->request_done_closure);
  gpr_mu_unlock(&handshaker->mu);
}

static const grpc_handshaker_vtable http_connect_handshaker_vtable = {
    http_connect_handshaker_destroy, http_connect_handshaker_shutdown,
    http_connect_handshaker_do_handshake};

grpc_handshaker* grpc_http_connect_handshaker_create(const char* proxy_server,
                                                     const char* server_name) {
  GPR_ASSERT(proxy_server != NULL);
  GPR_ASSERT(server_name != NULL);
  http_connect_handshaker* handshaker = gpr_malloc(sizeof(*handshaker));
  memset(handshaker, 0, sizeof(*handshaker));
  grpc_handshaker_init(&http_connect_handshaker_vtable, &handshaker->base);
  gpr_mu_init(&handshaker->mu);
  gpr_ref_init(&handshaker->refcount, 1);
  handshaker->proxy_server = gpr_strdup(proxy_server);
  handshaker->server_name = gpr_strdup(server_name);
  grpc_slice_buffer_init(&handshaker->write_buffer);
  grpc_closure_init(&handshaker->request_done_closure, on_write_done,
                    handshaker);
  grpc_closure_init(&handshaker->response_read_closure, on_read_done,
                    handshaker);
  grpc_http_parser_init(&handshaker->http_parser, GRPC_HTTP_RESPONSE,
                        &handshaker->http_response);
  return &handshaker->base;
}

char* grpc_get_http_proxy_server() {
  char* uri_str = gpr_getenv("http_proxy");
  if (uri_str == NULL) return NULL;
  grpc_uri* uri = grpc_uri_parse(uri_str, false /* suppress_errors */);
  char* proxy_name = NULL;
  if (uri == NULL || uri->authority == NULL) {
    gpr_log(GPR_ERROR, "cannot parse value of 'http_proxy' env var");
    goto done;
  }
  if (strcmp(uri->scheme, "http") != 0) {
    gpr_log(GPR_ERROR, "'%s' scheme not supported in proxy URI", uri->scheme);
    goto done;
  }
  if (strchr(uri->authority, '@') != NULL) {
    gpr_log(GPR_ERROR, "userinfo not supported in proxy URI");
    goto done;
  }
  proxy_name = gpr_strdup(uri->authority);
done:
  gpr_free(uri_str);
  grpc_uri_destroy(uri);
  return proxy_name;
}

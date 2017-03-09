/*
 *
 * Copyright 2017, Google Inc.
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

#include <stddef.h>

#include "src/core/ext/transport/inproc/inproc_transport.h"
#include "src/core/ext/transport/inproc/internal.h"

static void ref_transport(grpc_inproc_transport *t) {

}

static void unref_transport(grpc_inproc_transport *t) {
  if (gpr_unref(&t->refs)) {
    // no more references at this point
    destroy_transport(t);
  }
}


static void destroy_stream(grpc_exec_ctx *, grpc_transport *gt,
                           grpc_stream *gs, void *and_free_memory) {
  grpc_inproc_transport *t = (grpc_inproc_transport *)gt;
  grpc_inproc_stream *s = (grpc_inproc_stream *)gs;

  gpr_free(and_free_memory);
};

static char *get_peer(grpc_exec_ctx *, grpc_transport *t) {
  return gpr_strdup(((grpc_inproc_transport *)t)->peer_string);
}

static grpc_endpoint *get_endpoint(grpc_exec_ctx *, grpc_transport *) {
  return NULL;
}


static const grpc_transport_vtable vtable = {
  sizeof(grpc_inproc_stream),
  init_stream,
  set_pollset,
  set_pollset_setm
  perform_stream_op,
  perform_transport_op,
  destroy_stream,
  destroy_transport,
  get_peer,
  get_endpoint
}

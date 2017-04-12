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
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/wakeup_fd_posix.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/transport/transport_impl.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

// Define the structures that are passed around opaquely
// Borrow liberally from passthru_endpoint.c

int grpc_inproc_trace = 0;

typedef struct grpc_inproc_transport {
  grpc_transport base; /* must be first */
  gpr_refcount refs;
  char *peer_string;

  grpc_fd *polling_fd; /* Used only for polling */

  gpr_mu mu;
  struct gprc_inproc_transport *peer_transport; /* other side of connection */

  grpc_connectivity_state_tracker connectivity_state_tracker_;
} grpc_inproc_transport;

typedef struct {
  gpr_mu mu;
  gpr_refcount refs;
  bool shutdown;
  struct grpc_inproc_stream *client_side;
  struct grpc_inproc_stream *server_side;
} grpc_inproc_stream_root;

typedef struct grpc_inproc_stream {
  grpc_inproc_transport *t;
  grpc_inproc_stream_root *parent;
  grpc_stream_refcount *refcount;
  grpc_slice_buffer read_buffer;
  grpc_slice_buffer *on_read_out;
  grpc_closure *on_read;
} grpc_inproc_stream;


static void destroy_transport(grpc_exec_ctx *exec_ctx, grpc_transport *gt);

/*******************************************************************************
 * POLLSET STUFF
 */

static void set_pollset(grpc_exec_ctx *exec_ctx, grpc_transport *gt,
                        grpc_stream *gs, grpc_pollset *pollset) {
  grpc_inproc_transport *t = (grpc_inproc_transport *)gt;
  grpc_pollset_add_fd(exec_ctx, pollset, t->polling_fd);
}

static void set_pollset_set(grpc_exec_ctx *exec_ctx, grpc_transport *gt,
                            grpc_stream *gs, grpc_pollset_set *pollset_set) {
  grpc_inproc_transport *t = (grpc_inproc_transport *)gt;
  grpc_pollset_set_add_fd(exec_ctx, pollset_set, t->polling_fd);
}

/*******************************************************************************
 * SETUP AND DESTROY
 */

static void ref_transport(grpc_inproc_transport *t) {
  gpr_ref(&t->refs);
}

static void unref_transport(grpc_inproc_transport *t) {
  if (gpr_unref(&t->refs)) {
    // no more references at this point
    destroy_transport(t);
  }
}

// Implement a hash table mapping from address string to server listeners

grpc_transport *grpc_create_inproc_transport(
    grpc_exec_ctx *exec_ctx, const grpc_channel_args *channel_args,
    grpc_endpoint *ep, int is_client) {
  grpc_inproc_transport *t = gpr_malloc(sizeof(*t));
  grpc_connectivity_state_init(&t->connectivity_state_tracker_,
			       GRPC_CHANNEL_READY,
			       "inproc_transport");
  ref_transport(t);
  return (grpc_transport *)t;
}

static int init_stream(grpc_exec_ctx *exec_ctx, grpc_transport *gt,
                       grpc_stream *gs, grpc_stream_refcount *refcount,
                       const void *server_data, gpr_arena *arena) {
  grpc_inproc_transport *t = (grpc_inproc_transport *)gt;
  grpc_inproc_stream *s = (grpc_inproc_stream *)gs;

  s->t = t;
  s->refcount = refcount;
  if (server_data) {
    // this is a server-accepted stream
    grpc_inproc_stream_root *root = (grpc_inproc_stream_root *)server_data;
    gpr_mu_lock(&root->mu);
    // Do not need to increment ref count since this just transfers ref count
    // from the initiator
    root->server_side = s;
    gpr_mu_unlock(&root->mu);
  } else {
    // this is a stream being initiated by a client
    // it should create the stream root that will be used to connect both sides
    grpc_inproc_stream_root* root = (grpc_inproc_stream_root *)gpr_malloc(sizeof(*root));
    gpr_mu_init(&root->mu);
    gpr_ref_init(&root->refs, 1);
    root->shutdown = false;
    root->client_side = s;
    root->server_side = NULL;
    // Pass the root to the server side and ref it before doing so
  }

  return 0;
}

static void destroy_stream(grpc_exec_ctx *exec_ctx, grpc_transport *gt,
                           grpc_stream *gs, void *and_free_memory) {
  grpc_inproc_transport *t = (grpc_inproc_transport *)gt;
  grpc_inproc_stream *s = (grpc_inproc_stream *)gs;

  gpr_free(and_free_memory);
};

static void destroy_transport(grpc_exec_ctx *exec_ctx, grpc_transport *gt) {
  grpc_inproc_transport *t = (grpc_inproc_transport *)gt;
  grpc_connectivity_state_set(
      exec_ctx, &connectivity_state_tracker_, GRPC_CHANNEL_SHUTDOWN,
      GRPC_ERROR_CREATE_FROM_STATIC_STRING("Destroying transport."),
      "destroy transport");
}

/*******************************************************************************
 * OPERATIONS
 */

static void perform_stream_op(grpc_exec_ctx *exec_ctx, grpc_transport *gt,
                              grpc_stream *gs,
			      grpc_transport_stream_op_batch *op) {
  GPR_TIMER_BEGIN("perform_stream_op", 0);
  grpc_inproc_transport *t = (grpc_inproc_transport *)gt;
  grpc_inproc_stream *s = (grpc_inproc_stream *)gs;

  if (grpc_inproc_trace) {
    char *str = grpc_transport_stream_op_batch_string(op);
    gpr_log(GPR_DEBUG, "perform_stream_op[s=%p/%d]: %s", s, s->id, str);
    gpr_free(str);
  }

  op->handler_private.args[0] = gt;
  op->handler_private.args[1] = gs;
  GRPC_INPROC_STREAM_REF(s, "perform_stream_op");
  grpc_closure_sched(
      exec_ctx,
      grpc_closure_init(
          &op->handler_private.closure, perform_stream_op_locked, op,
          grpc_combiner_scheduler(t->combiner, op->covered_by_poller)),
      GRPC_ERROR_NONE);
  GPR_TIMER_END("perform_stream_op", 0);
}

static void perform_transport_op(grpc_exec_ctx *exec_ctx, grpc_transport *gt,
                                 grpc_transport_op *op) {
  grpc_inproc_transport *t = (grpc_inproc_transport *)gt;
  char *msg = grpc_transport_op_string(op);
  gpr_free(msg);
  op->transport_private.args[0] = gt;
  GRPC_INPROC_REF_TRANSPORT(t, "transport_op");
  grpc_closure_sched(
      exec_ctx, grpc_closure_init(&op->transport_private.closure,
                                  perform_transport_op_locked, op,
                                  grpc_combiner_scheduler(t->combiner, false)),
      GRPC_ERROR_NONE);
}


/*******************************************************************************
 * MISC STUFF
 */

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
  set_pollset_set,
  perform_stream_op,
  perform_transport_op,
  destroy_stream,
  destroy_transport,
  get_peer,
  get_endpoint
};

/*******************************************************************************
 * API functions: transport creation, channel creation, server port addition
 */


grpc_transport *grpc_create_inproc_transport(
    grpc_exec_ctx *exec_ctx, const grpc_channel_args *channel_args,
    grpc_endpoint *ep, int is_client) {

}

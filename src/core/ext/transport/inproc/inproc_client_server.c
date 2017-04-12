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

#include <string.h>
#include <grpc/support/alloc.h>

#include "src/core/ext/filters/client_channel/client_channel.h"
#include "src/core/ext/filters/client_channel/subchannel.h"
#include "src/core/lib/iomgr/inproc_address.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/surface/channel_stack_type.h"
#include "src/core/lib/surface/server.h"

/*******************************************************************************
 * DICTIONARY - TRACK INPROC PORTS
 */

/* Simple linked-list dictionary, not perf critical */
typedef struct port_info {
  grpc_server *server;
  int id;
} port_info;

static gpr_atm g_port_counter;

void port_info_init(port_info *portinfo, grpc_server *server) {
  portinfo->server = server;
  portinfo->id = (int)gpr_atm_no_barrier_fetch_add(&g_port_counter, (gpr_atm)1);
}

typedef struct port_list {
  const char *portname;
  port_info portinfo;
  struct port_list *next;
} port_list;

typedef struct {
  gpr_mu mu;
  port_list *head;
} port_dictionary;

static port_dictionary g_port_dictionary;

static bool port_lookup(const char *pathname, port_info *portinfo) {
  gpr_mu_lock(&g_port_dictionary.mu);
  port_list *i = g_port_dictionary.head;
  while (i != NULL) {
    if (strcmp(i->portname, pathname) == 0) {
      *portinfo = i->portinfo;
      gpr_mu_unlock(&g_port_dictionary.mu);
      return true;
    }
    i = i->next;
  }
  gpr_mu_unlock(&g_port_dictionary.mu);
  return false;
};

static bool port_add(const char *pathname, port_info portinfo) {
  port_info dummy;
  if (port_lookup(pathname, &dummy)) return false;

  port_list *newent = gpr_malloc(sizeof(*newent));
  if (newent == NULL) return false;

  gpr_mu_lock(&g_port_dictionary.mu);
  newent->portinfo = portinfo;
  newent->next = g_port_dictionary.head;
  g_port_dictionary.head = newent;
  gpr_mu_unlock(&g_port_dictionary.mu);
  return true;
}

static bool port_delete(const char *pathname) {
  gpr_mu_lock(&g_port_dictionary.mu);
  port_list *curr = g_port_dictionary.head;
  port_list *prev = NULL;
  while (curr != NULL) {
    if (strcmp(curr->portname, pathname) == 0) {
      if (prev != NULL) {
	prev->next = curr->next;
      } else {
	g_port_dictionary.head = curr->next;
      }
      gpr_free(curr);
      gpr_mu_unlock(&g_port_dictionary.mu);
      return true;
    }
    prev = curr;
    curr = curr->next;
  }
  gpr_mu_unlock(&g_port_dictionary.mu);
  return false;
}

static void dictionary_init() {
  gpr_mu_init(&g_port_dictionary.mu);
  g_port_dictionary.head = NULL;
}

static void dictionary_destroy() {
  port_list *i = g_port_dictionary.head;
  while (i != NULL) {
    port_list *del = i;
    i = i->next;
    gpr_free(del);
  }  
  g_port_dictionary.head = NULL;
  gpr_mu_destroy(&g_port_dictionary.mu);
}

/*******************************************************************************
 * CONNECTOR AND CLIENT-CHANNEL FACTORY
 */

typedef struct {
  grpc_connector base;
  gpr_refcount refs;

  grpc_connect_in_args args;
  grpc_connect_out_args* result;
} connector;

static void connector_ref(grpc_connector* con) {
  connector* c = (connector*)con;
  gpr_ref(&c->refs);
}

static void connector_unref(grpc_exec_ctx* exec_ctx, grpc_connector* con) {
  connector* c = (connector*)con;
  if (gpr_unref(&c->refs)) {
    gpr_free(c);
  }
}

static void connector_shutdown(grpc_exec_ctx* exec_ctx, grpc_connector* con,
                        grpc_error* why) {
  GRPC_ERROR_UNREF(why);
}

static void connector_connect(grpc_exec_ctx* exec_ctx, grpc_connector* con,
                       const grpc_connect_in_args* args,
                       grpc_connect_out_args* result, grpc_closure* notify) {
  grpc_resolved_address addr;
  grpc_get_subchannel_address_arg(exec_ctx, args->channel_args, &addr);
  grpc_transport* transport = NULL;
  port_info portinfo;
  grpc_sockaddr_inproc *saddr = (grpc_sockaddr_inproc *)(addr.addr);
  if (!port_lookup(saddr->sa_path, &portinfo)) {
    // TODO(vjpai): give an error that this port isn't available
    return;
  }
  // TODO(vjpai): assign this to a transport corresponding to that portinfo

  connector* c = (connector*)con;
  GPR_ASSERT(notify->cb);
  c->args = *args;
  c->result = result;
  c->result->transport = transport;
  c->result->channel_args = grpc_channel_args_copy(c->args.channel_args);

  // Performed synchronous connect, notify now.
  grpc_closure_sched(exec_ctx, notify, GRPC_ERROR_NONE);
}

static const grpc_connector_vtable connector_vtable = {
    connector_ref, connector_unref, connector_shutdown, connector_connect};

static void client_channel_factory_ref(grpc_client_channel_factory* scf) {}

static void client_channel_factory_unref(grpc_exec_ctx* exec_ctx,
                                  grpc_client_channel_factory* scf) {}

static grpc_subchannel* client_channel_factory_create_subchannel(
    grpc_exec_ctx* exec_ctx, grpc_client_channel_factory* f,
    const grpc_subchannel_args* args) {
  connector* c = (connector*)gpr_malloc(sizeof(*c));
  grpc_subchannel* s;
  memset(c, 0, sizeof(*c));
  c->base.vtable = &connector_vtable;
  gpr_ref_init(&c->refs, 1);
  s = grpc_subchannel_create(exec_ctx, &c->base, args);
  grpc_connector_unref(exec_ctx, &c->base);
  return s;
}

static grpc_channel* client_channel_factory_create_channel(
    grpc_exec_ctx* exec_ctx, grpc_client_channel_factory* f, const char* target,
    grpc_client_channel_type type, const grpc_channel_args* args) {
  // Add channel arg containing the server URI.
  grpc_arg arg;
  arg.type = GRPC_ARG_STRING;
  arg.key = (char*)GRPC_ARG_SERVER_URI;
  arg.value.string = (char*)target;
  grpc_channel_args* new_args = grpc_channel_args_copy_and_add(args, &arg, 1);
  grpc_channel* channel = grpc_channel_create(exec_ctx, target, new_args,
                                              GRPC_CLIENT_CHANNEL, NULL);
  grpc_channel_args_destroy(exec_ctx, new_args);

  return channel;
}

static const grpc_client_channel_factory_vtable client_channel_factory_vtable = {
    client_channel_factory_ref, client_channel_factory_unref,
    client_channel_factory_create_subchannel,
    client_channel_factory_create_channel};

static grpc_client_channel_factory g_client_channel_factory = {
    &client_channel_factory_vtable};

/*******************************************************************************
 * API FUNCTIONS
 */

int grpc_server_add_inproc_port(grpc_server *server, const char *addr) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  int port_num = -1;
  GRPC_API_TRACE("grpc_server_add_inproc_port(server=%p, addr=%s)", 2,
                 (server, addr));
  grpc_channel_args *channel_args =
    grpc_channel_args_copy(grpc_server_get_channel_args(server));

  port_info portinfo;
  port_info_init(&portinfo, server);
  if (!port_add(addr, portinfo)) {
    grpc_channel_args_destroy(exec_ctx, channel_args);
    return -1;
  }

  grpc_exec_ctx_finish(&exec_ctx);
  return portinfo.id;
}

grpc_channel *grpc_inproc_channel_create(const char *target,
                                         grpc_channel_args *channel_args,
                                         void *reserved) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  GRPC_API_TRACE("grpc_inproc_channel_create(target=%p, args=%p, reserved=%p)",
		 3, (target, channel_args, reserved));
  GPR_ASSERT(reserved == nullptr);
  // Add channel arg containing the client channel factory.
  grpc_client_channel_factory* f = &g_client_channel_factory;
  grpc_arg arg = grpc_client_channel_factory_create_channel_arg(f);
  grpc_channel_args* new_args = grpc_channel_args_copy_and_add(args, &arg, 1);

  grpc_channel* channel = client_channel_factory_create_channel(
      &exec_ctx, f, target, GRPC_CLIENT_CHANNEL_TYPE_REGULAR, new_args);

  grpc_channel_args_destroy(&exec_ctx, new_args);

  grpc_exec_ctx_finish(&exec_ctx);

  return channel != nullptr ? channel : grpc_lame_client_channel_create(
                                            target, GRPC_STATUS_INTERNAL,
                                            "Failed to create client channel");
}

void grpc_inproc_ports_init(void) {
  dictionary_init();
  gpr_atm_rel_store(&g_port_counter, (gpr_atm)1);
}

void grpc_inproc_ports_shutdown(void) {
  dictionary_destroy();
}

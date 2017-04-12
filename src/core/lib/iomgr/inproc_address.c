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

#include <inttypes.h>
#include <string.h>
#include <sys/socket.h>

#include "src/core/lib/iomgr/port.h"
#include "src/core/lib/iomgr/inproc_address.h"

#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>
#include <grpc/support/useful.h>

grpc_error *grpc_resolve_inproc_address(const char *name,
					  grpc_resolved_addresses **addrs) {
  const size_t maxlen = GPR_ARRAY_SIZE(((grpc_sockaddr_inproc *)0)->sa_path) - 1;
  if (strlen(name) > maxlen) {
    char *err_msg;
    grpc_error *err;
    gpr_asprintf(&err_msg,
		 "Inproc target name should not have more than %" PRIuPTR " characters.",
		 maxlen);
    err = GRPC_ERROR_CREATE_FROM_COPIED_STRING(err_msg);
    gpr_free(err_msg);
    return err;
  }
  *addrs = gpr_malloc(sizeof(grpc_resolved_addresses));
  (*addrs)->naddrs = 1;
  (*addrs)->addrs = gpr_malloc(sizeof(grpc_resolved_address));
  grpc_sockaddr_inproc *in = (grpc_sockaddr_inproc *)(*addrs)->addrs->addr;
  in->sa_family = AF_UNSPEC;
  strcpy(in->sa_path, name);
  (*addrs)->addrs->len = strlen(in->sa_path) + sizeof(in->sa_family) + 1;
  return GRPC_ERROR_NONE;
}




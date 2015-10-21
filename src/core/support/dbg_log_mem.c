/*
 *
 * Copyright 2015, Google Inc.
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

#include <stdio.h>
#include <string.h>

#include <grpc/support/sync.h>

#include "src/core/support/block_annotate.h"
#include "src/core/support/dbg_log_mem.h"

#define MAXSIZE ((size_t)(4<<20))
static gpr_uint8 g_dbg_log[MAXSIZE];
static size_t g_dbg_log_end;
static gpr_mu g_dbg_log_mu;

void gpr_dbg_log_init() {
  g_dbg_log_end = 0;
  gpr_mu_init(&g_dbg_log_mu);
}

void gpr_dbg_log_destroy() {
  gpr_mu_destroy(&g_dbg_log_mu);
}

void gpr_dbg_log_add(gpr_uint16 tag, gpr_uint16 size, void *obj) {
  gpr_mu_lock(&g_dbg_log_mu);
  if (g_dbg_log_end + size + sizeof(tag) + sizeof(size) > MAXSIZE) {
    /* This implementation only allows a limited size log. */
    g_dbg_log_end = 0;
  }
  memcpy(g_dbg_log+g_dbg_log_end, &tag, sizeof(tag));
  g_dbg_log_end += sizeof(tag);
  memcpy(g_dbg_log+g_dbg_log_end, &size, sizeof(size));
  g_dbg_log_end += sizeof(size);
  memcpy(g_dbg_log+g_dbg_log_end, obj, size);
  g_dbg_log_end += size;

  gpr_mu_unlock(&g_dbg_log_mu);
}

void gpr_dbg_log_report() {
  FILE *file;
  GRPC_SCHEDULING_START_BLOCKING_REGION;
  file = fopen("dbg-log.log", "wb");
  if (file == NULL)
    goto end;

  gpr_mu_lock(&g_dbg_log_mu);
  fwrite(g_dbg_log, sizeof(g_dbg_log[0]), g_dbg_log_end, file);
  gpr_mu_unlock(&g_dbg_log_mu);

  fclose(file);
end:
  GRPC_SCHEDULING_END_BLOCKING_REGION;
}

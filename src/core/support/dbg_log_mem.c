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

#ifdef __linux__
#ifndef _POSIX_SOURCE
#define _POSIX_SOURCE
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <linux/unistd.h>
#include <unistd.h>
#include <sys/syscall.h>

#else
#include <pthread.h>
#endif

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <grpc/support/port_platform.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>

#include "src/core/support/block_annotate.h"
#include "src/core/support/dbg_log_mem.h"

#define MAXSIZE ((size_t)(4<<20))
static gpr_uint8 g_dbg_log[MAXSIZE];
static size_t g_dbg_log_end;
static gpr_mu g_dbg_log_mu;

static long gettid(void) {
#ifdef GPR_LINUX
  return syscall(__NR_gettid);
#else
  return (long)pthread_self();
#endif
}

struct dbg_entry {
  gpr_uint64 tid;
  struct gpr_timespec now;
  const char *tag;
  gpr_uint16 size;
};

void gpr_dbg_log_init() {
  g_dbg_log_end = 0;
  gpr_mu_init(&g_dbg_log_mu);
}

void gpr_dbg_log_destroy() {
  gpr_mu_destroy(&g_dbg_log_mu);
}

void gpr_dbg_log_add(const char *tag, gpr_uint16 size, const void *obj) {
  struct dbg_entry entry;

  entry.now = gpr_now(GPR_CLOCK_MONOTONIC);
  entry.tid = (gpr_uint64)gettid();
  entry.tag = tag;
  entry.size = size;

  gpr_mu_lock(&g_dbg_log_mu);
  if (g_dbg_log_end + size + sizeof(entry) > MAXSIZE) {
    /* This implementation only allows a limited size log. */
    g_dbg_log_end = 0;
  }
  memcpy(g_dbg_log+g_dbg_log_end, &entry, sizeof(entry));
  g_dbg_log_end += sizeof(entry);
  memcpy(g_dbg_log+g_dbg_log_end, obj, size);
  g_dbg_log_end += size;

  gpr_mu_unlock(&g_dbg_log_mu);
}

void gpr_dbg_log_report() {
  FILE *file;
  char filename[256];
  struct timeval tv;
  struct dbg_entry entry;
  size_t posn;
  size_t i;
  int first_seen = 0;
  struct dbg_entry entry_first;

  gettimeofday(&tv, NULL);
  sprintf(filename, "dbg-log-%d-%06d.log", (int)tv.tv_sec, (int)tv.tv_usec);
  GRPC_SCHEDULING_START_BLOCKING_REGION;
  file = fopen(filename, "wb");
  if (file == NULL)
    goto end;

  gpr_mu_lock(&g_dbg_log_mu);
  for (posn = 0; posn < g_dbg_log_end; ) {
    entry = *((struct dbg_entry *)(g_dbg_log+posn));
    posn += sizeof(entry);
    if (!first_seen) {
      first_seen = 1;
      entry_first = entry;
    }
    fprintf(file, "T %lu %u.%09u %s size %u",
            (unsigned long)entry.tid, (unsigned)entry.now.tv_sec,
            (unsigned)entry.now.tv_nsec, entry.tag,
            (unsigned)entry.size);
    if (entry.size == sizeof(unsigned)) {
      fprintf(file, " (value 0x%x)", *((unsigned*)(g_dbg_log+posn)));
    } else if (entry.size == sizeof(unsigned long)) {
      fprintf(file, " (value 0x%lx)", *((unsigned long*)(g_dbg_log+posn)));
    }

    fprintf(file, "\n");
    for (i=0; i<entry.size; i++, posn++) {
      if (i && !(i&0xf))
        fprintf(file, "\n");
      else if (i)
        fprintf(file, " ");
      fprintf(file, "%02x", *((gpr_uint8*)(g_dbg_log+posn)));
    }
    if (entry.size)
      fprintf(file, "\n");
  }
  if (first_seen) {
    struct gpr_timespec dur;
    dur = gpr_time_sub(entry.now, entry_first.now);
    fprintf(file, "\nTotal log time %u.%09u\n", (unsigned)dur.tv_sec,
            (unsigned)dur.tv_nsec);
  }
  gpr_mu_unlock(&g_dbg_log_mu);

  fclose(file);
end:
  GRPC_SCHEDULING_END_BLOCKING_REGION;
}

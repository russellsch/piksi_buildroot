/*
 * Copyright (C) 2017 Swift Navigation Inc.
 * Contact: Swift Navigation <dev@swiftnav.com>
 *
 * This source is subject to the license found in the file 'LICENSE' which must
 * be be distributed together with this source. All other rights reserved.
 *
 * THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,
 * EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>

#include <libpiksi/sbp_zmq_pubsub.h>
#include <libpiksi/logging.h>
#include <libsbp/logging.h>

#include "cellmodem.h"
#include "cellmodem_inotify.h"
#include "cellmodem_probe.h"

#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

typedef struct inotify_ctx_s {
  sbp_zmq_pubsub_ctx_t *pubsub_ctx;
  zloop_t *loop;
  zmq_pollitem_t pollitem;
  int inotify_fd;
  int watch_descriptor;
  char *cellmodem_dev;
  enum modem_type modem_type;
} inotify_ctx_t;

bool cellmodem_tty_exists(const char* path) {

  char full_path[PATH_MAX];
  snprintf(full_path, sizeof(full_path), "/dev/%s", path);

  struct stat buf;
  int ret = stat(full_path, &buf);

  return ret == 0 && S_ISCHR(buf.st_mode);
}

static int update_dev_from_probe(inotify_ctx_t *ctx, char *dev)
{
  if (ctx == NULL) {
    return 1;
  }
  if (dev == NULL || dev[0] == '\0') {
    return 1;
  }
  if (!cellmodem_tty_exists(dev)) {
    piksi_log(LOG_WARNING,
              "Update dev tty does not exist: '%s'",
              dev);
    return 1;
  }
  char *dev_override = cellmodem_get_dev_override();
  if (dev_override != NULL && strcmp(dev_override, dev) != 0) {
    return 1;
  }
  ctx->modem_type = cellmodem_probe(dev);
  if (ctx->modem_type != MODEM_TYPE_INVALID) {
    ctx->cellmodem_dev = strdup(dev);
    cellmodem_set_dev(ctx->pubsub_ctx, ctx->cellmodem_dev , ctx->modem_type );
    return 0;
  } else if (dev_override != NULL && strcmp(dev_override, dev) == 0) {
    piksi_log(LOG_WARNING,
              "Override device failed probe: %s",
              dev);
  }
  return 1;
}

void cellmodem_set_dev_to_invalid(inotify_ctx_t *ctx)
{
  if (ctx == NULL) {
    return;
  }
  ctx->modem_type = MODEM_TYPE_INVALID;
  if (ctx->cellmodem_dev != NULL) {
    free(ctx->cellmodem_dev);
    ctx->cellmodem_dev = NULL;
  }
  cellmodem_set_dev(ctx->pubsub_ctx, NULL, MODEM_TYPE_INVALID);
}

void cellmodem_scan_for_modem(inotify_ctx_t *ctx)
{
  if (ctx == NULL) {
    return;
  }
  /* Try what's already here.  Inotify will only tell us about new devs */
  DIR *dir = opendir("/dev");
  struct dirent *ent = readdir(dir);
  for (struct dirent *ent = readdir(dir); ent; ent = readdir(dir)) {
    if (ent->d_type == DT_CHR) {
      if (update_dev_from_probe(ctx, ent->d_name) == 0) {
        break;
      }
    }
  }
  closedir(dir);
}

static int inotify_output_cb(zloop_t *loop, zmq_pollitem_t *item, void *arg)
{
  inotify_ctx_t *ctx = (inotify_ctx_t*) arg;

  char buf[BUF_LEN] __attribute__ ((aligned(8)));
  ssize_t count = read(ctx->inotify_fd, buf, BUF_LEN);

  if (count == 0) {
    piksi_log(LOG_ERR, "inotify read failed");
    return 0;
  }

  if (count < -1) {
    piksi_log(LOG_ERR, "inotify other error");
    return 0;
  }

  for (char *p = buf; p < buf + count; ) {

    struct inotify_event *event = (struct inotify_event *) p;

    if (event->mask & IN_CREATE) {
      piksi_log(LOG_DEBUG, "got notification that '%s' was created", event->name);
      if ((ctx->modem_type == MODEM_TYPE_INVALID) && cellmodem_tty_exists(event->name)) {
        usleep(100000);
        if (update_dev_from_probe(ctx, event->name) == 0) {
          piksi_log(LOG_DEBUG, "set modem device to '%s' from notification", event->name);
        }
      }
    } else if (event->mask & IN_DELETE) {
      piksi_log(LOG_DEBUG, "got notification that '%s' was deleted", event->name);
      if ((ctx->cellmodem_dev != NULL) &&
          (strcmp(ctx->cellmodem_dev, event->name) == 0)) {
        cellmodem_set_dev_to_invalid(ctx);
      }
    } else {
      piksi_log(LOG_WARNING, "unhandled inotify event");
    }

    p += sizeof(struct inotify_event) + event->len;
  }

  return 0;
}

inotify_ctx_t * async_wait_for_tty(sbp_zmq_pubsub_ctx_t *pubsub_ctx)
{
  int inotify_fd = inotify_init1(IN_NONBLOCK);

  if (inotify_fd < 0) {
    piksi_log(LOG_DEBUG, "inotify init failed: %d", errno);
    goto fail;
  }

  int watch_descriptor = inotify_add_watch(inotify_fd, "/dev", IN_CREATE | IN_DELETE);

  if (watch_descriptor < 0) {
    piksi_log(LOG_DEBUG, "inotify add failed: %d", errno);
    goto fail;
  }

  inotify_ctx_t *ctx = calloc(1, sizeof(*ctx));

  ctx->pubsub_ctx = pubsub_ctx;
  ctx->inotify_fd = inotify_fd;
  ctx->pollitem.fd = inotify_fd;
  ctx->pollitem.events = ZMQ_POLLIN|ZMQ_POLLERR;

  ctx->loop = sbp_zmq_pubsub_zloop_get(pubsub_ctx);
  zloop_poller(ctx->loop, &ctx->pollitem, inotify_output_cb, ctx);

  cellmodem_scan_for_modem(ctx);

  zloop_timer(sbp_zmq_pubsub_zloop_get(pubsub_ctx), 1000, 0, override_probe_retry, ctx);

  return ctx;

fail:
  piksi_log(LOG_DEBUG, "modem detection error, waiting to retry");
  zloop_timer(sbp_zmq_pubsub_zloop_get(pubsub_ctx), 5000, 1, pppd_respawn, pubsub_ctx);
  return NULL;
}

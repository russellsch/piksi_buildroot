/*
 * Copyright (C) 2018 Swift Navigation Inc.
 * Contact: Swift Navigation <dev@swiftnav.com>
 *
 * This source is subject to the license found in the file 'LICENSE' which must
 * be be distributed together with this source. All other rights reserved.
 *
 * THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,
 * EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <getopt.h>
#include <string.h>
#include <unistd.h>

#include <czmq.h>

#include <libpiksi/logging.h>
#include <libpiksi/sbp_zmq_pubsub.h>
#include <libpiksi/settings.h>
#include <libpiksi/util.h>

#include <libsbp/sbp.h>
#include <libsbp/piksi.h>
#include <libsbp/system.h>

#include "at_command_utils.h"

#define PROGRAM_NAME "cell_modem_daemon"

#define SBP_SUB_ENDPOINT ">tcp://127.0.0.1:43060" /* SBP Internal Out */
#define SBP_PUB_ENDPOINT ">tcp://127.0.0.1:43061" /* SBP Internal In */

#define SBP_FRAMING_MAX_PAYLOAD_SIZE (255u)
#define CELL_STATUS_UPDATE_INTERVAL (1000u)

bool cell_modem_enable_watch = false;

u8 *port_name = NULL;
u8 *command_string = NULL;

struct cell_modem_ctx_s {
  sbp_zmq_pubsub_ctx_t *sbp_ctx;
  at_serial_port_t *port;
};

static void usage(char *command)
{
  printf("Usage: %s\n", command);

  puts("\nMain options");
  puts("\t--serial-port <serial port for cell modem>");
  puts("\t--at-command <command to send to modem>");
}

static int parse_options(int argc, char *argv[])
{
  enum { OPT_ID_SERIAL_PORT = 1, OPT_ID_AT_COMMAND = 2 };

  const struct option long_opts[] = {
    { "serial-port", required_argument, 0, OPT_ID_SERIAL_PORT },
    { "at-command", required_argument, 0, OPT_ID_AT_COMMAND },
    { 0, 0, 0, 0 },
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
    switch (opt) {
    case OPT_ID_SERIAL_PORT: {
      port_name = (u8 *)optarg;
    } break;

    case OPT_ID_AT_COMMAND: {
      command_string = (u8 *)optarg;
    } break;

    default: {
      puts("Invalid option");
      return -1;
    } break;
    }
  }

  if (port_name == NULL) {
    puts("Missing port");
    return -1;
  }

  return 0;
}

/**
 * @brief send_cell_modem_status
 *
 * Sends relevant AT commands to cell modem and records the
 * result in an sbp message which is then sent to the external
 * sbp endpoint
 * @param cell_modem_ctx: local context with port and pubsub_ctx structures
 */
static void send_cell_modem_status(struct cell_modem_ctx_s *cell_modem_ctx)
{
  s8 signal_strength = 0;
  float error_rate = 0.0;
  if (at_command_report_signal_quality(cell_modem_ctx->port, &signal_strength, &error_rate) != 0)
  {
    // failed to parse command
    return;
  }
  msg_cell_modem_status_t cell_status_msg = {
    .signal_strength = signal_strength,
    .signal_error_rate = error_rate
  };
  size_t message_length = sizeof(msg_cell_modem_status_t);
  if (message_length > SBP_FRAMING_MAX_PAYLOAD_SIZE) {
    piksi_log(LOG_ERR, "Cell Modem Status surpassing SBP frame size");
    return;
  } else {
    sbp_zmq_tx_send(sbp_zmq_pubsub_tx_ctx_get(cell_modem_ctx->sbp_ctx),
                    SBP_MSG_CELL_MODEM_STATUS,
                    (u8)(0xFF & message_length),
                    (u8 *)&cell_status_msg);
  }
}

/**
 * @brief cell_status_timer_callback - used to trigger cell status updates
 */
static int cell_status_timer_callback(zloop_t *loop, int timer_id, void *arg)
{
  (void)loop;
  (void)timer_id;
  struct cell_modem_ctx_s *cell_modem_ctx = (struct cell_modem_ctx_s *)arg;

  if (cell_modem_enable_watch) {
    send_cell_modem_status(cell_modem_ctx);
  }

  return 0;
}

static int cleanup(settings_ctx_t **settings_ctx_loc,
                   sbp_zmq_pubsub_ctx_t **pubsub_ctx_loc,
                   at_serial_port_t **port_loc,
                   int status);

int main(int argc, char *argv[])
{
  settings_ctx_t *settings_ctx = NULL;
  sbp_zmq_pubsub_ctx_t *ctx = NULL;
  at_serial_port_t *port = NULL;
  struct cell_modem_ctx_s cell_modem_ctx = { .sbp_ctx = NULL, .port = NULL };

  logging_init(PROGRAM_NAME);

  if (parse_options(argc, argv) != 0) {
    piksi_log(LOG_ERR, "invalid arguments");
    usage(argv[0]);
    return cleanup(&settings_ctx, &ctx, &port, EXIT_FAILURE);
  }

  port = at_serial_port_create((char *)port_name);
  if (port == NULL) {
    return cleanup(&settings_ctx, &ctx, &port, EXIT_FAILURE);
  }
  cell_modem_ctx.port = port;

  if (command_string != NULL) {
    at_serial_port_command_t *at_command = at_serial_port_command_create((char *)command_string);
    if (at_command != NULL) {
      at_serial_port_execute_command(port, at_command);
      printf("%s\n", at_serial_port_command_result(at_command));
      at_serial_port_command_destroy(&at_command);
    }
  } else {
    /* Prevent czmq from catching signals */
    zsys_handler_set(NULL);

    ctx = sbp_zmq_pubsub_create(SBP_PUB_ENDPOINT, SBP_SUB_ENDPOINT);
    if (ctx == NULL) {
      return cleanup(&settings_ctx, &ctx, &port, EXIT_FAILURE);
    }
    cell_modem_ctx.sbp_ctx = ctx;

    zloop_t *loop = sbp_zmq_pubsub_zloop_get(ctx);
    if (loop == NULL) {
      return cleanup(&settings_ctx, &ctx, &port, EXIT_FAILURE);
    }

    if (zloop_timer(loop,
                    CELL_STATUS_UPDATE_INTERVAL,
                    0,
                    cell_status_timer_callback,
                    &cell_modem_ctx)
        == -1) {
      return cleanup(&settings_ctx, &ctx, &port, EXIT_FAILURE);
    }

    settings_ctx = settings_create();

    if (settings_ctx == NULL) {
      sbp_log(LOG_ERR, "Error registering for settings!");
      return cleanup(&settings_ctx, &ctx, &port, EXIT_FAILURE);
    }

    if (settings_reader_add(settings_ctx, loop) != 0) {
      sbp_log(LOG_ERR, "Error registering for settings read!");
      return cleanup(&settings_ctx, &ctx, &port, EXIT_FAILURE);
    }

    settings_add_watch(settings_ctx,
                       "cell_modem",
                       "enable",
                       &cell_modem_enable_watch,
                       sizeof(cell_modem_enable_watch),
                       SETTINGS_TYPE_BOOL,
                       NULL,
                       NULL);

    zmq_simple_loop(loop);
  }

  return cleanup(&settings_ctx, &ctx, &port, EXIT_SUCCESS);
}

static int cleanup(settings_ctx_t **settings_ctx_loc,
                   sbp_zmq_pubsub_ctx_t **pubsub_ctx_loc,
                   at_serial_port_t **port_loc,
                   int status)
{
  if (*pubsub_ctx_loc != NULL) {
    sbp_zmq_pubsub_destroy(pubsub_ctx_loc);
  }
  settings_destroy(settings_ctx_loc);
  at_serial_port_destroy(port_loc);
  logging_deinit();

  return status;
}

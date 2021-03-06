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

#ifndef __HEALTH_MONITOR_GLO_OBS_H
#define __HEALTH_MONITOR_GLO_OBS_H

int glo_obs_timeout_health_monitor_init(health_ctx_t *health_ctx);
void glo_obs_timeout_health_monitor_deinit(void);

#endif /* __HEALTH_MONITOR_GLO_OBS_H */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_RA_HTTP_H
#define SWITCH_RA_HTTP_H

#include <stdbool.h>
#include <rc_client.h>

#ifdef __cplusplus
extern "C" {
#endif
bool SwitchRAHttpInit(void);
void SwitchRAHttpDeinit(void);
void SwitchRAHttpSend(const rc_api_request_t* request, rc_client_server_callback_t callback, void* userdata, const char* userAgent);
void SwitchRAHttpPump(void);
#ifdef __cplusplus
}
#endif
#endif

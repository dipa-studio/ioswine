/*
 * daemon_names.h: the ONE wire-string table for the daemon services.
 *
 * Same pattern as fakeboard_names.h: every service name, selector,
 * message name and reply key the daemon services use lives in a
 * *_names.h table and nowhere else, so a wrong string can be
 * corrected after the first device trace without touching any
 * logic.
 *
 * The one key shared by every service is defined here: svchost
 * handlers put their error code under "error" (see svchost.h).
 *
 * No em dashes are used in this file.
 */
#pragma once

/* Any non-zero handler return goes verbatim into the reply dict's
 * "error" key (see svchost.h). */
#define DAEMON_KEY_ERROR "error"

#include "cfprefs_names.h"
#include "notifyd_names.h"
#include "mg_names.h"
#include "lsd_names.h"
#include "csops_names.h"

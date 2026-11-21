/*
 * daemons.h: in-process fake system services for ioswine.
 *
 * daemons_init() starts every daemon service the app needs before
 * UIApplicationMain reaches didFinishLaunching: cfprefsd, notifyd,
 * MobileGestalt and LaunchServices, on top of svchost. The container
 * layout and the code-signature/entitlement answers have no service
 * of their own, so they have no init call here.
 *
 * The service entry points are declared here and defined in
 * their own <svc>.c files.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include "svchost.h"

/* Start the daemon services, in order: svchost_init() first (it is
 * idempotent), then cfprefsd, notifyd, mg, lsd. Every service init
 * is idempotent (re-registering a name updates its handler and
 * returns the existing port), so calling daemons_init() twice is
 * safe. Returns KERN_SUCCESS or the first registration error. */
int daemons_init(void);

/* Service entry points. */
int cfprefsd_init(void);
int notifyd_init(void);
int mg_init(void);
int lsd_init(void);

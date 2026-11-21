/*
 * daemons.c: in-process fake system services for ioswine.
 *
 * daemons_init() wires the daemon services in order:
 * svchost first, then cfprefsd, notifyd, mg, lsd.
 * Container layout and csops/entitlements have no service and need
 * no init call. The first error stops the sequence and is returned.
 *
 * No em dashes are used in this file.
 */
#include "daemons.h"

int daemons_init(void) {
    int rc = svchost_init();
    if (rc != KERN_SUCCESS) return rc;
    rc = cfprefsd_init();
    if (rc != KERN_SUCCESS) return rc;
    rc = notifyd_init();
    if (rc != KERN_SUCCESS) return rc;
    rc = mg_init();
    if (rc != KERN_SUCCESS) return rc;
    rc = lsd_init();
    if (rc != KERN_SUCCESS) return rc;
    return KERN_SUCCESS;
}

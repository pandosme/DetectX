#ifndef STORAGE_H
#define STORAGE_H

/**
 * SD-card storage via the ACAP AXStorage API.
 *
 * Newer ACAP OS sandboxes SD access per application: an app must obtain its own
 * writable area from the SDK (ax_storage_get_path) instead of hardcoding a shared
 * path such as /var/spool/storage/SD_DISK/<app>, which another app may own and which
 * this app then cannot write into. This module subscribes to the SD_DISK device and
 * exposes the app's own path once it is set up.
 *
 * Requires a running GLib main loop (the async setup/subscribe callbacks fire on it).
 */

/** Start listing/subscribing to storage devices. Call once, before the main loop. */
int Storage_Init(void);

/**
 * The application's own writable SD_DISK path (e.g. /var/spool/storage/areas/SD_DISK/detectx),
 * with images/ and labels/ already created, or NULL while storage is absent or not yet
 * set up. Do NOT free. Callers must treat NULL as "SD not available".
 */
const char* Storage_GetPath(void);

/** Release and unsubscribe all storage devices. Call at shutdown. */
void Storage_Cleanup(void);

#endif

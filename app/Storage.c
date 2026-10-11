#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <axsdk/axstorage.h>

#include "ACAP.h"
#include "Storage.h"

#define LOG(fmt, args...)      { syslog(LOG_INFO,    fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }

/* We only use the internal SD card. */
#define SD_STORAGE_ID "SD_DISK"

typedef struct {
    AXStorage* storage;
    gchar*     storage_id;
    gchar*     path;
    guint      subscription_id;
    gboolean   setup;
    gboolean   available;
    gboolean   writable;
    gboolean   full;
    gboolean   exiting;
} StorageDisk;

static GList* storage_disks = NULL;

static StorageDisk* find_disk(const gchar* storage_id) {
    for (GList* n = storage_disks; n; n = n->next) {
        StorageDisk* d = n->data;
        if (g_strcmp0(storage_id, d->storage_id) == 0)
            return d;
    }
    return NULL;
}

static void on_release(gpointer user_data, GError* error) {
    gchar* id = (gchar*)user_data;
    if (error) {
        LOG_WARN("Storage release %s failed: %s\n", id, error->message);
        g_error_free(error);
    } else {
        LOG("Storage released %s\n", id);
    }
}

static void on_setup(AXStorage* storage, gpointer user_data, GError* error) {
    (void)user_data;
    if (!storage || error) {
        LOG_WARN("Storage setup failed: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        return;
    }

    GError* err = NULL;
    gchar* id = ax_storage_get_storage_id(storage, &err);
    if (err) { LOG_WARN("get_storage_id: %s\n", err->message); g_error_free(err); return; }

    gchar* path = ax_storage_get_path(storage, &err);
    if (err) { LOG_WARN("get_path: %s\n", err->message); g_error_free(err); g_free(id); return; }

    StorageDisk* disk = find_disk(id);
    if (disk) {
        disk->storage = storage;
        if (disk->path) g_free(disk->path);
        disk->path  = g_strdup(path);
        disk->setup = TRUE;

        /* Create the capture subdirectories inside our own area. */
        char imgdir[768], lbldir[768];
        snprintf(imgdir, sizeof(imgdir), "%s/images", path);
        snprintf(lbldir, sizeof(lbldir), "%s/labels", path);
        g_mkdir_with_parents(imgdir, 0755);
        g_mkdir_with_parents(lbldir, 0755);

        LOG("Storage %s ready at %s\n", id, path);
        ACAP_STATUS_SetBool("SDCARD", "available", 1);
    }
    g_free(id);
    g_free(path);
}

static void on_subscribe(gchar* storage_id, gpointer user_data, GError* error) {
    (void)user_data;
    if (error) {
        LOG_WARN("Storage subscribe %s error: %s\n", storage_id, error->message);
        g_error_free(error);
        return;
    }

    StorageDisk* disk = find_disk(storage_id);
    if (!disk) return;

    GError* err = NULL;
    disk->available = ax_storage_get_status(storage_id, AX_STORAGE_AVAILABLE_EVENT, &err);
    if (err) { g_clear_error(&err); return; }
    disk->writable = ax_storage_get_status(storage_id, AX_STORAGE_WRITABLE_EVENT, &err);
    if (err) { g_clear_error(&err); return; }
    disk->full = ax_storage_get_status(storage_id, AX_STORAGE_FULL_EVENT, &err);
    if (err) { g_clear_error(&err); return; }
    disk->exiting = ax_storage_get_status(storage_id, AX_STORAGE_EXITING_EVENT, &err);
    if (err) { g_clear_error(&err); return; }

    LOG("Storage %s: available=%d writable=%d full=%d exiting=%d\n",
        storage_id, disk->available, disk->writable, disk->full, disk->exiting);

    if (disk->exiting && disk->setup) {
        /* Card going away — release our handle and forget the path. */
        ACAP_STATUS_SetBool("SDCARD", "available", 0);
        ax_storage_release_async(disk->storage, on_release, disk->storage_id, &err);
        if (err) { LOG_WARN("release: %s\n", err->message); g_clear_error(&err); }
        else { disk->setup = FALSE; g_free(disk->path); disk->path = NULL; }
        return;
    }

    if (disk->writable && !disk->full && !disk->exiting && !disk->setup) {
        LOG("Setting up storage %s\n", storage_id);
        ax_storage_setup_async(storage_id, on_setup, NULL, &err);
        if (err) { LOG_WARN("setup_async: %s\n", err->message); g_clear_error(&err); }
    }
}

int Storage_Init(void) {
    GError* error = NULL;
    GList* disks = ax_storage_list(&error);
    if (error) {
        LOG_WARN("ax_storage_list: %s\n", error->message);
        g_error_free(error);
        return 0;
    }

    for (GList* n = disks; n; n = n->next) {
        gchar* id = (gchar*)n->data;
        GError* err = NULL;
        guint sub_id = ax_storage_subscribe(id, on_subscribe, NULL, &err);
        if (sub_id == 0 || err) {
            LOG_WARN("storage subscribe %s: %s\n", id, err ? err->message : "failed");
            g_clear_error(&err);
            g_free(id);
            continue;
        }
        StorageDisk* disk     = g_new0(StorageDisk, 1);
        disk->storage_id      = g_strdup(id);
        disk->subscription_id = sub_id;
        storage_disks = g_list_append(storage_disks, disk);
        g_free(id);
    }
    g_list_free(disks);
    LOG("Storage initialized, monitoring %d device(s)\n", g_list_length(storage_disks));
    return 1;
}

void Storage_Cleanup(void) {
    for (GList* n = storage_disks; n; n = n->next) {
        StorageDisk* disk = n->data;
        GError* err = NULL;
        if (disk->setup) {
            ax_storage_release_async(disk->storage, on_release, disk->storage_id, &err);
            if (err) { LOG_WARN("release: %s\n", err->message); g_clear_error(&err); }
        }
        ax_storage_unsubscribe(disk->subscription_id, &err);
        if (err) { LOG_WARN("unsubscribe: %s\n", err->message); g_clear_error(&err); }
        g_free(disk->storage_id);
        g_free(disk->path);
        g_free(disk);
    }
    g_list_free(storage_disks);
    storage_disks = NULL;
}

const char* Storage_GetPath(void) {
    StorageDisk* disk = find_disk(SD_STORAGE_ID);
    if (disk && disk->setup && disk->path)
        return disk->path;
    return NULL;
}

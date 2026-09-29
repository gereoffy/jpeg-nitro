// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitro-loader: background decoding with prefetch. See nitro-loader.h.
#include "nitro-loader.h"

#define AHEAD 3       // images decoded ahead in the paging direction
#define BEHIND 2      // ... and kept / decoded behind
#define MAX_CACHE 8

typedef struct {
    int index;
    NitroImage *image;
} Entry;

struct _NitroLoader {
    gint refs;                 // the owner + one per pending main-thread notification
    gint quit;                 // set by nitro_loader_free: no more callbacks
    GMutex lock;
    GCond cond;
    GThread *thread;
    GPtrArray *files;          // of GFile
    Entry cache[MAX_CACHE];
    int ncache;
    GHashTable *failed;        // index + 1 -> reason (char *)
    int cur, dir;
    guint gen;                 // bumped when the file list changes: in-flight decodes are dropped
    GPtrArray *grave;          // evicted images: freed on the main thread, where GTK uses textures
    NitroLoaderReady ready;
    gpointer user_data;
};

typedef struct {
    NitroLoader *loader;
    int index;                 // -1: nothing to report (only images to free)
} Notify;

static void free_images(GPtrArray *images) {
    for (guint i = 0; i < images->len; i++) nitro_image_free(g_ptr_array_index(images, i));
    g_ptr_array_set_size(images, 0);
}

static void reap(NitroLoader *l) {   // main thread
    GPtrArray *dead = g_ptr_array_new();
    g_mutex_lock(&l->lock);
    for (guint i = 0; i < l->grave->len; i++) g_ptr_array_add(dead, g_ptr_array_index(l->grave, i));
    g_ptr_array_set_size(l->grave, 0);
    g_mutex_unlock(&l->lock);
    free_images(dead);
    g_ptr_array_unref(dead);
}

static void loader_unref(NitroLoader *l) {   // main thread
    if (!g_atomic_int_dec_and_test(&l->refs)) return;
    for (int i = 0; i < l->ncache; i++) nitro_image_free(l->cache[i].image);
    free_images(l->grave);
    g_ptr_array_unref(l->grave);
    g_hash_table_unref(l->failed);
    if (l->files) g_ptr_array_unref(l->files);
    g_mutex_clear(&l->lock);
    g_cond_clear(&l->cond);
    g_free(l);
}

static gboolean notify_cb(gpointer data) {
    Notify *n = data;
    reap(n->loader);
    if (n->index >= 0 && !g_atomic_int_get(&n->loader->quit)) n->loader->ready(n->loader->user_data, n->index);
    return G_SOURCE_REMOVE;
}

static void notify_free(gpointer data) {
    Notify *n = data;
    loader_unref(n->loader);
    g_free(n);
}

// Priority order: current, then AHEAD in the paging direction, then BEHIND. Locked.
static int wanted_locked(NitroLoader *l, int *w) {
    int n = 0, nfiles = l->files ? (int)l->files->len : 0;
    for (int k = 0; k <= MAX(AHEAD, BEHIND); k++) {
        int cand[2] = {l->cur + l->dir * k, l->cur - l->dir * k};
        for (int c = 0; c < 2; c++) {
            int i = cand[c];
            if (k > 0 && ((c == 0 && k > AHEAD) || (c == 1 && k > BEHIND))) continue;
            if (i < 0 || i >= nfiles) continue;
            int dup = 0;
            for (int q = 0; q < n; q++) dup |= w[q] == i;
            if (!dup) w[n++] = i;
        }
    }
    return n;
}

static gboolean cached_locked(NitroLoader *l, int index) {
    for (int i = 0; i < l->ncache; i++)
        if (l->cache[i].index == index) return TRUE;
    return FALSE;
}

static gpointer run(gpointer data) {
    NitroLoader *l = data;
    g_mutex_lock(&l->lock);
    for (;;) {
        int w[2 * (AHEAD + BEHIND) + 2], nw = 0, next = -1;
        while (!g_atomic_int_get(&l->quit)) {
            nw = wanted_locked(l, w);
            for (int k = 0; k < nw && next < 0; k++)
                if (!cached_locked(l, w[k]) && !g_hash_table_contains(l->failed, GINT_TO_POINTER(w[k] + 1)))
                    next = w[k];
            if (next >= 0) break;
            g_cond_wait(&l->cond, &l->lock);
        }
        if (g_atomic_int_get(&l->quit)) break;
        for (int c = 0; c < l->ncache;) {   // evict everything outside the window
            gboolean in = FALSE;
            for (int k = 0; k < nw; k++) in |= l->cache[c].index == w[k];
            if (in) { c++; continue; }
            g_ptr_array_add(l->grave, l->cache[c].image);
            l->cache[c] = l->cache[--l->ncache];
        }
        GFile *file = g_object_ref(g_ptr_array_index(l->files, next));
        guint gen = l->gen;
        g_mutex_unlock(&l->lock);

        GError *error = NULL;
        NitroImage *image = nitro_image_load(file, &error);
        g_object_unref(file);

        g_mutex_lock(&l->lock);
        gboolean stale = gen != l->gen;   // the file list changed meanwhile: index no longer valid
        if (image && (stale || l->ncache == MAX_CACHE)) {
            g_ptr_array_add(l->grave, image);
        } else if (image) {
            l->cache[l->ncache].index = next;
            l->cache[l->ncache].image = image;
            l->ncache++;
        } else if (!stale) {
            g_hash_table_insert(l->failed, GINT_TO_POINTER(next + 1),
                                g_strdup(error ? error->message : "Cannot decode the image"));
        }
        g_clear_error(&error);
        if (!g_atomic_int_get(&l->quit)) {
            Notify *n = g_new(Notify, 1);
            n->loader = l;
            n->index = stale ? -1 : next;
            g_atomic_int_inc(&l->refs);
            g_idle_add_full(G_PRIORITY_DEFAULT, notify_cb, n, notify_free);
        }
    }
    g_mutex_unlock(&l->lock);
    return NULL;
}

NitroLoader *nitro_loader_new(NitroLoaderReady ready, gpointer user_data) {
    NitroLoader *l = g_new0(NitroLoader, 1);
    l->refs = 1;
    g_mutex_init(&l->lock);
    g_cond_init(&l->cond);
    l->failed = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    l->grave = g_ptr_array_new();
    l->dir = 1;
    l->ready = ready;
    l->user_data = user_data;
    l->thread = g_thread_new("nitro-loader", run, l);
    return l;
}

void nitro_loader_free(NitroLoader *l) {
    if (!l) return;
    g_mutex_lock(&l->lock);
    g_atomic_int_set(&l->quit, 1);
    g_cond_broadcast(&l->cond);
    g_mutex_unlock(&l->lock);
    g_thread_join(l->thread);   // (waits for a decode in progress)
    loader_unref(l);            // pending notifications hold their own references
}

void nitro_loader_set_files(NitroLoader *l, GPtrArray *files, int keep_index, int new_index) {
    g_mutex_lock(&l->lock);
    NitroImage *keep = NULL;
    for (int i = 0; i < l->ncache; i++) {
        if (keep_index >= 0 && l->cache[i].index == keep_index) keep = l->cache[i].image;
        else g_ptr_array_add(l->grave, l->cache[i].image);
    }
    l->ncache = 0;
    if (keep) {
        l->cache[0].index = new_index;
        l->cache[0].image = keep;
        l->ncache = 1;
    }
    char *keep_failed = keep_index >= 0 ? g_strdup(g_hash_table_lookup(l->failed, GINT_TO_POINTER(keep_index + 1))) : NULL;
    g_hash_table_remove_all(l->failed);
    if (keep_failed) g_hash_table_insert(l->failed, GINT_TO_POINTER(new_index + 1), keep_failed);
    if (files) g_ptr_array_ref(files);
    if (l->files) g_ptr_array_unref(l->files);
    l->files = files;
    l->cur = new_index >= 0 ? new_index : 0;
    l->gen++;
    g_cond_signal(&l->cond);
    g_mutex_unlock(&l->lock);
}

void nitro_loader_focus(NitroLoader *l, int index, int direction) {
    g_mutex_lock(&l->lock);
    l->cur = index;
    l->dir = direction < 0 ? -1 : 1;
    g_cond_signal(&l->cond);
    g_mutex_unlock(&l->lock);
}

NitroImage *nitro_loader_get(NitroLoader *l, int index) {
    NitroImage *copy = NULL;
    g_mutex_lock(&l->lock);
    for (int i = 0; i < l->ncache; i++)
        if (l->cache[i].index == index) {
            copy = g_new(NitroImage, 1);
            *copy = *l->cache[i].image;
            if (copy->texture) g_object_ref(copy->texture);
            if (copy->svg_bytes) g_bytes_ref(copy->svg_bytes);
            break;
        }
    g_mutex_unlock(&l->lock);
    return copy;
}

gboolean nitro_loader_failed(NitroLoader *l, int index, char **message) {
    g_mutex_lock(&l->lock);
    const char *m = g_hash_table_lookup(l->failed, GINT_TO_POINTER(index + 1));
    if (m && message) *message = g_strdup(m);
    g_mutex_unlock(&l->lock);
    return m != NULL;
}

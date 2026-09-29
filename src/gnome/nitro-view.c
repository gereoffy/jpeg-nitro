// SPDX-License-Identifier: MIT
#include "nitro-view.h"

#include <math.h>

struct _NitroView {
    GtkWidget parent_instance;

    GdkTexture *texture;
    int orientation;          // EXIF orientation of the texture (1..8), applied when drawing
    gboolean zoomed;
    gboolean fit_screen;
    gboolean fullscreen;
    gboolean user_sized;
    double scale;
    double center_x;
    double center_y;
    double fit_reference;
};

G_DEFINE_FINAL_TYPE(NitroView, nitro_view, GTK_TYPE_WIDGET)

enum {
    VIEW_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

// Image size as displayed: EXIF orientations 5..8 turn the texture by 90 degrees.
static double image_w(NitroView *self) {
    return self->orientation >= 5 ? gdk_texture_get_height(self->texture) : gdk_texture_get_width(self->texture);
}
static double image_h(NitroView *self) {
    return self->orientation >= 5 ? gdk_texture_get_width(self->texture) : gdk_texture_get_height(self->texture);
}

static int view_scale_factor(NitroView *self) {
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(self));
    return scale > 0 ? scale : 1;
}

static double fit_scale(NitroView *self) {
    if (!self->texture) return 1.0;

    int width = gtk_widget_get_width(GTK_WIDGET(self));
    int height = gtk_widget_get_height(GTK_WIDGET(self));
    double image_width = image_w(self);
    double image_height = image_h(self);
    int factor = view_scale_factor(self);

    if (width <= 0 || height <= 0 || image_width <= 0 || image_height <= 0) return 1.0;

    double scale = MIN((double)width * factor / image_width,
                       (double)height * factor / image_height);
    if (!self->fit_screen && !self->fullscreen && scale > 1.0) scale = 1.0;
    return scale;
}

static double current_scale(NitroView *self) {
    return self->zoomed ? self->scale : fit_scale(self);
}

static void clamp_center(NitroView *self, double scale) {
    if (!self->texture || scale <= 0.0) return;

    int factor = view_scale_factor(self);
    double image_width = image_w(self);
    double image_height = image_h(self);
    double visible_width = (double)gtk_widget_get_width(GTK_WIDGET(self)) * factor / scale;
    double visible_height = (double)gtk_widget_get_height(GTK_WIDGET(self)) * factor / scale;

    self->center_x = image_width <= visible_width
        ? image_width / 2.0
        : CLAMP(self->center_x, visible_width / 2.0, image_width - visible_width / 2.0);
    self->center_y = image_height <= visible_height
        ? image_height / 2.0
        : CLAMP(self->center_y, visible_height / 2.0, image_height - visible_height / 2.0);
}

static void emit_view_changed(NitroView *self) {
    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_signal_emit(self, signals[VIEW_CHANGED], 0);
}

static void nitro_view_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    NitroView *self = NITRO_VIEW(widget);
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);

    GdkRGBA black = {0.0, 0.0, 0.0, 1.0};
    graphene_rect_t background = GRAPHENE_RECT_INIT(0, 0, width, height);
    gtk_snapshot_append_color(snapshot, &black, &background);

    if (!self->texture || width <= 0 || height <= 0) return;

    int factor = view_scale_factor(self);
    double scale = current_scale(self);
    double image_width = image_w(self);
    double image_height = image_h(self);

    if (self->zoomed) clamp_center(self, scale);

    double center_x = self->zoomed ? self->center_x : image_width / 2.0;
    double center_y = self->zoomed ? self->center_y : image_height / 2.0;
    double draw_width = image_width * scale / factor;
    double draw_height = image_height * scale / factor;
    double x = round(width / 2.0 - center_x * scale / factor);
    double y = round(height / 2.0 - center_y * scale / factor);

    double rounded = round(scale);
    gboolean integer_zoom = scale >= 2.0 && fabs(scale - rounded) < 1e-9;
    // integer zoom >= 200%: exact pixel blocks; smaller than 100%: mipmaps (a plain linear
    // filter would skip most pixels of a big photo and shimmer); otherwise linear
    GskScalingFilter filter = integer_zoom ? GSK_SCALING_FILTER_NEAREST
                            : scale < 1.0  ? GSK_SCALING_FILTER_TRILINEAR
                                           : GSK_SCALING_FILTER_LINEAR;
    if (self->orientation <= 1) {
        graphene_rect_t bounds = GRAPHENE_RECT_INIT((float)x, (float)y, (float)draw_width, (float)draw_height);
        gtk_snapshot_append_scaled_texture(snapshot, self->texture, filter, &bounds);
        return;
    }
    // EXIF orientation: rotate / mirror the texture around the centre of its place on screen
    //   2 mirror   3 180    4 flip   5 transpose   6 90 cw   7 transverse   8 90 ccw
    static const float rot[9] = {0, 0, 0, 180, 0, 90, 90, 90, 270};
    static const float sx[9] = {1, 1, -1, 1, 1, 1, 1, -1, 1};
    static const float sy[9] = {1, 1, 1, 1, -1, -1, 1, 1, 1};
    int o = self->orientation;
    double tex_width = gdk_texture_get_width(self->texture) * scale / factor;
    double tex_height = gdk_texture_get_height(self->texture) * scale / factor;
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)(x + draw_width / 2), (float)(y + draw_height / 2)));
    if (rot[o] != 0) gtk_snapshot_rotate(snapshot, rot[o]);
    if (sx[o] != 1 || sy[o] != 1) gtk_snapshot_scale(snapshot, sx[o], sy[o]);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT((float)(-tex_width / 2), (float)(-tex_height / 2),
                                                 (float)tex_width, (float)tex_height);
    gtk_snapshot_append_scaled_texture(snapshot, self->texture, filter, &bounds);
    gtk_snapshot_restore(snapshot);
}

static void nitro_view_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
    NitroView *self = NITRO_VIEW(widget);
    (void)width;
    (void)height;
    (void)baseline;

    if (!self->texture) return;
    if (self->zoomed) clamp_center(self, self->scale);
    else self->fit_reference = fit_scale(self);
}

static void nitro_view_dispose(GObject *object) {
    NitroView *self = NITRO_VIEW(object);
    g_clear_object(&self->texture);
    G_OBJECT_CLASS(nitro_view_parent_class)->dispose(object);
}

static void nitro_view_class_init(NitroViewClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = nitro_view_dispose;
    widget_class->snapshot = nitro_view_snapshot;
    widget_class->size_allocate = nitro_view_size_allocate;
    gtk_widget_class_set_css_name(widget_class, "nitroview");

    signals[VIEW_CHANGED] = g_signal_new("view-changed",
                                         G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void nitro_view_init(NitroView *self) {
    self->scale = 1.0;
    self->fit_reference = 1.0;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

NitroView *nitro_view_new(void) {
    return g_object_new(NITRO_TYPE_VIEW, NULL);
}

void nitro_view_set_texture(NitroView *self, GdkTexture *texture, int orientation) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    g_return_if_fail(texture == NULL || GDK_IS_TEXTURE(texture));

    gboolean keep_position = self->zoomed;
    g_set_object(&self->texture, texture);
    self->orientation = orientation >= 1 && orientation <= 8 ? orientation : 1;

    if (self->texture) {
        if (!keep_position) {
            self->center_x = image_w(self) / 2.0;
            self->center_y = image_h(self) / 2.0;
        } else {
            clamp_center(self, self->scale);
        }
        self->fit_reference = fit_scale(self);
    }

    emit_view_changed(self);
}

void nitro_view_clear(NitroView *self) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    g_clear_object(&self->texture);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void nitro_view_fit(NitroView *self, gboolean fit_screen) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    self->fit_screen = fit_screen;
    self->zoomed = FALSE;
    self->fit_reference = fit_scale(self);
    emit_view_changed(self);
}

static double fit_snap_scale(NitroView *self) {
    if (!self->texture) return 1.0;
    if (self->fullscreen || self->user_sized) return fit_scale(self);
    if (self->fit_reference > 0.0) return self->fit_reference;
    return fit_scale(self);
}

static void set_scale(NitroView *self, double new_scale, double x, double y) {
    if (!self->texture || new_scale <= 0.0) return;

    double fit = fit_snap_scale(self);
    if (fabs(new_scale - fit) < 1e-9 && fabs(new_scale - 1.0) > 1e-9) {
        self->zoomed = FALSE;
        emit_view_changed(self);
        return;
    }

    int factor = view_scale_factor(self);
    double current = current_scale(self);
    double image_width = image_w(self);
    double image_height = image_h(self);
    double center_x = self->zoomed ? self->center_x : image_width / 2.0;
    double center_y = self->zoomed ? self->center_y : image_height / 2.0;

    if (self->zoomed) clamp_center(self, current);

    double anchor_x = (x - gtk_widget_get_width(GTK_WIDGET(self)) / 2.0) * factor;
    double anchor_y = (y - gtk_widget_get_height(GTK_WIDGET(self)) / 2.0) * factor;
    double point_x = center_x + anchor_x / current;
    double point_y = center_y + anchor_y / current;

    self->center_x = point_x - anchor_x / new_scale;
    self->center_y = point_y - anchor_y / new_scale;
    self->zoomed = TRUE;
    self->scale = new_scale;
    clamp_center(self, new_scale);
    emit_view_changed(self);
}


void nitro_view_set_fullscreen(NitroView *self, gboolean fullscreen) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    fullscreen = !!fullscreen;
    if (self->fullscreen == fullscreen) return;
    self->fullscreen = fullscreen;
    if (!self->zoomed) self->fit_reference = fit_scale(self);
    emit_view_changed(self);
}

void nitro_view_set_user_sized(NitroView *self, gboolean user_sized) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    self->user_sized = !!user_sized;
    if (!self->zoomed) self->fit_reference = fit_scale(self);
}

void nitro_view_zoom_to(NitroView *self, double scale, double x, double y) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    set_scale(self, scale, x, y);
}

void nitro_view_zoom_by(NitroView *self, double factor, double x, double y, gboolean continuous) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    if (!self->texture || factor <= 0.0) return;

    double fit = fit_snap_scale(self);
    double current = current_scale(self);
    double next = current * factor;

    double snaps[2] = {1.0, fit};
    for (int i = 0; i < 2; i++) {
        double snap = snaps[i];
        if ((current < snap - 1e-9 && next > snap + 1e-9) ||
            (current > snap + 1e-9 && next < snap - 1e-9)) {
            next = snap;
            break;
        }
    }

    next = CLAMP(next, MIN(fit, 1.0) / 4.0, 32.0);
    double rounded = round(next);
    if (rounded >= 1.0 && fabs(next - rounded) < 1e-6 * rounded) next = rounded;
    if (!continuous && fit > 0.0 && fabs(next - fit) < 0.02 * fit) next = fit;

    set_scale(self, next, x, y);
}

void nitro_view_toggle_actual_size(NitroView *self, double x, double y) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    if (!self->texture) return;
    if (self->zoomed) nitro_view_fit(self, self->fit_screen);
    else set_scale(self, 1.0, x, y);
}

void nitro_view_pan_pixels(NitroView *self, double dx, double dy) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    if (!self->texture || !self->zoomed) return;

    int factor = view_scale_factor(self);
    clamp_center(self, self->scale);
    self->center_x -= dx * factor / self->scale;
    self->center_y -= dy * factor / self->scale;
    clamp_center(self, self->scale);
    emit_view_changed(self);
}

void nitro_view_pan_fraction(NitroView *self, double fx, double fy) {
    g_return_if_fail(NITRO_IS_VIEW(self));
    nitro_view_pan_pixels(self,
                          -fx * gtk_widget_get_width(GTK_WIDGET(self)),
                          -fy * gtk_widget_get_height(GTK_WIDGET(self)));
}

gboolean nitro_view_is_zoomed(NitroView *self) {
    g_return_val_if_fail(NITRO_IS_VIEW(self), FALSE);
    return self->zoomed;
}

gboolean nitro_view_get_fit_screen(NitroView *self) {
    g_return_val_if_fail(NITRO_IS_VIEW(self), FALSE);
    return self->fit_screen;
}

double nitro_view_get_scale(NitroView *self) {
    g_return_val_if_fail(NITRO_IS_VIEW(self), 1.0);
    return current_scale(self);
}

int nitro_view_get_image_width(NitroView *self) {
    g_return_val_if_fail(NITRO_IS_VIEW(self), 0);
    return self->texture ? (int)image_w(self) : 0;
}

int nitro_view_get_image_height(NitroView *self) {
    g_return_val_if_fail(NITRO_IS_VIEW(self), 0);
    return self->texture ? (int)image_h(self) : 0;
}

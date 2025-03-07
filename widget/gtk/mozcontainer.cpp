/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:expandtab:shiftwidth=4:tabstop=4:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozcontainer.h"
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#ifdef MOZ_WAYLAND
#include "nsWaylandDisplay.h"
#include <wayland-egl.h>
#endif
#include <stdio.h>
#include <dlfcn.h>

#ifdef ACCESSIBILITY
#include <atk/atk.h>
#include "maiRedundantObjectFactory.h"
#endif

#ifdef MOZ_WAYLAND
using namespace mozilla;
using namespace mozilla::widget;
#endif

/* init methods */
static void moz_container_class_init(MozContainerClass *klass);
static void moz_container_init(MozContainer *container);

/* widget class methods */
static void moz_container_map(GtkWidget *widget);
#if defined(MOZ_WAYLAND)
static gboolean moz_container_map_wayland(GtkWidget *widget,
                                          GdkEventAny *event);
#endif
static void moz_container_unmap(GtkWidget *widget);
static void moz_container_realize(GtkWidget *widget);
static void moz_container_size_allocate(GtkWidget *widget,
                                        GtkAllocation *allocation);

/* container class methods */
static void moz_container_remove(GtkContainer *container,
                                 GtkWidget *child_widget);
static void moz_container_forall(GtkContainer *container,
                                 gboolean include_internals,
                                 GtkCallback callback, gpointer callback_data);
static void moz_container_add(GtkContainer *container, GtkWidget *widget);

typedef struct _MozContainerChild MozContainerChild;

struct _MozContainerChild {
  GtkWidget *widget;
  gint x;
  gint y;
};

static void moz_container_allocate_child(MozContainer *container,
                                         MozContainerChild *child);
static MozContainerChild *moz_container_get_child(MozContainer *container,
                                                  GtkWidget *child);

/* public methods */

GType moz_container_get_type(void) {
  static GType moz_container_type = 0;

  if (!moz_container_type) {
    static GTypeInfo moz_container_info = {
        sizeof(MozContainerClass),                /* class_size */
        NULL,                                     /* base_init */
        NULL,                                     /* base_finalize */
        (GClassInitFunc)moz_container_class_init, /* class_init */
        NULL,                                     /* class_destroy */
        NULL,                                     /* class_data */
        sizeof(MozContainer),                     /* instance_size */
        0,                                        /* n_preallocs */
        (GInstanceInitFunc)moz_container_init,    /* instance_init */
        NULL,                                     /* value_table */
    };

    moz_container_type =
        g_type_register_static(GTK_TYPE_CONTAINER, "MozContainer",
                               &moz_container_info, static_cast<GTypeFlags>(0));
#ifdef ACCESSIBILITY
    /* Set a factory to return accessible object with ROLE_REDUNDANT for
     * MozContainer, so that gail won't send focus notification for it */
    atk_registry_set_factory_type(atk_get_default_registry(),
                                  moz_container_type,
                                  mai_redundant_object_factory_get_type());
#endif
  }

  return moz_container_type;
}

GtkWidget *moz_container_new(void) {
  MozContainer *container;

  container =
      static_cast<MozContainer *>(g_object_new(MOZ_CONTAINER_TYPE, nullptr));

  return GTK_WIDGET(container);
}

void moz_container_put(MozContainer *container, GtkWidget *child_widget, gint x,
                       gint y) {
  MozContainerChild *child;

  child = g_new(MozContainerChild, 1);

  child->widget = child_widget;
  child->x = x;
  child->y = y;

  /*  printf("moz_container_put %p %p %d %d\n", (void *)container,
      (void *)child_widget, x, y); */

  container->children = g_list_append(container->children, child);

  /* we assume that the caller of this function will have already set
     the parent GdkWindow because we can have many anonymous children. */
  gtk_widget_set_parent(child_widget, GTK_WIDGET(container));
}

/* static methods */

void moz_container_class_init(MozContainerClass *klass) {
  /*GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    GtkObjectClass *object_class = GTK_OBJECT_CLASS (klass); */
  GtkContainerClass *container_class = GTK_CONTAINER_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  widget_class->map = moz_container_map;
#if defined(MOZ_WAYLAND)
  if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
    widget_class->map_event = moz_container_map_wayland;
  }
#endif
  widget_class->unmap = moz_container_unmap;
  widget_class->realize = moz_container_realize;
  widget_class->size_allocate = moz_container_size_allocate;

  container_class->remove = moz_container_remove;
  container_class->forall = moz_container_forall;
  container_class->add = moz_container_add;
}

void moz_container_init(MozContainer *container) {
  gtk_widget_set_can_focus(GTK_WIDGET(container), TRUE);
  gtk_container_set_resize_mode(GTK_CONTAINER(container), GTK_RESIZE_IMMEDIATE);
  gtk_widget_set_redraw_on_allocate(GTK_WIDGET(container), FALSE);

#if defined(MOZ_WAYLAND)
  container->surface = nullptr;
  container->subsurface = nullptr;
  container->eglwindow = nullptr;
  container->frame_callback_handler = nullptr;
  // We can draw to x11 window any time.
  container->ready_to_draw = GDK_IS_X11_DISPLAY(gdk_display_get_default());
  container->surface_needs_clear = true;
#endif
}

#if defined(MOZ_WAYLAND)
static wl_surface *moz_container_get_gtk_container_surface(
    MozContainer *container) {
  static auto sGdkWaylandWindowGetWlSurface = (wl_surface * (*)(GdkWindow *))
      dlsym(RTLD_DEFAULT, "gdk_wayland_window_get_wl_surface");

  GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(container));
  return sGdkWaylandWindowGetWlSurface(window);
}

static void frame_callback_handler(void *data, struct wl_callback *callback,
                                   uint32_t time) {
  MozContainer *container = MOZ_CONTAINER(data);
  g_clear_pointer(&container->frame_callback_handler, wl_callback_destroy);
  container->ready_to_draw = true;
}

static const struct wl_callback_listener frame_listener = {
    frame_callback_handler};

static gboolean moz_container_map_wayland(GtkWidget *widget,
                                          GdkEventAny *event) {
  MozContainer *container = MOZ_CONTAINER(widget);

  if (container->ready_to_draw || container->frame_callback_handler) {
    return FALSE;
  }

  wl_surface *gtk_container_surface =
      moz_container_get_gtk_container_surface(container);

  if (gtk_container_surface) {
    container->frame_callback_handler = wl_surface_frame(gtk_container_surface);
    wl_callback_add_listener(container->frame_callback_handler, &frame_listener,
                             container);
  }

  return FALSE;
}

static void moz_container_unmap_wayland(MozContainer *container) {
  g_clear_pointer(&container->eglwindow, wl_egl_window_destroy);
  g_clear_pointer(&container->subsurface, wl_subsurface_destroy);
  g_clear_pointer(&container->surface, wl_surface_destroy);
  g_clear_pointer(&container->frame_callback_handler, wl_callback_destroy);

  container->surface_needs_clear = true;
  container->ready_to_draw = false;
}

static gint moz_container_get_scale(MozContainer *container) {
  static auto sGdkWindowGetScaleFactorPtr =
      (gint(*)(GdkWindow *))dlsym(RTLD_DEFAULT, "gdk_window_get_scale_factor");

  if (sGdkWindowGetScaleFactorPtr) {
    GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(container));
    return (*sGdkWindowGetScaleFactorPtr)(window);
  }

  return 1;
}

void moz_container_scale_changed(MozContainer *container,
                                 GtkAllocation *aAllocation) {
  if (!container->surface) {
    return;
  }

  // Set correct scaled/unscaled mozcontainer offset
  // especially when wl_egl is used but we don't recreate it as Gtk+ does.
  gint x, y;
  gdk_window_get_position(gtk_widget_get_window(GTK_WIDGET(container)), &x, &y);
  wl_subsurface_set_position(container->subsurface, x, y);

  // Try to only resize wl_egl_window on scale factor change.
  // It's a bit risky as Gtk+ recreates it at that event.
  if (container->eglwindow) {
    gint scale = moz_container_get_scale(container);
    wl_surface_set_buffer_scale(container->surface,
                                moz_container_get_scale(container));
    wl_egl_window_resize(container->eglwindow, aAllocation->width * scale,
                         aAllocation->height * scale, 0, 0);
  }
}
#endif

void moz_container_map(GtkWidget *widget) {
  MozContainer *container;
  GList *tmp_list;
  GtkWidget *tmp_child;

  g_return_if_fail(IS_MOZ_CONTAINER(widget));
  container = MOZ_CONTAINER(widget);

  gtk_widget_set_mapped(widget, TRUE);

  tmp_list = container->children;
  while (tmp_list) {
    tmp_child = ((MozContainerChild *)tmp_list->data)->widget;

    if (gtk_widget_get_visible(tmp_child)) {
      if (!gtk_widget_get_mapped(tmp_child)) gtk_widget_map(tmp_child);
    }
    tmp_list = tmp_list->next;
  }

  if (gtk_widget_get_has_window(widget)) {
    gdk_window_show(gtk_widget_get_window(widget));
#if defined(MOZ_WAYLAND)
    if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
      moz_container_map_wayland(widget, nullptr);
    }
#endif
  }
}

void moz_container_unmap(GtkWidget *widget) {
  g_return_if_fail(IS_MOZ_CONTAINER(widget));

  gtk_widget_set_mapped(widget, FALSE);

  if (gtk_widget_get_has_window(widget)) {
    gdk_window_hide(gtk_widget_get_window(widget));
#if defined(MOZ_WAYLAND)
    if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
      moz_container_unmap_wayland(MOZ_CONTAINER(widget));
    }
#endif
  }
}

void moz_container_realize(GtkWidget *widget) {
  GdkWindow *parent = gtk_widget_get_parent_window(widget);
  GdkWindow *window;

  gtk_widget_set_realized(widget, TRUE);

  if (gtk_widget_get_has_window(widget)) {
    GdkWindowAttr attributes;
    gint attributes_mask = GDK_WA_VISUAL | GDK_WA_X | GDK_WA_Y;
    GtkAllocation allocation;

    gtk_widget_get_allocation(widget, &allocation);
    attributes.event_mask = gtk_widget_get_events(widget);
    attributes.x = allocation.x;
    attributes.y = allocation.y;
    attributes.width = allocation.width;
    attributes.height = allocation.height;
    attributes.wclass = GDK_INPUT_OUTPUT;
    attributes.visual = gtk_widget_get_visual(widget);
    attributes.window_type = GDK_WINDOW_CHILD;

    window = gdk_window_new(parent, &attributes, attributes_mask);
    gdk_window_set_user_data(window, widget);
  } else {
    window = parent;
    g_object_ref(window);
  }

  gtk_widget_set_window(widget, window);
}

void moz_container_size_allocate(GtkWidget *widget, GtkAllocation *allocation) {
  MozContainer *container;
  GList *tmp_list;
  GtkAllocation tmp_allocation;

  g_return_if_fail(IS_MOZ_CONTAINER(widget));

  /*  printf("moz_container_size_allocate %p %d %d %d %d\n",
      (void *)widget,
      allocation->x,
      allocation->y,
      allocation->width,
      allocation->height); */

  /* short circuit if you can */
  container = MOZ_CONTAINER(widget);
  gtk_widget_get_allocation(widget, &tmp_allocation);
  if (!container->children && tmp_allocation.x == allocation->x &&
      tmp_allocation.y == allocation->y &&
      tmp_allocation.width == allocation->width &&
      tmp_allocation.height == allocation->height) {
    return;
  }

  gtk_widget_set_allocation(widget, allocation);

  tmp_list = container->children;

  while (tmp_list) {
    MozContainerChild *child = static_cast<MozContainerChild *>(tmp_list->data);

    moz_container_allocate_child(container, child);

    tmp_list = tmp_list->next;
  }

  if (gtk_widget_get_has_window(widget) && gtk_widget_get_realized(widget)) {
    gdk_window_move_resize(gtk_widget_get_window(widget), allocation->x,
                           allocation->y, allocation->width,
                           allocation->height);
  }

#if defined(MOZ_WAYLAND)
  // We need to position our subsurface according to GdkWindow
  // when offset changes (GdkWindow is maximized for instance).
  // see gtk-clutter-embed.c for reference.
  if (container->subsurface) {
    gint x, y;
    gdk_window_get_position(gtk_widget_get_window(widget), &x, &y);
    wl_subsurface_set_position(container->subsurface, x, y);
  }
  if (container->eglwindow) {
    gint scale = moz_container_get_scale(container);
    wl_egl_window_resize(container->eglwindow, allocation->width * scale,
                         allocation->height * scale, 0, 0);
  }
#endif
}

void moz_container_remove(GtkContainer *container, GtkWidget *child_widget) {
  MozContainerChild *child;
  MozContainer *moz_container;
  GdkWindow *parent_window;

  g_return_if_fail(IS_MOZ_CONTAINER(container));
  g_return_if_fail(GTK_IS_WIDGET(child_widget));

  moz_container = MOZ_CONTAINER(container);

  child = moz_container_get_child(moz_container, child_widget);
  g_return_if_fail(child);

  /* gtk_widget_unparent will remove the parent window (as well as the
   * parent widget), but, in Mozilla's window hierarchy, the parent window
   * may need to be kept because it may be part of a GdkWindow sub-hierarchy
   * that is being moved to another MozContainer.
   *
   * (In a conventional GtkWidget hierarchy, GdkWindows being reparented
   * would have their own GtkWidget and that widget would be the one being
   * reparented.  In Mozilla's hierarchy, the parent_window needs to be
   * retained so that the GdkWindow sub-hierarchy is maintained.)
   */
  parent_window = gtk_widget_get_parent_window(child_widget);
  if (parent_window) g_object_ref(parent_window);

  gtk_widget_unparent(child_widget);

  if (parent_window) {
    /* The child_widget will always still exist because g_signal_emit,
     * which invokes this function, holds a reference.
     *
     * If parent_window is the container's root window then it will not be
     * the parent_window if the child_widget is placed in another
     * container.
     */
    if (parent_window != gtk_widget_get_window(GTK_WIDGET(container)))
      gtk_widget_set_parent_window(child_widget, parent_window);

    g_object_unref(parent_window);
  }

  moz_container->children = g_list_remove(moz_container->children, child);
  g_free(child);
}

void moz_container_forall(GtkContainer *container, gboolean include_internals,
                          GtkCallback callback, gpointer callback_data) {
  MozContainer *moz_container;
  GList *tmp_list;

  g_return_if_fail(IS_MOZ_CONTAINER(container));
  g_return_if_fail(callback != NULL);

  moz_container = MOZ_CONTAINER(container);

  tmp_list = moz_container->children;
  while (tmp_list) {
    MozContainerChild *child;
    child = static_cast<MozContainerChild *>(tmp_list->data);
    tmp_list = tmp_list->next;
    (*callback)(child->widget, callback_data);
  }
}

static void moz_container_allocate_child(MozContainer *container,
                                         MozContainerChild *child) {
  GtkAllocation allocation;

  gtk_widget_get_allocation(child->widget, &allocation);
  allocation.x = child->x;
  allocation.y = child->y;

  gtk_widget_size_allocate(child->widget, &allocation);
}

MozContainerChild *moz_container_get_child(MozContainer *container,
                                           GtkWidget *child_widget) {
  GList *tmp_list;

  tmp_list = container->children;
  while (tmp_list) {
    MozContainerChild *child;

    child = static_cast<MozContainerChild *>(tmp_list->data);
    tmp_list = tmp_list->next;

    if (child->widget == child_widget) return child;
  }

  return NULL;
}

static void moz_container_add(GtkContainer *container, GtkWidget *widget) {
  moz_container_put(MOZ_CONTAINER(container), widget, 0, 0);
}

#ifdef MOZ_WAYLAND
struct wl_surface *moz_container_get_wl_surface(MozContainer *container) {
  if (!container->surface) {
    if (!container->ready_to_draw) {
      return nullptr;
    }
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(container));

    // Available as of GTK 3.8+
    static auto sGdkWaylandDisplayGetWlCompositor =
        (wl_compositor * (*)(GdkDisplay *))
            dlsym(RTLD_DEFAULT, "gdk_wayland_display_get_wl_compositor");
    struct wl_compositor *compositor =
        sGdkWaylandDisplayGetWlCompositor(display);
    container->surface = wl_compositor_create_surface(compositor);

    nsWaylandDisplay *waylandDisplay = WaylandDisplayGet(display);
    container->subsurface = wl_subcompositor_get_subsurface(
        waylandDisplay->GetSubcompositor(), container->surface,
        moz_container_get_gtk_container_surface(container));
    WaylandDisplayRelease(waylandDisplay);

    GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(container));
    gint x, y;
    gdk_window_get_position(window, &x, &y);
    wl_subsurface_set_position(container->subsurface, x, y);
    wl_subsurface_set_desync(container->subsurface);

    // Route input to parent wl_surface owned by Gtk+ so we get input
    // events from Gtk+.
    wl_region *region = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(container->surface, region);
    wl_region_destroy(region);

    wl_surface_set_buffer_scale(container->surface,
                                moz_container_get_scale(container));
  }

  return container->surface;
}

struct wl_egl_window *moz_container_get_wl_egl_window(MozContainer *container) {
  if (!container->eglwindow) {
    wl_surface *surface = moz_container_get_wl_surface(container);
    if (!surface) {
      return nullptr;
    }

    GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(container));
    gint scale = moz_container_get_scale(container);
    container->eglwindow =
        wl_egl_window_create(surface, gdk_window_get_width(window) * scale,
                             gdk_window_get_height(window) * scale);
    wl_surface_set_buffer_scale(surface, scale);
  }
  return container->eglwindow;
}

gboolean moz_container_has_wl_egl_window(MozContainer *container) {
  return container->eglwindow ? true : false;
}

gboolean moz_container_surface_needs_clear(MozContainer *container) {
  gboolean state = container->surface_needs_clear;
  container->surface_needs_clear = false;
  return state;
}
#endif

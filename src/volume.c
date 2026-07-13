// waybar CFFI plugin: audio volume.
//  - Bar pill: speaker icon (by level / muted) + NN%.
//  - Scroll = ±5%, right-click = mute toggle, left-click = popover with a volume
//    slider, a mute button, and an output-device picker.
//  - Control via wpctl (PipeWire); updates are event-driven off `pactl subscribe`
//    (no polling).
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "waybar_cffi_module.h"
const size_t wbcffi_version = 1;

#define IC_MUTE "\xf3\xb0\x9d\x9f"   // 󰝟 volume-off/mute
#define IC_LOW  "\xf3\xb0\x95\xbf"   // 󰕿 volume-low
#define IC_MED  "\xf3\xb0\x96\x80"   // 󰖀 volume-medium
#define IC_HIGH "\xf3\xb0\x95\xbe"   // 󰕾 volume-high

typedef struct {
  GtkWidget *box, *icon, *label, *popover, *scale;
  double vol; int muted, step, updating;
  GCancellable *cancel; GSubprocess *sub;
  char *icon_dir; int icon_size;
} Inst;

static const char *vol_icon_name(Inst *s) {
  if (s->muted || s->vol <= 0.001) return "vol-mute.svg";
  if (s->vol < 0.34) return "vol-low.svg";
  if (s->vol < 0.67) return "vol-med.svg";
  return "vol-high.svg";
}
static GdkPixbuf *vol_pixbuf(Inst *self, const char *name) {
  char *path = g_build_filename(self->icon_dir, name, NULL);
  GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(path, self->icon_size, self->icon_size, NULL);
  g_free(path);
  return pb;
}
static void update_bar(Inst *self) {
  GdkPixbuf *pb = vol_pixbuf(self, vol_icon_name(self));
  if (pb) { gtk_image_set_from_pixbuf(GTK_IMAGE(self->icon), pb); g_object_unref(pb); }
  char t[16]; g_snprintf(t, sizeof t, "%d%%", (int)lround(self->vol * 100));
  gtk_label_set_text(GTK_LABEL(self->label), t);
  GtkStyleContext *c = gtk_widget_get_style_context(self->box);
  if (self->muted) gtk_style_context_add_class(c, "muted");
  else gtk_style_context_remove_class(c, "muted");
}

// wpctl get-volume @DEFAULT_AUDIO_SINK@ -> "Volume: 0.65 [MUTED]"
static void read_volume(Inst *self) {
  char *out = NULL;
  if (g_spawn_command_line_sync("wpctl get-volume @DEFAULT_AUDIO_SINK@", &out, NULL, NULL, NULL) && out) {
    double v = 0; char *p = strstr(out, "Volume:");
    if (p) v = g_ascii_strtod(p + 7, NULL);
    self->vol = v;
    self->muted = strstr(out, "MUTED") != NULL;
  }
  g_free(out);
  update_bar(self);
  if (self->scale && gtk_widget_is_visible(self->popover)) {
    self->updating = 1;
    gtk_range_set_value(GTK_RANGE(self->scale), self->vol * 100);
    self->updating = 0;
  }
}

static void wpctl(const char *arg1, const char *arg2, const char *arg3) {
  const char *argv[8] = {"wpctl", arg1, "@DEFAULT_AUDIO_SINK@", arg2, arg3, NULL};
  int i = 3; if (!arg2) argv[i] = NULL; else if (!arg3) argv[4] = NULL;
  GSubprocess *sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (sp) g_object_unref(sp);
}
static void set_volume_pct(int pct) {
  if (pct < 0) pct = 0; if (pct > 150) pct = 150;
  char v[16]; g_snprintf(v, sizeof v, "%d%%", pct);
  wpctl("set-volume", v, NULL);
}

// ─── scroll / right-click ────────────────────────────────────────────────────
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer d) {
  (void)w; Inst *self = d; int cur = (int)lround(self->vol * 100);
  if (e->direction == GDK_SCROLL_UP) set_volume_pct(cur + self->step);
  else if (e->direction == GDK_SCROLL_DOWN) set_volume_pct(cur - self->step);
  else if (e->direction == GDK_SCROLL_SMOOTH) {
    double dx, dy; gdk_event_get_scroll_deltas((GdkEvent*)e, &dx, &dy);
    if (dy < 0) set_volume_pct(cur + self->step); else if (dy > 0) set_volume_pct(cur - self->step);
  }
  return TRUE;
}
static gboolean on_pop_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
  (void)d; if (e->keyval == GDK_KEY_Escape) { gtk_popover_popdown(GTK_POPOVER(w)); return TRUE; }
  return FALSE;
}

// ─── popover ─────────────────────────────────────────────────────────────────
static void on_scale_changed(GtkRange *r, gpointer d) {
  Inst *self = d; if (self->updating) return;
  set_volume_pct((int)lround(gtk_range_get_value(r)));
}
static void on_mute_clicked(GtkButton *b, gpointer d) { (void)b; wpctl("set-mute", "toggle", NULL); }
typedef struct { char name[256]; } SinkCtx;
static void on_sink_clicked(GtkButton *b, gpointer d) {
  (void)b; SinkCtx *s = d;
  const char *argv[] = {"pactl", "set-default-sink", s->name, NULL};
  GSubprocess *sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (sp) g_object_unref(sp);
}
static void sinkctx_free(gpointer p, GClosure *c) { (void)c; g_free(p); }

static void rebuild_popover(Inst *self) {
  GtkWidget *old = gtk_bin_get_child(GTK_BIN(self->popover));
  if (old) gtk_widget_destroy(old);
  GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(v, 16); gtk_widget_set_margin_end(v, 16);
  gtk_widget_set_margin_top(v, 14); gtk_widget_set_margin_bottom(v, 14);
  gtk_widget_set_size_request(v, 320, -1);
  gtk_style_context_add_class(gtk_widget_get_style_context(v), "vo-pop");

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *mute = gtk_button_new();
  { GdkPixbuf *mpb = vol_pixbuf(self, vol_icon_name(self));
    if (mpb) { GtkWidget *mi = gtk_image_new_from_pixbuf(mpb); g_object_unref(mpb); gtk_button_set_image(GTK_BUTTON(mute), mi); } }
  gtk_style_context_add_class(gtk_widget_get_style_context(mute), "vo-mute");
  g_signal_connect(mute, "clicked", G_CALLBACK(on_mute_clicked), self);
  self->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_scale_set_draw_value(GTK_SCALE(self->scale), FALSE);
  gtk_widget_set_hexpand(self->scale, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->scale), "vo-scale");
  self->updating = 1; gtk_range_set_value(GTK_RANGE(self->scale), self->vol * 100); self->updating = 0;
  g_signal_connect(self->scale, "value-changed", G_CALLBACK(on_scale_changed), self);
  gtk_box_pack_start(GTK_BOX(row), mute, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row), self->scale, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(v), row, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
  GtkWidget *oh = gtk_label_new("Output"); gtk_widget_set_halign(oh, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(oh), "vo-head");
  gtk_box_pack_start(GTK_BOX(v), oh, FALSE, FALSE, 0);

  char *def = NULL; g_spawn_command_line_sync("pactl get-default-sink", &def, NULL, NULL, NULL);
  if (def) g_strchomp(def);
  char *out = NULL;
  if (g_spawn_command_line_sync("pactl list sinks", &out, NULL, NULL, NULL) && out) {
    char *save = NULL, cur_name[256] = ""; char cur_desc[256] = "";
    for (char *ln = strtok_r(out, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
      char *t = g_strstrip(g_strdup(ln));
      if (g_str_has_prefix(t, "Name: ")) g_strlcpy(cur_name, t + 6, sizeof cur_name);
      else if (g_str_has_prefix(t, "Description: ")) {
        g_strlcpy(cur_desc, t + 13, sizeof cur_desc);
        if (cur_name[0]) {
          GtkWidget *b = gtk_button_new_with_label(cur_desc);
          gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(b)), GTK_ALIGN_START);
          gtk_style_context_add_class(gtk_widget_get_style_context(b), "vo-sink");
          if (def && !strcmp(def, cur_name)) gtk_style_context_add_class(gtk_widget_get_style_context(b), "vo-active");
          SinkCtx *sc = g_new0(SinkCtx, 1); g_strlcpy(sc->name, cur_name, sizeof sc->name);
          g_signal_connect_data(b, "clicked", G_CALLBACK(on_sink_clicked), sc, sinkctx_free, 0);
          gtk_box_pack_start(GTK_BOX(v), b, FALSE, FALSE, 0);
          cur_name[0] = 0;
        }
      }
      g_free(t);
    }
  }
  g_free(out); g_free(def);
  gtk_container_add(GTK_CONTAINER(self->popover), v);
  gtk_widget_show_all(v);
}
static gboolean on_click(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  (void)w; Inst *self = data;
  if (ev->button == 3) { wpctl("set-mute", "toggle", NULL); return TRUE; }
  if (ev->button != 1) return FALSE;
  read_volume(self);
  rebuild_popover(self);
  gtk_popover_popup(GTK_POPOVER(self->popover));
  gtk_widget_grab_focus(self->popover);
  return TRUE;
}

// ─── pactl subscribe (event-driven refresh) ─────────────────────────────────
static void read_next(Inst *self, GDataInputStream *in);
static void on_line(GObject *src, GAsyncResult *res, gpointer data) {
  Inst *self = data; GDataInputStream *in = G_DATA_INPUT_STREAM(src);
  gsize len = 0; char *line = g_data_input_stream_read_line_finish(in, res, &len, NULL);
  if (g_cancellable_is_cancelled(self->cancel)) { g_free(line); g_object_unref(in); return; }
  if (!line) { g_object_unref(in); return; }   // stream closed
  if (strstr(line, "sink") || strstr(line, "server")) read_volume(self);
  g_free(line);
  read_next(self, in);
}
static void read_next(Inst *self, GDataInputStream *in) {
  g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, self->cancel, on_line, self);
}
static void start_subscribe(Inst *self) {
  self->sub = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                               NULL, "pactl", "subscribe", NULL);
  if (!self->sub) return;
  GInputStream *st = g_subprocess_get_stdout_pipe(self->sub);
  GDataInputStream *in = g_data_input_stream_new(st);
  read_next(self, in);
}

static GtkWidget *mklabel(const char *t, const char *cls) {
  GtkWidget *l = gtk_label_new(t);
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(l), cls); return l;
}
void *wbcffi_init(const wbcffi_init_info *info, const wbcffi_config_entry *entries, size_t entries_len) {
  Inst *self = g_new0(Inst, 1);
  self->step = 5; self->icon_size = 24;
  for (size_t i = 0; i < entries_len; i++) {
    if (!strcmp(entries[i].key, "scroll-step")) { self->step = atoi(entries[i].value); if (self->step < 1) self->step = 1; }
    else if (!strcmp(entries[i].key, "icon-size")) { self->icon_size = atoi(entries[i].value); if (self->icon_size < 8) self->icon_size = 8; }
    else if (!strcmp(entries[i].key, "icon-dir")) { g_free(self->icon_dir); self->icon_dir = g_strdup(entries[i].value); }
  }
  if (!self->icon_dir) {
    const char *dh = g_getenv("XDG_DATA_HOME");
    self->icon_dir = (dh && *dh) ? g_build_filename(dh, "waybar-volume", NULL)
                                 : g_build_filename(g_get_home_dir(), ".local/share/waybar-volume", NULL);
  }
  self->cancel = g_cancellable_new();

  GtkContainer *root = info->get_root_widget(info->obj);
  self->box = gtk_event_box_new();
  gtk_widget_set_name(self->box, "volume");
  gtk_widget_add_events(self->box, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
  gtk_widget_set_margin_start(self->box, 6); gtk_widget_set_margin_end(self->box, 6);
  GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  self->icon = gtk_image_new();
  gtk_widget_set_valign(self->icon, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->icon), "vo-icon");
  self->label = mklabel("--%", "vo-label");
  gtk_box_pack_start(GTK_BOX(h), self->icon, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(h), self->label, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(self->box), h);
  self->popover = gtk_popover_new(self->box);
  gtk_popover_set_position(GTK_POPOVER(self->popover), GTK_POS_BOTTOM);
  gtk_popover_set_constrain_to(GTK_POPOVER(self->popover), GTK_POPOVER_CONSTRAINT_NONE);
  gtk_popover_set_modal(GTK_POPOVER(self->popover), TRUE);
  gtk_widget_add_events(self->popover, GDK_KEY_PRESS_MASK);
  g_signal_connect(self->popover, "key-press-event", G_CALLBACK(on_pop_key), NULL);
  g_signal_connect(self->box, "button-press-event", G_CALLBACK(on_click), self);
  g_signal_connect(self->box, "scroll-event", G_CALLBACK(on_scroll), self);
  gtk_container_add(root, self->box);
  gtk_widget_show_all(GTK_WIDGET(root));

  read_volume(self);
  start_subscribe(self);
  return self;
}
void wbcffi_deinit(void *instance) {
  Inst *self = instance;
  if (self->cancel) g_cancellable_cancel(self->cancel);
  if (self->sub) { g_subprocess_force_exit(self->sub); g_object_unref(self->sub); }
  g_clear_object(&self->cancel);
  g_free(self->icon_dir);
  g_free(self);
}

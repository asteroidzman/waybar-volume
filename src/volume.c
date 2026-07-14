// waybar CFFI plugin: audio volume.
//  - Bar pill: speaker icon (by level / muted) + NN%.
//  - Scroll = ±5%, right-click = mute toggle, left-click = popup with output
//    volume/mute + device picker and input (mic) volume/mute + device picker.
//  - Control via wpctl (PipeWire); updates are event-driven off `pactl subscribe`
//    (no polling; the subscribe child respawns if the audio server restarts).
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "wbcommon.h"
#include "waybar_cffi_module.h"
const size_t wbcffi_version = 1;

typedef struct {
  GtkWidget *box, *icon, *label, *scale, *in_scale;
  double vol, in_vol;
  int muted, in_muted, step, updating;
  char *icon_dir; int icon_size;
  WbPop pop;
  WbReader *rdr;
} Inst;

static const char *vol_icon_name(Inst *s) {
  if (s->muted || s->vol <= 0.001) return "vol-mute.svg";
  if (s->vol < 0.34) return "vol-low.svg";
  if (s->vol < 0.67) return "vol-med.svg";
  return "vol-high.svg";
}

static void update_bar(Inst *self) {
  wb_icon_set(self->icon, vol_icon_name(self));
  char t[16]; g_snprintf(t, sizeof t, "%d%%", (int)lround(self->vol * 100));
  gtk_label_set_text(GTK_LABEL(self->label), t);
  GtkStyleContext *c = gtk_widget_get_style_context(self->box);
  if (self->muted) gtk_style_context_add_class(c, "muted");
  else gtk_style_context_remove_class(c, "muted");
}

// wpctl get-volume @DEFAULT_AUDIO_SINK@ -> "Volume: 0.65 [MUTED]"
static void read_target(const char *target, double *vol, int *muted) {
  char *cmd = g_strdup_printf("wpctl get-volume %s", target);
  char *out = NULL;
  if (g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL) && out) {
    double v = 0; char *p = strstr(out, "Volume:");
    if (p) v = g_ascii_strtod(p + 7, NULL);
    *vol = v;
    *muted = strstr(out, "MUTED") != NULL;
  }
  g_free(out); g_free(cmd);
}
static void read_volume(Inst *self) {
  read_target("@DEFAULT_AUDIO_SINK@", &self->vol, &self->muted);
  read_target("@DEFAULT_AUDIO_SOURCE@", &self->in_vol, &self->in_muted);
  update_bar(self);
  if (wbpop_visible(&self->pop)) {
    self->updating = 1;
    if (self->scale) gtk_range_set_value(GTK_RANGE(self->scale), self->vol * 100);
    if (self->in_scale) gtk_range_set_value(GTK_RANGE(self->in_scale), self->in_vol * 100);
    self->updating = 0;
  }
}

static void wpctl(const char *target, const char *arg1, const char *arg2) {
  const char *argv[] = {"wpctl", arg1, target, arg2, NULL, NULL};
  GSubprocess *sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (sp) g_object_unref(sp);
}
static void set_target_pct(const char *target, int pct) {
  if (pct < 0) pct = 0;
  if (pct > 150) pct = 150;
  char v[16]; g_snprintf(v, sizeof v, "%d%%", pct);
  wpctl(target, "set-volume", v);
}

// ─── scroll / right-click ────────────────────────────────────────────────────
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer d) {
  (void)w; Inst *self = d; int cur = (int)lround(self->vol * 100);
  if (e->direction == GDK_SCROLL_UP) set_target_pct("@DEFAULT_AUDIO_SINK@", cur + self->step);
  else if (e->direction == GDK_SCROLL_DOWN) set_target_pct("@DEFAULT_AUDIO_SINK@", cur - self->step);
  else if (e->direction == GDK_SCROLL_SMOOTH) {
    double dx, dy; gdk_event_get_scroll_deltas((GdkEvent*)e, &dx, &dy);
    if (dy < 0) set_target_pct("@DEFAULT_AUDIO_SINK@", cur + self->step);
    else if (dy > 0) set_target_pct("@DEFAULT_AUDIO_SINK@", cur - self->step);
  }
  return TRUE;
}

// ─── popup ───────────────────────────────────────────────────────────────────
static void on_scale_changed(GtkRange *r, gpointer d) {
  Inst *self = d; if (self->updating) return;
  set_target_pct("@DEFAULT_AUDIO_SINK@", (int)lround(gtk_range_get_value(r)));
}
static void on_in_scale_changed(GtkRange *r, gpointer d) {
  Inst *self = d; if (self->updating) return;
  set_target_pct("@DEFAULT_AUDIO_SOURCE@", (int)lround(gtk_range_get_value(r)));
}
static void on_mute_clicked(GtkButton *b, gpointer d) { (void)b; (void)d; wpctl("@DEFAULT_AUDIO_SINK@", "set-mute", "toggle"); }
static void on_in_mute_clicked(GtkButton *b, gpointer d) { (void)b; (void)d; wpctl("@DEFAULT_AUDIO_SOURCE@", "set-mute", "toggle"); }

typedef struct { char name[256]; char set_cmd[32]; } DevCtx;
static void on_dev_clicked(GtkButton *b, gpointer d) {
  (void)b; DevCtx *s = d;
  const char *argv[] = {"pactl", s->set_cmd, s->name, NULL};
  GSubprocess *sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (sp) g_object_unref(sp);
}
static void devctx_free(gpointer p, GClosure *c) { (void)c; g_free(p); }

// One slider row: mute button (themed icon) + volume scale.
static GtkWidget *slider_row(Inst *self, const char *icon_name, GtkWidget **scale_out,
                             GCallback mute_cb, GCallback scale_cb, double value) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *mute = gtk_button_new();
  gtk_button_set_image(GTK_BUTTON(mute), wb_icon_new(self->icon_dir, self->icon_size, icon_name));
  gtk_style_context_add_class(gtk_widget_get_style_context(mute), "vo-mute");
  g_signal_connect(mute, "clicked", mute_cb, self);
  GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
  gtk_widget_set_hexpand(sc, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(sc), "vo-scale");
  self->updating = 1; gtk_range_set_value(GTK_RANGE(sc), value * 100); self->updating = 0;
  g_signal_connect(sc, "value-changed", scale_cb, self);
  gtk_box_pack_start(GTK_BOX(row), mute, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row), sc, TRUE, TRUE, 0);
  *scale_out = sc;
  return row;
}

// Device list (sinks or sources) appended to `v`. Returns the button for the
// active device (or the first), and writes the active device's description to
// cur_desc for the "current device" caption. Monitor sources are skipped.
static GtkWidget *device_list(GtkWidget *v, const char *kind, const char *set_cmd,
                              int skip_monitors, char *cur_desc, size_t cur_desc_len) {
  GtkWidget *first = NULL, *active = NULL;
  char *cmd = g_strdup_printf("pactl get-default-%s", kind);
  char *def = NULL; g_spawn_command_line_sync(cmd, &def, NULL, NULL, NULL);
  g_free(cmd);
  if (def) g_strchomp(def);
  cmd = g_strdup_printf("pactl list %ss", kind);
  char *out = NULL;
  gboolean ok = g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL);
  g_free(cmd);
  int count = 0;
  if (ok && out) {
    char *save = NULL, cur_name[256] = "";
    for (char *ln = strtok_r(out, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
      char *t = g_strstrip(g_strdup(ln));
      if (g_str_has_prefix(t, "Name: ")) g_strlcpy(cur_name, t + 6, sizeof cur_name);
      else if (g_str_has_prefix(t, "Description: ") && cur_name[0]) {
        if (skip_monitors && g_str_has_suffix(cur_name, ".monitor")) { cur_name[0] = 0; g_free(t); continue; }
        GtkWidget *b = gtk_button_new_with_label(t + 13);
        gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(b)), GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "vo-sink");
        if (def && !strcmp(def, cur_name)) {
          gtk_style_context_add_class(gtk_widget_get_style_context(b), "vo-active");
          active = b;
          if (cur_desc) g_strlcpy(cur_desc, t + 13, cur_desc_len);
        }
        DevCtx *dc = g_new0(DevCtx, 1);
        g_strlcpy(dc->name, cur_name, sizeof dc->name);
        g_strlcpy(dc->set_cmd, set_cmd, sizeof dc->set_cmd);
        g_signal_connect_data(b, "clicked", G_CALLBACK(on_dev_clicked), dc, devctx_free, 0);
        gtk_box_pack_start(GTK_BOX(v), b, FALSE, FALSE, 0);
        if (!first) first = b;
        count++;
        cur_name[0] = 0;
      }
      g_free(t);
    }
  }
  g_free(out); g_free(def);
  if (!count) {   // degradation: say why the list is empty instead of showing nothing
    GtkWidget *e = gtk_label_new(ok && out && *out ? "No devices found"
                                                   : "Audio server unavailable");
    gtk_widget_set_halign(e, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(e), "vo-err");
    gtk_box_pack_start(GTK_BOX(v), e, FALSE, FALSE, 0);
  }
  return active ? active : first;
}

static void section_head(GtkWidget *v, const char *txt) {
  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
  GtkWidget *h = gtk_label_new(txt);
  gtk_widget_set_halign(h, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(h), "vo-head");
  gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
}

static void rebuild_popover(Inst *self) {
  GtkWidget *old = gtk_bin_get_child(GTK_BIN(self->pop.win));
  if (old) gtk_widget_destroy(old);
  GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(v, 18); gtk_widget_set_margin_end(v, 18);
  gtk_widget_set_margin_top(v, 16); gtk_widget_set_margin_bottom(v, 16);
  gtk_widget_set_size_request(v, 420, -1);
  gtk_style_context_add_class(gtk_widget_get_style_context(v), "vo-pop");

  // ── output ──
  gtk_box_pack_start(GTK_BOX(v),
      slider_row(self, vol_icon_name(self), &self->scale,
                 G_CALLBACK(on_mute_clicked), G_CALLBACK(on_scale_changed), self->vol),
      FALSE, FALSE, 0);
  char cur[256] = "";
  section_head(v, "Output");
  GtkWidget *focus = device_list(v, "sink", "set-default-sink", 0, cur, sizeof cur);
  if (cur[0]) {   // caption: which device the slider drives
    GtkWidget *cl = gtk_label_new(cur);
    gtk_widget_set_halign(cl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(cl), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(cl), "vo-cursink");
    gtk_box_pack_start(GTK_BOX(v), cl, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(v), cl, 1);   // directly under the slider row
  }

  // ── input (mic) ──
  gtk_box_pack_start(GTK_BOX(v),
      slider_row(self, self->in_muted ? "mic-mute.svg" : "mic.svg", &self->in_scale,
                 G_CALLBACK(on_in_mute_clicked), G_CALLBACK(on_in_scale_changed), self->in_vol),
      FALSE, FALSE, 0);
  section_head(v, "Input");
  device_list(v, "source", "set-default-source", 1, NULL, 0);

  if (focus) g_object_set_data(G_OBJECT(self->pop.win), "wb-focus", focus);
  gtk_container_add(GTK_CONTAINER(self->pop.win), v);
  gtk_widget_show_all(v);
}
static void rebuild_cb(gpointer user) { rebuild_popover(user); }

static gboolean on_click(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  (void)w; Inst *self = data;
  if (ev->button == 3) { wpctl("@DEFAULT_AUDIO_SINK@", "set-mute", "toggle"); return TRUE; }
  if (ev->button != 1) return FALSE;
  read_volume(self);
  wbpop_toggle(&self->pop);
  return TRUE;
}

// ─── pactl subscribe (event-driven refresh; respawns via WbReader) ──────────
static void on_pactl_line(const char *line, gpointer user) {
  Inst *self = user;
  if (strstr(line, "sink") || strstr(line, "source") || strstr(line, "server"))
    read_volume(self);
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

  GtkContainer *root = info->get_root_widget(info->obj);
  self->box = gtk_event_box_new();
  gtk_widget_set_name(self->box, "volume");
  gtk_widget_add_events(self->box, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
  gtk_widget_set_margin_start(self->box, 6); gtk_widget_set_margin_end(self->box, 6);
  GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  self->icon = wb_icon_new(self->icon_dir, self->icon_size, NULL);
  gtk_widget_set_valign(self->icon, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->icon), "vo-icon");
  self->label = mklabel("--%", "vo-label");
  gtk_box_pack_start(GTK_BOX(h), self->icon, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(h), self->label, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(self->box), h);
  wbpop_init(&self->pop, self->box, rebuild_cb, self);
  g_signal_connect(self->box, "button-press-event", G_CALLBACK(on_click), self);
  g_signal_connect(self->box, "scroll-event", G_CALLBACK(on_scroll), self);
  gtk_container_add(root, self->box);
  gtk_widget_show_all(GTK_WIDGET(root));

  read_volume(self);
  const char *argv[] = {"pactl", "subscribe", NULL};
  self->rdr = wb_reader_start(argv, on_pactl_line, self, G_PRIORITY_DEFAULT);
  return self;
}
void wbcffi_deinit(void *instance) {
  Inst *self = instance;
  wb_reader_free(self->rdr);
  wbpop_destroy(&self->pop);
  g_free(self->icon_dir);
  g_free(self);
}

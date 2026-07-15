#include <gtk/gtk.h>
#include <adwaita.h>
#include "dbus_proxy.h"

/* ----------------------------------------------------------------
 * App state
 * ---------------------------------------------------------------- */

typedef struct {
    DbusProxy *proxy;
    GtkListBox *game_list;
    GtkTextView *log_view;
    GtkLabel *lbl_mode;
    GtkLabel *lbl_scene;
    GtkLabel *lbl_heavy;
    GtkLabel *lbl_max_temp;
    GtkLabel *lbl_thermal;
    GtkProgressBar *load_bar;
    GtkProgressBar *temp_bar;
    GtkWidget **freq_labels;
    GtkWidget **load_labels;
    GtkStack *stack;
    GtkAdjustment *freq_adjustments[4];
    GtkSwitch *freq_toggle;
} AppState;

static AppState g_app;

static void refresh_games(void);

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static void refresh_display(void) {
    if (!g_app.proxy) return;

    if (g_app.lbl_mode)
        gtk_label_set_text(g_app.lbl_mode, g_app.proxy->current_mode);
    if (g_app.lbl_scene)
        gtk_label_set_text(g_app.lbl_scene, g_app.proxy->current_scene);
    if (g_app.lbl_heavy)
        gtk_label_set_text(g_app.lbl_heavy,
            g_app.proxy->is_heavy_load ? "HEAVY LOAD ACTIVE" : "Normal Load");
    if (g_app.load_bar)
        gtk_progress_bar_set_fraction(g_app.load_bar,
            g_app.proxy->is_heavy_load ? 0.9 : 0.3);
    if (g_app.lbl_max_temp) {
        double temp_c = g_app.proxy->max_temp / 1000.0;
        gchar buf[64];
        g_snprintf(buf, sizeof(buf), "%.1f C", temp_c);
        gtk_label_set_text(g_app.lbl_max_temp, buf);
    }
    if (g_app.lbl_thermal)
        gtk_label_set_text(g_app.lbl_thermal, g_app.proxy->thermal_state);
    if (g_app.temp_bar) {
        double frac = g_app.proxy->max_temp > 0
            ? CLAMP((g_app.proxy->max_temp / 1000.0 - 40.0) / 60.0, 0.0, 1.0)
            : 0.0;
        gtk_progress_bar_set_fraction(g_app.temp_bar, frac);
    }
    for (int i = 0; i < g_app.proxy->nr_freqs && i < 3; i++) {
        if (g_app.freq_labels && g_app.freq_labels[i]) {
            gchar buf[32];
            g_snprintf(buf, sizeof(buf), "%.2f GHz", g_app.proxy->freqs[i] / 1000.0);
            gtk_label_set_text(GTK_LABEL(g_app.freq_labels[i]), buf);
        }
    }
    for (int i = 0; i < g_app.proxy->nr_loads && i < 8; i++) {
        if (g_app.load_labels && g_app.load_labels[i]) {
            gchar buf[32];
            g_snprintf(buf, sizeof(buf), "%.0f%%", g_app.proxy->loads[i]);
            gtk_label_set_text(GTK_LABEL(g_app.load_labels[i]), buf);
        }
    }
}

/* Callback helpers to bridge GCallback with our functions */
static void on_mode_changed(DbusProxy *p, gchar *m, gpointer ud) {
    (void)p; (void)m; (void)ud;
    refresh_display();
}
static void on_scene_changed(DbusProxy *p, gchar *m, gpointer ud) {
    (void)p; (void)m; (void)ud;
    refresh_display();
}
static void on_stats_updated(DbusProxy *p, gpointer ud) {
    (void)p; (void)ud;
    refresh_display();
    refresh_games();
}
static void on_heavy_changed(DbusProxy *p, gboolean h, gpointer ud) {
    (void)p; (void)h; (void)ud;
    refresh_display();
}
static void on_thermal_changed(DbusProxy *p, gint32 t, gpointer ud) {
    (void)p; (void)t; (void)ud;
    refresh_display();
}

static void on_set_mode_clicked(GtkButton *btn, const gchar *mode) {
    (void)btn;
    if (g_app.proxy) dbus_proxy_set_mode(g_app.proxy, mode);
    refresh_display();
}

typedef struct {
    gint pid;
    gchar *app;
} GameModeTarget;

static void game_mode_target_free(gpointer data, GClosure *closure) {
    (void)closure;
    GameModeTarget *target = data;
    if (!target) return;
    g_free(target->app);
    g_free(target);
}

static void on_set_game_mode(GObject *object, GParamSpec *pspec,
                             gpointer userdata) {
    (void)pspec;
    if (!g_app.proxy) return;
    GtkDropDown *dropdown = GTK_DROP_DOWN(object);
    GameModeTarget *target = userdata;
    guint idx = gtk_drop_down_get_selected(dropdown);
    const gchar *modes[] = {"balance", "powersave", "performance"};
    if (target && idx < G_N_ELEMENTS(modes))
        dbus_proxy_set_game_mode(g_app.proxy, target->pid, target->app,
                                 modes[idx]);
}

static void on_refresh_logs_clicked(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(g_app.log_view);
    GError *error = NULL;
    GSubprocess *process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
        &error, "journalctl", "-u", "uperf-linux.service", "-n", "200",
        "--no-pager", NULL);
    if (!process) {
        gtk_text_buffer_set_text(buf,
            error ? error->message : "Unable to start journalctl", -1);
        g_clear_error(&error);
        return;
    }
    gchar *output = NULL;
    if (!g_subprocess_communicate_utf8(process, NULL, NULL, &output, NULL,
                                       &error)) {
        gtk_text_buffer_set_text(buf,
            error ? error->message : "Unable to read system journal", -1);
        g_clear_error(&error);
    } else {
        gtk_text_buffer_set_text(buf, output ? output : "", -1);
    }
    g_free(output);
    g_object_unref(process);
}

static void on_clear_logs_clicked(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(g_app.log_view);
    gtk_text_buffer_set_text(buf, "", -1);
}

static void on_apply_freq_clicked(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    if (!g_app.proxy || !g_app.freq_toggle) return;
    if (!gtk_switch_get_active(g_app.freq_toggle)) {
        dbus_proxy_release_freq_override(g_app.proxy);
        return;
    }

    /* CPU controls are displayed in kHz; the daemon API accepts Hz. */
    gint64 prime = (gint64)gtk_adjustment_get_value(g_app.freq_adjustments[0]) * 1000;
    gint64 perf = (gint64)gtk_adjustment_get_value(g_app.freq_adjustments[1]) * 1000;
    gint64 efficiency = (gint64)gtk_adjustment_get_value(g_app.freq_adjustments[2]) * 1000;
    gint64 gpu = (gint64)gtk_adjustment_get_value(g_app.freq_adjustments[3]);
    if (!dbus_proxy_apply_freq_override(g_app.proxy, prime, perf, efficiency,
                                        gpu))
        gtk_switch_set_active(g_app.freq_toggle, FALSE);
}

static void on_release_freq_clicked(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    if (g_app.proxy) dbus_proxy_release_freq_override(g_app.proxy);
    if (g_app.freq_toggle) gtk_switch_set_active(g_app.freq_toggle, FALSE);
}

static void on_reload_config_clicked(GtkButton *btn, gpointer ud) {
    (void)ud;
    gboolean success = g_app.proxy &&
        dbus_proxy_reload_config(g_app.proxy);
    gtk_button_set_label(btn, success ? "Reloaded" : "Reload failed");
}

static void on_tab_clicked(GtkButton *btn, const gchar *page_name) {
    (void)btn;
    gtk_stack_set_visible_child_name(g_app.stack, page_name);
}

/* ----------------------------------------------------------------
 * Dashboard page
 * ---------------------------------------------------------------- */

static GtkWidget *create_dashboard_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    /* Title */
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(title), "uperf-linux");
    gtk_widget_set_css_classes(title, (const gchar *[]){"heading", NULL});
    gtk_box_append(GTK_BOX(box), title);

    /* SOC badge */
    GtkWidget *soc = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(soc), "SM8550 - Snapdragon 8 Gen 2");
    gtk_widget_set_css_classes(soc, (const gchar *[]){"caption", NULL});
    gtk_box_append(GTK_BOX(box), soc);

    /* Power Mode buttons */
    GtkWidget *mode_frame = adw_clamp_new();
    gtk_widget_set_valign(mode_frame, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(mode_frame, TRUE);
    GtkWidget *mode_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(mode_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(mode_grid), 8);
    adw_clamp_set_child(ADW_CLAMP(mode_frame), mode_grid);

    g_app.lbl_mode = GTK_LABEL(gtk_label_new("balance"));
    gtk_widget_set_css_classes(GTK_WIDGET(g_app.lbl_mode), (const gchar *[]){"caption", NULL});
    gtk_label_set_justify(g_app.lbl_mode, GTK_JUSTIFY_CENTER);

    const char *modes[] = {"balance", "powersave", "performance"};
    const char *icons[] = {"B", "P", "X"};
    const char *labels[] = {"Balance", "Powersave", "Performance"};

    for (int i = 0; i < 3; i++) {
        GtkWidget *btn = gtk_button_new();
        gtk_widget_set_css_classes(btn, (const gchar *[]){"flat", NULL});
        gtk_widget_set_hexpand(btn, TRUE);
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_set_spacing(GTK_BOX(vbox), 4);
        GtkWidget *icon = gtk_label_new(icons[i]);
        gtk_widget_add_css_class(icon, "heading");
        GtkWidget *lbl = gtk_label_new(labels[i]);
        gtk_widget_add_css_class(lbl, "heading");
        gtk_box_append(GTK_BOX(vbox), icon);
        gtk_box_append(GTK_BOX(vbox), lbl);
        gtk_button_set_child(GTK_BUTTON(btn), vbox);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_set_mode_clicked), (gpointer)modes[i]);
        gtk_grid_attach(GTK_GRID(mode_grid), btn, i, 0, 1, 1);
    }
    gtk_grid_attach(GTK_GRID(mode_grid), GTK_WIDGET(g_app.lbl_mode), 0, 1, 3, 1);
    gtk_box_append(GTK_BOX(box), mode_frame);

    /* Scene badge */
    g_app.lbl_scene = GTK_LABEL(gtk_label_new("IDLE"));
    gtk_widget_set_css_classes(GTK_WIDGET(g_app.lbl_scene), (const gchar *[]){"heading", NULL});
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(g_app.lbl_scene));

    /* Heavy load */
    g_app.lbl_heavy = GTK_LABEL(gtk_label_new("Normal"));
    gtk_widget_add_css_class(GTK_WIDGET(g_app.lbl_heavy), "heading");
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(g_app.lbl_heavy));

    /* Per-policy frequency grid */
    GtkWidget *freq_frame = adw_clamp_new();
    gtk_widget_set_valign(freq_frame, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(freq_frame, TRUE);
    GtkWidget *freq_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(freq_grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(freq_grid), 4);
    adw_clamp_set_child(ADW_CLAMP(freq_frame), freq_grid);

    g_app.freq_labels = g_malloc0(3 * sizeof(GtkWidget *));
    const gchar *policy_names[] = {"Prime", "Performance", "Efficiency"};
    for (int i = 0; i < 3; i++) {
        GtkWidget *policy_lbl = gtk_label_new(policy_names[i]);
        gtk_widget_set_css_classes(policy_lbl,
                                   (const gchar *[]){"caption", NULL});
        GtkWidget *freq_lbl = gtk_label_new("--");
        g_app.freq_labels[i] = freq_lbl;
        GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(cell), policy_lbl);
        gtk_box_append(GTK_BOX(cell), freq_lbl);
        gtk_grid_attach(GTK_GRID(freq_grid), cell, i, 0, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), freq_frame);

    /* Per-CPU utilization grid */
    GtkWidget *load_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(load_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(load_grid), 4);
    g_app.load_labels = g_malloc0(8 * sizeof(GtkWidget *));
    for (int i = 0; i < 8; i++) {
        gchar cpu_name[16];
        g_snprintf(cpu_name, sizeof(cpu_name), "CPU%d", i);
        GtkWidget *cpu_lbl = gtk_label_new(cpu_name);
        gtk_widget_set_css_classes(cpu_lbl,
                                   (const gchar *[]){"caption", NULL});
        GtkWidget *load_lbl = gtk_label_new("--");
        g_app.load_labels[i] = load_lbl;
        GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(cell), cpu_lbl);
        gtk_box_append(GTK_BOX(cell), load_lbl);
        gtk_grid_attach(GTK_GRID(load_grid), cell, i % 4, i / 4, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), load_grid);

    /* Load bar */
    g_app.load_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_hexpand(GTK_WIDGET(g_app.load_bar), TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(g_app.load_bar));
    gtk_progress_bar_set_fraction(g_app.load_bar, 0.3);

    /* Thermal section */
    GtkWidget *therm_frame = adw_clamp_new();
    gtk_widget_set_valign(therm_frame, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(therm_frame, TRUE);
    GtkWidget *therm_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_set_spacing(GTK_BOX(therm_box), 8);
    gtk_widget_set_margin_top(GTK_WIDGET(therm_box), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(therm_box), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(therm_box), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(therm_box), 12);
    adw_clamp_set_child(ADW_CLAMP(therm_frame), therm_box);

    g_app.lbl_max_temp = GTK_LABEL(gtk_label_new("-- C"));
    gtk_widget_set_css_classes(GTK_WIDGET(g_app.lbl_max_temp), (const gchar *[]){"heading", NULL});
    gtk_box_append(GTK_BOX(therm_box), GTK_WIDGET(g_app.lbl_max_temp));

    g_app.lbl_thermal = GTK_LABEL(gtk_label_new("NORMAL"));
    gtk_box_append(GTK_BOX(therm_box), GTK_WIDGET(g_app.lbl_thermal));

    g_app.temp_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_hexpand(GTK_WIDGET(g_app.temp_bar), TRUE);
    gtk_box_append(GTK_BOX(therm_box), GTK_WIDGET(g_app.temp_bar));

    gtk_box_append(GTK_BOX(box), therm_frame);

    return box;
}

/* ----------------------------------------------------------------
 * Games page
 * ---------------------------------------------------------------- */

static void refresh_games(void) {
    if (!g_app.game_list || !g_app.proxy) return;

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(g_app.game_list));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(g_app.game_list, child);
        child = next;
    }

    for (int i = 0; i < g_app.proxy->nr_games; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_top(hbox, 8);
        gtk_widget_set_margin_bottom(hbox, 8);
        gtk_widget_set_margin_start(hbox, 8);
        gtk_widget_set_margin_end(hbox, 8);

        GtkWidget *icon = gtk_label_new("G");
        gtk_widget_add_css_class(icon, "heading");

        GtkWidget *name_lbl = gtk_label_new(g_app.proxy->game_comm[i]);
        gtk_widget_add_css_class(name_lbl, "heading");

        GtkWidget *detail_lbl = gtk_label_new(NULL);
        gchar detail[256];
        g_snprintf(detail, sizeof(detail), "PID: %d", g_app.proxy->game_pid[i]);
        gtk_label_set_text(GTK_LABEL(detail_lbl), detail);
        gtk_widget_set_css_classes(detail_lbl, (const gchar *[]){"caption", NULL});

        /* Use GtkDropDown instead of deprecated gtk_combo_box_string_new */
        const gchar *choices[] = {"balance", "powersave", "performance", NULL};
        GtkStringList *strlist = gtk_string_list_new(choices);
        GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(strlist), NULL);

        const gchar *cur = g_app.proxy->game_mode[i];
        if (cur) {
            /* Find index by string comparison */
            for (guint j = 0; j < 3; j++) {
                const gchar *s = gtk_string_list_get_string(strlist, j);
                if (g_strcmp0(s, cur) == 0) {
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), j);
                    break;
                }
            }
        }
        g_object_unref(strlist);

        GameModeTarget *target = g_new0(GameModeTarget, 1);
        target->pid = g_app.proxy->game_pid[i];
        target->app = g_strdup(g_app.proxy->game_comm[i]);
        g_signal_connect_data(dropdown, "notify::selected",
                              G_CALLBACK(on_set_game_mode), target,
                              game_mode_target_free, 0);

        gtk_box_append(GTK_BOX(hbox), icon);
        gtk_box_append(GTK_BOX(hbox), name_lbl);
        gtk_box_append(GTK_BOX(hbox), detail_lbl);
        gtk_box_append(GTK_BOX(hbox), dropdown);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
        gtk_list_box_insert(g_app.game_list, row, -1);
    }
}

static GtkWidget *create_games_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(title), "Detected Games");
    gtk_widget_set_css_classes(title, (const gchar *[]){"heading", NULL});
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *hint = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(hint), "Running game and game-like processes");
    gtk_widget_set_css_classes(hint, (const gchar *[]){"caption", NULL});
    gtk_box_append(GTK_BOX(box), hint);

    g_app.game_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_set_vexpand(GTK_WIDGET(g_app.game_list), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(g_app.game_list), TRUE);
    gtk_list_box_set_selection_mode(g_app.game_list, GTK_SELECTION_NONE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(g_app.game_list));

    return box;
}

/* ----------------------------------------------------------------
 * Settings page
 * ---------------------------------------------------------------- */

static GtkWidget *create_settings_page(void) {
    GtkWidget *page = adw_preferences_page_new();
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page),
                                   "Configuration");
    GtkWidget *group = adw_preferences_group_new();
    g_object_set(group, "title", "Daemon configuration", NULL);
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  "/etc/uperf-linux/config.json");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
        "Edit the JSON with administrator privileges, then reload it here.");
    GtkWidget *reload = gtk_button_new_with_label("Reload config");
    gtk_widget_set_valign(reload, GTK_ALIGN_CENTER);
    g_signal_connect(reload, "clicked",
                     G_CALLBACK(on_reload_config_clicked), NULL);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), reload);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}

/* ----------------------------------------------------------------
 * Logs page
 * ---------------------------------------------------------------- */

static GtkWidget *create_logs_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(title), "Log Output");
    gtk_widget_set_css_classes(title, (const gchar *[]){"heading", NULL});
    gtk_box_append(GTK_BOX(box), title);

    GtkTextBuffer *buf = gtk_text_buffer_new(NULL);
    g_app.log_view = GTK_TEXT_VIEW(gtk_text_view_new_with_buffer(buf));
    gtk_text_view_set_editable(g_app.log_view, FALSE);
    gtk_text_view_set_wrap_mode(g_app.log_view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(g_app.log_view, TRUE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(g_app.log_view));
    gtk_box_append(GTK_BOX(box), scroll);

    gtk_text_buffer_set_text(buf,
        "Click Refresh to read the latest uperf-linux.service journal.\n", -1);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_css_classes(refresh_btn, (const gchar *[]){"flat", NULL});
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_logs_clicked), NULL);
    gtk_box_append(GTK_BOX(hbox), refresh_btn);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear");
    gtk_widget_set_css_classes(clear_btn, (const gchar *[]){"flat", NULL});
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_logs_clicked), NULL);
    gtk_box_append(GTK_BOX(hbox), clear_btn);

    gtk_box_append(GTK_BOX(box), hbox);
    return box;
}

/* ----------------------------------------------------------------
 * Frequency override page
 * ---------------------------------------------------------------- */

static GtkWidget *create_frequency_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(title), "Manual Frequency Override");
    gtk_widget_set_css_classes(title, (const gchar *[]){"heading", NULL});
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *hint = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(hint), "Lock CPU/GPU frequency to a fixed value. Set to 0 to release back to auto-scaling.");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_css_classes(hint, (const gchar *[]){"caption", NULL});
    gtk_box_append(GTK_BOX(box), hint);

    /* Enable toggle */
    GtkWidget *toggle_row = adw_action_row_new();
    GtkWidget *toggle = gtk_switch_new();
    g_app.freq_toggle = GTK_SWITCH(toggle);
    g_object_set(toggle_row, "title", "Override Enabled", NULL);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(toggle_row), toggle);
    adw_action_row_add_suffix(ADW_ACTION_ROW(toggle_row), toggle);
    gtk_box_append(GTK_BOX(box), toggle_row);

    struct { const char *title; gdouble min; gdouble max; gdouble def; } clusters[] = {
        { "CPU Prime policy (kHz)", 595200, 2956800, 2956800 },
        { "CPU Performance policy (kHz)", 499200, 2803200, 2803200 },
        { "CPU Efficiency policy (kHz)", 307200, 2016000, 2016000 },
        { "GPU (Hz)", 220000000, 680000000, 680000000 },
    };

    for (int i = 0; i < 4; i++) {
        GtkWidget *frame = adw_clamp_new();
        gtk_widget_set_valign(frame, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(frame, TRUE);
        GtkWidget *slider_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_set_spacing(GTK_BOX(slider_box), 4);
        gtk_widget_set_margin_top(slider_box, 12);
        gtk_widget_set_margin_bottom(slider_box, 12);
        gtk_widget_set_margin_start(slider_box, 12);
        gtk_widget_set_margin_end(slider_box, 12);
        adw_clamp_set_child(ADW_CLAMP(frame), slider_box);

        GtkWidget *cluster_label = gtk_label_new(clusters[i].title);
        gtk_label_set_xalign(GTK_LABEL(cluster_label), 0.0f);
        gtk_widget_add_css_class(cluster_label, "heading");
        gtk_box_append(GTK_BOX(slider_box), cluster_label);

        GtkAdjustment *adj = gtk_adjustment_new(clusters[i].def,
            clusters[i].min, clusters[i].max, 50000, 100000, 0);
        g_app.freq_adjustments[i] = adj;
        GtkWidget *slider = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
        gtk_scale_set_draw_value(GTK_SCALE(slider), TRUE);
        gtk_widget_set_hexpand(slider, TRUE);
        gtk_box_append(GTK_BOX(slider_box), slider);

        gtk_box_append(GTK_BOX(box), frame);
    }

    /* Buttons */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(box), hbox);

    GtkWidget *apply_btn = gtk_button_new_with_label("Apply");
    gtk_widget_set_hexpand(apply_btn, TRUE);
    gtk_widget_set_css_classes(apply_btn, (const gchar *[]){"suggested-action", "pill", NULL});
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply_freq_clicked), NULL);
    gtk_box_append(GTK_BOX(hbox), apply_btn);

    GtkWidget *release_btn = gtk_button_new_with_label("Release All");
    gtk_widget_set_css_classes(release_btn, (const gchar *[]){"destructive-action", "pill", NULL});
    g_signal_connect(release_btn, "clicked", G_CALLBACK(on_release_freq_clicked), NULL);
    gtk_box_append(GTK_BOX(hbox), release_btn);

    return box;
}

/* ----------------------------------------------------------------
 * Activate handler (pure C, no lambdas)
 * ---------------------------------------------------------------- */

static void on_activate(GtkApplication *app, gpointer ud) {
    (void)ud;

    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "uperf-linux");
    gtk_window_set_default_size(GTK_WINDOW(window), 1080, 2400);

    /* Stack with pages */
    g_app.stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(g_app.stack,
        GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(g_app.stack, 200);

    gtk_stack_add_named(g_app.stack,
        create_dashboard_page(), "dashboard");
    gtk_stack_add_named(g_app.stack,
        create_games_page(), "games");
    gtk_stack_add_named(g_app.stack,
        create_settings_page(), "settings");
    gtk_stack_add_named(g_app.stack,
        create_logs_page(), "logs");
    gtk_stack_add_named(g_app.stack,
        create_frequency_page(), "frequency");

    /* Adwaita windows own their title area. Place navigation in a toolbar
     * view rather than using gtk_window_set_titlebar(), which is unsupported. */
    GtkWidget *toolbar_view = adw_toolbar_view_new();
    GtkWidget *tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(tab_bar, 6);
    gtk_widget_set_margin_bottom(tab_bar, 6);
    gtk_widget_set_margin_start(tab_bar, 6);
    gtk_widget_set_margin_end(tab_bar, 6);

    struct { const char *label; const char *page; } tabs[] = {
        { "Dashboard",  "dashboard" },
        { "Games",      "games" },
        { "Settings",   "settings" },
        { "Logs",       "logs" },
        { "Freq",       "frequency" },
    };
    for (int i = 0; i < 5; i++) {
        GtkWidget *btn = gtk_button_new_with_label(tabs[i].label);
        gtk_widget_set_css_classes(btn, (const gchar *[]){"flat", "pill", NULL});
        gtk_widget_set_hexpand(btn, TRUE);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_clicked),
            (gpointer)tabs[i].page);
        gtk_box_append(GTK_BOX(tab_bar), btn);
    }

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view),
                                 GTK_WIDGET(g_app.stack));
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view), tab_bar);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window),
                                       toolbar_view);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    adw_init();

    /* Create DBus proxy */
    DbusProxy *proxy = g_object_new(UPERF_TYPE_DBUS_PROXY, NULL);
    g_app.proxy = proxy;
    dbus_proxy_start_polling(proxy);

    /* Live update callbacks */
    dbus_proxy_set_mode_cb(proxy, G_CALLBACK(on_mode_changed), NULL);
    dbus_proxy_set_scene_cb(proxy, G_CALLBACK(on_scene_changed), NULL);
    dbus_proxy_set_stats_cb(proxy, G_CALLBACK(on_stats_updated), NULL);
    dbus_proxy_set_heavy_cb(proxy, G_CALLBACK(on_heavy_changed), NULL);
    dbus_proxy_set_thermal_cb(proxy, G_CALLBACK(on_thermal_changed), NULL);

    /* Application */
    GtkApplication *app = gtk_application_new("org.uperflinux.gui",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_clear_object(&app);
    g_clear_object(&proxy);
    g_clear_pointer(&g_app.freq_labels, g_free);
    g_clear_pointer(&g_app.load_labels, g_free);
    return status;
}

#include <gtk/gtk.h>
#include <libadwaita.h>
#include "dbus_proxy.h"

/* ----------------------------------------------------------------
 * App state
 * ---------------------------------------------------------------- */

typedef struct {
    DbusProxy *proxy;
    GtkListBox *game_list;
    GtkTextView  *log_view;
    GtkLabel     *lbl_mode;
    GtkLabel     *lbl_scene;
    GtkLabel     *lbl_heavy;
    GtkLabel     *lbl_max_temp;
    GtkLabel     *lbl_thermal;
    GtkProgressBar *load_bar;
    GtkProgressBar *temp_bar;
    GtkWidget **freq_labels;
    GtkWidget **load_labels;
} AppState;

static AppState g_app;

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
    for (int i = 0; i < g_app.proxy->nr_freqs && i < 8; i++) {
        if (g_app.freq_labels && g_app.freq_labels[i]) {
            gchar buf[32];
            g_snprintf(buf, sizeof(buf), "%.2f GHz", g_app.proxy->freqs[i] / 1000.0);
            gtk_label_set_text(GTK_LABEL(g_app.freq_labels[i]), buf);
        }
        if (g_app.load_labels && g_app.load_labels[i]) {
            gchar buf[32];
            g_snprintf(buf, sizeof(buf), "%.0f%%", g_app.proxy->loads[i]);
            gtk_label_set_text(GTK_LABEL(g_app.load_labels[i]), buf);
        }
    }
}

/* ----------------------------------------------------------------
 * Dashboard page
 * ---------------------------------------------------------------- */

static GtkWidget *create_dashboard_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(title), "uperf-linux");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "heading");
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *soc = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(soc), "SM8550 - Snapdragon 8 Gen 2");
    gtk_style_context_add_class(gtk_widget_get_style_context(soc), "caption");
    gtk_box_append(GTK_BOX(box), soc);

    /* Power Mode buttons */
    GtkWidget *mode_frame = adw_clamped_frame_new(NULL, ADW_CLAMP_SIZE_TIGHTEN, 180);
    gtk_frame_set_title(GTK_FRAME(mode_frame), "Power Mode");
    GtkWidget *mode_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(mode_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(mode_grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(mode_grid), 12);
    adw_clamped_frame_set_child(ADW_CLAMPED_FRAME(mode_frame), mode_grid);

    g_app.lbl_mode = GTK_LABEL(gtk_label_new("balance"));
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(g_app.lbl_mode)), "caption");
    gtk_label_set_justify(g_app.lbl_mode, GTK_JUSTIFY_CENTER);

    const char *modes[] = {"balance", "powersave", "performance"};
    const char *icons[] = {"B", "P", "X"};
    const char *labels[] = {"Balance", "Powersave", "Performance"};

    for (int i = 0; i < 3; i++) {
        GtkWidget *btn = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), "flat");
        gtk_widget_set_hexpand(btn, TRUE);
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *icon = gtk_label_new(icons[i]);
        gtk_label_set_font_size(GTK_LABEL(icon), "36");
        GtkWidget *lbl = gtk_label_new(labels[i]);
        gtk_label_set_font_weight(GTK_LABEL(lbl), GTK_FONT_WEIGHT_BOLD);
        gtk_box_append(GTK_BOX(vbox), icon);
        gtk_box_append(GTK_BOX(vbox), lbl);
        gtk_button_set_child(GTK_BUTTON(btn), vbox);
        g_signal_connect(btn, "clicked", G_CALLBACK(
            [](GtkWidget *w, gpointer ud) {
                dbus_proxy_set_mode(g_app.proxy, (const gchar *)ud);
                refresh_display();
            }, (gpointer)modes[i]));
        gtk_grid_attach(GTK_GRID(mode_grid), btn, i, 0, 1, 1);
    }
    gtk_grid_attach(GTK_GRID(mode_grid), GTK_WIDGET(g_app.lbl_mode), 0, 1, 3, 1);
    gtk_box_append(GTK_BOX(box), mode_frame);

    /* Scene badge */
    g_app.lbl_scene = GTK_LABEL(gtk_label_new("IDLE"));
    gtk_label_set_font_weight(g_app.lbl_scene, GTK_FONT_WEIGHT_BOLD);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(g_app.lbl_scene)), "heading");
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(g_app.lbl_scene));

    /* Heavy load */
    g_app.lbl_heavy = GTK_LABEL(gtk_label_new("Normal"));
    gtk_label_set_font_weight(g_app.lbl_heavy, GTK_FONT_WEIGHT_BOLD);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(g_app.lbl_heavy));

    /* CPU frequency grid */
    GtkWidget *freq_frame = adw_clamped_frame_new(NULL, ADW_CLAMP_SIZE_TIGHTEN, 400);
    gtk_frame_set_title(GTK_FRAME(freq_frame), "CPU Frequency");
    GtkWidget *freq_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(freq_grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(freq_grid), 4);
    gtk_container_set_border_width(GTK_CONTAINER(freq_grid), 8);
    adw_clamped_frame_set_child(ADW_CLAMPED_FRAME(freq_frame), freq_grid);

    g_app.freq_labels = g_malloc0(8 * sizeof(GtkWidget *));
    g_app.load_labels = g_malloc0(8 * sizeof(GtkWidget *));

    for (int i = 0; i < 8; i++) {
        int col = i % 4;
        int row = i / 4;
        GtkWidget *cpu_lbl = gtk_label_new(NULL);
        gchar cpu_name[16];
        g_snprintf(cpu_name, sizeof(cpu_name), "CPU%d", i);
        gtk_label_set_text(GTK_LABEL(cpu_lbl), cpu_name);
        gtk_style_context_add_class(gtk_widget_get_style_context(cpu_lbl), "caption");
        GtkWidget *freq_lbl = gtk_label_new("--");
        g_app.freq_labels[i] = freq_lbl;
        GtkWidget *load_lbl = gtk_label_new("--");
        g_app.load_labels[i] = load_lbl;
        gtk_style_context_add_class(gtk_widget_get_style_context(load_lbl), "caption");

        GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(cell), cpu_lbl);
        gtk_box_append(GTK_BOX(cell), freq_lbl);
        gtk_box_append(GTK_BOX(cell), load_lbl);
        gtk_grid_attach(GTK_GRID(freq_grid), cell, col, row, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), freq_frame);

    /* Load bar */
    g_app.load_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_hexpand(GTK_WIDGET(g_app.load_bar), TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(g_app.load_bar));
    gtk_progress_bar_set_fraction(g_app.load_bar, 0.3);

    /* Thermal section */
    GtkWidget *therm_frame = adw_clamped_frame_new(NULL, ADW_CLAMP_SIZE_TIGHTEN, 200);
    gtk_frame_set_title(GTK_FRAME(therm_frame), "Thermal");
    GtkWidget *therm_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(therm_box), 12);
    adw_clamped_frame_set_child(ADW_CLAMPED_FRAME(therm_frame), therm_box);

    g_app.lbl_max_temp = GTK_LABEL(gtk_label_new("--C"));
    gtk_label_set_font_weight(g_app.lbl_max_temp, GTK_FONT_WEIGHT_BOLD);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(g_app.lbl_max_temp)), "heading");
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
    if (!g_app.game_list) return;

    GtkWidget *child = gtk_list_box_get_first_child(g_app.game_list);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(g_app.game_list, child);
        child = next;
    }

    for (int i = 0; i < g_app.proxy->nr_games; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(hbox), 8);

        GtkWidget *icon = gtk_label_new("G");
        gtk_label_set_font_size(GTK_LABEL(icon), "24");

        GtkWidget *name_lbl = gtk_label_new(g_app.proxy->game_comm[i]);
        gtk_label_set_font_weight(GTK_LABEL(name_lbl), GTK_FONT_WEIGHT_BOLD);

        GtkWidget *detail_lbl = gtk_label_new(NULL);
        gchar detail[256];
        g_snprintf(detail, sizeof(detail), "PID: %d", g_app.proxy->game_pid[i]);
        gtk_label_set_text(GTK_LABEL(detail_lbl), detail);
        gtk_style_context_add_class(gtk_widget_get_style_context(detail_lbl), "caption");

        GtkWidget *combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "balance", "Balance");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "powersave", "Powersave");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "performance", "Performance");

        const char *cur = g_app.proxy->game_mode[i];
        if (cur)
            gtk_combo_box_set_active_string(GTK_COMBO_BOX(combo), cur);

        g_signal_connect(combo, "changed", G_CALLBACK(
            [](GtkComboBox *cb, gpointer ud) {
                const char *modes[] = {"balance", "powersave", "performance"};
                int idx = gtk_combo_box_get_active(cb);
                if (idx >= 0 && idx < 3)
                    dbus_proxy_set_game_mode(g_app.proxy, 0, "unknown", modes[idx]);
            }, NULL));

        gtk_box_append(GTK_BOX(hbox), icon);
        gtk_box_append(GTK_BOX(hbox), name_lbl);
        gtk_box_append(GTK_BOX(hbox), detail_lbl);
        gtk_box_pack_end(GTK_BOX(hbox), combo, FALSE, FALSE, 0);

        gtk_list_box_row_set_child(row, hbox);
        gtk_list_box_insert(g_app.game_list, row, -1);
    }
}

static GtkWidget *create_games_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(title), "Detected Games");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "heading");
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *hint = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(hint), "Running game and game-like processes");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "caption");
    gtk_box_append(GTK_BOX(box), hint);

    g_app.game_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(g_app.game_list)), "frame");
    gtk_list_box_set_selection_mode(g_app.game_list, GTK_SELECTION_NONE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(g_app.game_list));

    return box;
}

/* ----------------------------------------------------------------
 * Settings page
 * ---------------------------------------------------------------- */

static GtkWidget *create_settings_page(void) {
    GtkWidget *prefs = adw_preferences_window_new();

    /* Load Detection group */
    GtkWidget *grp1 = adw_preferences_group_new("Load Detection");
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(prefs), grp1);

    struct { const char *title; const char *subtitle; const char *init; } load_rows[] = {
        { "HeavyLoad Threshold", "System load % above which boost mode activates", "60" },
        { "Idle Load Threshold", "Load % below which boost mode deactivates", "20" },
        { "Sample Time", "Interval between /proc/stat samples (ms)", "10" },
        { "Burst Slack", "Cooldown before re-entering boost (ms)", "3000" },
    };
    for (int i = 0; i < 4; i++) {
        GtkWidget *row = adw_entry_row_new();
        adw_preferences_group_add(grp1, row);
        adw_entry_row_set_title(ADW_ENTRY_ROW(row), load_rows[i].title);
        adw_entry_row_set_subtitle(ADW_ENTRY_ROW(row), load_rows[i].subtitle);
        adw_entry_row_set_numeric(ADW_ENTRY_ROW(row), TRUE);
        GtkEntry *e = GTK_ENTRY(adw_entry_row_get_entry(ADW_ENTRY_ROW(row)));
        gtk_entry_set_text(e, load_rows[i].init);
        gtk_entry_set_width_chars(e, 6);
    }

    /* Response Timing group */
    GtkWidget *grp2 = adw_preferences_group_new("Response Timing");
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(prefs), grp2);

    struct { const char *title; const char *subtitle; const char *init; } resp_rows[] = {
        { "Latency Time", "Response delay before boosting frequency (ms)", "200" },
        { "Margin", "Headroom multiplier for frequency selection (%)", "25" },
        { "Burst", "Additional burst intensity modifier (%)", "0" },
    };
    for (int i = 0; i < 3; i++) {
        GtkWidget *row = adw_entry_row_new();
        adw_preferences_group_add(grp2, row);
        adw_entry_row_set_title(ADW_ENTRY_ROW(row), resp_rows[i].title);
        adw_entry_row_set_subtitle(ADW_ENTRY_ROW(row), resp_rows[i].subtitle);
        adw_entry_row_set_numeric(ADW_ENTRY_ROW(row), TRUE);
        GtkEntry *e = GTK_ENTRY(adw_entry_row_get_entry(ADW_ENTRY_ROW(row)));
        gtk_entry_set_text(e, resp_rows[i].init);
        gtk_entry_set_width_chars(e, 6);
    }

    /* Power Budgets group */
    GtkWidget *grp3 = adw_preferences_group_new("Power Budgets");
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(prefs), grp3);

    struct { const char *title; const char *subtitle; const char *init; } pwr_rows[] = {
        { "Slow Limit", "Power budget for slow response mode (Watts)", "3.0" },
        { "Fast Limit", "Power budget for fast response mode (Watts)", "6.0" },
        { "Fast Limit Capacity", "Maximum capacity cap during burst", "10.0" },
    };
    for (int i = 0; i < 3; i++) {
        GtkWidget *row = adw_entry_row_new();
        adw_preferences_group_add(grp3, row);
        adw_entry_row_set_title(ADW_ENTRY_ROW(row), pwr_rows[i].title);
        adw_entry_row_set_subtitle(ADW_ENTRY_ROW(row), pwr_rows[i].subtitle);
        adw_entry_row_set_numeric(ADW_ENTRY_ROW(row), TRUE);
        GtkEntry *e = GTK_ENTRY(adw_entry_row_get_entry(ADW_ENTRY_ROW(row)));
        gtk_entry_set_text(e, pwr_rows[i].init);
        gtk_entry_set_width_chars(e, 6);
    }

    /* Thermal Thresholds group */
    GtkWidget *grp4 = adw_preferences_group_new("Thermal Thresholds");
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(prefs), grp4);

    struct { const char *title; const char *subtitle; const char *init; } thr_rows[] = {
        { "Warn Temp", "Temperature at which warnings are logged (C)", "70" },
        { "Throttle Temp", "Temperature at which CPU/GPU frequency is reduced (C)", "80" },
        { "Critical Temp", "Emergency threshold (C)", "95" },
        { "Recovery Temp", "Temperature below which normal operation resumes (C)", "75" },
    };
    for (int i = 0; i < 4; i++) {
        GtkWidget *row = adw_entry_row_new();
        adw_preferences_group_add(grp4, row);
        adw_entry_row_set_title(ADW_ENTRY_ROW(row), thr_rows[i].title);
        adw_entry_row_set_subtitle(ADW_ENTRY_ROW(row), thr_rows[i].subtitle);
        adw_entry_row_set_numeric(ADW_ENTRY_ROW(row), TRUE);
        GtkEntry *e = GTK_ENTRY(adw_entry_row_get_entry(ADW_ENTRY_ROW(row)));
        gtk_entry_set_text(e, thr_rows[i].init);
        gtk_entry_set_width_chars(e, 6);
    }

    /* Apply button */
    GtkWidget *apply_btn = gtk_button_new_with_label("Apply Settings");
    gtk_style_context_add_class(gtk_widget_get_style_context(apply_btn), "suggested-action");
    gtk_widget_set_hexpand(apply_btn, TRUE);
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(
        [](GtkWidget *w, gpointer ud) {
            g_debug("Apply settings clicked");
        }, NULL));
    adw_preferences_window_add_bottom(ADW_PREFERENCES_WINDOW(prefs), apply_btn);

    return prefs;
}

/* ----------------------------------------------------------------
 * Logs page
 * ---------------------------------------------------------------- */

static GtkWidget *create_logs_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(title), "Log Output");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "heading");
    gtk_box_append(GTK_BOX(box), title);

    GtkTextBuffer *buf = gtk_text_buffer_new(NULL);
    g_app.log_view = GTK_TEXT_VIEW(gtk_text_view_new_with_buffer(buf));
    gtk_text_view_set_editable(g_app.log_view, FALSE);
    gtk_text_view_set_wrap_mode(g_app.log_view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_font_family(g_app.log_view, "monospace");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(g_app.log_view));
    gtk_box_append(GTK_BOX(box), scroll);

    gtk_text_buffer_set_text(buf,
        "[info] uperf-linux daemon started\n"
        "[info] Config loaded: sm8550 by uperf-linux\n"
        "[info] DBus manager created on system bus\n"
        "[info] CgroupManager initialized\n"
        "[info] HeavyLoadDetector created: nr_cpus=8\n"
        "[info] InputMonitor: no touchscreen devices found\n"
        "[info] === uperf-linux ready ===\n", -1);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_button_set_relief(refresh_btn, GTK_RELIEF_NONE);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(
        [](GtkWidget *w, gpointer ud) {
            GtkTextBuffer *buf = gtk_text_view_get_buffer(g_app.log_view);
            gtk_text_buffer_set_text(buf,
                "[info] Logs refreshed\n"
                "[info] (Production: reads from journald via DBus)\n", -1);
        }, NULL));
    gtk_box_append(GTK_BOX(hbox), refresh_btn);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear");
    gtk_button_set_relief(clear_btn, GTK_RELIEF_NONE);
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(
        [](GtkWidget *w, gpointer ud) {
            GtkTextBuffer *buf = gtk_text_view_get_buffer(g_app.log_view);
            gtk_text_buffer_set_text(buf, "", -1);
        }, NULL));
    gtk_box_append(GTK_BOX(hbox), clear_btn);

    gtk_box_append(GTK_BOX(box), hbox);
    return box;
}

/* ----------------------------------------------------------------
 * Frequency override page
 * ---------------------------------------------------------------- */

static GtkWidget *create_frequency_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(title), "Manual Frequency Override");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "heading");
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *hint = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(hint), "Lock CPU/GPU frequency to a fixed value. Set to 0 to release back to auto-scaling.");
    gtk_label_set_wrap(hint, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "caption");
    gtk_box_append(GTK_BOX(box), hint);

    /* Enable toggle */
    GtkWidget *toggle_row = adw_action_row_new();
    GtkWidget *toggle = gtk_switch_new();
    adw_action_row_set_title(ADW_ACTION_ROW(toggle_row), "Override Enabled");
    gtk_widget_set_halign(toggle, GTK_ALIGN_END);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(toggle_row), toggle);
    gtk_box_append(GTK_BOX(box), toggle_row);

    struct { const char *title; gdouble min; gdouble max; gdouble def; } clusters[] = {
        { "CPU Prime (cpu0)", 600000, 3200000, 2400000 },
        { "CPU Performance (cpu1-2)", 500000, 2800000, 2200000 },
        { "CPU Efficiency (cpu3-7)", 300000, 2000000, 1600000 },
        { "GPU", 300000000, 1000000000, 600000000 },
    };

    for (int i = 0; i < 4; i++) {
        GtkWidget *frame = adw_clamped_frame_new(NULL, ADW_CLAMP_SIZE_TIGHTEN, 80);
        gtk_frame_set_title(GTK_FRAME(frame), clusters[i].title);
        GtkWidget *slider_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(slider_box), 12);
        adw_clamped_frame_set_child(ADW_CLAMPED_FRAME(frame), slider_box);

        GtkAdjustment *adj = gtk_adjustment_new(clusters[i].def,
            clusters[i].min, clusters[i].max, 50000, 100000, 0);
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
    gtk_style_context_add_class(gtk_widget_get_style_context(apply_btn), "suggested-action");
    gtk_style_context_add_class(gtk_widget_get_style_context(apply_btn), "pill");
    gtk_widget_set_hexpand(apply_btn, TRUE);
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(
        [](GtkWidget *w, gpointer ud) {
            g_debug("Apply freq override (simplified - needs slider refs)");
        }, NULL));
    gtk_box_append(GTK_BOX(hbox), apply_btn);

    GtkWidget *release_btn = gtk_button_new_with_label("Release All");
    gtk_style_context_add_class(gtk_widget_get_style_context(release_btn), "destructive-action");
    gtk_style_context_add_class(gtk_widget_get_style_context(release_btn), "pill");
    g_signal_connect(release_btn, "clicked", G_CALLBACK(
        [](GtkWidget *w, gpointer ud) {
            if (!g_app.proxy) return;
            dbus_proxy_release_freq_override(g_app.proxy);
        }, NULL));
    gtk_box_append(GTK_BOX(hbox), release_btn);

    return box;
}

/* ----------------------------------------------------------------
 * Main window
 * ---------------------------------------------------------------- */

static void on_tab_clicked(GtkButton *btn, gpointer page_name) {
    gtk_stack_set_visible_child_name(
        GTK_STACK(g_app.switcher), (const gchar *)page_name);
}

int main(int argc, char **argv) {
    adw_init();

    /* Create DBus proxy */
    DbusProxy *proxy = g_object_new(UPERF_TYPE_DBUS_PROXY, NULL);
    g_app.proxy = proxy;
    dbus_proxy_start_polling(proxy);

    /* Live update callbacks */
    dbus_proxy_set_mode_cb(proxy, G_CALLBACK(
        [](DbusProxy *p, gchar *m, gpointer ud) { (void)p;(void)m; refresh_display(); }, NULL));
    dbus_proxy_set_scene_cb(proxy, G_CALLBACK(
        [](DbusProxy *p, gchar *m, gpointer ud) { (void)p;(void)m; refresh_display(); }, NULL));
    dbus_proxy_set_stats_cb(proxy, G_CALLBACK(
        [](DbusProxy *p, gpointer ud) { (void)p; refresh_display(); }, NULL));
    dbus_proxy_set_heavy_cb(proxy, G_CALLBACK(
        [](DbusProxy *p, gboolean h, gpointer ud) { (void)p; (void)h; refresh_display(); }, NULL));
    dbus_proxy_set_thermal_cb(proxy, G_CALLBACK(
        [](DbusProxy *p, gint32 t, gpointer ud) { (void)p; (void)t; refresh_display(); }, NULL));

    /* Application */
    GtkApplication *app = gtk_application_new("org.uperflinux.gui",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(
        [](GtkApplication *a, gpointer ud) {
            (void)a; (void)ud;

            GtkWidget *window = adw_application_window_new(ADW_APPLICATION(a));
            gtk_window_set_title(GTK_WINDOW(window), "uperf-linux");
            gtk_window_set_default_size(GTK_WINDOW(window), 1080, 2400);

            /* Stack with pages */
            GtkWidget *stack = gtk_stack_new();
            gtk_stack_set_transition_type(GTK_STACK(stack),
                GTK_STACK_TRANSITION_TYPE_CROSSFADE);
            gtk_stack_set_transition_duration(GTK_STACK(stack), 200);
            g_app.switcher = stack;

            gtk_stack_add_named(stack, create_dashboard_page(), "dashboard");
            gtk_stack_add_named(stack, create_games_page(), "games");
            gtk_stack_add_named(stack, create_settings_page(), "settings");
            gtk_stack_add_named(stack, create_logs_page(), "logs");
            gtk_stack_add_named(stack, create_frequency_page(), "frequency");

            /* Bottom tab bar */
            GtkWidget *tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_style_context_add_class(gtk_widget_get_style_context(tab_bar), "inline-toolbar");

            struct { const char *label; const char *page; } tabs[] = {
                { "Dashboard",  "dashboard" },
                { "Games",      "games" },
                { "Settings",   "settings" },
                { "Logs",       "logs" },
                { "Freq",       "frequency" },
            };
            for (int i = 0; i < 5; i++) {
                GtkWidget *btn = gtk_button_new_with_label(tabs[i].label);
                gtk_style_context_add_class(gtk_widget_get_style_context(btn), "flat");
                gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
                gtk_widget_set_hexpand(btn, TRUE);
                gtk_box_pack_end(GTK_BOX(tab_bar), btn, TRUE, TRUE, 0);
                g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_clicked),
                    (gpointer)tabs[i].page);
            }

            /* Layout: stack + bottom bar */
            GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_box_append(GTK_BOX(main_box), stack);
            gtk_box_append(GTK_BOX(main_box), tab_bar);

            adw_window_set_content(ADW_WINDOW(window), main_box);
            gtk_window_present(GTK_WINDOW(window));
        }, NULL));

    return g_application_run(G_APPLICATION(app), argc, argv);
}

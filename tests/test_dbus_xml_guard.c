/* P1.8: guard against D-Bus interface drift.
 *
 * The daemon registers the embedded introspection XML in src/dbus_interface.c.
 * A copy lives in config/dbus-interface.xml, installed for documentation and
 * for clients that read it instead of introspecting a live bus.  The two must
 * describe byte-for-byte the same interface.  This test parses both with GLib
 * and asserts every interface, property, method (with full arg signatures),
 * and signal matches.  If they drift, this test fails at build/CI time rather
 * than silently shipping a lying interface file. */

#include "dbus_interface.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static gint g_ascii_strcasecmp_ptr(gconstpointer a, gconstpointer b);

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); \
                   fprintf(stderr, "\n"); failures++; } \
} while (0)

static char *read_file(const char *path) {
    char *contents = NULL;
    GError *err = NULL;
    if (!g_file_get_contents(path, &contents, NULL, &err)) {
        fprintf(stderr, "FAIL: cannot read %s: %s\n", path,
                err ? err->message : "?");
        if (err) g_error_free(err);
        exit(2);
    }
    return contents;
}

static GDBusNodeInfo *parse(const char *xml, const char *label) {
    GError *err = NULL;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, &err);
    if (!node) {
        fprintf(stderr, "FAIL: %s is not valid introspection XML: %s\n",
                label, err ? err->message : "?");
        if (err) g_error_free(err);
        exit(2);
    }
    return node;
}

/* Compose a stable signature string for one arg: "direction:name:type". */
static void append_arg(GString *out, const char *dir, GDBusArgInfo *arg) {
    g_string_append_printf(out, "    arg %s %s:%s\n", dir,
                           arg->name ? arg->name : "", arg->signature);
}

/* Serialize an interface into a canonical, order-independent multiline string
 * covering every property, method arg, and signal arg. */
static char *canonical_interface(GDBusInterfaceInfo *iface) {
    GString *out = g_string_new(NULL);
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);

    if (iface->properties) {
        for (int i = 0; iface->properties[i]; i++) {
            GDBusPropertyInfo *p = iface->properties[i];
            g_ptr_array_add(lines, g_strdup_printf(
                "property %s:%s:flags=%d", p->name, p->signature, p->flags));
        }
    }
    if (iface->methods) {
        for (int i = 0; iface->methods[i]; i++) {
            GDBusMethodInfo *m = iface->methods[i];
            GString *sig = g_string_new(NULL);
            g_string_append_printf(sig, "method %s\n", m->name);
            if (m->in_args)
                for (int a = 0; m->in_args[a]; a++)
                    append_arg(sig, "in", m->in_args[a]);
            if (m->out_args)
                for (int a = 0; m->out_args[a]; a++)
                    append_arg(sig, "out", m->out_args[a]);
            g_ptr_array_add(lines, g_string_free(sig, FALSE));
        }
    }
    if (iface->signals) {
        for (int i = 0; iface->signals[i]; i++) {
            GDBusSignalInfo *s = iface->signals[i];
            GString *sig = g_string_new(NULL);
            g_string_append_printf(sig, "signal %s\n", s->name);
            if (s->args)
                for (int a = 0; s->args[a]; a++)
                    append_arg(sig, "arg", s->args[a]);
            g_ptr_array_add(lines, g_string_free(sig, FALSE));
        }
    }

    /* Sort so member declaration order does not matter. */
    g_ptr_array_sort(lines, (GCompareFunc)g_ascii_strcasecmp_ptr);
    for (guint i = 0; i < lines->len; i++)
        g_string_append_printf(out, "%s\n", (char *)lines->pdata[i]);
    g_ptr_array_free(lines, TRUE);
    return g_string_free(out, FALSE);
}

/* g_ptr_array_sort passes pointers-to-elements. */
static gint g_ascii_strcasecmp_ptr(gconstpointer a, gconstpointer b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return g_strcmp0(sa, sb);
}

int main(void) {
    const char *embedded_xml = dbus_manager_introspection_xml();
    char *file_xml = read_file(CONFIG_DBUS_XML);

    GDBusNodeInfo *embedded = parse(embedded_xml, "embedded introspection_xml");
    GDBusNodeInfo *file = parse(file_xml, CONFIG_DBUS_XML);

    /* Count interfaces on each side. */
    int embedded_count = 0, file_count = 0;
    for (int i = 0; embedded->interfaces && embedded->interfaces[i]; i++)
        embedded_count++;
    for (int i = 0; file->interfaces && file->interfaces[i]; i++)
        file_count++;
    CHECK(embedded_count == file_count,
          "interface count differs: embedded=%d file=%d",
          embedded_count, file_count);

    /* Every embedded interface must exist in the file with identical members. */
    for (int i = 0; embedded->interfaces && embedded->interfaces[i]; i++) {
        GDBusInterfaceInfo *e = embedded->interfaces[i];
        GDBusInterfaceInfo *f =
            g_dbus_node_info_lookup_interface(file, e->name);
        CHECK(f != NULL, "interface %s missing from %s", e->name,
              CONFIG_DBUS_XML);
        if (!f) continue;
        char *ec = canonical_interface(e);
        char *fc = canonical_interface(f);
        if (g_strcmp0(ec, fc) != 0) {
            failures++;
            fprintf(stderr, "FAIL: interface %s differs between the embedded "
                    "XML and %s\n--- embedded ---\n%s\n--- file ---\n%s\n",
                    e->name, CONFIG_DBUS_XML, ec, fc);
        }
        g_free(ec);
        g_free(fc);
    }

    g_dbus_node_info_unref(embedded);
    g_dbus_node_info_unref(file);
    g_free(file_xml);

    if (failures == 0)
        puts("dbus_xml_guard: embedded introspection matches "
             "config/dbus-interface.xml");
    return failures == 0 ? 0 : 1;
}

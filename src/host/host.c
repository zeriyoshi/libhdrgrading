/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "table.h"

static const hdrg_plugin_api_t hdrg_api;

static inline const char *hdrg_text_str(const hdrg_text_t *text)
{
    return (text->data != NULL) ? text->data : "";
}

static inline bool hdrg_text_equal(const hdrg_text_t *text, const char *s, size_t len)
{
    return text->size == len && (len == 0 || memcmp(text->data, s, len) == 0);
}

/* the table is the first member of its host */
static inline const hdrg_allocator_t *hdrg_api_alloc(const hdrg_plugin_api_t *api)
{
    return &((const hdrg_plugin_host_t *)(const void *)api)->alloc;
}

static bool hdrg_text_check(const char *s, size_t len, size_t max, bool required, uint8_t kind)
{
    if ((s == NULL && len != 0) || len > max || (required && len == 0)) {
        return false;
    }

    if (len == 0) {
        return true;
    }

    switch (kind) {
    case HDRG_KIND_LINE:
        return hdrg_valid_text(s, len, 0);
    case HDRG_KIND_LINES:
        return hdrg_valid_text(s, len, HDRG_TEXT_NEWLINE);
    case HDRG_KIND_BLOCK:
        return hdrg_valid_text(s, len, HDRG_TEXT_NEWLINE | HDRG_TEXT_TAB);
    case HDRG_KIND_ID:
        return hdrg_valid_id(s, len);
    case HDRG_KIND_VERSION:
        return hdrg_valid_version(s, len);
    case HDRG_KIND_KEY:
        return hdrg_valid_key(s, len);
    case HDRG_KIND_SPDX:
        return hdrg_valid_spdx(s, len);
    case HDRG_KIND_REF:
        return hdrg_valid_license_ref(s, len);
    default:
        return false;
    }
}

/* copies already checked bytes; the previous value is released */
static hdrg_status_t hdrg_text_copy(const hdrg_allocator_t *alloc, hdrg_text_t *out, const char *s,
                                    size_t len)
{
    char *copy;

    copy = (char *)hdrg_mem_alloc(alloc, len + 1);
    if (copy == NULL) {
        return HDRG_ERR_NOMEM;
    }

    if (len > 0) {
        memcpy(copy, s, len);
    }

    copy[len] = '\0';

    hdrg_mem_free(alloc, out->data);

    out->data = copy;
    out->size = len;

    return HDRG_OK;
}

static void hdrg_text_free(const hdrg_allocator_t *alloc, hdrg_text_t *text)
{
    hdrg_mem_free(alloc, text->data);

    text->data = NULL;
    text->size = 0;
}

static hdrg_status_t hdrg_plugin_check(const hdrg_plugin_t *plugin)
{
    if (plugin == NULL || plugin->magic != HDRG_PLUGIN_MAGIC) {
        return HDRG_ERR_INVALID_ARG;
    }

    return plugin->frozen ? HDRG_ERR_INVALID_STATE : HDRG_OK;
}

static hdrg_status_t hdrg_class_check(const hdrg_class_t *cls)
{
    hdrg_status_t status;

    if (cls == NULL || cls->magic != HDRG_CLASS_MAGIC) {
        return HDRG_ERR_INVALID_ARG;
    }

    status = hdrg_plugin_check(cls->plugin);
    if (status != HDRG_OK) {
        return status;
    }

    return cls->registered ? HDRG_ERR_INVALID_STATE : HDRG_OK;
}

/*
 * a class's setter of a kind: it makes the class one (HDRG_CLASS_FILTER /
 * CODEC), of another HDRG_ERR_INVALID_ARG
 */
static hdrg_status_t hdrg_class_check_kind(hdrg_class_t *cls, uint8_t kind)
{
    hdrg_status_t status;

    status = hdrg_class_check(cls);
    if (status != HDRG_OK) {
        return status;
    }

    if (cls->kind != 0 && cls->kind != kind) {
        hdrg_host_set_error(cls->plugin->host, "a class is a filter or a codec, not both", NULL, 0);

        return HDRG_ERR_INVALID_ARG;
    }

    cls->kind = kind;

    return HDRG_OK;
}

/* reports a rejected argument of a registration call */
static hdrg_status_t hdrg_plugin_fail(hdrg_plugin_t *plugin, hdrg_status_t status,
                                      const char *message)
{
    hdrg_host_set_error(plugin->host, message, NULL, 0);

    return status;
}

static void hdrg_plugin_unload(hdrg_plugin_t *plugin)
{
    if (plugin->deinit != NULL) {
        plugin->deinit(plugin->user, &plugin->host->api);
    }

    if (plugin->module != NULL) {
        hdrg_module_close(plugin->module);
    }

    plugin->deinit = NULL;
    plugin->module = NULL;
}

/* freezes the plugin as unusable, keeps it listed with the reason */
static hdrg_status_t hdrg_plugin_reject(hdrg_plugin_t *plugin, uint32_t reason, const char *message,
                                        hdrg_status_t status)
{
    plugin->frozen = true;
    plugin->accepted = false;
    plugin->status = HDRG_PLUGIN_UNAVAILABLE;
    plugin->reason = reason;
    (void)hdrg_text_copy(&plugin->host->alloc, &plugin->message, message, strlen(message));

    hdrg_host_set_error(plugin->host, message, hdrg_text_str(&plugin->id), plugin->id.size);
    hdrg_plugin_unload(plugin);

    return status;
}

static void hdrg_plugin_free(const hdrg_allocator_t *alloc, hdrg_plugin_t *plugin)
{
    hdrg_class_t *cls;
    uint32_t i, j;

    for (i = 0; i < plugin->class_count; i++) {
        cls = plugin->classes[i];
        for (j = 0; j < cls->option_count; j++) {
            hdrg_text_free(alloc, &cls->options[j].key);
            hdrg_text_free(alloc, &cls->options[j].description);
            hdrg_text_free(alloc, &cls->options[j].def_text);
        }

        hdrg_text_free(alloc, &cls->id);
        hdrg_text_free(alloc, &cls->extensions);
        hdrg_mem_free(alloc, cls->options);
        hdrg_mem_free(alloc, cls);
    }

    for (i = 0; i < plugin->component_count; i++) {
        hdrg_text_free(alloc, &plugin->components[i].name);
        hdrg_text_free(alloc, &plugin->components[i].version);
        hdrg_text_free(alloc, &plugin->components[i].license);
        hdrg_text_free(alloc, &plugin->components[i].copyright);
        hdrg_text_free(alloc, &plugin->components[i].notice);
    }

    for (i = 0; i < plugin->text_count; i++) {
        hdrg_text_free(alloc, &plugin->texts[i].id);
        hdrg_text_free(alloc, &plugin->texts[i].text);
    }

    hdrg_text_free(alloc, &plugin->id);
    hdrg_text_free(alloc, &plugin->name);
    hdrg_text_free(alloc, &plugin->version);
    hdrg_text_free(alloc, &plugin->vendor);
    hdrg_text_free(alloc, &plugin->license);
    hdrg_text_free(alloc, &plugin->copyright);
    hdrg_text_free(alloc, &plugin->message);
    hdrg_mem_free(alloc, plugin->classes);
    hdrg_mem_free(alloc, plugin->components);
    hdrg_mem_free(alloc, plugin->texts);
    hdrg_mem_free(alloc, plugin);
}

/* every LicenseRef- an expression uses has its text */
static bool hdrg_plugin_refs_known(const hdrg_plugin_t *plugin, const hdrg_text_t *expression)
{
    const char *ref;
    uint32_t i;
    size_t pos, ref_len;
    bool found;

    pos = 0;
    while (hdrg_spdx_next_ref(expression->data, expression->size, &pos, &ref, &ref_len)) {
        found = false;
        for (i = 0; i < plugin->text_count && !found; i++) {
            found = hdrg_text_equal(&plugin->texts[i].id, ref, ref_len);
        }

        if (!found) {
            return false;
        }
    }

    return true;
}

/* the report is complete and consistent; message = why not */
static bool hdrg_plugin_complete(const hdrg_plugin_t *plugin, const char **message)
{
    const hdrg_plugin_host_t *host = plugin->host;
    uint32_t i;

    if (!plugin->has_info || !plugin->has_license || !plugin->has_status) {
        *message = "plugin report incomplete (info, license and status are required)";

        return false;
    }

    for (i = 0; i < plugin->class_count; i++) {
        if (!plugin->classes[i]->registered) {
            *message = "plugin created a filter class it did not register";

            return false;
        }
    }

    if (!hdrg_plugin_refs_known(plugin, &plugin->license)) {
        *message = "plugin license uses a LicenseRef- without its text";

        return false;
    }

    for (i = 0; i < plugin->component_count; i++) {
        if (!hdrg_plugin_refs_known(plugin, &plugin->components[i].license)) {
            *message = "plugin component license uses a LicenseRef- without its text";

            return false;
        }
    }

    for (i = 0; i < host->plugin_count; i++) {
        if (host->plugins[i] != plugin && host->plugins[i]->accepted &&
            hdrg_text_equal(&host->plugins[i]->id, plugin->id.data, plugin->id.size)) {
            *message = "a plugin with the same id is already registered";

            return false;
        }
    }

    return true;
}

static const hdrg_option_spec_t *hdrg_class_option(const hdrg_class_t *cls, const char *key,
                                                   size_t key_len)
{
    uint32_t i;

    for (i = 0; i < cls->option_count; i++) {
        if (hdrg_text_equal(&cls->options[i].key, key, key_len)) {
            return &cls->options[i];
        }
    }

    return NULL;
}

static const hdrg_option_t *hdrg_options_find(const hdrg_options_t *options, const char *key,
                                              size_t key_len)
{
    uint32_t i;

    for (i = 0; i < options->count; i++) {
        if (hdrg_text_equal(&options->items[i].key, key, key_len)) {
            return &options->items[i];
        }
    }

    return NULL;
}

static void *HDRG_PLUGIN_CALL hdrg_api_mem_alloc(const hdrg_plugin_api_t *api, size_t size)
{
    return hdrg_mem_alloc(hdrg_api_alloc(api), size);
}

static void *HDRG_PLUGIN_CALL hdrg_api_mem_calloc(const hdrg_plugin_api_t *api, size_t count,
                                                  size_t size)
{
    return hdrg_mem_calloc(hdrg_api_alloc(api), count, size);
}

static void *HDRG_PLUGIN_CALL hdrg_api_mem_realloc(const hdrg_plugin_api_t *api, void *ptr,
                                                   size_t size)
{
    return hdrg_mem_realloc(hdrg_api_alloc(api), ptr, size);
}

static void HDRG_PLUGIN_CALL hdrg_api_mem_free(const hdrg_plugin_api_t *api, void *ptr)
{
    hdrg_mem_free(hdrg_api_alloc(api), ptr);
}

static void *HDRG_PLUGIN_CALL hdrg_api_mem_aligned_alloc(const hdrg_plugin_api_t *api, size_t size,
                                                         size_t align)
{
    return hdrg_mem_aligned_alloc(hdrg_api_alloc(api), size, align);
}

static void HDRG_PLUGIN_CALL hdrg_api_mem_aligned_free(const hdrg_plugin_api_t *api, void *ptr)
{
    hdrg_mem_aligned_free(hdrg_api_alloc(api), ptr);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_set_info(hdrg_plugin_t *plugin, const char *id,
                                                        size_t id_len, const char *name,
                                                        size_t name_len, const char *version,
                                                        size_t version_len, const char *vendor,
                                                        size_t vendor_len)
{
    const hdrg_allocator_t *alloc;
    hdrg_status_t status;

    status = hdrg_plugin_check(plugin);
    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_text_check(id, id_len, HDRG_LIMIT_ID, true, HDRG_KIND_ID) ||
        !hdrg_text_check(name, name_len, HDRG_LIMIT_NAME, true, HDRG_KIND_LINE) ||
        !hdrg_text_check(version, version_len, HDRG_LIMIT_VERSION, true, HDRG_KIND_VERSION) ||
        !hdrg_text_check(vendor, vendor_len, HDRG_LIMIT_NAME, false, HDRG_KIND_LINE)) {
        return hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "invalid plugin info");
    }

    if (!plugin->internal && id_len >= strlen(HDRG_RESERVED_ID) &&
        memcmp(id, HDRG_RESERVED_ID, strlen(HDRG_RESERVED_ID)) == 0) {
        return hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "plugin id prefix hdrg. is reserved");
    }

    alloc = &plugin->host->alloc;
    status = hdrg_text_copy(alloc, &plugin->id, id, id_len);
    if (status == HDRG_OK) {
        status = hdrg_text_copy(alloc, &plugin->name, name, name_len);
    }

    if (status == HDRG_OK) {
        status = hdrg_text_copy(alloc, &plugin->version, version, version_len);
    }

    if (status == HDRG_OK) {
        status = hdrg_text_copy(alloc, &plugin->vendor, vendor, vendor_len);
    }

    plugin->has_info = status == HDRG_OK;

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_set_license(hdrg_plugin_t *plugin,
                                                           const char *expression,
                                                           size_t expression_len,
                                                           const char *copyright,
                                                           size_t copyright_len)
{
    hdrg_status_t status;

    status = hdrg_plugin_check(plugin);
    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_text_check(expression, expression_len, HDRG_LIMIT_LICENSE, true, HDRG_KIND_SPDX) ||
        !hdrg_text_check(copyright, copyright_len, HDRG_LIMIT_COPYRIGHT, true, HDRG_KIND_LINES)) {
        return hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG,
                                "invalid plugin license (SPDX expression) or copyright");
    }

    status = hdrg_text_copy(&plugin->host->alloc, &plugin->license, expression, expression_len);
    if (status == HDRG_OK) {
        status = hdrg_text_copy(&plugin->host->alloc, &plugin->copyright, copyright, copyright_len);
    }

    plugin->has_license = status == HDRG_OK;

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_add_license_text(hdrg_plugin_t *plugin,
                                                                const char *spdx_id, size_t id_len,
                                                                const char *text, size_t text_len)
{
    hdrg_license_text_t *texts, *entry;
    hdrg_status_t status;
    uint32_t i;

    status = hdrg_plugin_check(plugin);
    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_text_check(spdx_id, id_len, HDRG_LIMIT_ID, true, HDRG_KIND_REF) ||
        !hdrg_text_check(text, text_len, HDRG_LIMIT_TEXT, true, HDRG_KIND_BLOCK)) {
        return hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "invalid license text");
    }

    if (plugin->text_count >= HDRG_LIMIT_COMPONENTS ||
        text_len > HDRG_LIMIT_TEXT_TOTAL - plugin->text_bytes) {
        return hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "too many license texts");
    }

    for (i = 0; i < plugin->text_count; i++) {
        if (hdrg_text_equal(&plugin->texts[i].id, spdx_id, id_len)) {
            return hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "duplicate license text id");
        }
    }

    texts = (hdrg_license_text_t *)hdrg_mem_realloc(&plugin->host->alloc, plugin->texts,
                                                    (plugin->text_count + 1u) * sizeof(*texts));
    if (texts == NULL) {
        return HDRG_ERR_NOMEM;
    }

    plugin->texts = texts;
    entry = &texts[plugin->text_count];
    memset(entry, 0, sizeof(*entry));

    status = hdrg_text_copy(&plugin->host->alloc, &entry->id, spdx_id, id_len);
    if (status == HDRG_OK) {
        status = hdrg_text_copy(&plugin->host->alloc, &entry->text, text, text_len);
    }

    plugin->text_count++;
    plugin->text_bytes += text_len;

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_add_component(
    hdrg_plugin_t *plugin, const char *name, size_t name_len, const char *version,
    size_t version_len, const char *expression, size_t expression_len, const char *copyright,
    size_t copyright_len, const char *notice, size_t notice_len)
{
    const hdrg_allocator_t *alloc;
    hdrg_component_t *components, *entry;
    hdrg_status_t status;

    status = hdrg_plugin_check(plugin);
    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_text_check(name, name_len, HDRG_LIMIT_NAME, true, HDRG_KIND_LINE) ||
        !hdrg_text_check(version, version_len, HDRG_LIMIT_VERSION, false, HDRG_KIND_VERSION) ||
        !hdrg_text_check(expression, expression_len, HDRG_LIMIT_LICENSE, true, HDRG_KIND_SPDX) ||
        !hdrg_text_check(copyright, copyright_len, HDRG_LIMIT_COPYRIGHT, true, HDRG_KIND_LINES) ||
        !hdrg_text_check(notice, notice_len, HDRG_LIMIT_TEXT, false, HDRG_KIND_BLOCK)) {
        return hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "invalid component report");
    }

    if (plugin->component_count >= HDRG_LIMIT_COMPONENTS ||
        notice_len > HDRG_LIMIT_TEXT_TOTAL - plugin->text_bytes) {
        return hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "too many components");
    }

    alloc = &plugin->host->alloc;
    components = (hdrg_component_t *)hdrg_mem_realloc(
        alloc, plugin->components, (plugin->component_count + 1u) * sizeof(*components));
    if (components == NULL) {
        return HDRG_ERR_NOMEM;
    }

    plugin->components = components;
    entry = &components[plugin->component_count];
    memset(entry, 0, sizeof(*entry));

    status = hdrg_text_copy(alloc, &entry->name, name, name_len);
    if (status == HDRG_OK) {
        status = hdrg_text_copy(alloc, &entry->version, version, version_len);
    }

    if (status == HDRG_OK) {
        status = hdrg_text_copy(alloc, &entry->license, expression, expression_len);
    }

    if (status == HDRG_OK) {
        status = hdrg_text_copy(alloc, &entry->copyright, copyright, copyright_len);
    }

    if (status == HDRG_OK) {
        status = hdrg_text_copy(alloc, &entry->notice, notice, notice_len);
    }

    plugin->component_count++;
    plugin->text_bytes += notice_len;

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_set_status(hdrg_plugin_t *plugin, uint32_t status,
                                                          uint32_t reason, const char *message,
                                                          size_t message_len)
{
    hdrg_status_t result;
    bool valid;

    result = hdrg_plugin_check(plugin);
    if (result != HDRG_OK) {
        return result;
    }

    /* host-only reasons (version check, rejection) are never the plugin's */
    valid = (status == HDRG_PLUGIN_READY && reason == HDRG_REASON_NONE) ||
            (status == HDRG_PLUGIN_DEGRADED && reason != HDRG_REASON_NONE && message_len > 0) ||
            (status == HDRG_PLUGIN_UNAVAILABLE && reason != HDRG_REASON_NONE);
    valid = valid && reason != HDRG_REASON_HOST_TOO_OLD && reason != HDRG_REASON_PLUGIN_TOO_OLD &&
            reason <= HDRG_REASON_OTHER;

    if (!valid ||
        !hdrg_text_check(message, message_len, HDRG_LIMIT_MESSAGE, false, HDRG_KIND_LINE)) {
        return hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "invalid plugin status");
    }

    result = hdrg_text_copy(&plugin->host->alloc, &plugin->message, message, message_len);
    if (result != HDRG_OK) {
        return result;
    }

    plugin->status = status;
    plugin->reason = reason;
    plugin->has_status = true;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_set_deinit(hdrg_plugin_t *plugin,
                                                          hdrg_plugin_deinit_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_plugin_check(plugin);
    if (status != HDRG_OK) {
        return status;
    }

    if (fn == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    plugin->deinit = fn;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_set_user(hdrg_plugin_t *plugin, void *user)
{
    hdrg_status_t status;

    status = hdrg_plugin_check(plugin);
    if (status != HDRG_OK) {
        return status;
    }

    plugin->user = user;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_class_user(const hdrg_filter_t *filter,
                                                                 void **user)
{
    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || user == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *user = filter->cls->user;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_user(hdrg_class_t *cls, void *user)
{
    hdrg_status_t status;

    status = hdrg_class_check(cls);
    if (status != HDRG_OK) {
        return status;
    }

    cls->user = user;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_plugin_user(const hdrg_filter_t *filter,
                                                                  void **user)
{
    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || user == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *user = filter->cls->plugin->user;

    return HDRG_OK;
}

static hdrg_class_t *HDRG_PLUGIN_CALL hdrg_api_class_create(hdrg_plugin_t *plugin, const char *id,
                                                            size_t id_len)
{
    hdrg_class_t *cls, **classes;
    uint32_t i;

    if (hdrg_plugin_check(plugin) != HDRG_OK) {
        return NULL;
    }

    if (!hdrg_text_check(id, id_len, HDRG_LIMIT_ID, true, HDRG_KIND_ID) ||
        (!plugin->internal && id_len >= strlen(HDRG_RESERVED_ID) &&
         memcmp(id, HDRG_RESERVED_ID, strlen(HDRG_RESERVED_ID)) == 0)) {
        (void)hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "invalid filter class id");

        return NULL;
    }

    if (plugin->class_count >= HDRG_LIMIT_CLASSES) {
        (void)hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "too many filter classes");

        return NULL;
    }

    for (i = 0; i < plugin->class_count; i++) {
        if (hdrg_text_equal(&plugin->classes[i]->id, id, id_len)) {
            (void)hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG, "duplicate filter class id");

            return NULL;
        }
    }

    if (hdrg_host_find_class(plugin->host, id, id_len) != NULL) {
        (void)hdrg_plugin_fail(plugin, HDRG_ERR_INVALID_ARG,
                               "a filter class with the same id is already registered");

        return NULL;
    }

    classes = (hdrg_class_t **)hdrg_mem_realloc(&plugin->host->alloc, plugin->classes,
                                                (plugin->class_count + 1u) * sizeof(*classes));
    if (classes == NULL) {
        return NULL;
    }

    plugin->classes = classes;

    cls = (hdrg_class_t *)hdrg_mem_calloc(&plugin->host->alloc, 1, sizeof(*cls));
    if (cls == NULL) {
        return NULL;
    }

    if (hdrg_text_copy(&plugin->host->alloc, &cls->id, id, id_len) != HDRG_OK) {
        hdrg_mem_free(&plugin->host->alloc, cls);

        return NULL;
    }

    cls->magic = HDRG_CLASS_MAGIC;
    cls->plugin = plugin;
    classes[plugin->class_count] = cls;
    plugin->class_count++;

    return cls;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_stages(hdrg_class_t *cls, uint32_t stages)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status != HDRG_OK) {
        return status;
    }

    if (stages == 0 || (stages & ~HDRG_STAGES_ALL) != 0) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG, "invalid filter stages");
    }

    cls->stages = stages;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_flags(hdrg_class_t *cls, uint32_t flags)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status != HDRG_OK) {
        return status;
    }

    if ((flags & ~HDRG_FLAGS_ALL) != 0) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG, "invalid filter flags");
    }

    cls->flags = flags;

    return HDRG_OK;
}

/* a new option spec with a checked key and description */
static hdrg_status_t hdrg_class_add_option(hdrg_class_t *cls, const char *key, size_t key_len,
                                           const char *description, size_t description_len,
                                           hdrg_option_spec_t **out)
{
    const hdrg_allocator_t *alloc;
    hdrg_option_spec_t *options, *spec;
    hdrg_status_t status;

    status = hdrg_class_check(cls);
    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_text_check(key, key_len, HDRG_LIMIT_KEY, true, HDRG_KIND_KEY) ||
        !hdrg_text_check(description, description_len, HDRG_LIMIT_MESSAGE, false, HDRG_KIND_LINE) ||
        cls->option_count >= HDRG_LIMIT_OPTIONS || hdrg_class_option(cls, key, key_len) != NULL) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG, "invalid filter option");
    }

    alloc = &cls->plugin->host->alloc;
    options = (hdrg_option_spec_t *)hdrg_mem_realloc(alloc, cls->options,
                                                     (cls->option_count + 1u) * sizeof(*options));
    if (options == NULL) {
        return HDRG_ERR_NOMEM;
    }

    cls->options = options;
    spec = &options[cls->option_count];
    memset(spec, 0, sizeof(*spec));

    status = hdrg_text_copy(alloc, &spec->key, key, key_len);
    if (status == HDRG_OK) {
        status = hdrg_text_copy(alloc, &spec->description, description, description_len);
    }

    cls->option_count++;
    *out = spec;

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_add_option_int(
    hdrg_class_t *cls, const char *key, size_t key_len, int64_t def, int64_t min, int64_t max,
    const char *description, size_t description_len)
{
    hdrg_option_spec_t *spec;
    hdrg_status_t status;

    if (cls != NULL && cls->magic == HDRG_CLASS_MAGIC && !(min <= def && def <= max)) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                "filter option default outside its range");
    }

    status = hdrg_class_add_option(cls, key, key_len, description, description_len, &spec);
    if (status != HDRG_OK) {
        return status;
    }

    spec->type = HDRG_OPTION_INT;
    spec->def_int = def;
    spec->min_int = min;
    spec->max_int = max;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_add_option_float(
    hdrg_class_t *cls, const char *key, size_t key_len, double def, double min, double max,
    const char *description, size_t description_len)
{
    hdrg_option_spec_t *spec;
    hdrg_status_t status;

    /* also rejects NaN and infinities */
    if (cls != NULL && cls->magic == HDRG_CLASS_MAGIC &&
        !(min >= -DBL_MAX && min <= def && def <= max && max <= DBL_MAX)) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                "filter option default outside its range");
    }

    status = hdrg_class_add_option(cls, key, key_len, description, description_len, &spec);
    if (status != HDRG_OK) {
        return status;
    }

    spec->type = HDRG_OPTION_FLOAT;
    spec->def_float = def;
    spec->min_float = min;
    spec->max_float = max;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_add_option_string(
    hdrg_class_t *cls, const char *key, size_t key_len, const char *def, size_t def_len,
    const char *description, size_t description_len)
{
    hdrg_option_spec_t *spec;
    hdrg_status_t status;

    if (cls != NULL && cls->magic == HDRG_CLASS_MAGIC &&
        !hdrg_text_check(def, def_len, HDRG_LIMIT_MESSAGE, false, HDRG_KIND_LINE)) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG, "invalid filter option default");
    }

    status = hdrg_class_add_option(cls, key, key_len, description, description_len, &spec);
    if (status != HDRG_OK) {
        return status;
    }

    spec->type = HDRG_OPTION_STRING;

    return hdrg_text_copy(&cls->plugin->host->alloc, &spec->def_text, def, def_len);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_create(hdrg_class_t *cls,
                                                                hdrg_filter_create_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status == HDRG_OK && fn == NULL) {
        status = HDRG_ERR_INVALID_ARG;
    }

    if (status == HDRG_OK) {
        cls->create = fn;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_destroy(hdrg_class_t *cls,
                                                                 hdrg_filter_destroy_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status == HDRG_OK && fn == NULL) {
        status = HDRG_ERR_INVALID_ARG;
    }

    if (status == HDRG_OK) {
        cls->destroy = fn;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_configure(hdrg_class_t *cls,
                                                                   hdrg_filter_configure_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status == HDRG_OK && fn == NULL) {
        status = HDRG_ERR_INVALID_ARG;
    }

    if (status == HDRG_OK) {
        cls->configure = fn;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_process(hdrg_class_t *cls,
                                                                 hdrg_filter_process_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status == HDRG_OK && fn == NULL) {
        status = HDRG_ERR_INVALID_ARG;
    }

    if (status == HDRG_OK) {
        cls->process = fn;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_input_rows(hdrg_class_t *cls,
                                                                    hdrg_filter_input_rows_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status == HDRG_OK && fn == NULL) {
        status = HDRG_ERR_INVALID_ARG;
    }

    if (status == HDRG_OK) {
        cls->input_rows = fn;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_frames(hdrg_class_t *cls,
                                                                hdrg_filter_frame_alloc_fn_t alloc,
                                                                hdrg_filter_frame_free_fn_t release)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status == HDRG_OK && (alloc == NULL) != (release == NULL)) {
        status = HDRG_ERR_INVALID_ARG;
    }

    if (status == HDRG_OK) {
        cls->frame_alloc = alloc;
        cls->frame_free = release;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_cancel(hdrg_class_t *cls,
                                                                hdrg_filter_cancel_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status == HDRG_OK && fn == NULL) {
        status = HDRG_ERR_INVALID_ARG;
    }

    if (status == HDRG_OK) {
        cls->cancel = fn;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL
hdrg_api_class_set_device_release(hdrg_class_t *cls, hdrg_filter_device_release_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status == HDRG_OK && fn == NULL) {
        status = HDRG_ERR_INVALID_ARG;
    }

    if (status == HDRG_OK) {
        cls->device_release = fn;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_stats(
    hdrg_class_t *cls, hdrg_stats_window_fn_t window, hdrg_stats_fade_fn_t fade,
    hdrg_stats_frame_fn_t frame, hdrg_stats_render_fn_t render, hdrg_stats_fit_fn_t fit,
    hdrg_stats_rendered_fn_t rendered)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status != HDRG_OK) {
        return status;
    }

    if (window == NULL || fade == NULL || frame == NULL || render == NULL || fit == NULL ||
        rendered == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    cls->stats_window = window;
    cls->stats_fade = fade;
    cls->stats_frame = frame;
    cls->stats_render = render;
    cls->stats_fit = fit;
    cls->stats_rendered = rendered;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_analyze(hdrg_class_t *cls,
                                                                 hdrg_filter_analyze_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status != HDRG_OK) {
        return status;
    }

    if (fn == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    cls->analyze = fn;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_submit(hdrg_class_t *cls,
                                                                hdrg_filter_submit_fn_t fn)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_FILTER);
    if (status != HDRG_OK) {
        return status;
    }

    if (fn == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    cls->submit = fn;

    return HDRG_OK;
}

/* "jpg,jpeg": lowercase [a-z0-9] names of 1 to 16 bytes, comma-separated */
static bool hdrg_extensions_valid(const char *list, size_t len)
{
    size_t i, run;

    run = 0;

    for (i = 0; i < len; i++) {
        if (list[i] == ',') {
            if (run == 0) {
                return false;
            }

            run = 0;

            continue;
        }

        if (!((list[i] >= 'a' && list[i] <= 'z') || (list[i] >= '0' && list[i] <= '9')) ||
            ++run > 16u) {
            return false;
        }
    }

    return len > 0 && run > 0;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_extensions(hdrg_class_t *cls,
                                                                    const char *list,
                                                                    size_t list_len)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_CODEC);
    if (status != HDRG_OK) {
        return status;
    }

    if (list == NULL || list_len > HDRG_LIMIT_MESSAGE || !hdrg_extensions_valid(list, list_len)) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG, "invalid codec extensions");
    }

    hdrg_text_free(&cls->plugin->host->alloc, &cls->extensions);

    return hdrg_text_copy(&cls->plugin->host->alloc, &cls->extensions, list, list_len);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_reader(hdrg_class_t *cls,
                                                                hdrg_codec_probe_fn_t probe,
                                                                hdrg_codec_read_fn_t read)
{
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_CODEC);
    if (status == HDRG_OK && (probe == NULL || read == NULL)) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                "a codec's reader needs probe and read");
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls->probe = probe;
    cls->read = read;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_set_writer(
    hdrg_class_t *cls, uint32_t forms, uint32_t default_form, uint32_t rows, uint32_t color,
    uint32_t max_side, hdrg_codec_create_fn_t create, hdrg_codec_rows_fn_t write_rows,
    hdrg_codec_finish_fn_t finish, hdrg_codec_destroy_fn_t destroy)
{
    const uint32_t all = HDRG_CODEC_FORM(HDRG_OUTPUT_GAINMAP) |
                         HDRG_CODEC_FORM(HDRG_OUTPUT_HYBRID) | HDRG_CODEC_FORM(HDRG_OUTPUT_PQ);
    hdrg_status_t status;

    status = hdrg_class_check_kind(cls, HDRG_CLASS_CODEC);
    if (status != HDRG_OK) {
        return status;
    }

    if (forms == 0 || (forms & ~all) != 0 || default_form < HDRG_OUTPUT_GAINMAP ||
        default_form > HDRG_OUTPUT_PQ || (forms & HDRG_CODEC_FORM(default_form)) == 0 ||
        (rows != HDRG_CODEC_ROWS_RGB8 && rows != HDRG_CODEC_ROWS_RGB16) ||
        (color != HDRG_CODEC_COLOR_CICP && color != HDRG_CODEC_COLOR_ICC) || max_side == 0 ||
        max_side > 65535u || create == NULL || write_rows == NULL || finish == NULL ||
        destroy == NULL) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG, "invalid codec writer");
    }

    cls->forms = forms;
    cls->default_form = default_form;
    cls->rows = rows;
    cls->color = color;
    cls->max_side = max_side;
    cls->write_create = create;
    cls->write_rows = write_rows;
    cls->write_finish = finish;
    cls->write_destroy = destroy;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_class_register(hdrg_class_t *cls)
{
    hdrg_status_t status;

    status = hdrg_class_check(cls);
    if (status != HDRG_OK) {
        return status;
    }

    if (cls->kind == HDRG_CLASS_CODEC) {
        if (cls->probe == NULL && cls->write_create == NULL) {
            return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                    "codec class needs a reader or a writer");
        }

        cls->registered = true;

        return HDRG_OK;
    }

    /* a STATS class keeps a session's statistics alone: it runs in no other stage, on no frames */
    if ((cls->stages & HDRG_STAGE_STATS) != 0 || cls->stats_render != NULL) {
        if (cls->stages != HDRG_STAGE_STATS || cls->stats_render == NULL || cls->create == NULL ||
            cls->destroy == NULL || cls->configure != NULL || cls->process != NULL ||
            cls->submit != NULL || cls->analyze != NULL ||
            (cls->flags & ~HDRG_FILTER_DETERMINISTIC) != 0) {
            return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                    "a STATS class needs that stage alone, create, destroy and its "
                                    "work, and no frames' callbacks or flags");
        }

        cls->registered = true;

        return HDRG_OK;
    }

    /* an ANALYZE class judges lights alone: it runs in no other stage, on no frames */
    if ((cls->stages & HDRG_STAGE_ANALYZE) != 0 || cls->analyze != NULL) {
        if (cls->stages != HDRG_STAGE_ANALYZE || cls->analyze == NULL || cls->create == NULL ||
            cls->destroy == NULL || cls->configure != NULL || cls->process != NULL ||
            cls->submit != NULL || (cls->flags & ~HDRG_FILTER_DETERMINISTIC) != 0) {
            return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                    "an ANALYZE class needs that stage alone, create, destroy and "
                                    "analyze, and no frames' callbacks or flags");
        }

        cls->registered = true;

        return HDRG_OK;
    }

    if (cls->stages == 0 || cls->create == NULL || cls->destroy == NULL || cls->configure == NULL ||
        ((cls->flags & HDRG_FILTER_ASYNC) == 0 && cls->process == NULL)) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                "filter class needs stages and the create, destroy, configure "
                                "and process callbacks");
    }

    /* an ASYNC class submits whole frames */
    if ((cls->flags & HDRG_FILTER_ASYNC) != 0 &&
        (cls->submit == NULL || (cls->flags & HDRG_FILTER_BANDS) != 0)) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                "an ASYNC filter class needs submit and works on whole frames");
    }

    /* state carried from job to job: whole frames, in order */
    if ((cls->flags & HDRG_FILTER_SEQUENTIAL) != 0 && (cls->flags & HDRG_FILTER_BANDS) != 0) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                "a SEQUENTIAL filter class works on whole frames");
    }

    /* the filter's own frames are whole frames */
    if (cls->frame_alloc != NULL && (cls->flags & HDRG_FILTER_BANDS) != 0) {
        return hdrg_plugin_fail(cls->plugin, HDRG_ERR_INVALID_ARG,
                                "a filter class allocating its frames works on whole frames");
    }

    cls->registered = true;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_option_get_int(const hdrg_options_t *options,
                                                              const char *key, size_t key_len,
                                                              int64_t *value)
{
    const hdrg_option_t *item;

    if (options == NULL || options->magic != HDRG_OPTIONS_MAGIC || !options->resolved ||
        value == NULL || (key == NULL && key_len != 0) || key_len > HDRG_LIMIT_KEY) {
        return HDRG_ERR_INVALID_ARG;
    }

    item = hdrg_options_find(options, key, key_len);
    if (item == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    if (item->type != HDRG_OPTION_INT) {
        return HDRG_ERR_INVALID_ARG;
    }

    *value = item->int_value;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_option_get_float(const hdrg_options_t *options,
                                                                const char *key, size_t key_len,
                                                                double *value)
{
    const hdrg_option_t *item;

    if (options == NULL || options->magic != HDRG_OPTIONS_MAGIC || !options->resolved ||
        value == NULL || (key == NULL && key_len != 0) || key_len > HDRG_LIMIT_KEY) {
        return HDRG_ERR_INVALID_ARG;
    }

    item = hdrg_options_find(options, key, key_len);
    if (item == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    if (item->type != HDRG_OPTION_FLOAT) {
        return HDRG_ERR_INVALID_ARG;
    }

    *value = item->float_value;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_option_get_string(const hdrg_options_t *options,
                                                                 const char *key, size_t key_len,
                                                                 char *buf, size_t cap,
                                                                 size_t *needed)
{
    const hdrg_option_t *item;

    if (options == NULL || options->magic != HDRG_OPTIONS_MAGIC || !options->resolved ||
        needed == NULL || (buf == NULL && cap != 0) || (key == NULL && key_len != 0) ||
        key_len > HDRG_LIMIT_KEY) {
        return HDRG_ERR_INVALID_ARG;
    }

    item = hdrg_options_find(options, key, key_len);
    if (item == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    if (item->type != HDRG_OPTION_STRING) {
        return HDRG_ERR_INVALID_ARG;
    }

    *needed = item->value.size + 1u;

    if (cap >= *needed) {
        memcpy(buf, item->value.data, item->value.size);
        buf[item->value.size] = '\0';
    }

    return HDRG_OK;
}

static const hdrg_plugin_api_t hdrg_api = {
    .mem_alloc = hdrg_api_mem_alloc,
    .mem_calloc = hdrg_api_mem_calloc,
    .mem_realloc = hdrg_api_mem_realloc,
    .mem_free = hdrg_api_mem_free,
    .mem_aligned_alloc = hdrg_api_mem_aligned_alloc,
    .mem_aligned_free = hdrg_api_mem_aligned_free,
    .plugin_set_info = hdrg_api_set_info,
    .plugin_set_license = hdrg_api_set_license,
    .plugin_add_license_text = hdrg_api_add_license_text,
    .plugin_add_component = hdrg_api_add_component,
    .plugin_set_status = hdrg_api_set_status,
    .plugin_set_deinit = hdrg_api_set_deinit,
    .class_create = hdrg_api_class_create,
    .class_set_stages = hdrg_api_class_set_stages,
    .class_set_flags = hdrg_api_class_set_flags,
    .class_add_option_int = hdrg_api_class_add_option_int,
    .class_add_option_float = hdrg_api_class_add_option_float,
    .class_set_create = hdrg_api_class_set_create,
    .class_set_destroy = hdrg_api_class_set_destroy,
    .class_set_configure = hdrg_api_class_set_configure,
    .class_set_process = hdrg_api_class_set_process,
    .class_set_input_rows = hdrg_api_class_set_input_rows,
    .class_set_frames = hdrg_api_class_set_frames,
    .class_set_cancel = hdrg_api_class_set_cancel,
    .class_set_device_release = hdrg_api_class_set_device_release,
    .class_register = hdrg_api_class_register,
    .class_set_extensions = hdrg_api_class_set_extensions,
    .class_set_reader = hdrg_api_class_set_reader,
    .class_set_writer = hdrg_api_class_set_writer,
    .run_workers = hdrg_api_run_workers,
    .run_parallel = hdrg_api_run_parallel,
    .yuv_to_rgb16 = hdrg_api_yuv_to_rgb16,
    .rgb16_to_yuv = hdrg_api_rgb16_to_yuv,
    .lights_get = hdrg_api_lights_get,
    .lights_frame = hdrg_api_lights_frame,
    .lights_set_chance = hdrg_api_lights_set_chance,
    .filter_stats_session = hdrg_api_filter_stats_session,
    .stats_head = hdrg_api_stats_head,
    .stats_queued = hdrg_api_stats_queued,
    .read_users = hdrg_api_read_users,
    .read_limits = hdrg_api_read_limits,
    .read_image = hdrg_api_read_image,
    .read_set_cicp = hdrg_api_read_set_cicp,
    .read_set_xy = hdrg_api_read_set_xy,
    .read_set_icc = hdrg_api_read_set_icc,
    .read_set_exif = hdrg_api_read_set_exif,
    .read_set_orientation = hdrg_api_read_set_orientation,
    .write_users = hdrg_api_write_users,
    .write_image = hdrg_api_write_image,
    .write_exif = hdrg_api_write_exif,
    .write_icc = hdrg_api_write_icc,
    .write_gainmap = hdrg_api_write_gainmap,
    .write_alternate = hdrg_api_write_alternate,
    .write_iso21496 = hdrg_api_write_iso21496,
    .write_apple_xmp = hdrg_api_write_apple_xmp,
    .write_light = hdrg_api_write_light,
    .write_output = hdrg_api_write_output,
    .nego_input = hdrg_api_nego_input,
    .nego_requested_size = hdrg_api_nego_requested_size,
    .nego_set_output = hdrg_api_nego_set_output,
    .nego_set_halo = hdrg_api_nego_set_halo,
    .job_rows = hdrg_api_job_rows,
    .job_input = hdrg_api_job_input,
    .job_output = hdrg_api_job_output,
    .job_scratch = hdrg_api_job_scratch,
    .option_get_int = hdrg_api_option_get_int,
    .option_get_float = hdrg_api_option_get_float,
    .nego_side_data = hdrg_api_nego_side_data,
    .nego_set_passthrough = hdrg_api_nego_set_passthrough,
    .plugin_set_user = hdrg_api_set_user,
    .filter_plugin_user = hdrg_api_filter_plugin_user,
    .filter_class_user = hdrg_api_filter_class_user,
    .class_set_user = hdrg_api_class_set_user,
    .class_add_option_string = hdrg_api_class_add_option_string,
    .option_get_string = hdrg_api_option_get_string,
    .nego_input_color = hdrg_api_nego_input_color,
    .class_set_submit = hdrg_api_class_set_submit,
    .class_set_analyze = hdrg_api_class_set_analyze,
    .class_set_stats = hdrg_api_class_set_stats,
    .job_done = hdrg_api_job_done,
    .filter_set_max_inflight = hdrg_api_filter_set_max_inflight,
    .filter_device = hdrg_api_filter_device,
    .filter_gpu_share = hdrg_api_filter_gpu_share,
    .filter_set_device = hdrg_api_filter_set_device,
    .filter_set_input = hdrg_api_filter_set_input,
    .filter_set_window = hdrg_api_filter_set_window,
    .filter_set_clip = hdrg_api_filter_set_clip,
    .job_window = hdrg_api_job_window,
    .job_input_frame = hdrg_api_job_input_frame,
    .job_device = hdrg_api_job_device,
    .job_device_window = hdrg_api_job_device_window,
    .job_set_output_device = hdrg_api_job_set_output_device,
    .job_set_side_data = hdrg_api_job_set_side_data,
    .job_frame = hdrg_api_job_frame,
    .job_phase = hdrg_api_job_phase,
    .job_phases = hdrg_api_job_phases,
    .job_shot = hdrg_api_job_shot,
    .job_input_handle = hdrg_api_job_input_handle,
    .job_output_handle = hdrg_api_job_output_handle,
};

extern void hdrg_host_init(hdrg_plugin_host_t *host, const hdrg_allocator_t *alloc,
                           const hdrg_context_t *ctx)
{
    memset(host, 0, sizeof(*host));

    host->api = hdrg_api;
    host->alloc = *alloc;
    host->ctx = ctx;
    host->own = (hdrg_host_own_t *)hdrg_mem_calloc(&host->alloc, 1, sizeof(hdrg_host_own_t));

    /* without it the context has the app's devices alone, and no shares */
    if (host->own != NULL && !hdrg_mutex_init(&host->own->lock)) {
        hdrg_mem_free(&host->alloc, host->own);
        host->own = NULL;
    }

    if (host->own != NULL && !hdrg_mutex_init(&host->own->share_lock)) {
        hdrg_mutex_destroy(&host->own->lock);
        hdrg_mem_free(&host->alloc, host->own);
        host->own = NULL;
    }
}

extern void hdrg_host_destroy(hdrg_plugin_host_t *host)
{
    uint32_t i;

    for (i = 0; i < host->filter_count; i++) {
        hdrg_options_destroy(host->filters[i].options);
    }

    hdrg_options_destroy(host->writer_options);
    host->writer_options = NULL;
    host->writer = NULL;

    for (i = host->plugin_count; i > 0; i--) {
        hdrg_plugin_unload(host->plugins[i - 1]);
        hdrg_plugin_free(&host->alloc, host->plugins[i - 1]);
    }

    hdrg_mem_free(&host->alloc, host->filters);
    hdrg_mem_free(&host->alloc, host->plugins);

    /* the shared objects and own devices after the plugins that held them */
    if (host->own != NULL) {
        hdrg_host_wrapped_destroy(host);
#if defined(HDRG_WITH_METAL)
        hdrg_metal_destroy((hdrg_metal_t *)host->own->objects[HDRG_DEVICE_METAL - 1u]);
#endif
#if defined(HDRG_WITH_VULKAN)
        hdrg_vulkan_destroy((hdrg_vulkan_t *)host->own->objects[HDRG_DEVICE_VULKAN - 1u]);
#endif
#if defined(HDRG_WITH_WEBGPU)
        hdrg_gpu_destroy((hdrg_gpu_t *)host->own->objects[HDRG_DEVICE_WEBGPU - 1u]);
#endif
        hdrg_mutex_destroy(&host->own->share_lock);
        hdrg_mutex_destroy(&host->own->lock);
        hdrg_mem_free(&host->alloc, host->own);
    }

    host->filters = NULL;
    host->plugins = NULL;
    host->own = NULL;
    host->filter_count = 0;
    host->plugin_count = 0;
}

extern hdrg_status_t hdrg_host_register(hdrg_plugin_host_t *host, hdrg_plugin_init_fn_t init,
                                        void *module, uint32_t abi_version, bool internal)
{
    const char *message;
    hdrg_plugin_t *plugin, **plugins;
    hdrg_status_t status;
    uint32_t index;

    host->error[0] = '\0';

    plugins = (hdrg_plugin_t **)hdrg_mem_realloc(&host->alloc, host->plugins,
                                                 (host->plugin_count + 1u) * sizeof(*plugins));
    if (plugins == NULL) {
        hdrg_module_close(module);

        return HDRG_ERR_NOMEM;
    }

    host->plugins = plugins;

    plugin = (hdrg_plugin_t *)hdrg_mem_calloc(&host->alloc, 1, sizeof(*plugin));
    if (plugin == NULL) {
        hdrg_module_close(module);

        return HDRG_ERR_NOMEM;
    }

    plugin->magic = HDRG_PLUGIN_MAGIC;
    plugin->host = host;
    plugin->module = module;
    plugin->abi_version = abi_version;
    plugin->internal = internal;
    plugin->status = HDRG_PLUGIN_UNAVAILABLE;
    index = host->plugin_count;
    plugins[index] = plugin;
    host->plugin_count++;

    /* the table must not be touched by a module built for another table */
    if (abi_version != HDRG_PLUGIN_ABI_VERSION) {
        return hdrg_plugin_reject(
            plugin,
            (abi_version > HDRG_PLUGIN_ABI_VERSION) ? HDRG_REASON_HOST_TOO_OLD
                                                    : HDRG_REASON_PLUGIN_TOO_OLD,
            "plugin built against another plugin ABI than this library's", HDRG_ERR_UNSUPPORTED);
    }

    status = init(plugin, &plugin->host->api);
    plugin->frozen = true;

    /* out of memory stays so (the context's allocator refused the plugin's registration) */
    if (status != HDRG_OK) {
        return hdrg_plugin_reject(plugin, HDRG_REASON_REJECTED, "plugin entry point failed",
                                  (status == HDRG_ERR_NOMEM) ? HDRG_ERR_NOMEM : HDRG_ERR_PLUGIN);
    }

    if (!hdrg_plugin_complete(plugin, &message)) {
        return hdrg_plugin_reject(plugin, HDRG_REASON_REJECTED, message, HDRG_ERR_PLUGIN);
    }

    if (!internal && host->policy != NULL &&
        host->policy(host->ctx, index, host->policy_user) != HDRG_OK) {
        return hdrg_plugin_reject(plugin, HDRG_REASON_REJECTED,
                                  "plugin rejected by the host policy", HDRG_ERR_PLUGIN);
    }

    plugin->accepted = plugin->status != HDRG_PLUGIN_UNAVAILABLE;

    if (!plugin->accepted) {
        hdrg_plugin_unload(plugin);
    }

    return HDRG_OK;
}

extern const hdrg_class_t *hdrg_host_find_class(const hdrg_plugin_host_t *host, const char *id,
                                                size_t id_len)
{
    const hdrg_plugin_t *plugin;
    uint32_t i, j;

    for (i = 0; i < host->plugin_count; i++) {
        plugin = host->plugins[i];
        if (!plugin->accepted) {
            continue;
        }

        for (j = 0; j < plugin->class_count; j++) {
            if (plugin->classes[j]->registered &&
                hdrg_text_equal(&plugin->classes[j]->id, id, id_len)) {
                return plugin->classes[j];
            }
        }
    }

    return NULL;
}

extern const hdrg_filter_entry_t *hdrg_host_stage_filter(const hdrg_plugin_host_t *host,
                                                         uint32_t stage)
{
    uint32_t i;

    for (i = 0; i < host->filter_count; i++) {
        if (host->filters[i].stage == stage) {
            return &host->filters[i];
        }
    }

    return NULL;
}

extern uint32_t hdrg_host_stage_count(const hdrg_plugin_host_t *host, uint32_t stage)
{
    uint32_t i, n;

    n = 0;

    for (i = 0; i < host->filter_count; i++) {
        n += (host->filters[i].stage == stage) ? 1u : 0u;
    }

    return n;
}

extern hdrg_status_t hdrg_host_resolve_options(hdrg_plugin_host_t *host, const hdrg_class_t *cls,
                                               const hdrg_options_t *text, hdrg_options_t **out)
{
    const hdrg_option_spec_t *spec;
    const hdrg_option_t *given;
    hdrg_options_t *resolved;
    hdrg_option_t *item;
    hdrg_status_t status;
    uint32_t i;

    *out = NULL;

    for (i = 0; text != NULL && i < text->count; i++) {
        if (hdrg_class_option(cls, text->items[i].key.data, text->items[i].key.size) == NULL) {
            hdrg_host_set_error(host, "unknown filter option", text->items[i].key.data,
                                text->items[i].key.size);

            return HDRG_ERR_NOT_FOUND;
        }
    }

    resolved = (hdrg_options_t *)hdrg_mem_calloc(&host->alloc, 1, sizeof(*resolved));
    if (resolved == NULL) {
        return HDRG_ERR_NOMEM;
    }

    resolved->magic = HDRG_OPTIONS_MAGIC;
    resolved->host = host;
    resolved->resolved = true;

    if (cls->option_count > 0) {
        resolved->items =
            (hdrg_option_t *)hdrg_mem_calloc(&host->alloc, cls->option_count, sizeof(*item));
        if (resolved->items == NULL) {
            hdrg_options_destroy(resolved);

            return HDRG_ERR_NOMEM;
        }
    }

    status = HDRG_OK;
    for (i = 0; i < cls->option_count && status == HDRG_OK; i++) {
        spec = &cls->options[i];
        item = &resolved->items[i];
        resolved->count++;

        status = hdrg_text_copy(&host->alloc, &item->key, spec->key.data, spec->key.size);
        if (status != HDRG_OK) {
            break;
        }

        item->type = spec->type;
        item->int_value = spec->def_int;
        item->float_value = spec->def_float;
        given = (text != NULL) ? hdrg_options_find(text, spec->key.data, spec->key.size) : NULL;

        /* a string is the text given (checked by hdrg_options_set) or the default */
        if (spec->type == HDRG_OPTION_STRING) {
            status = (given != NULL) ? hdrg_text_copy(&host->alloc, &item->value, given->value.data,
                                                      given->value.size)
                                     : hdrg_text_copy(&host->alloc, &item->value,
                                                      spec->def_text.data, spec->def_text.size);

            continue;
        }

        if (given == NULL) {
            continue;
        }

        if ((spec->type == HDRG_OPTION_INT &&
             !hdrg_parse_int(given->value.data, given->value.size, &item->int_value)) ||
            (spec->type == HDRG_OPTION_FLOAT &&
             !hdrg_parse_float(given->value.data, given->value.size, &item->float_value))) {
            hdrg_host_set_error(host, "invalid value for filter option", spec->key.data,
                                spec->key.size);
            status = HDRG_ERR_INVALID_ARG;

            break;
        }

        if ((spec->type == HDRG_OPTION_INT &&
             (item->int_value < spec->min_int || item->int_value > spec->max_int)) ||
            (spec->type == HDRG_OPTION_FLOAT &&
             !(item->float_value >= spec->min_float && item->float_value <= spec->max_float))) {
            hdrg_host_set_error(host, "filter option out of range", spec->key.data, spec->key.size);
            status = HDRG_ERR_INVALID_ARG;
        }
    }

    if (status != HDRG_OK) {
        hdrg_options_destroy(resolved);

        return status;
    }

    *out = resolved;

    return HDRG_OK;
}

extern void hdrg_host_set_error(hdrg_plugin_host_t *host, const char *message, const char *detail,
                                size_t detail_len)
{
    size_t n, room;

    n = HDRG_MIN(strlen(message), (size_t)HDRG_LIMIT_ERROR - 1u);
    memcpy(host->error, message, n);

    if (detail != NULL && detail_len > 0 && n + 2u < HDRG_LIMIT_ERROR - 1u) {
        host->error[n] = ':';
        host->error[n + 1] = ' ';
        n += 2;
        room = HDRG_LIMIT_ERROR - 1u - n;
        memcpy(host->error + n, detail, HDRG_MIN(detail_len, room));
        n += HDRG_MIN(detail_len, room);
    }

    host->error[n] = '\0';
}

extern const hdrg_plugin_api_t *hdrg_class_api(const hdrg_class_t *cls)
{
    return &cls->plugin->host->api;
}

extern bool hdrg_class_library_gpu(const hdrg_class_t *cls)
{
    return (cls->flags & HDRG_FILTER_GPU_FRAMES) != 0;
}

/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "../priv.h"
#include "codec_host.h"
#include "defaults.h"
#include "host.h"
#include "validate.h"

#define HDRG_PATH_MAX 4096u

static inline const char *hdrg_public_str(const hdrg_text_t *text)
{
    return (text->data != NULL) ? text->data : "";
}

/* bounded length of a host string: max + 1 = too long */
static size_t hdrg_public_strlen(const char *s, size_t max)
{
    size_t n;

    n = 0;
    while (n <= max && s[n] != '\0') {
        n++;
    }

    return n;
}

/* the index-th registered class of a kind (HDRG_CLASS_*) across all plugins */
static const hdrg_class_t *hdrg_public_class(const hdrg_plugin_host_t *host, uint8_t kind,
                                             uint32_t index, uint32_t *plugin_index)
{
    uint32_t i, j, n;

    n = 0;
    for (i = 0; i < host->plugin_count; i++) {
        for (j = 0; j < host->plugins[i]->class_count; j++) {
            if (!host->plugins[i]->classes[j]->registered ||
                host->plugins[i]->classes[j]->kind != kind) {
                continue;
            }

            if (n == index) {
                *plugin_index = i;

                return host->plugins[i]->classes[j];
            }

            n++;
        }
    }

    return NULL;
}

/* an option spec as the public struct */
static void hdrg_public_option(const hdrg_option_spec_t *spec, hdrg_option_info_t *out)
{
    out->key = hdrg_public_str(&spec->key);
    out->description = hdrg_public_str(&spec->description);
    out->type = spec->type;
    out->default_int = spec->def_int;
    out->min_int = spec->min_int;
    out->max_int = spec->max_int;
    out->default_float = spec->def_float;
    out->min_float = spec->min_float;
    out->max_float = spec->max_float;
    out->default_text =
        (spec->type == HDRG_OPTION_STRING) ? hdrg_public_str(&spec->def_text) : NULL;
}

extern hdrg_status_t hdrg_context_set_plugin_policy(hdrg_context_t *ctx, hdrg_plugin_policy_fn_t fn,
                                                    void *user)
{
    if (ctx == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    ctx->host.policy = fn;
    ctx->host.policy_user = user;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_context_register_plugin(hdrg_context_t *ctx, hdrg_plugin_init_fn_t init)
{
    if (ctx == NULL || init == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    return hdrg_host_register(&ctx->host, init, NULL, HDRG_PLUGIN_ABI_VERSION, false);
}

extern hdrg_status_t hdrg_context_load_plugin(hdrg_context_t *ctx, const char *path)
{
    hdrg_plugin_init_fn_t init;
    hdrg_status_t status;
    uint32_t abi_version;
    void *module;

    if (ctx == NULL || path == NULL || path[0] == '\0' ||
        hdrg_public_strlen(path, HDRG_PATH_MAX) > HDRG_PATH_MAX) {
        return HDRG_ERR_INVALID_ARG;
    }

    ctx->host.error[0] = '\0';

    status = hdrg_module_open(&ctx->alloc, path, &module, &abi_version, &init);
    if (status == HDRG_ERR_NOT_FOUND) {
        hdrg_host_set_error(&ctx->host, "cannot open the plugin module", NULL, 0);

        return status;
    }

    if (status != HDRG_OK) {
        hdrg_host_set_error(&ctx->host,
                            (status == HDRG_ERR_UNSUPPORTED)
                                ? "not a plugin module, or no module loader on this platform"
                                : "cannot load the plugin module",
                            NULL, 0);

        return status;
    }

    return hdrg_host_register(&ctx->host, init, module, abi_version, false);
}

extern const char *hdrg_context_error(const hdrg_context_t *ctx)
{
    return (ctx != NULL) ? ctx->host.error : "";
}

extern uint32_t hdrg_context_plugin_count(const hdrg_context_t *ctx)
{
    return (ctx != NULL) ? ctx->host.plugin_count : 0;
}

extern hdrg_status_t hdrg_context_plugin_info(const hdrg_context_t *ctx, uint32_t index,
                                              hdrg_plugin_info_t *out)
{
    const hdrg_plugin_t *plugin;
    uint32_t i;

    if (ctx == NULL || out == NULL || out->struct_size != sizeof(*out) ||
        index >= ctx->host.plugin_count) {
        return HDRG_ERR_INVALID_ARG;
    }

    plugin = ctx->host.plugins[index];

    out->id = hdrg_public_str(&plugin->id);
    out->name = hdrg_public_str(&plugin->name);
    out->version = hdrg_public_str(&plugin->version);
    out->vendor = hdrg_public_str(&plugin->vendor);
    out->license = hdrg_public_str(&plugin->license);
    out->copyright = hdrg_public_str(&plugin->copyright);
    out->status_message = hdrg_public_str(&plugin->message);
    out->abi_version = plugin->abi_version;
    out->status = plugin->status;
    out->reason = plugin->reason;
    out->component_count = plugin->component_count;
    out->license_text_count = plugin->text_count;
    out->filter_count = 0;
    out->codec_count = 0;
    for (i = 0; i < plugin->class_count; i++) {
        if (plugin->classes[i]->registered) {
            out->filter_count += (plugin->classes[i]->kind == HDRG_CLASS_FILTER) ? 1u : 0u;
            out->codec_count += (plugin->classes[i]->kind == HDRG_CLASS_CODEC) ? 1u : 0u;
        }
    }

    out->internal = plugin->internal;
    out->accepted = plugin->accepted;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_context_plugin_component(const hdrg_context_t *ctx, uint32_t plugin_index,
                                                   uint32_t index, hdrg_component_info_t *out)
{
    const hdrg_component_t *component;

    if (ctx == NULL || out == NULL || out->struct_size != sizeof(*out) ||
        plugin_index >= ctx->host.plugin_count ||
        index >= ctx->host.plugins[plugin_index]->component_count) {
        return HDRG_ERR_INVALID_ARG;
    }

    component = &ctx->host.plugins[plugin_index]->components[index];

    out->name = hdrg_public_str(&component->name);
    out->version = hdrg_public_str(&component->version);
    out->license = hdrg_public_str(&component->license);
    out->copyright = hdrg_public_str(&component->copyright);
    out->notice = hdrg_public_str(&component->notice);

    return HDRG_OK;
}

extern hdrg_status_t hdrg_context_plugin_license_text(const hdrg_context_t *ctx,
                                                      uint32_t plugin_index, uint32_t index,
                                                      const char **spdx_id, const char **text)
{
    const hdrg_license_text_t *entry;

    if (ctx == NULL || spdx_id == NULL || text == NULL || plugin_index >= ctx->host.plugin_count ||
        index >= ctx->host.plugins[plugin_index]->text_count) {
        return HDRG_ERR_INVALID_ARG;
    }

    entry = &ctx->host.plugins[plugin_index]->texts[index];
    *spdx_id = hdrg_public_str(&entry->id);
    *text = hdrg_public_str(&entry->text);

    return HDRG_OK;
}

extern uint32_t hdrg_context_filter_count(const hdrg_context_t *ctx)
{
    uint32_t i, n;

    if (ctx == NULL) {
        return 0;
    }

    n = 0;
    while (hdrg_public_class(&ctx->host, HDRG_CLASS_FILTER, n, &i) != NULL) {
        n++;
    }

    return n;
}

extern hdrg_status_t hdrg_context_filter_info(const hdrg_context_t *ctx, uint32_t index,
                                              hdrg_filter_info_t *out)
{
    const hdrg_class_t *cls;
    uint32_t plugin_index;

    if (ctx == NULL || out == NULL || out->struct_size != sizeof(*out)) {
        return HDRG_ERR_INVALID_ARG;
    }

    cls = hdrg_public_class(&ctx->host, HDRG_CLASS_FILTER, index, &plugin_index);
    if (cls == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    out->id = hdrg_public_str(&cls->id);
    out->plugin_index = plugin_index;
    out->stages = cls->stages;
    out->flags = cls->flags;
    out->option_count = cls->option_count;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_context_filter_option(const hdrg_context_t *ctx, uint32_t filter_index,
                                                uint32_t option_index, hdrg_option_info_t *out)
{
    const hdrg_class_t *cls;
    uint32_t plugin_index;

    if (ctx == NULL || out == NULL || out->struct_size != sizeof(*out)) {
        return HDRG_ERR_INVALID_ARG;
    }

    cls = hdrg_public_class(&ctx->host, HDRG_CLASS_FILTER, filter_index, &plugin_index);
    if (cls == NULL || option_index >= cls->option_count) {
        return HDRG_ERR_INVALID_ARG;
    }

    hdrg_public_option(&cls->options[option_index], out);

    return HDRG_OK;
}

extern uint32_t hdrg_context_codec_count(const hdrg_context_t *ctx)
{
    uint32_t i, n;

    if (ctx == NULL) {
        return 0;
    }

    n = 0;
    while (hdrg_public_class(&ctx->host, HDRG_CLASS_CODEC, n, &i) != NULL) {
        n++;
    }

    return n;
}

extern hdrg_status_t hdrg_context_codec_info(const hdrg_context_t *ctx, uint32_t index,
                                             hdrg_codec_info_t *out)
{
    const hdrg_class_t *cls;
    uint32_t plugin_index;

    if (ctx == NULL || out == NULL || out->struct_size != sizeof(*out)) {
        return HDRG_ERR_INVALID_ARG;
    }

    cls = hdrg_public_class(&ctx->host, HDRG_CLASS_CODEC, index, &plugin_index);
    if (cls == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    out->id = hdrg_public_str(&cls->id);
    out->extensions = hdrg_public_str(&cls->extensions);
    out->plugin_index = plugin_index;
    out->roles = ((cls->probe != NULL) ? HDRG_CODEC_READ : 0u) |
                 ((cls->write_create != NULL) ? HDRG_CODEC_WRITE : 0u);
    out->forms = cls->forms;
    out->default_form = cls->default_form;
    out->option_count = cls->option_count;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_context_codec_option(const hdrg_context_t *ctx, uint32_t codec_index,
                                               uint32_t option_index, hdrg_option_info_t *out)
{
    const hdrg_class_t *cls;
    uint32_t plugin_index;

    if (ctx == NULL || out == NULL || out->struct_size != sizeof(*out)) {
        return HDRG_ERR_INVALID_ARG;
    }

    cls = hdrg_public_class(&ctx->host, HDRG_CLASS_CODEC, codec_index, &plugin_index);
    if (cls == NULL || option_index >= cls->option_count) {
        return HDRG_ERR_INVALID_ARG;
    }

    hdrg_public_option(&cls->options[option_index], out);

    return HDRG_OK;
}

extern hdrg_status_t hdrg_options_create(hdrg_context_t *ctx, hdrg_options_t **out)
{
    hdrg_options_t *options;

    if (ctx == NULL || out == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *out = NULL;

    options = (hdrg_options_t *)hdrg_mem_calloc(&ctx->alloc, 1, sizeof(*options));
    if (options == NULL) {
        return HDRG_ERR_NOMEM;
    }

    options->magic = HDRG_OPTIONS_MAGIC;
    options->host = &ctx->host;
    *out = options;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_options_set(hdrg_options_t *options, const char *key, const char *value)
{
    const hdrg_allocator_t *alloc;
    hdrg_option_t *items, *item;
    uint32_t i;
    size_t key_len, value_len;
    char *key_copy, *value_copy;

    if (options == NULL || options->magic != HDRG_OPTIONS_MAGIC || options->resolved ||
        key == NULL || value == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    key_len = hdrg_public_strlen(key, HDRG_LIMIT_KEY);
    value_len = hdrg_public_strlen(value, HDRG_LIMIT_MESSAGE);
    if (key_len > HDRG_LIMIT_KEY || value_len > HDRG_LIMIT_MESSAGE ||
        !hdrg_valid_key(key, key_len) || !hdrg_valid_text(value, value_len, 0)) {
        hdrg_host_set_error(options->host, "invalid option key or value", NULL, 0);

        return HDRG_ERR_INVALID_ARG;
    }

    alloc = &options->host->alloc;
    item = NULL;
    for (i = 0; i < options->count; i++) {
        if (options->items[i].key.size == key_len &&
            memcmp(options->items[i].key.data, key, key_len) == 0) {
            item = &options->items[i];
        }
    }

    value_copy = (char *)hdrg_mem_alloc(alloc, value_len + 1);
    if (value_copy == NULL) {
        return HDRG_ERR_NOMEM;
    }

    memcpy(value_copy, value, value_len + 1);

    if (item != NULL) {
        hdrg_mem_free(alloc, item->value.data);

        item->value.data = value_copy;
        item->value.size = value_len;

        return HDRG_OK;
    }

    if (options->count >= HDRG_LIMIT_OPTIONS) {
        hdrg_mem_free(alloc, value_copy);
        hdrg_host_set_error(options->host, "too many options", NULL, 0);

        return HDRG_ERR_INVALID_ARG;
    }

    key_copy = (char *)hdrg_mem_alloc(alloc, key_len + 1);
    items = (hdrg_option_t *)hdrg_mem_realloc(alloc, options->items,
                                              (options->count + 1u) * sizeof(*items));
    if (key_copy == NULL || items == NULL) {
        hdrg_mem_free(alloc, key_copy);
        hdrg_mem_free(alloc, value_copy);

        if (items != NULL) {
            options->items = items;
        }

        return HDRG_ERR_NOMEM;
    }

    memcpy(key_copy, key, key_len + 1);

    options->items = items;
    item = &items[options->count];
    memset(item, 0, sizeof(*item));
    item->key.data = key_copy;
    item->key.size = key_len;
    item->value.data = value_copy;
    item->value.size = value_len;
    options->count++;

    return HDRG_OK;
}

extern void hdrg_options_destroy(hdrg_options_t *options)
{
    const hdrg_allocator_t *alloc;
    uint32_t i;

    if (options == NULL || options->magic != HDRG_OPTIONS_MAGIC) {
        return;
    }

    alloc = &options->host->alloc;
    for (i = 0; i < options->count; i++) {
        hdrg_mem_free(alloc, options->items[i].key.data);
        hdrg_mem_free(alloc, options->items[i].value.data);
    }

    hdrg_mem_free(alloc, options->items);
    hdrg_mem_free(alloc, options);
}

extern hdrg_status_t hdrg_context_add_filter(hdrg_context_t *ctx, uint32_t stage, const char *id,
                                             const hdrg_options_t *options)
{
    const hdrg_class_t *cls;
    hdrg_filter_entry_t *filters;
    hdrg_options_t *resolved;
    hdrg_status_t status;
    size_t id_len;

    if (ctx == NULL || id == NULL ||
        (options != NULL && (options->magic != HDRG_OPTIONS_MAGIC || options->resolved ||
                             options->host != &ctx->host))) {
        return HDRG_ERR_INVALID_ARG;
    }

    ctx->host.error[0] = '\0';

    if (stage != HDRG_STAGE_SCALE && stage != HDRG_STAGE_PREFILTER && stage != HDRG_STAGE_INTERP &&
        stage != HDRG_STAGE_ANALYZE && stage != HDRG_STAGE_STATS) {
        hdrg_host_set_error(&ctx->host, "not a stage", NULL, 0);

        return HDRG_ERR_INVALID_ARG;
    }

    id_len = hdrg_public_strlen(id, HDRG_LIMIT_ID);
    if (id_len > HDRG_LIMIT_ID || !hdrg_valid_id(id, id_len)) {
        hdrg_host_set_error(&ctx->host, "invalid filter id", NULL, 0);

        return HDRG_ERR_INVALID_ARG;
    }

    cls = hdrg_host_find_class(&ctx->host, id, id_len);
    if (cls == NULL) {
        hdrg_host_set_error(&ctx->host, "no usable filter with this id", id, id_len);

        return HDRG_ERR_NOT_FOUND;
    }

    if ((cls->stages & stage) == 0) {
        hdrg_host_set_error(&ctx->host, "filter does not support the stage", id, id_len);

        return HDRG_ERR_INVALID_ARG;
    }

    if (hdrg_host_stage_count(&ctx->host, stage) >=
        ((stage == HDRG_STAGE_PREFILTER) ? HDRG_PREFILTER_MAX : 1u)) {
        hdrg_host_set_error(&ctx->host, "the stage has no room for another filter", NULL, 0);

        return HDRG_ERR_INVALID_STATE;
    }

    status = hdrg_host_resolve_options(&ctx->host, cls, options, &resolved);
    if (status != HDRG_OK) {
        return status;
    }

    filters = (hdrg_filter_entry_t *)hdrg_mem_realloc(
        &ctx->alloc, ctx->host.filters, (ctx->host.filter_count + 1u) * sizeof(*filters));
    if (filters == NULL) {
        hdrg_options_destroy(resolved);

        return HDRG_ERR_NOMEM;
    }

    ctx->host.filters = filters;
    filters[ctx->host.filter_count].cls = cls;
    filters[ctx->host.filter_count].options = resolved;
    filters[ctx->host.filter_count].stage = stage;
    ctx->host.filter_count++;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_context_set_writer(hdrg_context_t *ctx, const char *id,
                                             const hdrg_options_t *options)
{
    const hdrg_class_t *cls;
    hdrg_options_t *resolved;
    hdrg_status_t status;
    size_t id_len;

    if (ctx == NULL || (options != NULL && options->host != &ctx->host)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (id == NULL && options == NULL) {
        hdrg_options_destroy(ctx->host.writer_options);
        ctx->host.writer_options = NULL;
        ctx->host.writer = NULL;

        return HDRG_OK;
    }

    /* the default writer's options */
    id = (id != NULL) ? id : hdrg_defaults[HDRG_DEFAULT_WRITER].id;
    id_len = hdrg_public_strlen(id, HDRG_LIMIT_ID);
    if (id_len > HDRG_LIMIT_ID || !hdrg_valid_id(id, id_len)) {
        hdrg_host_set_error(&ctx->host, "invalid codec id", NULL, 0);

        return HDRG_ERR_INVALID_ARG;
    }

    cls = hdrg_host_find_class(&ctx->host, id, id_len);
    if (cls == NULL || cls->kind != HDRG_CLASS_CODEC || cls->write_create == NULL) {
        hdrg_host_set_error(&ctx->host, "no usable writer with this id", id, id_len);

        return HDRG_ERR_NOT_FOUND;
    }

    status = hdrg_host_resolve_options(&ctx->host, cls, options, &resolved);
    if (status != HDRG_OK) {
        return status;
    }

    hdrg_options_destroy(ctx->host.writer_options);
    ctx->host.writer_options = resolved;
    ctx->host.writer = cls;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_context_set_device(hdrg_context_t *ctx, uint32_t kind, void *device,
                                             void *queue)
{
    if (ctx == NULL || kind == 0 || kind > HDRG_DEVICE_KINDS) {
        return HDRG_ERR_INVALID_ARG;
    }

    ctx->host.devices[kind - 1u][0] = device;
    ctx->host.devices[kind - 1u][1] = (device != NULL) ? queue : NULL;

    return HDRG_OK;
}

extern void hdrg_context_clear_filters(hdrg_context_t *ctx)
{
    uint32_t i;

    if (ctx == NULL) {
        return;
    }

    for (i = 0; i < ctx->host.filter_count; i++) {
        hdrg_options_destroy(ctx->host.filters[i].options);
    }

    ctx->host.filter_count = 0;
}

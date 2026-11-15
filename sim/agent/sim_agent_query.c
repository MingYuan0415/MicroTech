/** @file Agent-side widget query and geometry helpers.
 *
 * All entry points run under the caller-held LVGL lock. Queries never serialize
 * the whole screen: only matching nodes are converted to JSON.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "app_manager.h"
#include "app_manager_control_center.h"
#include "lvgl.h"
#include "core/lv_obj_class_private.h"
#include "core/lv_obj_private.h"

#include "sim_agent_query.h"
#include "sim_agent_tree.h"

#define SIM_QUERY_MAX_DEPTH 64

extern const lv_obj_class_t lv_label_class;

static const char *_class_name(lv_obj_t *obj)
{
    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    return (cls != NULL && cls->name != NULL) ? cls->name : "lv_obj";
}

static bool _bool_value(const cJSON *want, bool value)
{
    if (cJSON_IsBool(want))
    {
        return cJSON_IsTrue(want) == value;
    }
    if (cJSON_IsNumber(want))
    {
        return (want->valuedouble != 0.0) == value;
    }
    return false;
}

static bool _number_value(const cJSON *want, double value)
{
    if (cJSON_IsNumber(want))
    {
        return fabs(value - want->valuedouble) < 0.0001;
    }
    if (cJSON_IsObject(want))
    {
        const cJSON *minimum = cJSON_GetObjectItemCaseSensitive(want, "min");
        const cJSON *maximum = cJSON_GetObjectItemCaseSensitive(want, "max");
        if ((minimum != NULL) && !cJSON_IsNumber(minimum))
        {
            return false;
        }
        if ((maximum != NULL) && !cJSON_IsNumber(maximum))
        {
            return false;
        }
        return ((minimum == NULL) || value >= minimum->valuedouble) &&
               ((maximum == NULL) || value <= maximum->valuedouble);
    }
    return false;
}

static bool _query_number_or_range(const cJSON *item)
{
    const cJSON *minimum;
    const cJSON *maximum;
    const cJSON *member = NULL;

    if (cJSON_IsNumber(item))
    {
        return true;
    }
    if (!cJSON_IsObject(item))
    {
        return false;
    }
    minimum = cJSON_GetObjectItemCaseSensitive(item, "min");
    maximum = cJSON_GetObjectItemCaseSensitive(item, "max");
    if (minimum == NULL && maximum == NULL)
    {
        return false;
    }
    if (((minimum != NULL) && !cJSON_IsNumber(minimum)) ||
            ((maximum != NULL) && !cJSON_IsNumber(maximum)))
    {
        return false;
    }
    cJSON_ArrayForEach(member, item)
    {
        if ((member->string == NULL) ||
                ((strcmp(member->string, "min") != 0) &&
                 (strcmp(member->string, "max") != 0)))
        {
            return false;
        }
    }
    return true;
}

static bool _query_suffix_known(const char *key, const char *prefix,
                                const char *const *suffixes,
                                size_t suffix_count)
{
    const size_t prefix_len = strlen(prefix);
    if (strncmp(key, prefix, prefix_len) != 0)
    {
        return false;
    }
    for (size_t i = 0U; i < suffix_count; i++)
    {
        if (strcmp(key + prefix_len, suffixes[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

/** @return NULL when the query is valid, otherwise an API error string. */
static const char *_validate_query(const cJSON *query)
{
    static const char *const coord_suffixes[] = { "x", "y", "w", "h" };
    static const char *const flag_suffixes[] =
    {
        "visible", "hidden", "clickable", "scrollable", "floating"
    };
    static const char *const state_suffixes[] =
    {
        "pressed", "checked", "focused", "disabled"
    };
    const cJSON *item = NULL;

    if (!cJSON_IsObject(query) || cJSON_GetArraySize(query) == 0)
    {
        return "empty query";
    }
    cJSON_ArrayForEach(item, query)
    {
        const char *key = item->string;
        if (key == NULL)
        {
            return "unsupported query";
        }
        if (strcmp(key, "type") == 0 || strcmp(key, "text") == 0 ||
                strcmp(key, "app_id") == 0)
        {
            if (!cJSON_IsString(item))
            {
                return "invalid query";
            }
        }
        else if (strncmp(key, "coords.", 7) == 0)
        {
            if (!_query_suffix_known(key, "coords.", coord_suffixes,
                                     sizeof(coord_suffixes) /
                                     sizeof(coord_suffixes[0])) ||
                    !_query_number_or_range(item))
            {
                return "invalid query";
            }
        }
        else if (strncmp(key, "flags.", 6) == 0)
        {
            if (!_query_suffix_known(key, "flags.", flag_suffixes,
                                     sizeof(flag_suffixes) /
                                     sizeof(flag_suffixes[0])) ||
                    (!cJSON_IsBool(item) && !cJSON_IsNumber(item)))
            {
                return "invalid query";
            }
        }
        else if (strncmp(key, "state.", 6) == 0)
        {
            if (!_query_suffix_known(key, "state.", state_suffixes,
                                     sizeof(state_suffixes) /
                                     sizeof(state_suffixes[0])) ||
                    (!cJSON_IsBool(item) && !cJSON_IsNumber(item)))
            {
                return "invalid query";
            }
        }
        else
        {
            return "unsupported query";
        }
    }
    return NULL;
}

/** @return true when the object matches; sets unsupported for unknown keys. */
static bool _matches_obj(lv_obj_t *obj, const cJSON *query,
                         bool *unsupported)
{
    const cJSON *item = NULL;

    cJSON_ArrayForEach(item, query)
    {
        const char *key = item->string;
        if (key == NULL)
        {
            *unsupported = true;
            return false;
        }
        if (strcmp(key, "type") == 0)
        {
            if (!cJSON_IsString(item) ||
                    strcmp(_class_name(obj), item->valuestring) != 0)
            {
                return false;
            }
        }
        else if (strcmp(key, "text") == 0)
        {
            const char *label_text;
            if (lv_obj_get_class(obj) != &lv_label_class ||
                    !cJSON_IsString(item))
            {
                return false;
            }
            label_text = lv_label_get_text(obj);
            if ((label_text == NULL) ||
                    (strcmp(label_text, item->valuestring) != 0))
            {
                return false;
            }
        }
        else if (strcmp(key, "app_id") == 0)
        {
            const char *app_id = app_manager_control_center_card_identity(obj);
            if (!cJSON_IsString(item) || app_id == NULL ||
                    strcmp(app_id, item->valuestring) != 0)
            {
                return false;
            }
        }
        else if (strncmp(key, "coords.", 7) == 0)
        {
            lv_area_t coords;
            double value;
            lv_obj_get_coords(obj, &coords);
            if (strcmp(key + 7, "x") == 0)
            {
                value = (double)coords.x1;
            }
            else if (strcmp(key + 7, "y") == 0)
            {
                value = (double)coords.y1;
            }
            else if (strcmp(key + 7, "w") == 0)
            {
                value = (double)lv_area_get_width(&coords);
            }
            else if (strcmp(key + 7, "h") == 0)
            {
                value = (double)lv_area_get_height(&coords);
            }
            else
            {
                *unsupported = true;
                return false;
            }
            if (!_number_value(item, value))
            {
                return false;
            }
        }
        else if (strncmp(key, "flags.", 6) == 0)
        {
            const char *name = key + 6;
            bool value;
            if (strcmp(name, "visible") == 0)
            {
                value = lv_obj_is_visible(obj);
            }
            else if (strcmp(name, "hidden") == 0)
            {
                value = lv_obj_is_hidden(obj);
            }
            else if (strcmp(name, "clickable") == 0)
            {
                value = lv_obj_is_clickable(obj);
            }
            else if (strcmp(name, "scrollable") == 0)
            {
                value = lv_obj_is_scrollable(obj);
            }
            else if (strcmp(name, "floating") == 0)
            {
                value = lv_obj_is_floating(obj);
            }
            else
            {
                *unsupported = true;
                return false;
            }
            if (!_bool_value(item, value))
            {
                return false;
            }
        }
        else if (strncmp(key, "state.", 6) == 0)
        {
            const char *name = key + 6;
            const lv_state_t state = lv_obj_get_state(obj);
            bool value;
            if (strcmp(name, "pressed") == 0)
            {
                value = (state & LV_STATE_PRESSED) != 0U;
            }
            else if (strcmp(name, "checked") == 0)
            {
                value = (state & LV_STATE_CHECKED) != 0U;
            }
            else if (strcmp(name, "focused") == 0)
            {
                value = (state & LV_STATE_FOCUSED) != 0U;
            }
            else if (strcmp(name, "disabled") == 0)
            {
                value = (state & LV_STATE_DISABLED) != 0U;
            }
            else
            {
                *unsupported = true;
                return false;
            }
            if (!_bool_value(item, value))
            {
                return false;
            }
        }
        else
        {
            *unsupported = true;
            return false;
        }
    }
    return true;
}

static cJSON *_path_json(const int32_t *path, size_t path_len)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL)
    {
        return NULL;
    }
    for (size_t i = 0U; i < path_len; i++)
    {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(path[i]));
    }
    return array;
}

static bool _add_path_field(cJSON *object, const int32_t *path,
                            size_t path_len)
{
    cJSON *path_json = _path_json(path, path_len);
    if (path_json == NULL)
    {
        return false;
    }
    if (!cJSON_AddItemToObject(object, "path", path_json))
    {
        cJSON_Delete(path_json);
        return false;
    }
    return true;
}

static bool _append_match(cJSON *matches, const char *layer,
                          const int32_t *path, size_t path_len,
                          lv_obj_t *obj, uint32_t limit)
{
    char *node_text;
    cJSON *entry;
    cJSON *node;
    cJSON *path_json;

    if (cJSON_GetArraySize(matches) >= (int)limit)
    {
        return true;
    }
    node_text = sim_agent_tree_dump_object(obj, 0, 1U, NULL);
    if (node_text == NULL)
    {
        return false;
    }
    node = cJSON_Parse(node_text);
    free(node_text);
    if (node == NULL)
    {
        return false;
    }
    entry = cJSON_CreateObject();
    path_json = _path_json(path, path_len);
    if (entry == NULL || path_json == NULL)
    {
        cJSON_Delete(entry);
        cJSON_Delete(path_json);
        cJSON_Delete(node);
        return false;
    }
    if (cJSON_AddStringToObject(entry, "layer", layer) == NULL ||
            !cJSON_AddItemToObject(entry, "path", path_json))
    {
        cJSON_Delete(entry);
        cJSON_Delete(path_json);
        cJSON_Delete(node);
        return false;
    }
    path_json = NULL;
    if (!cJSON_AddItemToObject(entry, "node", node))
    {
        cJSON_Delete(entry);
        cJSON_Delete(node);
        return false;
    }
    node = NULL;
    if (!cJSON_AddItemToArray(matches, entry))
    {
        cJSON_Delete(entry);
        return false;
    }
    return true;
}

static bool _find_walk(lv_obj_t *obj, const char *layer, const cJSON *query,
                       bool visible_only, bool hidden_ancestor,
                       int32_t *path, size_t path_len, uint32_t limit,
                       uint32_t *total, cJSON *matches, bool *unsupported,
                       const char **error)
{
    const bool hidden = hidden_ancestor || lv_obj_is_hidden(obj);

    if ((!visible_only || (!hidden && lv_obj_is_visible(obj))) &&
            _matches_obj(obj, query, unsupported))
    {
        (*total)++;
        if (cJSON_GetArraySize(matches) < (int)limit &&
                !_append_match(matches, layer, path, path_len, obj, limit))
        {
            *error = "find serialization failed";
            return false;
        }
    }
    if (*unsupported)
    {
        *error = "unsupported query";
        return false;
    }
    for (uint32_t i = 0U; i < lv_obj_get_child_count(obj); i++)
    {
        if (path_len >= SIM_QUERY_MAX_DEPTH)
        {
            *error = "tree too deep";
            return false;
        }
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if (child == NULL)
        {
            continue;
        }
        path[path_len] = (int32_t)i;
        if (!_find_walk(child, layer, query, visible_only, hidden, path,
                        path_len + 1U, limit, total, matches, unsupported,
                        error))
        {
            return false;
        }
    }
    return true;
}

static lv_obj_t *_layer_root(const char *layer)
{
    if (layer == NULL || strcmp(layer, "screen") == 0)
    {
        return lv_screen_active();
    }
    if (strcmp(layer, "top") == 0)
    {
        return lv_layer_top();
    }
    if (strcmp(layer, "sys") == 0)
    {
        return lv_layer_sys();
    }
    if (strcmp(layer, "bottom") == 0)
    {
        return lv_layer_bottom();
    }
    return NULL;
}

char *sim_agent_query_find_active_screen(const char *query_json,
        bool include_layers,
        uint32_t limit,
        bool visible_only,
        const char **error)
{
    cJSON *query = NULL;
    cJSON *root = NULL;
    cJSON *matches = NULL;
    int32_t path[SIM_QUERY_MAX_DEPTH];
    uint32_t total = 0U;
    bool unsupported = false;

    *error = NULL;
    if (query_json == NULL)
    {
        *error = "empty query";
        return NULL;
    }
    query = cJSON_Parse(query_json);
    if (query == NULL || !cJSON_IsObject(query) ||
            cJSON_GetArraySize(query) == 0U)
    {
        cJSON_Delete(query);
        *error = "empty query";
        return NULL;
    }
    *error = _validate_query(query);
    if (*error != NULL)
    {
        cJSON_Delete(query);
        return NULL;
    }
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL)
    {
        cJSON_Delete(query);
        *error = "no active screen";
        return NULL;
    }
    root = cJSON_CreateObject();
    if (root == NULL)
    {
        cJSON_Delete(query);
        *error = "find serialization failed";
        return NULL;
    }
    matches = cJSON_AddArrayToObject(root, "matches");
    if (matches == NULL)
    {
        cJSON_Delete(query);
        cJSON_Delete(root);
        *error = "find serialization failed";
        return NULL;
    }
    lv_obj_update_layout(screen);
    if (!_find_walk(screen, "screen", query, visible_only, false,
                    path, 0U, limit, &total, matches, &unsupported, error))
    {
        cJSON_Delete(query);
        cJSON_Delete(root);
        return NULL;
    }
    if (include_layers)
    {
        const char *names[] = { "top", "sys" };
        for (size_t i = 0U; i < (sizeof(names) / sizeof(names[0])); i++)
        {
            lv_obj_t *layer_root = _layer_root(names[i]);
            if (layer_root != NULL)
            {
                lv_obj_update_layout(layer_root);
            }
            if (layer_root != NULL &&
                    !_find_walk(layer_root, names[i], query, visible_only,
                                false, path, 0U, limit, &total, matches,
                                &unsupported, error))
            {
                cJSON_Delete(query);
                cJSON_Delete(root);
                return NULL;
            }
        }
    }
    cJSON_AddNumberToObject(root, "total", (double)total);
    cJSON_AddBoolToObject(root, "truncated",
                          total > (uint32_t)cJSON_GetArraySize(matches));
    {
        char *text = cJSON_PrintUnformatted(root);
        cJSON_Delete(query);
        cJSON_Delete(root);
        if (text == NULL)
        {
            *error = "find serialization failed";
        }
        return text;
    }
}

static lv_obj_t *_resolve_path(lv_obj_t *root, const int32_t *path,
                               size_t path_len)
{
    lv_obj_t *obj = root;
    for (size_t i = 0U; i < path_len; i++)
    {
        if (path[i] < 0)
        {
            return NULL;
        }
        obj = lv_obj_get_child(obj, path[i]);
        if (obj == NULL)
        {
            return NULL;
        }
    }
    return obj;
}

char *sim_agent_query_inspect_active_screen(const char *layer,
        const int32_t *path,
        size_t path_len,
        uint32_t depth,
        uint32_t max_nodes,
        bool *truncated,
        const char **error)
{
    lv_obj_t *root = _layer_root(layer);
    lv_obj_t *obj;
    char *node_text;
    cJSON *node;
    cJSON *result;
    char *text;

    *error = NULL;
    if (root == NULL)
    {
        *error = "invalid layer";
        return NULL;
    }
    lv_obj_update_layout(root);
    obj = _resolve_path(root, path, path_len);
    if (obj == NULL)
    {
        *error = "object not found";
        return NULL;
    }
    node_text = sim_agent_tree_dump_object(obj, (int)depth, max_nodes, truncated);
    if (node_text == NULL)
    {
        *error = "inspect serialization failed";
        return NULL;
    }
    node = cJSON_Parse(node_text);
    free(node_text);
    if (node == NULL)
    {
        *error = "inspect serialization failed";
        return NULL;
    }
    result = cJSON_CreateObject();
    if (result == NULL)
    {
        cJSON_Delete(node);
        *error = "inspect serialization failed";
        return NULL;
    }
    cJSON_AddStringToObject(result, "layer",
                            layer != NULL ? layer : "screen");
    if (!_add_path_field(result, path, path_len))
    {
        cJSON_Delete(result);
        cJSON_Delete(node);
        *error = "inspect serialization failed";
        return NULL;
    }
    cJSON_AddItemToObject(result, "node", node);
    cJSON_AddBoolToObject(result, "truncated",
                          truncated != NULL && *truncated);
    text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (text == NULL)
    {
        *error = "inspect serialization failed";
    }
    return text;
}

static lv_obj_t *_hit_object(int32_t x, int32_t y, const char **layer)
{
    lv_point_t point = { .x = x, .y = y };
    const char *names[] = { "sys", "top", "screen", "bottom" };
    for (size_t i = 0U; i < (sizeof(names) / sizeof(names[0])); i++)
    {
        lv_obj_t *root = _layer_root(names[i]);
        lv_obj_t *found;
        if (root == NULL)
        {
            continue;
        }
        lv_obj_update_layout(root);
        found = lv_indev_search_obj(root, &point);
        if (found != NULL)
        {
            *layer = names[i];
            return found;
        }
    }
    *layer = "screen";
    return NULL;
}

static bool _path_to_root(lv_obj_t *obj, lv_obj_t *root, int32_t *path,
                          size_t *path_len)
{
    size_t length = 0U;
    while (obj != root)
    {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        int32_t index;
        if (parent == NULL || length >= SIM_QUERY_MAX_DEPTH)
        {
            return false;
        }
        index = lv_obj_get_index(obj);
        if (index < 0)
        {
            return false;
        }
        path[length++] = index;
        obj = parent;
    }
    for (size_t i = 0U; i < length / 2U; i++)
    {
        const int32_t swap = path[i];
        path[i] = path[length - 1U - i];
        path[length - 1U - i] = swap;
    }
    *path_len = length;
    return true;
}

char *sim_agent_query_hit_test(int32_t x, int32_t y, const char **error)
{
    const char *layer = "screen";
    lv_obj_t *found;
    int32_t path[SIM_QUERY_MAX_DEPTH];
    size_t path_len = 0U;
    cJSON *result;
    cJSON *node;
    char *node_text;
    char *text;

    *error = NULL;
    found = _hit_object(x, y, &layer);
    if (found == NULL)
    {
        result = cJSON_CreateObject();
        if (result == NULL)
        {
            *error = "hit-test serialization failed";
            return NULL;
        }
        cJSON_AddBoolToObject(result, "found", false);
        cJSON_AddStringToObject(result, "layer", layer);
        text = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);
        return text;
    }
    if (!_path_to_root(found, _layer_root(layer), path, &path_len))
    {
        *error = "hit-test path failed";
        return NULL;
    }
    node_text = sim_agent_tree_dump_object(found, 0, 1U, NULL);
    node = node_text != NULL ? cJSON_Parse(node_text) : NULL;
    free(node_text);
    if (node == NULL)
    {
        *error = "hit-test serialization failed";
        return NULL;
    }
    result = cJSON_CreateObject();
    if (result == NULL)
    {
        cJSON_Delete(node);
        *error = "hit-test serialization failed";
        return NULL;
    }
    cJSON_AddBoolToObject(result, "found", true);
    cJSON_AddStringToObject(result, "layer", layer);
    if (!_add_path_field(result, path, path_len))
    {
        cJSON_Delete(result);
        cJSON_Delete(node);
        *error = "hit-test serialization failed";
        return NULL;
    }
    cJSON_AddItemToObject(result, "node", node);
    text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (text == NULL)
    {
        *error = "hit-test serialization failed";
    }
    return text;
}

static cJSON *_geometry_object(lv_obj_t *obj)
{
    lv_area_t coords;
    lv_area_t content;
    lv_area_t click;
    cJSON *geometry = cJSON_CreateObject();
    cJSON *box;
    cJSON *scroll;

    if (geometry == NULL)
    {
        return NULL;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &coords);
    lv_obj_get_content_coords(obj, &content);
    lv_obj_get_click_area(obj, &click);
    box = cJSON_AddObjectToObject(geometry, "coords");
    if (box == NULL)
    {
        goto fail;
    }
    cJSON_AddNumberToObject(box, "x", coords.x1);
    cJSON_AddNumberToObject(box, "y", coords.y1);
    cJSON_AddNumberToObject(box, "w", lv_area_get_width(&coords));
    cJSON_AddNumberToObject(box, "h", lv_area_get_height(&coords));
    box = cJSON_AddObjectToObject(geometry, "content");
    if (box == NULL)
    {
        goto fail;
    }
    cJSON_AddNumberToObject(box, "x", content.x1);
    cJSON_AddNumberToObject(box, "y", content.y1);
    cJSON_AddNumberToObject(box, "w", lv_area_get_width(&content));
    cJSON_AddNumberToObject(box, "h", lv_area_get_height(&content));
    box = cJSON_AddObjectToObject(geometry, "click_area");
    if (box == NULL)
    {
        goto fail;
    }
    cJSON_AddNumberToObject(box, "x", click.x1);
    cJSON_AddNumberToObject(box, "y", click.y1);
    cJSON_AddNumberToObject(box, "w", lv_area_get_width(&click));
    cJSON_AddNumberToObject(box, "h", lv_area_get_height(&click));
    scroll = cJSON_AddObjectToObject(geometry, "scroll");
    if (scroll == NULL)
    {
        goto fail;
    }
    cJSON_AddNumberToObject(scroll, "dir", lv_obj_get_scroll_dir(obj));
    cJSON_AddNumberToObject(scroll, "left", lv_obj_get_scroll_left(obj));
    cJSON_AddNumberToObject(scroll, "right", lv_obj_get_scroll_right(obj));
    cJSON_AddNumberToObject(scroll, "top", lv_obj_get_scroll_top(obj));
    cJSON_AddNumberToObject(scroll, "bottom", lv_obj_get_scroll_bottom(obj));
    cJSON_AddNumberToObject(scroll, "x", (double)lv_obj_get_scroll_x(obj));
    cJSON_AddNumberToObject(scroll, "y", (double)lv_obj_get_scroll_y(obj));
    cJSON_AddNumberToObject(geometry, "ext_click_pad",
                            obj->spec_attr != NULL
                            ? (double)obj->spec_attr->ext_click_pad : 0.0);
    cJSON_AddBoolToObject(geometry, "overflow_visible",
                          lv_obj_is_overflow_visible(obj));
    return geometry;

fail:
    cJSON_Delete(geometry);
    return NULL;
}

char *sim_agent_query_geometry(int32_t x, int32_t y, bool use_point,
                               const char *layer, const int32_t *path,
                               size_t path_len, const char **error)
{
    lv_obj_t *obj;
    const char *resolved_layer = layer != NULL ? layer : "screen";
    int32_t hit_path[SIM_QUERY_MAX_DEPTH];
    size_t hit_path_len = 0U;
    cJSON *result;
    cJSON *geometry;
    char *text;

    *error = NULL;
    if (use_point)
    {
        obj = _hit_object(x, y, &resolved_layer);
        if (obj == NULL)
        {
            result = cJSON_CreateObject();
            if (result == NULL)
            {
                *error = "geometry serialization failed";
                return NULL;
            }
            cJSON_AddBoolToObject(result, "found", false);
            cJSON_AddStringToObject(result, "layer", resolved_layer);
            text = cJSON_PrintUnformatted(result);
            cJSON_Delete(result);
            return text;
        }
        if (!_path_to_root(obj, _layer_root(resolved_layer),
                           hit_path, &hit_path_len))
        {
            *error = "geometry path failed";
            return NULL;
        }
        path = hit_path;
        path_len = hit_path_len;
    }
    else
    {
        lv_obj_t *root = _layer_root(resolved_layer);
        if (root == NULL)
        {
            *error = "invalid layer";
            return NULL;
        }
        obj = _resolve_path(root, path, path_len);
        if (obj == NULL)
        {
            *error = "object not found";
            return NULL;
        }
    }
    result = cJSON_CreateObject();
    geometry = _geometry_object(obj);
    if (result == NULL || geometry == NULL)
    {
        cJSON_Delete(result);
        cJSON_Delete(geometry);
        *error = "geometry serialization failed";
        return NULL;
    }
    cJSON_AddBoolToObject(result, "found", true);
    cJSON_AddStringToObject(result, "layer", resolved_layer);
    if (!_add_path_field(result, path, path_len))
    {
        cJSON_Delete(result);
        cJSON_Delete(geometry);
        *error = "geometry serialization failed";
        return NULL;
    }
    cJSON_AddItemToObject(result, "geometry", geometry);
    text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (text == NULL)
    {
        *error = "geometry serialization failed";
    }
    return text;
}

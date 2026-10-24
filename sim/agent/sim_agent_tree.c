/** @file LVGL object-tree JSON dump for the Agent (read-only, under lock).
 *
 * Class names come from lv_obj_class_private.h (the public lv_obj_class_t is
 * opaque); type discrimination for specialized values uses the exported
 * class object pointers. Coordinates are absolute screen pixels.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "app_manager.h"
#include "app_resources_generated.h"
#include "lvgl.h"
#include "core/lv_obj_class_private.h"

#include "sim_agent_tree.h"

extern const lv_obj_class_t lv_label_class;
extern const lv_obj_class_t lv_image_class;
extern const lv_obj_class_t lv_slider_class;
extern const lv_obj_class_t lv_bar_class;
extern const lv_obj_class_t lv_switch_class;
extern const lv_obj_class_t lv_checkbox_class;
extern const lv_obj_class_t lv_dropdown_class;
extern const lv_obj_class_t lv_roller_class;
extern const lv_obj_class_t lv_arc_class;

typedef struct image_alias
{
    const lv_image_dsc_t *dsc;
    uint32_t semantic_id;
} image_alias_t;

static char *_color_hex(lv_color_t color)
{
    char *text = malloc(8);

    if (text != NULL)
    {
        snprintf(text, 8, "#%02x%02x%02x", (unsigned)color.red,
                 (unsigned)color.green, (unsigned)color.blue);
    }
    return text;
}

/** Record the byte offset of each UTF-8 code point into offsets[0..cap-1].
 *  offsets[count] receives the byte offset where the next code point starts,
 *  i.e. the end of the last recorded one.
 *  @return number of recorded code points (<= cap) */
static uint32_t _utf8_offsets(const char *text, uint32_t *offsets,
                              uint32_t cap)
{
    uint32_t count = 0;
    uint32_t i = 0;

    while (text[i] != '\0' && count < cap)
    {
        if (((unsigned char)text[i] & 0xC0U) != 0x80U)
        {
            offsets[count] = i;
            count++;
        }
        i++;
    }
    /* The cap may stop us inside a multi-byte sequence; the tail must start
     * at the next code point (or at the terminator) to stay valid UTF-8. */
    while (text[i] != '\0' && ((unsigned char)text[i] & 0xC0U) == 0x80U)
    {
        i++;
    }
    offsets[count] = i;
    return count;
}

static bool _add_line_entry(cJSON *lines, lv_obj_t *obj, const char *text,
                            uint32_t begin, uint32_t end)
{
    lv_point_t size;
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    const int32_t letter_space = lv_obj_get_style_text_letter_space(obj,
        LV_PART_MAIN);
    char *line;
    cJSON *entry;

    while (end > begin && (text[end - 1U] == '\n' || text[end - 1U] == '\r'))
    {
        end--;
    }
    if (end == begin)
    {
        return true;
    }
    line = malloc((size_t)(end - begin) + 1U);
    if (line == NULL)
    {
        return false;
    }
    memcpy(line, &text[begin], (size_t)(end - begin));
    line[end - begin] = '\0';
    lv_text_get_size(&size, line, font, letter_space, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_EXPAND);
    entry = cJSON_CreateObject();
    if (entry == NULL)
    {
        free(line);
        return false;
    }
    if (cJSON_AddStringToObject(entry, "text", line) == NULL ||
            cJSON_AddNumberToObject(entry, "w", size.x) == NULL ||
            !cJSON_AddItemToArray(lines, entry))
    {
        cJSON_Delete(entry);
        free(line);
        return false;
    }
    free(line);
    return true;
}

/* Ask LVGL where each code point lands and cut the text at every y change.
 * SCROLL / SCROLL_CIRCULAR / CLIP set LV_TEXT_FLAG_EXPAND inside the label,
 * so for those this returns the explicit "\n" lines only. */
static void _add_label_lines(cJSON *node, lv_obj_t *obj)
{
    enum { MAX_LINES = 64 };
    const char *text = lv_label_get_text(obj);
    const size_t byte_len = strlen(text);
    uint32_t *offsets;
    uint32_t leads;
    uint32_t line_start = 0;
    uint32_t emitted = 0;
    int32_t line_y;
    cJSON *lines = cJSON_AddArrayToObject(node, "lines");
    cJSON *complete = cJSON_AddBoolToObject(node, "lines_complete", false);
    lv_point_t pos;

    if (lines == NULL || complete == NULL)
    {
        return;
    }
    if (byte_len == 0U)
    {
        cJSON_SetBoolValue(complete, true);
        return;
    }
    /* Code points never outnumber bytes, so this also fits the terminator. */
    offsets = malloc(sizeof(uint32_t) * (byte_len + 1U));
    if (offsets == NULL)
    {
        return;
    }
    leads = _utf8_offsets(text, offsets, (uint32_t)byte_len);

    lv_label_get_letter_pos(obj, 0, &pos);
    line_y = pos.y;
    for (uint32_t k = 1; k < leads; k++)
    {
        lv_label_get_letter_pos(obj, k, &pos);
        if (pos.y == line_y)
        {
            continue;
        }
        if (emitted == MAX_LINES ||
                !_add_line_entry(lines, obj, text, line_start, offsets[k]))
        {
            free(offsets);
            return;
        }
        line_start = offsets[k];
        line_y = pos.y;
        emitted++;
    }
    if (emitted == MAX_LINES ||
            !_add_line_entry(lines, obj, text, line_start, offsets[leads]))
    {
        free(offsets);
        return;
    }
    free(offsets);
    cJSON_SetBoolValue(complete, true);
}


static void _add_style(cJSON *node, lv_obj_t *obj)
{
    cJSON *styles = cJSON_AddObjectToObject(node, "styles");
    lv_color_t bg = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
    lv_color_t fg = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    char *bg_hex = _color_hex(bg);
    char *fg_hex = _color_hex(fg);

    if (styles == NULL)
    {
        free(bg_hex);
        free(fg_hex);
        return;
    }
    if (bg_hex != NULL)
    {
        cJSON_AddStringToObject(styles, "bg_color", bg_hex);
    }
    if (fg_hex != NULL)
    {
        cJSON_AddStringToObject(styles, "text_color", fg_hex);
    }
    free(bg_hex);
    free(fg_hex);
    cJSON_AddNumberToObject(styles, "bg_opa",
                            (double)lv_obj_get_style_bg_opa(obj,
                                    LV_PART_MAIN));
    cJSON_AddNumberToObject(styles, "radius",
                            (double)lv_obj_get_style_radius(obj,
                                    LV_PART_MAIN));
    cJSON_AddNumberToObject(styles, "pad_left",
                            (double)lv_obj_get_style_pad_left(obj,
                                    LV_PART_MAIN));
    cJSON_AddNumberToObject(styles, "pad_right",
                            (double)lv_obj_get_style_pad_right(obj,
                                    LV_PART_MAIN));
    cJSON_AddNumberToObject(styles, "pad_top",
                            (double)lv_obj_get_style_pad_top(obj,
                                    LV_PART_MAIN));
    cJSON_AddNumberToObject(styles, "pad_bottom",
                            (double)lv_obj_get_style_pad_bottom(obj,
                                    LV_PART_MAIN));
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    if (font != NULL)
    {
        cJSON_AddNumberToObject(styles, "line_height",
                                (double)font->line_height);
    }
}

static void _add_type_value(cJSON *node, lv_obj_t *obj,
                            const image_alias_t *aliases, size_t alias_count)
{
    const lv_obj_class_t *cls = lv_obj_get_class(obj);

    if (cls == &lv_label_class)
    {
        const lv_label_long_mode_t long_mode = lv_label_get_long_mode(obj);
        lv_area_t text_area;

        cJSON_AddStringToObject(node, "text", lv_label_get_text(obj));
        cJSON_AddNumberToObject(node, "long_mode", (double)long_mode);
        cJSON_AddNumberToObject(node, "max_lines",
                                (double)lv_label_get_max_lines(obj));
        cJSON_AddNumberToObject(node, "line_space",
                                (double)lv_obj_get_style_text_line_space(obj,
                                        LV_PART_MAIN));
        /* Text set since the last layout pass leaves size/content stale; the
         * dump is a debug path, so settle it before measuring. */
        lv_obj_update_layout(obj);

        lv_obj_get_content_coords(obj, &text_area);
        cJSON_AddNumberToObject(node, "content_h",
                                lv_area_get_height(&text_area));
        _add_label_lines(node, obj);
        return;
    }
    if (cls == &lv_image_class)
    {
        const void *src = lv_image_get_src(obj);
        for (size_t i = 0; i < alias_count; i++)
        {
            if (src == (const void *)aliases[i].dsc)
            {
                char id[16];
                snprintf(id, sizeof(id), "0x%04X",
                         (unsigned)aliases[i].semantic_id);
                cJSON_AddStringToObject(node, "image_semantic_id", id);
                return;
            }
        }
        cJSON_AddStringToObject(node, "image_semantic_id", "unknown");
        return;
    }
    if (cls == &lv_slider_class)
    {
        cJSON_AddNumberToObject(node, "value",
                                (double)lv_slider_get_value(obj));
        cJSON_AddNumberToObject(node, "min",
                                (double)lv_slider_get_min_value(obj));
        cJSON_AddNumberToObject(node, "max",
                                (double)lv_slider_get_max_value(obj));
        return;
    }
    if (cls == &lv_bar_class)
    {
        cJSON_AddNumberToObject(node, "value", (double)lv_bar_get_value(obj));
        cJSON_AddNumberToObject(node, "min", (double)lv_bar_get_min_value(obj));
        cJSON_AddNumberToObject(node, "max", (double)lv_bar_get_max_value(obj));
        return;
    }
    if (cls == &lv_switch_class || cls == &lv_checkbox_class)
    {
        cJSON_AddBoolToObject(node, "checked",
                              lv_obj_has_state(obj, LV_STATE_CHECKED));
        return;
    }
    if (cls == &lv_dropdown_class)
    {
        cJSON_AddNumberToObject(node, "selected",
                                (double)lv_dropdown_get_selected(obj));
        return;
    }
    if (cls == &lv_roller_class)
    {
        cJSON_AddNumberToObject(node, "selected",
                                (double)lv_roller_get_selected(obj));
        return;
    }
    if (cls == &lv_arc_class)
    {
        cJSON_AddNumberToObject(node, "value", (double)lv_arc_get_value(obj));
    }
}

static cJSON *_dump_obj(lv_obj_t *obj, const image_alias_t *aliases,
                        size_t alias_count)
{
    cJSON *node = cJSON_CreateObject();
    lv_area_t coords;
    uint32_t state;

    if (node == NULL)
    {
        return NULL;
    }
    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    cJSON_AddStringToObject(node, "type",
                            (cls != NULL && cls->name != NULL) ? cls->name
                            : "lv_obj");
    lv_obj_get_coords(obj, &coords);
    cJSON *box = cJSON_AddObjectToObject(node, "coords");
    if (box != NULL)
    {
        cJSON_AddNumberToObject(box, "x", coords.x1);
        cJSON_AddNumberToObject(box, "y", coords.y1);
        cJSON_AddNumberToObject(box, "w", lv_area_get_width(&coords));
        cJSON_AddNumberToObject(box, "h", lv_area_get_height(&coords));
    }
    state = lv_obj_get_state(obj);
    cJSON *flags = cJSON_AddObjectToObject(node, "flags");
    if (flags != NULL)
    {
        cJSON_AddBoolToObject(flags, "visible", lv_obj_is_visible(obj));
        cJSON_AddBoolToObject(flags, "hidden",
                              lv_obj_is_hidden(obj));
        cJSON_AddBoolToObject(flags, "clickable",
                              lv_obj_is_clickable(obj));
        cJSON_AddBoolToObject(flags, "scrollable",
                              lv_obj_is_scrollable(obj));
        cJSON_AddBoolToObject(flags, "floating", lv_obj_is_floating(obj));
    }
    cJSON *states = cJSON_AddObjectToObject(node, "state");
    if (states != NULL)
    {
        cJSON_AddBoolToObject(states, "pressed", (state & LV_STATE_PRESSED)
                              != 0U);
        cJSON_AddBoolToObject(states, "checked", (state & LV_STATE_CHECKED)
                              != 0U);
        cJSON_AddBoolToObject(states, "focused", (state & LV_STATE_FOCUSED)
                              != 0U);
        cJSON_AddBoolToObject(states, "disabled", (state & LV_STATE_DISABLED)
                              != 0U);
    }
    cJSON *scroll = cJSON_AddObjectToObject(node, "scroll");
    if (scroll != NULL)
    {
        cJSON_AddNumberToObject(scroll, "dir", lv_obj_get_scroll_dir(obj));
        cJSON_AddNumberToObject(scroll, "left", lv_obj_get_scroll_left(obj));
        cJSON_AddNumberToObject(scroll, "right", lv_obj_get_scroll_right(obj));
        cJSON_AddNumberToObject(scroll, "top", lv_obj_get_scroll_top(obj));
        cJSON_AddNumberToObject(scroll, "bottom", lv_obj_get_scroll_bottom(obj));
        cJSON_AddNumberToObject(scroll, "y", (double)lv_obj_get_scroll_y(obj));
    }
    _add_style(node, obj);
    _add_type_value(node, obj, aliases, alias_count);

    const uint32_t child_count = lv_obj_get_child_count(obj);
    if (child_count > 0U)
    {
        cJSON *children = cJSON_AddArrayToObject(node, "children");
        for (uint32_t i = 0; i < child_count; i++)
        {
            lv_obj_t *child = lv_obj_get_child(obj, i);
            cJSON *kid = _dump_obj(child, aliases, alias_count);
            if (kid != NULL && children != NULL)
            {
                cJSON_AddItemToArray(children, kid);
            }
            else if (kid != NULL)
            {
                cJSON_Delete(kid);
            }
        }
    }
    return node;
}

char *sim_agent_tree_dump_active_screen(bool include_layers)
{
    lv_obj_t *scr = lv_screen_active();
    /* One spare slot keeps the array non-empty even when no images are compiled
     * in (APP_RESOURCES_IMAGE_COUNT is a sizeof expression, not a #if constant). */
    image_alias_t aliases[APP_RESOURCES_IMAGE_COUNT + 1];
    size_t alias_count = 0;
    char *text = NULL;

    if (scr == NULL)
    {
        return NULL;
    }
#if APP_RESOURCES_ENABLED
    const app_manager_image_resource_config_t *table =
        APP_RESOURCES_IMAGE_TABLE;
    for (size_t i = 0U; i < APP_RESOURCES_IMAGE_COUNT &&
            alias_count < (sizeof(aliases) / sizeof(aliases[0]));
            i++)
    {
        const lv_image_dsc_t *image = NULL;
        if (app_manager_get_image(table[i].semantic_id, &image) == ESP_OK &&
                image != NULL)
        {
            aliases[alias_count].dsc = image;
            aliases[alias_count].semantic_id = table[i].semantic_id;
            alias_count++;
        }
    }
#endif
    cJSON *root = _dump_obj(scr, aliases, alias_count);
    if ((root != NULL) && include_layers)
    {
        cJSON *overlays = cJSON_AddArrayToObject(root, "overlays");
        if (overlays != NULL)
        {
            lv_obj_t *const layers[] = { lv_layer_top(), lv_layer_sys() };
            for (size_t i = 0U; i < (sizeof(layers) / sizeof(layers[0])); i++)
            {
                cJSON *dump = (layers[i] != NULL)
                              ? _dump_obj(layers[i], aliases, alias_count)
                              : NULL;
                if (dump != NULL)
                {
                    cJSON_AddItemToArray(overlays, dump);
                }
            }
        }
    }
    if (root != NULL)
    {
        text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }
    return text;
}

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
        cJSON_AddStringToObject(node, "text", lv_label_get_text(obj));
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
                              lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN));
        cJSON_AddBoolToObject(flags, "clickable",
                              lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE));
        cJSON_AddBoolToObject(flags, "scrollable",
                              lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE));
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

char *sim_agent_tree_dump_active_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    image_alias_t aliases[64];
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
    if (root != NULL)
    {
        text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }
    return text;
}

#include <rg_system.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

#include "gui.h"

#define HEADER_HEIGHT       (50)
#define LOGO_WIDTH          (46)
#define LIST_LINE_COUNT     ((gui.height - (HEADER_HEIGHT + 6)) / rg_gui_get_info()->font_points)
#define PREVIEW_HEIGHT      ((int)(gui.height * 0.70f))
#define PREVIEW_WIDTH       ((int)(gui.width * 0.50f))

static const theme_t gui_themes[] = {
    {{C_TRANSPARENT, C_GRAY, C_TRANSPARENT, C_WHITE}},
    {{C_TRANSPARENT, C_GRAY, C_TRANSPARENT, C_GREEN}},
    {{C_TRANSPARENT, C_GRAY, C_WHITE, C_BLACK}},
    {{C_TRANSPARENT, C_DARK_GRAY, C_WHITE, C_BLACK}},
};
const int gui_themes_count = sizeof(gui_themes) / sizeof(theme_t);

retro_gui_t gui;

#define SETTING_SELECTED_TAB    "SelectedTab"
#define SETTING_START_SCREEN    "StartScreen"
#define SETTING_STARTUP_MODE    "StartupMode"
#define SETTING_GUI_THEME       "ColorTheme"
#define SETTING_SHOW_PREVIEW    "ShowPreview"
#define SETTING_TAB_SELECTION   "TabSelection"
#define SETTING_TAB_HIDDEN      "TabHidden"


void gui_init(void)
{
    gui = (retro_gui_t){
        .selected     = rg_settings_get_number(NS_APP, SETTING_SELECTED_TAB, 0),
        .theme        = rg_settings_get_number(NS_APP, SETTING_GUI_THEME, 0),
        .startup      = rg_settings_get_number(NS_APP, SETTING_STARTUP_MODE, 0),
        .start_screen = rg_settings_get_number(NS_APP, SETTING_START_SCREEN, 0),
        .show_preview = rg_settings_get_number(NS_APP, SETTING_SHOW_PREVIEW, 2),
        .width        = rg_display_get_status()->screen.width,
        .height       = rg_display_get_status()->screen.height,
    };
    // Always enter browse mode when leaving an emulator
    // boot reason should probably be abstracted by rg_system >_<
    gui.browse = gui.start_screen == 2 || (!gui.start_screen && esp_reset_reason() != ESP_RST_POWERON);
}

void gui_event(gui_event_t event, tab_t *tab)
{
    if (tab && tab->event_handler)
        (*tab->event_handler)(event, tab);
}

tab_t *gui_add_tab(const char *name, const char *desc, void *arg, void *event_handler)
{
    tab_t *tab = calloc(1, sizeof(tab_t));

    sprintf(tab->name, "%.63s", name);
    sprintf(tab->desc, "%.127s", desc);
    sprintf(tab->status[1].left, "Loading...");

    tab->event_handler = event_handler;
    tab->initialized = false;
    tab->enabled = !rg_settings_get_number(tab->name, SETTING_TAB_HIDDEN, 0);
    tab->is_empty = false;
    tab->arg = arg;
    tab->listbox.sort_mode = SORT_TEXT_ASC;
    tab->listbox.cursor = -1;

    gui.tabs[gui.tabcount++] = tab;

    RG_LOGI("Tab '%s' added at index %d\n", tab->name, gui.tabcount - 1);

    return tab;
}

void gui_init_tab(tab_t *tab)
{
    if (tab->initialized)
        return;

    tab->initialized = true;
    // tab->status[0] = 0;

    gui_event(TAB_INIT, tab);

    if (!tab->is_empty)
    {
        // gui_sort_list(tab);

        // -1 means that we should find our last saved position
        if (tab->listbox.cursor == -1)
        {
            tab->listbox.cursor = 0;
            char *selected = rg_settings_get_string(tab->name, SETTING_TAB_SELECTION, NULL);
            if (selected && strlen(selected) > 1)
            {
                for (int i = 0; i < tab->listbox.length; i++)
                {
                    if (strcmp(selected, tab->listbox.items[i].text) == 0)
                    {
                        tab->listbox.cursor = i;
                        break;
                    }
                }
            }
            free(selected);
        }
    }

    gui_event(TAB_SCROLL, tab);
}

tab_t *gui_get_tab(int index)
{
    return (index >= 0 && index < gui.tabcount) ? gui.tabs[index] : NULL;
}

const rg_image_t *gui_get_image(const char *type, const char *subtype)
{
    char path[256], name[64];

    if (subtype && *subtype)
        sprintf(name, "%s_%s.png", type, subtype);
    else
        sprintf(name, "%s.png", type);

    uint32_t fileid = crc32_le(0, (uint8_t *)name, strlen(name));
    image_t *image = gui.images;

    for (; image->id; ++image)
    {
        if (image->id == fileid)
            return image->img;
    }

    // Append to list
    image->id = fileid;

    // Try SD card, then search the built-ins
    sprintf(path, RG_BASE_PATH "/theme/%s", name);
    if (!(image->img = rg_image_load_from_file(path, 0)))
    {
        for (const binfile_t **img = builtin_images; *img; img++)
        {
            if (strcmp((*img)->name, name) == 0)
            {
                image->img = rg_image_load_from_memory((*img)->data, (*img)->size, 0);
                break;
            }
        }
    }

    if (strcmp(type, "background") == 0)
    {
        if (image->img && (image->img->width != gui.width || image->img->height != gui.height))
        {
            rg_image_t *temp = rg_image_copy_resampled(image->img, gui.width, gui.height, 0);
            if (temp)
            {
                rg_image_free(image->img);
                image->img = temp;
            }
        }
    }

    return image->img;
}

tab_t *gui_get_current_tab(void)
{
    return gui_get_tab(gui.selected);
}

tab_t *gui_set_current_tab(int index)
{
    index %= gui.tabcount;

    if (index < 0)
        index += gui.tabcount;

    gui.selected = index;

    return gui_get_tab(gui.selected);
}

void gui_set_status(tab_t *tab, const char *left, const char *right)
{
    if (!tab)
        tab = gui_get_current_tab();
    if (left)
        strcpy(tab->status[1].left, left);
    if (right)
        strcpy(tab->status[1].right, right);
}

void gui_save_config(bool commit)
{
    rg_settings_set_number(NS_APP, SETTING_SELECTED_TAB, gui.selected);
    rg_settings_set_number(NS_APP, SETTING_START_SCREEN, gui.start_screen);
    rg_settings_set_number(NS_APP, SETTING_SHOW_PREVIEW, gui.show_preview);
    rg_settings_set_number(NS_APP, SETTING_GUI_THEME, gui.theme);
    rg_settings_set_number(NS_APP, SETTING_STARTUP_MODE, gui.startup);

    for (int i = 0; i < gui.tabcount; i++)
    {
        tab_t *tab = gui.tabs[i];

        if (tab->enabled)
        {
            listbox_item_t *item = gui_get_selected_item(tab);
            rg_settings_set_string(tab->name, SETTING_TAB_SELECTION, item ? item->text : NULL);
            rg_settings_delete(tab->name, SETTING_TAB_HIDDEN);
        }
        else
        {
            rg_settings_delete(tab->name, SETTING_TAB_SELECTION);
            rg_settings_set_number(tab->name, SETTING_TAB_HIDDEN, 1);
        }
    }

    if (commit)
        rg_storage_commit();
}

listbox_item_t *gui_get_selected_item(tab_t *tab)
{
    listbox_t *list = &tab->listbox;

    if (list->cursor >= 0 && list->cursor < list->length)
        return &list->items[list->cursor];

    return NULL;
}

static int list_comp_text_asc(const void *a, const void *b)
{
    return strcasecmp(((listbox_item_t*)a)->text, ((listbox_item_t*)b)->text);
}

static int list_comp_text_desc(const void *a, const void *b)
{
    return strcasecmp(((listbox_item_t*)b)->text, ((listbox_item_t*)a)->text);
}

static int list_comp_id_asc(const void *a, const void *b)
{
    return ((listbox_item_t*)a)->id - ((listbox_item_t*)b)->id;
}

static int list_comp_id_desc(const void *a, const void *b)
{
    return ((listbox_item_t*)b)->id - ((listbox_item_t*)a)->id;
}

void gui_sort_list(tab_t *tab)
{
    void *comp[] = {&list_comp_id_asc, &list_comp_id_desc, &list_comp_text_asc, &list_comp_text_desc};
    int sort_mode = tab->listbox.sort_mode - 1;

    if (tab->is_empty || !tab->listbox.length)
        return;

    if (sort_mode < 0 || sort_mode > 3)
        return;

    qsort((void*)tab->listbox.items, tab->listbox.length, sizeof(listbox_item_t), comp[sort_mode]);
}

void gui_resize_list(tab_t *tab, int new_size)
{
    int cur_size = tab->listbox.length;

    if (new_size == cur_size)
        return;

    if (new_size == 0)
    {
        free(tab->listbox.items);
        tab->listbox.items = NULL;
    }
    else
    {
        tab->listbox.items = realloc(tab->listbox.items, new_size * sizeof(listbox_item_t));
        for (int i = cur_size; i < new_size; i++)
            memset(&tab->listbox.items[i], 0, sizeof(listbox_item_t));
    }

    tab->listbox.length = new_size;

    if (tab->listbox.cursor >= new_size)
        tab->listbox.cursor = new_size - 1;

    gui_event(TAB_SCROLL, tab);

    RG_LOGI("Resized list '%s' from %d to %d items\n", tab->name, cur_size, new_size);
}

void gui_scroll_list(tab_t *tab, scroll_mode_t mode, int arg)
{
    listbox_t *list = &tab->listbox;

    int cur_cursor = RG_MAX(RG_MIN(list->cursor, list->length - 1), 0);
    int old_cursor = list->cursor;
    int max = LIST_LINE_COUNT - 2;

    if (mode == SCROLL_ABSOLUTE)
    {
        cur_cursor = arg;
    }
    else if (mode == SCROLL_LINE_UP)
    {
        cur_cursor--;
    }
    else if (mode == SCROLL_LINE_DOWN)
    {
        cur_cursor++;
    }
    else if (mode == SCROLL_PAGE_UP)
    {
        char start = list->items[cur_cursor].text[0];
        while (--cur_cursor > 0 && max-- > 0)
        {
           if (start != list->items[cur_cursor].text[0]) break;
        }
    }
    else if (mode == SCROLL_PAGE_DOWN)
    {
        char start = list->items[cur_cursor].text[0];
        while (++cur_cursor < list->length - 1 && max-- > 0)
        {
           if (start != list->items[cur_cursor].text[0]) break;
        }
    }

    if (cur_cursor < 0) cur_cursor = list->length - 1;
    if (cur_cursor >= list->length) cur_cursor = 0;

    list->cursor = cur_cursor;
    gui_event(TAB_SCROLL, tab);

    if (cur_cursor != old_cursor)
    {
        gui_redraw();
    }
}

void gui_redraw(void)
{
    tab_t *tab = gui_get_current_tab();
    if (gui.browse)
    {
        gui_draw_background(tab, 4);
        gui_draw_header(tab, 0);
        gui_draw_status(tab);
        gui_draw_list(tab);
        if (tab->preview)
        {
            int height = RG_MIN(tab->preview->height, PREVIEW_HEIGHT);
            int width = RG_MIN(tab->preview->width, PREVIEW_WIDTH);
            rg_gui_draw_image(-width, -height, width, height, tab->preview);
        }
    }
    else
    {
        gui_draw_background(tab, 0);
        gui_draw_header(tab, (gui.height - HEADER_HEIGHT) / 2);
    }
    rg_gui_flush();
}

void gui_draw_background(tab_t *tab, int shade)
{
    static rg_image_t *buffer = NULL;
    static void *buffer_content = 0;

    const rg_image_t *img = gui_get_image("background", tab->name);

    if (img && shade > 0)
    {
        // Only regenerate the shaded buffer if the background has changed
        if (buffer_content != (void*)img + shade)
        {
            if (!buffer) buffer = rg_image_alloc(img->width, img->height);
            for (int x = 0; x < buffer->width * buffer->height; ++x)
            {
                int pixel = img->data[x];
                int r = ((pixel >> 11) & 0x1F) / shade;
                int g = ((pixel >> 5) & 0x3F) / shade;
                int b = ((pixel) & 0x1F) / shade;
                buffer->data[x] = ((r & 0x1F) << 11) | ((g & 0x3F) << 5) | ((b & 0x1F) << 0);
            }
            buffer_content = (void*)img + shade;
        }
        img = buffer;
    }

    rg_gui_draw_image(0, 0, gui.width, gui.height, img);
}

void gui_draw_header(tab_t *tab, int offset)
{
    rg_gui_draw_image(0, offset, LOGO_WIDTH, HEADER_HEIGHT, gui_get_image("logo", tab->name));
    rg_gui_draw_image(LOGO_WIDTH + 1, offset + 8, 0, HEADER_HEIGHT - 8, gui_get_image("banner", tab->name));
}

void gui_draw_status(tab_t *tab)
{
    const int status_x = LOGO_WIDTH + 12;
    const int status_y = HEADER_HEIGHT - 16;
    char *txt_left = tab->status[tab->status[1].left[0] ? 1 : 0].left;
    char *txt_right = tab->status[tab->status[1].right[0] ? 1 : 0].right;

    rg_gui_draw_battery(-27, 3);
    rg_gui_draw_text(status_x, status_y, gui.width, txt_right, C_SNOW, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
    rg_gui_draw_text(status_x, status_y, 0, txt_left, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);
}

void gui_draw_list(tab_t *tab)
{
    const theme_t *theme = &gui_themes[gui.theme % gui_themes_count];
    rg_color_t fg[2] = {theme->list.standard_fg, theme->list.selected_fg};
    rg_color_t bg[2] = {theme->list.standard_bg, theme->list.selected_bg};

    const listbox_t *list = &tab->listbox;
    int top = HEADER_HEIGHT + 6;

    for (int i = 0, lines = LIST_LINE_COUNT; i < lines; i++)
    {
        int idx = list->cursor + i - (lines / 2);
        int selected = idx == list->cursor;
        char *label = (idx >= 0 && idx < list->length) ? list->items[idx].text : "";

        top += rg_gui_draw_text(0, top, gui.width, label, fg[selected], bg[selected], 0).height;
    }
}

void gui_load_preview(tab_t *tab)
{
    bool show_missing_cover = false;
    uint32_t order;
    char path[RG_PATH_MAX + 1];

    rg_image_free(tab->preview);
    tab->preview = NULL;

    if (!gui_get_selected_item(tab))
        return;

    switch (gui.show_preview)
    {
        case PREVIEW_MODE_COVER_SAVE:
            show_missing_cover = true;
            order = 0x3124;
            break;
        case PREVIEW_MODE_SAVE_COVER:
            show_missing_cover = true;
            order = 0x1243;
            break;
        case PREVIEW_MODE_COVER_ONLY:
            show_missing_cover = true;
            order = 0x0124;
            break;
        case PREVIEW_MODE_SAVE_ONLY:
            show_missing_cover = false;
            order = 0x0003;
            break;
        default:
            show_missing_cover = false;
            order = 0x0000;
    }

    retro_emulator_file_t *file = gui_get_selected_item(tab)->arg;
    const char *dirname = file->emulator->short_name;
    uint32_t errors = 0;

    while (order && !tab->preview)
    {
        int type = order & 0xF;

        order >>= 4;

        if (file->missing_cover & (1 << type))
            continue;

        if (type == 0x1 || type == 0x2)
            emulator_get_file_crc32(file);

        // Give up on any button press to improve responsiveness
        if ((gui.joystick |= rg_input_read_gamepad()))
            break;

        if (type == 0x1) // Game cover (old format)
            sprintf(path, RG_BASE_PATH_COVERS "/%s/%X/%08X.art", dirname, file->checksum >> 28, file->checksum);
        else if (type == 0x2) // Game cover (png)
            sprintf(path, RG_BASE_PATH_COVERS "/%s/%X/%08X.png", dirname, file->checksum >> 28, file->checksum);
        else if (type == 0x3) // Save state screenshot (png)
            sprintf(path, RG_BASE_PATH_SAVES "/%s/%s.png", file->folder + strlen(RG_BASE_PATH_ROMS), file->name);
        else if (type == 0x4) // Game cover (based on filename)
            sprintf(path, RG_BASE_PATH_COVERS "/%s/%s.png", dirname, file->name);
        else if (type == 0xF) // use generic cover image (not currently used)
            sprintf(path, RG_BASE_PATH_COVERS "/%s/default.png", dirname);
        else
            continue;

        if (access(path, F_OK) == 0)
        {
            tab->preview = rg_image_load_from_file(path, 0);
            if (!tab->preview)
                errors++;
        }

        file->missing_cover |= (tab->preview ? 0 : 1) << type;
    }

    if (tab->preview)
    {
        if (tab->preview->width > PREVIEW_WIDTH || tab->preview->height > PREVIEW_HEIGHT)
        {
            rg_image_t *temp = rg_image_copy_resampled(tab->preview, PREVIEW_WIDTH, PREVIEW_HEIGHT, 0);
            if (temp)
            {
                rg_image_free(tab->preview);
                tab->preview = temp;
            }
        }
    }
    else if (file->checksum && (show_missing_cover || errors))
    {
        RG_LOGI("No image found for '%s'\n", file->name);
        gui_set_status(tab, NULL, errors ? "Bad cover" : "No cover");
        // gui_draw_status(tab);
    }
}

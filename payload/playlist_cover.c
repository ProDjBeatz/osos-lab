/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Playlist photos.
 *
 * Hold Center on a playlist and choose Set Playlist Photo: the Photos grid (All
 * Photos) opens, and clicking a photo makes it that playlist's cover. The photo is
 * center-cropped to a square, like album artwork, shrunk to the size of the
 * playlist icon, and drawn in place of the icon on the Playlists list (and the
 * Add to Playlist chooser, which shares it).
 *
 * Covers are kept in iPod_Control\Device\CFWCovers as ready-to-draw bitmaps keyed
 * by playlist persistent ID (and, for playlists made on the iPod, whose ID does
 * not last, by a hash of their own name, as Add to Playlist does).
 */
#include "osos.h"
#include "cfw.h"
#include "patch.h"
#include "preferences.h"
#include "patches/sites.h"
#include "playlist_add.h"
#include "playlist_rename.h"
#include "playlist_cover.h"
#include "playlist_cover_ui.h"

#define COVERS_MAGIC 0x50574643u /* "CFWP" */
#define COVERS_VERSION 1
#define COVERS_MAX 32
#define COVER_SIZE 24
#define COVER_BYTES (COVER_SIZE * COVER_SIZE * 4)

/* A native BMap resource: 28-byte header, then 32-bit pixels (B, G, R, A). */
struct cover_bitmap {
    uint16_t format, flags, stride, depth;
    int32_t top, left, bottom, right;
    uint32_t size;
    uint8_t pixels[COVER_BYTES];
};

struct cover {
    uint32_t list[2];
    uint32_t list_name;
    uint32_t reserved;
    struct cover_bitmap bitmap;
};

struct cover_file {
    uint32_t magic, version, count, reserved;
    struct cover entries[COVERS_MAX];
};

static struct cover_file covers;
static int covers_loaded;

static const struct osos_path covers_path = {0, "iPod_Control\\Device\\CFWCovers"};

/* Native playlist row reference (OSOS_PLAYLISTS_ROW_ITEM). */
struct playlist_ref {
    uintptr_t *vtable;
    void *list;
    uint32_t reserved[6];
};

/* Playlist waiting for a photo; NULL when not choosing one. */
static void *target_list;
/* Set by Set Playlist Photo until the Photos menu it opens exists. */
static int pick_pending;

static const uint32_t *list_id(void *list)
{
    return (const uint32_t *)((const uint8_t *)list + OSOS_PLAYLIST_PERSISTENT_ID);
}

static void load_covers(void)
{
    if (covers_loaded)
        return;
    covers_loaded = 1;
    if (!cfw_preferences_read(&covers_path, &covers, sizeof(covers)) ||
        covers.magic != COVERS_MAGIC || covers.version != COVERS_VERSION ||
        covers.count > COVERS_MAX)
        covers.count = 0;
    covers.magic = COVERS_MAGIC;
    covers.version = COVERS_VERSION;
}

static int is_list(void *list)
{
    return list && *(const uint32_t *)((const uint8_t *)list + 4) == OSOS_LIST_MAGIC;
}

static struct cover *find_cover(void *list)
{
    if (!is_list(list))
        return NULL;
    load_covers();
    if (!covers.count)
        return NULL;
    const uint32_t *id = list_id(list);
    for (uint32_t i = 0; i < covers.count; i++) {
        struct cover *cover = &covers.entries[i];
        if ((id[0] || id[1]) && cover->list[0] == id[0] && cover->list[1] == id[1])
            return cover;
    }
    uint32_t name = cfw_playlist_name_hash(list);
    for (uint32_t i = 0; name && i < covers.count; i++)
        if (covers.entries[i].list_name == name)
            return &covers.entries[i];
    return NULL;
}

/* ---- Making a cover from a photo ---- */

/* Photo pixels as the Photos browse model hands them to its views. */
struct photo_image {
    const uint8_t *pixels;
    uint32_t unused;
    int32_t height, width, stride;
    uint32_t reserved;
    int32_t left, top;
    uint32_t extra;
    uint16_t format;
    uint16_t padding;
};

static int photo_image(int index, int size_class, struct photo_image *image)
{
    typedef int (*get)(void *, struct photo_image *, int, int);
    typedef void *(*model_get)(void);
    void *model = ((model_get)OSOS_PHOTOS_BROWSE_MODEL_GET)();
    image->pixels = NULL;
    return model && ((get)OSOS_PHOTOS_BROWSE_IMAGE)(model, image, index, size_class) &&
           image->pixels && image->width > 0 && image->height > 0 && image->stride > 0;
}

/* Source pixel as 8-bit R, G, B; 0 for an unknown pixel format. */
static int read_pixel(const struct photo_image *image, int x, int y, uint32_t *rgb)
{
    const uint8_t *row = image->pixels + (image->top + y) * image->stride;
    uint32_t format = image->format & ~0x2000u;
    if (format == 0x565 || format == 0x555) {
        const uint8_t *p = row + (image->left + x) * 2;
        uint32_t v = p[0] | (uint32_t)p[1] << 8;
        uint32_t r, g, b;
        if (format == 0x565) {
            r = (v >> 11) & 31;
            g = (v >> 5) & 63;
            b = v & 31;
            g = (g << 2) | (g >> 4);
        } else {
            r = (v >> 10) & 31;
            g = (v >> 5) & 31;
            b = v & 31;
            g = (g << 3) | (g >> 2);
        }
        rgb[0] = (r << 3) | (r >> 2);
        rgb[1] = g;
        rgb[2] = (b << 3) | (b >> 2);
        return 1;
    }
    if (format == 0x1888) {
        const uint8_t *p = row + (image->left + x) * 4;
        rgb[0] = p[2];
        rgb[1] = p[1];
        rgb[2] = p[0];
        return 1;
    }
    return 0;
}

/* Center square of the photo, averaged down to COVER_SIZE. */
static int make_cover(const struct photo_image *image, struct cover_bitmap *bitmap)
{
    int side = image->width < image->height ? image->width : image->height;
    int x0 = (image->width - side) / 2, y0 = (image->height - side) / 2;
    uint32_t rgb[3];
    if (side < 1 || !read_pixel(image, x0, y0, rgb))
        return 0;
    bitmap->format = 0x1888;
    bitmap->flags = 0;
    bitmap->stride = COVER_SIZE * 4;
    bitmap->depth = 32;
    bitmap->top = 0;
    bitmap->left = 0;
    bitmap->bottom = COVER_SIZE;
    bitmap->right = COVER_SIZE;
    bitmap->size = COVER_BYTES;
    for (int oy = 0; oy < COVER_SIZE; oy++) {
        int ya = y0 + oy * side / COVER_SIZE, yb = y0 + (oy + 1) * side / COVER_SIZE;
        if (yb <= ya)
            yb = ya + 1;
        for (int ox = 0; ox < COVER_SIZE; ox++) {
            int xa = x0 + ox * side / COVER_SIZE, xb = x0 + (ox + 1) * side / COVER_SIZE;
            if (xb <= xa)
                xb = xa + 1;
            uint32_t red = 0, green = 0, blue = 0, n = 0;
            for (int y = ya; y < yb; y++)
                for (int x = xa; x < xb; x++) {
                    read_pixel(image, x, y, rgb);
                    red += rgb[0];
                    green += rgb[1];
                    blue += rgb[2];
                    n++;
                }
            uint8_t *out = &bitmap->pixels[(oy * COVER_SIZE + ox) * 4];
            out[0] = (uint8_t)(blue / n);
            out[1] = (uint8_t)(green / n);
            out[2] = (uint8_t)(red / n);
            out[3] = 255;
        }
    }
    return 1;
}

static void log_line(const char *what, uint32_t a, uint32_t b);

static struct cover_bitmap scratch;

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static int set_cover(void *list, int index)
{
    struct photo_image image;
    /* The large (full screen) image first; the grid thumbnail if that fails. */
    int made = 0;
    for (int size_class = 3; !made && size_class >= 0; size_class -= 3) {
        int found = photo_image(index, size_class, &image);
        log_line(found ? "image" : "no-image", (uint32_t)size_class,
                 found ? (uint32_t)image.format : 0);
        if (!found)
            continue;
        log_line("size", (uint32_t)image.width << 16 | (uint32_t)image.height,
                 (uint32_t)image.stride);
        made = make_cover(&image, &scratch);
    }
    if (!made)
        return 0;
    struct cover *cover = find_cover(list);
    if (!cover) {
        if (covers.count == COVERS_MAX)
            return 0;
        cover = &covers.entries[covers.count++];
    }
    const uint32_t *id = list_id(list);
    cover->list[0] = id[0];
    cover->list[1] = id[1];
    cover->list_name = cfw_playlist_name_hash(list);
    cover->reserved = 0;
    /* Word copy: the payload has no memcpy. */
    const uint32_t *from = (const uint32_t *)&scratch;
    uint32_t *to = (uint32_t *)&cover->bitmap;
    for (uint32_t i = 0; i < sizeof(scratch) / 4; i++)
        to[i] = from[i];
    cfw_preferences_write(&covers_path, &covers, sizeof(covers));
    return 1;
}

/* ---- Drawing ---- */

/* Playlists model property getter: a row's 'Draw' value is the
   bitmap of its icon (23x24; the Now Playing speaker is smaller and stays). */
PATCH_ARM int cfw_playlists_property(void *model, uint32_t key, uint32_t part, int row,
                                     void **out)
{
    typedef int (*property)(void *, uint32_t, uint32_t, int, void **);
    typedef int (*map_row)(void *, int);
    typedef void (*row_item)(struct playlist_ref *, void *, int);
    typedef void (*ref_destroy)(struct playlist_ref *);
    typedef void (*load)(void *, const void *, uint32_t, uint32_t);
    int result = ((property)OSOS_PLAYLISTS_PROPERTY)(model, key, part, row, out);
    if (!result || key != OSOS_RESOURCE_DRAW || row < 0 || !out || !*out)
        return result;
    load_covers();
    if (!covers.count)
        return result;
    const int32_t *rect = (const int32_t *)((const uint8_t *)*out + OSOS_BITMAP_RECT);
    int32_t height = rect[2] - rect[0], width = rect[3] - rect[1];
    if (width < 20 || width > 30 || height < 20 || height > 30)
        return result;
    struct playlist_ref ref;
    ref.vtable = NULL;
    ref.list = NULL;
    ((row_item)OSOS_PLAYLISTS_ROW_ITEM)(&ref, model,
                                        ((map_row)OSOS_PLAYLISTS_MAP_ROW)(model, row));
    struct cover *cover = find_cover(ref.list);
    ((ref_destroy)OSOS_LIST_REF_DESTROY)(&ref);
    if (cover)
        ((load)OSOS_BITMAP_LOAD_RESOURCE)(*out, &cover->bitmap, 0, 0);
    return result;
}

/* ---- Choosing the photo ---- */

/* Playlists hold menu: Set Playlist Photo. */
void cfw_playlist_cover_menu(struct osos_context_menu *menu)
{
    osos_context_menu_append(menu, CFW_SET_PLAYLIST_PHOTO, "CFW_SetPlaylistPhoto", 1);
}

static void *returning;

/* Picker log (iPod_Control\Device\CFWDebugPhoto): the last events of a pick,
   so a failed one can be read back from the disk. */
static struct {
    uint32_t magic, count;
    char lines[30][32];
} photo_log;
static const struct osos_path photo_log_path = {0, "iPod_Control\\Device\\CFWDebugPhoto"};

static void log_line(const char *what, uint32_t a, uint32_t b)
{
    static const char hex[] = "0123456789abcdef";
    char *line = photo_log.lines[photo_log.count % 30];
    int n = 0;
    for (; what[n] && n < 13; n++)
        line[n] = what[n];
    const uint32_t values[2] = {a, b};
    for (int v = 0; v < 2; v++) {
        line[n++] = ' ';
        for (int shift = 28; shift >= 0; shift -= 4)
            line[n++] = hex[(values[v] >> shift) & 15];
    }
    while (n < 31)
        line[n++] = ' ';
    line[31] = '\n';
    photo_log.magic = 0x47474f4c;
    photo_log.count++;
    cfw_preferences_write(&photo_log_path, &photo_log, sizeof(photo_log));
}

/* Playlist waiting for a photo while picking; picking ends with a pick or when
   Photos is next opened some other way. */
static int picking;

int cfw_playlist_cover_action(void *controller, const char *action, uint32_t argument)
{
    (void)controller;
    if (!cfw_string_equal(action, "CFW_SetPlaylistPhoto"))
        return 0;
    if (argument == OSOS_ACTION_SUPPORT_QUERY)
        return 1;
    target_list = cfw_playlist_held();
    pick_pending = target_list != NULL;
    picking = 0;
    log_line("set-photo", (uint32_t)(uintptr_t)target_list, 0);
    /* playlist_cover.ui then opens Photos, as the main menu does. */
    return 1;
}

/* A Photos menu controller: the one Set Playlist Photo opened starts picking;
   any other one (Photos opened from the main menu) ends a pick left unfinished. */
static void *menu_claim(void *controller)
{
    returning = NULL;
    picking = pick_pending;
    pick_pending = 0;
    if (!picking)
        target_list = NULL;
    log_line("photos-menu", (uint32_t)picking, (uint32_t)(uintptr_t)controller);
    return controller;
}

PATCH_ARM void *cfw_photos_menu_new(void *arg)
{
    typedef void *(*make)(void *);
    return menu_claim(((make)OSOS_PHOTOS_MENU_NEW)(arg));
}

PATCH_ARM void *cfw_photos_menu_new_transition(void *a, void *b, void *c, void *d)
{
    typedef void *(*make)(void *, void *, void *, void *);
    return menu_claim(((make)OSOS_PHOTOS_MENU_NEW_TRANSITION)(a, b, c, d));
}

/* Photos browse controller that just took a photo: its grid's Center event then
   pushes the full-size photo screen (PhotoBrowse_Template_Small), which
   cfw_navigator_action turns into the way back to the Playlists list (declared
   above). */

/* TPhotosBrowseCntlr actions: while picking, Center in the grid takes the photo. */
PATCH_ARM int cfw_photo_browse_action(void *controller, const char *action,
                                      uint32_t argument)
{
    typedef int (*dispatch)(void *, const char *, uint32_t);
    typedef void *(*model_get)(void);
    int take = picking && target_list && argument != OSOS_ACTION_SUPPORT_QUERY &&
               cfw_string_equal(action, "HandleBrowseLarge");
    if (!take)
        return ((dispatch)OSOS_PHOTOS_BROWSE_ACTION)(controller, action, argument);
    uint8_t *model = ((model_get)OSOS_PHOTOS_BROWSE_MODEL_GET)();
    void *list = target_list;
    target_list = NULL;
    picking = 0;
    int index = model ? *(int *)(model + OSOS_PHOTOS_BROWSE_CURRENT) : -1;
    int done = model && list && index >= 0 ? set_cover(list, index) : 0;
    log_line("picked", (uint32_t)index, (uint32_t)done);
    returning = controller;
    return 1;
}

/* A screen event's navigator actions run one by one through
   Controller_ExecuteNavigatorAction(controller, action, argument, stop); action
   starts with its name. After a pick, the grid's push of the full-size photo
   becomes two pops instead (photo grid, then Photos menu), each as
   navigator.PopTopScreen does it, and stop ends that event's actions. */
PATCH_ARM void cfw_navigator_action(void *controller, const char **action, void *argument,
                                    uint8_t *stop)
{
    typedef void (*execute)(void *, const char **, void *, uint8_t *);
    if (!returning || controller != returning || !action || !*action ||
        !cfw_string_equal(*action, "navigator.PushScreen")) {
        ((execute)OSOS_NAVIGATOR_EXECUTE_ACTION)(controller, action, argument, stop);
        return;
    }
    returning = NULL;
    *stop = 1;
    log_line("return", 0, 0);
    ((void (*)(int))OSOS_NAVIGATOR_POP_TOP)(OSOS_NAVIGATOR_POP_FLAGS);
    ((void (*)(int))OSOS_NAVIGATOR_POP_TOP)(OSOS_NAVIGATOR_POP_FLAGS);
}

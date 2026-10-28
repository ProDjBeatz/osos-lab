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
/* Set by Set Playlist Photo until the Photos browse controller it opens exists. */
static int pick_pending;
/* That Photos browse controller; the normal Photos app is left alone. */
static void *pick_controller;

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

static struct cover_bitmap scratch;

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static int set_cover(void *list, int index)
{
    struct photo_image image;
    /* The large (full screen) image first; the grid thumbnail if that fails. */
    if ((!photo_image(index, 3, &image) || !make_cover(&image, &scratch)) &&
        (!photo_image(index, 0, &image) || !make_cover(&image, &scratch)))
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

static void emit(void *controller, const char *event)
{
    typedef void *(*init)(void *, const char *, void *);
    typedef void (*destroy)(void *);
    typedef void (*navigate)(void *, void *);
    uint32_t name[2], scratch[2];
    ((init)OSOS_CPP_STRING_INIT)(name, event, scratch);
    ((navigate)osos_method(controller, OSOS_CONTROLLER_EMIT_EVENT))(controller, name);
    ((destroy)OSOS_CPP_STRING_DESTROY)(name);
}

/* Playlists hold menu: Set Playlist Photo. */
void cfw_playlist_cover_menu(struct osos_context_menu *menu)
{
    osos_context_menu_append(menu, CFW_SET_PLAYLIST_PHOTO, "CFW_SetPlaylistPhoto", 1);
}

int cfw_playlist_cover_action(void *controller, const char *action, uint32_t argument)
{
    typedef void *(*model_get)(void);
    typedef void (*model_int)(void *, int);
    typedef void (*model_void)(void *);
    (void)controller;
    if (!cfw_string_equal(action, "CFW_SetPlaylistPhoto"))
        return 0;
    if (argument == OSOS_ACTION_SUPPORT_QUERY)
        return 1;
    target_list = cfw_playlist_held();
    pick_pending = target_list != NULL;
    pick_controller = NULL;
    void *model = ((model_get)OSOS_PHOTOS_BROWSE_MODEL_GET)();
    if (model) {
        /* As the Photos menu does for All Photos: album 0, reload its photos (the
           menu's highlight does that; without it the grid can show 0 photos),
           grid, first photo. */
        ((model_int)OSOS_PHOTOS_BROWSE_SET_ALBUM)(model, 0);
        ((model_void)OSOS_PHOTOS_BROWSE_RELOAD)(model);
        ((model_void)OSOS_PHOTOS_BROWSE_SMALL)(model);
        ((model_int)OSOS_PHOTOS_BROWSE_SELECT)(model, 0);
    }
    /* playlist_cover.ui then pushes the Photos grid. */
    return 1;
}

static void *claim(void *controller)
{
    if (pick_pending) {
        pick_pending = 0;
        pick_controller = controller;
    } else {
        pick_controller = NULL;
        target_list = NULL;
    }
    return controller;
}

/* Photos browse controller factories (screen controller registry). */
PATCH_ARM void *cfw_photo_browse_new(void *arg)
{
    typedef void *(*make)(void *);
    return claim(((make)OSOS_PHOTOS_BROWSE_NEW)(arg));
}

PATCH_ARM void *cfw_photo_browse_new_transition(void *a, void *b, void *c, void *d)
{
    typedef void *(*make)(void *, void *, void *, void *);
    return claim(((make)OSOS_PHOTOS_BROWSE_NEW_TRANSITION)(a, b, c, d));
}

/* TPhotosBrowseCntlr actions: while choosing, Center takes the photo. */
PATCH_ARM int cfw_photo_browse_action(void *controller, const char *action,
                                      uint32_t argument)
{
    typedef int (*dispatch)(void *, const char *, uint32_t);
    typedef void *(*model_get)(void);
    if (!pick_controller || controller != pick_controller ||
        (!cfw_string_equal(action, "HandleBrowseLarge") &&
         !cfw_string_equal(action, "HandleBrowseSlideshow")))
        return ((dispatch)OSOS_PHOTOS_BROWSE_ACTION)(controller, action, argument);
    if (argument == OSOS_ACTION_SUPPORT_QUERY)
        return 1;
    uint8_t *model = ((model_get)OSOS_PHOTOS_BROWSE_MODEL_GET)();
    void *list = target_list;
    target_list = NULL;
    pick_controller = NULL;
    if (model && list)
        set_cover(list, *(int *)(model + OSOS_PHOTOS_BROWSE_CURRENT));
    /* playlist_cover.ui closes the grid. */
    emit(controller, "CFW_PlaylistPhotoSet");
    return 1;
}

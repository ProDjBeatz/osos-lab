/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Add to Playlist.
 *
 * Hold Center on a song and choose Add to Playlist. A copy of the Playlists screen
 * opens (playlist_add.ui); clicking a playlist appends the song to it and returns
 * to the song list. Songs already in that playlist are not added twice; folders
 * cannot take songs.
 *
 * The iPod never writes iTunesDB, so additions to synced playlists are kept in
 * iPod_Control\Device\CFWAdds as (playlist persistent ID, track persistent ID)
 * pairs and re-applied whenever the library is loaded: at boot and after a sync.
 * On-The-Go is saved by the iPod itself, so it is marked changed instead.
 */
#include "osos.h"
#include "cfw.h"
#include "patch.h"
#include "preferences.h"
#include "patches/sites.h"
#include "play_next.h"
#include "playlist_add.h"
#include "playlist_add_ui.h"

#define ADDS_MAGIC 0x41574643u /* "CFWA" */
#define ADDS_VERSION 1
#define ADDS_MAX 256

struct saved_add {
    uint32_t list[2];
    uint32_t track[2];
};

static struct {
    uint32_t magic, version, count;
    struct saved_add entries[ADDS_MAX];
} adds;

static const struct osos_path adds_path = {0, "iPod_Control\\Device\\CFWAdds"};

/* Native playlist row reference built by OSOS_PLAYLISTS_ROW_ITEM. */
struct playlist_ref {
    uintptr_t *vtable;
    struct osos_media_list *list;
    uint32_t reserved[6];
};

/* Media database seen at the last library load; NULL until then. */
static uint8_t *database;
/* Song chosen from a hold menu, waiting for a playlist. */
static struct osos_media_entry *pending;

static const uint32_t *word_pair(const void *object, unsigned offset)
{
    return (const uint32_t *)((const uint8_t *)object + offset);
}

static const uint32_t *list_id(struct osos_media_list *list)
{
    return word_pair(list, OSOS_PLAYLIST_PERSISTENT_ID);
}

static const uint32_t *track_id(struct osos_media_entry *entry)
{
    return word_pair(entry->song.record, OSOS_RECORD_PERSISTENT_ID);
}

static int is_list(struct osos_media_list *list)
{
    return list && *word_pair(list, 4) == OSOS_LIST_MAGIC;
}

static int contains(struct osos_media_list *list, struct osos_media_record *record)
{
    struct osos_media_entry *root = osos_list_root(list);
    struct osos_media_entry *entry = root->group.first;
    for (unsigned i = 0; i < root->tracks && entry; i++, entry = entry->next)
        if (entry->song.record == record)
            return 1;
    return 0;
}

static struct osos_media_list *otg_list(void)
{
    /* Media database objects have type byte 1 (see MediaBrowser_GetOwner). */
    if (!database || database[0] != 1)
        return NULL;
    return *(struct osos_media_list **)(database + OSOS_DATABASE_OTG_LIST);
}

static void mark_otg_changed(void)
{
    uint8_t *state = *(uint8_t **)(database + OSOS_DATABASE_OTG_STATE);
    if (state)
        state[OSOS_OTG_STATE_DIRTY] = 1;
}

/* Caller holds the player lock (NULL while loading). 1 when the song is in the
   list afterwards. */
static int append(struct osos_player *player, struct osos_media_list *list,
                  struct osos_media_entry *entry)
{
    if (!is_list(list) || !entry || !entry->song.record || !osos_list_root(list))
        return 0;
    if (osos_list_root(list)->groups)
        return 0;
    if (contains(list, entry->song.record))
        return 1;
    if (!cfw_list_append(player, list, entry))
        return 0;
    /* Lists cache their song count ("N Songs", and the row count the song list
       sizes its artist lines by); this byte makes the next read recount, as the
       native list reference's mark-changed method (0x0827aa2c) does. */
    *((volatile uint8_t *)list + OSOS_LIST_COUNTS_STALE) = 1;
    return 1;
}

static void remember(struct osos_media_list *list, struct osos_media_entry *entry)
{
    const uint32_t *playlist = list_id(list), *track = track_id(entry);
    if ((!playlist[0] && !playlist[1]) || (!track[0] && !track[1]))
        return;
    for (uint32_t i = 0; i < adds.count; i++) {
        struct saved_add *saved = &adds.entries[i];
        if (saved->list[0] == playlist[0] && saved->list[1] == playlist[1] &&
            saved->track[0] == track[0] && saved->track[1] == track[1])
            return;
    }
    if (adds.count == ADDS_MAX)
        return;
    struct saved_add *saved = &adds.entries[adds.count++];
    saved->list[0] = playlist[0];
    saved->list[1] = playlist[1];
    saved->track[0] = track[0];
    saved->track[1] = track[1];
    adds.magic = ADDS_MAGIC;
    adds.version = ADDS_VERSION;
    cfw_preferences_write(&adds_path, &adds, sizeof(adds));
}

static struct osos_media_list *next_list(struct osos_media_list *list)
{
    typedef struct osos_media_list *(*next)(struct osos_media_list *);
    return ((next)OSOS_TRACK_DATA_NEXT_LIST)(list);
}

/* Library entry for a saved track: the first list holding it (normally the
   library list, which is walked first). */
static struct osos_media_entry *find_track(struct osos_media_list *first,
                                           const uint32_t *id)
{
    unsigned lists = 0;
    for (struct osos_media_list *list = first; is_list(list) && lists < 4096;
         list = next_list(list), lists++) {
        struct osos_media_entry *root = osos_list_root(list);
        if (!root || root->groups)
            continue;
        struct osos_media_entry *entry = root->group.first;
        for (unsigned i = 0; i < root->tracks && entry; i++, entry = entry->next) {
            if (!entry->song.record)
                continue;
            const uint32_t *track = track_id(entry);
            if (track[0] == id[0] && track[1] == id[1])
                return entry;
        }
    }
    return NULL;
}

static struct osos_media_list *find_list(struct osos_media_list *first,
                                         const uint32_t *id)
{
    unsigned lists = 0;
    for (struct osos_media_list *list = first; is_list(list) && lists < 4096;
         list = next_list(list), lists++) {
        const uint32_t *playlist = list_id(list);
        if (playlist[0] == id[0] && playlist[1] == id[1])
            return list;
    }
    return NULL;
}

static void apply_saved(void)
{
    /* Load first, so a later add keeps the saved list even if nothing applies. */
    if (!cfw_preferences_read(&adds_path, &adds, sizeof(adds)) ||
        adds.magic != ADDS_MAGIC || adds.version != ADDS_VERSION ||
        adds.count > ADDS_MAX) {
        adds.count = 0;
        return;
    }
    uint8_t *tracks = *(uint8_t **)(database + OSOS_DATABASE_TRACK_DATA);
    if (!tracks || *word_pair(tracks, 4) != OSOS_TRACK_DATA_MAGIC)
        return;
    struct osos_media_list *first =
        *(struct osos_media_list **)(tracks + OSOS_TRACK_DATA_FIRST_LIST);
    /* The library is still loading: nothing plays yet, so no player is needed. */
    for (uint32_t i = 0; i < adds.count; i++) {
        struct osos_media_list *list = find_list(first, adds.entries[i].list);
        struct osos_media_entry *entry =
            list ? find_track(first, adds.entries[i].track) : NULL;
        if (entry)
            append(NULL, list, entry);
    }
}

/* Both library loads (boot and after a sync). */
PATCH_ARM int cfw_database_load(void *media_database)
{
    typedef int (*load)(void *);
    pending = NULL;
    int result = ((load)OSOS_DATABASE_LOAD)(media_database);
    database = media_database;
    if (database)
        apply_saved();
    return result;
}

void cfw_playlist_add_menu(struct osos_context_menu *menu)
{
    osos_context_menu_append(menu, CFW_ADD_TO_PLAYLIST, "CFW_AddToPlaylist", 1);
}

void cfw_playlist_add_select(struct osos_media_item *item)
{
    pending = item ? osos_media_item_entry(item) : NULL;
}

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

static struct osos_media_list *chosen_list(void *controller)
{
    typedef void *(*model)(void);
    typedef int (*highlighted)(void *);
    typedef int (*map_row)(void *, int);
    typedef void *(*instance)(void);
    typedef void *(*row_item)(struct playlist_ref *, void *, int);
    void *view = ((model)OSOS_LIST_VIEW_MODEL)();
    if (!view)
        return NULL;
    int row = ((highlighted)osos_method(view, OSOS_LIST_VIEW_HIGHLIGHTED))(view);
    if (row < 0)
        return NULL;
    row = ((map_row)osos_method(controller, OSOS_MEDIA_LIST_MAP_ROW))(controller, row);
    if (row < 0)
        return NULL;
    struct playlist_ref ref;
    ref.vtable = NULL;
    ref.list = NULL;
    ((row_item)OSOS_PLAYLISTS_ROW_ITEM)(&ref, ((instance)OSOS_PLAYLISTS_GET)(), row);
    return ref.list;
}

int cfw_playlist_add_action(void *controller, const char *action, uint32_t argument)
{
    if (!cfw_string_equal(action, "CFW_AddToPlaylistChosen"))
        return 0;
    if (argument == OSOS_ACTION_SUPPORT_QUERY)
        return 1;
    struct osos_media_entry *entry = pending;
    pending = NULL;
    struct osos_media_list *list = entry ? chosen_list(controller) : NULL;
    if (list) {
        struct osos_player *player = osos_player_get();
        osos_player_lock(player);
        int added = append(player, list, entry);
        osos_player_unlock(player);
        if (added) {
            if (list == otg_list())
                mark_otg_changed();
            else
                remember(list, entry);
        }
    }
    /* playlist_add.ui binds controller.CFW_AddedToPlaylist to PopTopScreen. */
    emit(controller, "CFW_AddedToPlaylist");
    return 1;
}

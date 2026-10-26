/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Rename Playlist.
 *
 * Hold Center on a playlist and choose Rename Playlist. The stock Search screen
 * opens with its click-wheel keyboard (see playlist_rename.ui). While renaming:
 *   - typed text is title-cased (first letter and each letter after a space);
 *   - pressing Menu renames the playlist to the typed text and returns to
 *     Playlists. Menu with nothing typed cancels.
 *
 * The Search keyboard keeps its text in the search model's 'Str ' 0x7684
 * resource; every keystroke rewrites it through OSOS_MODEL_RESOURCE_SET. The
 * playlist is renamed in memory with the native set-name routine used when
 * On-The-Go and Genius playlists are saved. Renames do not yet persist across
 * reboot or sync.
 */
#include "osos.h"
#include "cfw.h"
#include "patch.h"
#include "patches/sites.h"
#include "playlist_rename.h"
#include "playlist_rename_ui.h"

#define STR_TYPE 0x53747220u /* 'Str ' */
#define NAME_MAX 255

/* Native playlist row reference built by OSOS_PLAYLISTS_ROW_ITEM. */
struct playlist_ref {
    uintptr_t *vtable;
    void *list;
    uint32_t reserved[6];
};

/* Length-prefixed UTF-16 name, as passed to OSOS_PLAYLIST_SET_NAME. */
struct native_name {
    uint16_t length;
    uint16_t text[NAME_MAX];
};

/* Row highlighted when the Playlists hold menu was built; -1 when none. */
static int menu_row = -1;
/* Playlist chosen for renaming; NULL when idle. */
static void *rename_list;
/* Set by the Rename action until the Search controller it opens is built. */
static int rename_pending;
/* The Search controller doing the rename; other Search screens behave normally. */
static void *rename_controller;
static char typed[NAME_MAX + 1];

static void rename_reset(void)
{
    rename_list = NULL;
    rename_pending = 0;
    rename_controller = NULL;
}

static void *search_model(void)
{
    typedef void *(*lookup)(uint32_t);
    return ((lookup)OSOS_SEARCH_MODEL_LOOKUP)(OSOS_SEARCH_MODEL_ID);
}

static int set_query(void *model, const char *text, uint32_t length)
{
    typedef int (*set)(void *, uint32_t, uint32_t, const char *, uint32_t);
    return ((set)OSOS_MODEL_RESOURCE_SET)(model, STR_TYPE, OSOS_SEARCH_QUERY_ID, text,
                                    length);
}

static const char *get_query(void *model)
{
    typedef const char *(*get)(void *, uint32_t, uint32_t);
    return ((get)OSOS_MODEL_RESOURCE_GET)(model, STR_TYPE, OSOS_SEARCH_QUERY_ID);
}

static void *playlist_at(int row)
{
    typedef void *(*instance)(void);
    typedef void *(*row_item)(struct playlist_ref *, void *, int);
    /* Field-wise init: the payload links without libc, so no {0} (memset). */
    struct playlist_ref ref;
    ref.vtable = NULL;
    ref.list = NULL;
    if (row < 0)
        return NULL;
    ((row_item)OSOS_PLAYLISTS_ROW_ITEM)(&ref, ((instance)OSOS_PLAYLISTS_GET)(), row);
    return ref.list;
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

static uint32_t title_case(const char *source, uint32_t length)
{
    uint32_t n = 0;
    for (; n < length && n < NAME_MAX && source[n]; n++) {
        char c = source[n];
        if (c >= 'a' && c <= 'z' && (n == 0 || typed[n - 1] == ' '))
            c -= 'a' - 'A';
        typed[n] = c;
    }
    typed[n] = 0;
    return n;
}

static int rename_to(void *list, const char *text)
{
    typedef int (*set_name)(void *, struct native_name *);
    struct native_name name;
    uint16_t n = 0;
    /* The Search keyboard only produces ASCII letters, digits and spaces. Title-case
       here too: the first keystroke can reach the query without our hook. */
    for (; n < NAME_MAX && text[n]; n++) {
        char c = text[n];
        if (c >= 'a' && c <= 'z' && (n == 0 || text[n - 1] == ' '))
            c -= 'a' - 'A';
        name.text[n] = (uint8_t)c;
    }
    while (n && name.text[n - 1] == ' ')
        n--;
    if (!n)
        return -1;
    name.length = n;
    return ((set_name)OSOS_PLAYLIST_SET_NAME)(list, &name);
}

/* TSilverMediaListCntlr_Playlists vtable +0x148: remember the held row. */
PATCH_ARM int cfw_playlists_menu(void *controller, int row)
{
    typedef int (*build)(void *, int);
    menu_row = row;
    return ((build)OSOS_PLAYLISTS_BUILD_MENU)(controller, row);
}

/* Replaces the Cancel append in the Playlists hold menu: Rename, then Cancel. */
PATCH_ARM void cfw_playlists_menu_cancel(struct osos_context_menu *menu,
                                         uint32_t label, const char *action,
                                         uint32_t flags)
{
    osos_context_menu_append(menu, CFW_RENAME_PLAYLIST, "CFW_RenamePlaylist", 1);
    osos_context_menu_append(menu, label, action, flags);
}

int cfw_playlist_rename_action(void *controller, const char *action,
                               uint32_t argument)
{
    (void)controller;
    if (!cfw_string_equal(action, "CFW_RenamePlaylist"))
        return 0;
    if (argument != OSOS_ACTION_SUPPORT_QUERY) {
        rename_reset();
        rename_list = playlist_at(menu_row);
        rename_pending = rename_list != NULL;
        /* Start from an empty name instead of the last search. */
        void *model = rename_list ? search_model() : NULL;
        if (model) {
            set_query(model, "", 0);
            /* TSearchCntlr (vtable +0xec) restores Search_Main_Screen_WithKeyboard
               when this "keyboard was showing" flag is set; the rename screen has
               only its own layout, so switching would abort. */
            ((volatile uint8_t *)model)[OSOS_SEARCH_MODEL_KEYBOARD_FLAG] = 0;
        }
    }
    /* The screen event then pushes Search with its keyboard. */
    return 1;
}

/* Search controller construction (the only native site): claim it for a pending
   rename; any other new Search screen ends a rename that was abandoned. */
PATCH_ARM void *cfw_search_construct(void *controller, void *arg)
{
    typedef void *(*construct)(void *, void *);
    controller = ((construct)OSOS_SEARCH_CONSTRUCT)(controller, arg);
    if (rename_pending) {
        rename_pending = 0;
        rename_controller = controller;
    } else {
        rename_reset();
    }
    return controller;
}

/* Keystroke handler's query update: title-case the name while renaming. */
PATCH_ARM int cfw_search_set_query(void *model, uint32_t type, uint32_t id,
                                   const char *text, uint32_t length)
{
    typedef int (*set)(void *, uint32_t, uint32_t, const char *, uint32_t);
    if (rename_controller && type == STR_TYPE && id == OSOS_SEARCH_QUERY_ID && text)
        return ((set)OSOS_MODEL_RESOURCE_SET)(model, type, id, typed,
                                        title_case(text, length));
    return ((set)OSOS_MODEL_RESOURCE_SET)(model, type, id, text, length);
}

/* TSearchCntlr vtable +0x150 (HandleMenuOnKeyboard). */
PATCH_ARM int cfw_search_menu_on_keyboard(void *controller, uint32_t argument)
{
    typedef int (*handler)(void *, uint32_t);
    if (!rename_controller || controller != rename_controller)
        return ((handler)OSOS_SEARCH_MENU_ON_KEYBOARD)(controller, argument);
    void *list = rename_list;
    rename_reset();
    void *model = search_model();
    const char *text = model ? get_query(model) : NULL;
    if (text && *text)
        rename_to(list, text);
    if (model)
        set_query(model, "", 0);
    /* playlist_rename.ui binds controller.CFW_RenameDone to PopTopScreen. */
    emit(controller, "CFW_RenameDone");
    return 1;
}

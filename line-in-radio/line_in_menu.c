#include "line_in_capture.h"
#include "logging.h"
#include "support.h"

/* Line-in Radio's rows in the game's Audio options, under Radio Volume: the source (the Songs folder
   or a sound device) and Shuffle. Built the way Extra Options builds its rows
   (extra-options/features/video_options.c): the page's XAML gets two SpinnerControls when the game
   loads it, and their bindings are properties registered on the Audio page's type.

   The XAML comes through the game's resource call at RESOURCE_CALL, which Extra Options may already
   send to its own thunk. The loader loads mods by name, so Extra Options has patched (and checked)
   that call before this runs; this sends the call to its own thunk, which calls whichever target the
   call had. Extra Options then edits the XAML first, and these rows go in after it, with an LI_ prefix
   Extra Options doesn't look for. */

enum {
    RESOURCE_CALL = 0x16ebb37,   // call GetResource on the UIRenderer LoadComponent path
    GET_RESOURCE = 0xaabec0,
    ALLOCATE = 0x3229c0,
    FREE = 0x31c7b0,
    OBJECT_INIT = 0x143eb80,
    OBJECT_CLEANUP = 0x1442050,
    DELETE_OBJECT = 0x257730,
    COLLECTION_TYPE = 0x1476110,
    ALLOCATE_TYPED = 0x14424e0,
    COLLECTION_INIT = 0xaefd60,
    MAKE_STRING = 0x13fe8e0,
    INSERT_ITEM = 0xb52170,
    INT_PROPERTY = 0xed81f0,
    COLLECTION_PROPERTY = 0xed77d0,
    REGISTER_PROPERTY = 0x155ee30,
    BOX_INT = 0x4702d0,
    PROPERTY_CHANGED = 0xcf9c60,
    AUDIO_TYPE_INIT = 0xc803c0,
    AUDIO_TYPE_REGISTRY = 0x35107b0,
};

typedef struct {
    void **vtable;
    unsigned char header[0x20];
    void *getter, *setter, *unused;
    const char *name;
    void *type;
    uint64_t flags;
} NativeProperty;

typedef struct NativePropertyLink {
    NativeProperty *property;
    struct NativePropertyLink *next;
} NativePropertyLink;

typedef struct {
    void **vtable;
    unsigned char header[0x20];
    char *bytes;
    int length;
    int capacity;
} NativeResource;

_Static_assert(sizeof(NativeProperty) == 0x58, "native property descriptor");
_Static_assert(offsetof(NativeResource, bytes) == 0x28, "native resource buffer");

typedef void **(*GetResource)(void *reader, void **output);
enum { SOURCE, SOURCE_CHOICES, SHUFFLE, SHUFFLE_CHOICES, PROPERTY_COUNT };
static const char *const property_names[PROPERTY_COUNT] = {"LineInSource", "LineInSourceChoices", "LineInShuffle",
                                                           "LineInShuffleChoices"};

static unsigned char *image;
static GetResource next_resource;
static NativeProperty properties[PROPERTY_COUNT];
static NativePropertyLink links[PROPERTY_COUNT];
static int registered;
static void *source_choices, *shuffle_choices, *shuffle_locked; // locked: a device plays, Shuffle has nothing to do
#define MAX_DEVICES 32
static wchar_t devices[MAX_DEVICES][128];
static unsigned device_count;
static void *resource_vtable[7];

static void release(void *object) {
    if (object && InterlockedDecrement((volatile LONG *)((unsigned char *)object + 0x10)) == 0) {
        ((void (*)(void *))(image + DELETE_OBJECT))(object);
    }
}

static void *make_choices(const wchar_t *const *labels, unsigned count) {
    void *type = ((void *(*)(void *))(image + COLLECTION_TYPE))(NULL);
    void *items = ((void *(*)(size_t, void *))(image + ALLOCATE_TYPED))(0x58, type);
    if (!items) {
        return NULL;
    }
    ((void (*)(void *))(image + COLLECTION_INIT))(items);
    static const unsigned vtables[] = {0x2db73f8, 0x2db7470, 0x2db74d0, 0x2db74e8, 0x2db7508};
    static const unsigned offsets[] = {0, 0x30, 0x38, 0x40, 0x48};
    for (unsigned i = 0; i < ARRAY_COUNT(vtables); ++i) {
        *(void **)((unsigned char *)items + offsets[i]) = image + vtables[i];
    }
    for (unsigned i = 0; i < count; ++i) {
        void *label = NULL;
        ((void **(*)(void **, const wchar_t *))(image + MAKE_STRING))(&label, labels[i]);
        if (!label) {
            release(items);
            return NULL;
        }
        ((void (*)(void *, int, void *))(image + INSERT_ITEM))(items, (int)i, label);
        release(label);
    }
    return items;
}

/* The source list: Songs, then the devices connected now, then the picked device if it is not. */
static void build_source_choices(void) {
    static const wchar_t *labels[MAX_DEVICES + 2];
    static wchar_t picked[128];
    device_count = line_in_list_devices(devices, MAX_DEVICES);
    unsigned count = 0;
    labels[count++] = L"Songs folder";
    for (unsigned i = 0; i < device_count; ++i) labels[count++] = devices[i];
    if (!line_in_use_songs && line_in_device[0]) { // a picked device that is not connected now
        MultiByteToWideChar(CP_UTF8, 0, line_in_device, -1, picked, 128);
        int listed = 0;
        for (unsigned i = 0; i < device_count && !listed; ++i) {
            char name[384];
            WideCharToMultiByte(CP_UTF8, 0, devices[i], -1, name, sizeof(name), NULL, NULL);
            listed = text_equal_ignore_case(name, line_in_device);
        }
        if (!listed && device_count < MAX_DEVICES) {
            memory_copy(devices[device_count++], picked, sizeof(picked));
            labels[count++] = picked;
        }
    }
    void *old = source_choices;
    source_choices = make_choices(labels, count);
    release(old);
}

static int source_index(void) {
    if (line_in_use_songs) {
        return 0;
    }
    for (unsigned i = 0; i < device_count; ++i) {
        char name[384];
        WideCharToMultiByte(CP_UTF8, 0, devices[i], -1, name, sizeof(name), NULL, NULL);
        if (line_in_name_matches(name, line_in_device)) {
            return (int)i + 1;
        }
    }
    return 0;
}

static void **get_value(void **output, NativeProperty *property, void *page) {
    (void)page;
    *output = NULL;
    if (property == &properties[SOURCE_CHOICES] || property == &properties[SHUFFLE_CHOICES]) {
        *output = property == &properties[SOURCE_CHOICES] ? source_choices
                  : line_in_use_songs                    ? shuffle_choices
                                                         : shuffle_locked;
        if (*output) {
            InterlockedIncrement((volatile LONG *)((unsigned char *)*output + 0x10));
        }
        return output;
    }
    int value = property == &properties[SOURCE] ? source_index() : line_in_use_songs && line_in_shuffle;
    return ((void **(*)(void **, const void *))(image + BOX_INT))(output, &value);
}

static void set_value(NativeProperty *property, void *page, void *boxed) {
    int value;
    if (!boxed || !memory_read((unsigned char *)boxed + 0x28, &value, sizeof(value))) {
        return;
    }
    if (property == &properties[SOURCE]) {
        if (value < 0 || (unsigned)value > device_count || value == source_index()) {
            return;
        }
        if (value == 0) {
            line_in_select(NULL);
        } else {
            static char name[384];
            WideCharToMultiByte(CP_UTF8, 0, devices[value - 1], -1, name, sizeof(name), NULL, NULL);
            line_in_select(name);
        }
        // Shuffle only means something for the Songs folder: its row shows that at once
        ((void (*)(void *, void *))(image + PROPERTY_CHANGED))(page, &properties[SHUFFLE_CHOICES]);
        ((void (*)(void *, void *))(image + PROPERTY_CHANGED))(page, &properties[SHUFFLE]);
    } else if (property == &properties[SHUFFLE] && line_in_use_songs && (value == 0 || value == 1)) {
        line_in_set_shuffle(value);
    } else {
        return;
    }
    ((void (*)(void *, void *))(image + PROPERTY_CHANGED))(page, property);
}

static int register_properties(void) {
    if (registered) {
        return 1;
    }
    static const wchar_t *const shuffle_labels[] = {L"Off", L"On"};
    static const wchar_t *const locked_label[] = {L"Songs folder only"};
    shuffle_choices = make_choices(shuffle_labels, 2);
    shuffle_locked = make_choices(locked_label, 1);
    if (!shuffle_choices || !shuffle_locked) {
        return 0;
    }
    ((void (*)(void))(image + AUDIO_TYPE_INIT))();
    for (unsigned i = 0; i < PROPERTY_COUNT; ++i) {
        int collection = i == SOURCE_CHOICES || i == SHUFFLE_CHOICES;
        NativeProperty *original = ((NativeProperty * (*)(void))(image + (collection ? COLLECTION_PROPERTY : INT_PROPERTY)))();
        NativeProperty *property = &properties[i];
        ((void (*)(void *))(image + OBJECT_INIT))(property);
        property->vtable = original->vtable;
        memory_copy((unsigned char *)property + 0x28, (unsigned char *)original + 0x28, 0x30);
        property->name = property_names[i];
        property->getter = (void *)get_value;
        property->setter = collection ? NULL : (void *)set_value;
        links[i].property = property;
        ((void (*)(void *, NativePropertyLink *))(image + REGISTER_PROPERTY))(image + AUDIO_TYPE_REGISTRY, &links[i]);
    }
    registered = 1;
    return 1;
}

static const char resources_xaml[] =
    "<local:TileMenuDataItem x:Key=\"LI_Source\" Heading=\"Line-in Radio\" Description=\"What the Line-in Radio "
    "station plays: the music files from the mod's Songs folder, or a sound device, such as a virtual cable "
    "or a spare output your music player plays into.\"/>"
    "<local:TileMenuDataItem x:Key=\"LI_Shuffle\" Heading=\"Shuffle\" Description=\"Plays the songs from the "
    "Songs folder in random order. Off plays them in alphabetical order. Only for the Songs folder.\"/>";
static const char rows_xaml[] =
    "<local:SpinnerControl Tag=\"{StaticResource LI_Source}\" Style=\"{StaticResource "
    "A_Style_SpinnerControl_Default}\" Label=\"Line-in Radio\" SelectedIndex=\"{Binding LineInSource}\" "
    "ItemsSource=\"{Binding LineInSourceChoices}\"/>"
    "<local:SpinnerControl Tag=\"{StaticResource LI_Shuffle}\" Style=\"{StaticResource "
    "A_Style_SpinnerControl_Default}\" Label=\"Shuffle\" SelectedIndex=\"{Binding LineInShuffle}\" "
    "ItemsSource=\"{Binding LineInShuffleChoices}\"/>";

/* The Audio page with the rows: resources before </ResourceDictionary>, rows after the Radio
   Volume slider. 0: not the Audio page, or not the layout this expects. */
size_t line_in_menu_xaml(const char *text, size_t length, char *output, size_t capacity) {
    static const char radio[] = "<Slider Value=\"{Binding RadioVolume}\"";
    if (!text_find(text, length, "x:Class=\"ForzaUI.A_AudioOptions\"") || text_find(text, length, "LI_Source")) {
        return 0;
    }
    const char *resources = text_find(text, length, "</ResourceDictionary>");
    const char *slider = text_find_unique(text, length, radio);
    const char *row_end = slider ? text_find(slider, length - (size_t)(slider - text), "/>") : NULL;
    if (!resources || !row_end || resources > slider) {
        return 0;
    }
    row_end += 2;
    size_t a = (size_t)(resources - text), b = (size_t)(row_end - text);
    size_t total = length + sizeof(resources_xaml) - 1 + sizeof(rows_xaml) - 1;
    if (total + 1 > capacity) {
        return 0;
    }
    char *out = output;
    memory_copy(out, text, a), out += a;
    memory_copy(out, resources_xaml, sizeof(resources_xaml) - 1), out += sizeof(resources_xaml) - 1;
    memory_copy(out, text + a, b - a), out += b - a;
    memory_copy(out, rows_xaml, sizeof(rows_xaml) - 1), out += sizeof(rows_xaml) - 1;
    memory_copy(out, text + b, length - b), out += length - b;
    *out = 0;
    return total;
}

static void *delete_resource(NativeResource *resource, unsigned flags) {
    ((void (*)(void *))(image + OBJECT_CLEANUP))(resource);
    if (flags & 1) {
        ((void (*)(void *))(image + FREE))(resource);
    }
    return resource;
}

static void **get_resource(void *reader, void **output) {
    next_resource(reader, output);
    NativeResource *original = *output;
    if (!original || !original->bytes || original->length <= 0 || original->length > 262144 ||
        !text_find(original->bytes, (size_t)original->length, "x:Class=\"ForzaUI.A_AudioOptions\"")) {
        return output;
    }
    size_t capacity = (size_t)original->length + sizeof(resources_xaml) + sizeof(rows_xaml);
    NativeResource *replacement = ((void *(*)(size_t))(image + ALLOCATE))(sizeof(NativeResource) + capacity);
    if (!replacement) {
        return output;
    }
    char *bytes = (char *)(replacement + 1);
    size_t length = line_in_menu_xaml(original->bytes, (size_t)original->length, bytes, capacity);
    if (length) {
        build_source_choices(); // the devices connected now, every time the page opens
    }
    if (!length || !source_choices || !register_properties()) {
        ((void (*)(void *))(image + FREE))(replacement);
        log_append("LINE-IN: the Audio options layout did not match; no Line-in Radio rows.\r\n");
        return output;
    }
    ((void (*)(void *))(image + OBJECT_INIT))(replacement);
    memory_copy(resource_vtable, original->vtable - 1, sizeof(resource_vtable));
    resource_vtable[1] = (void *)delete_resource;
    replacement->vtable = resource_vtable + 1;
    replacement->bytes = bytes;
    replacement->length = (int)length;
    replacement->capacity = (int)capacity;
    *output = replacement;
    release(original);
    return output;
}

/* A 12-byte thunk (mov rax, get_resource; jmp rax) within rel32 reach of the call: written into the
   int3 padding between two of the game's own functions just after it (no memory of our own made
   executable). Extra Options has filled the padding it uses by now, so that isn't picked. */
static unsigned char *padding_thunk(unsigned char *site) {
    for (unsigned char *at = site; at < site + 0x100000; ++at) {
        unsigned run = 0;
        while (run < 16 && at[run] == 0xcc) ++run;
        if (run < 16) {
            at += run;
            continue;
        }
        unsigned char *thunk = at + 2; // an int3 stays on either side
        static const unsigned char code[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
        uintptr_t target = (uintptr_t)get_resource;
        DWORD old;
        if (!VirtualProtect(thunk, sizeof(code), PAGE_EXECUTE_READWRITE, &old)) {
            return NULL;
        }
        memory_copy(thunk, code, sizeof(code));
        memory_copy(thunk + 2, &target, sizeof(target));
        VirtualProtect(thunk, sizeof(code), old, &old);
        FlushInstructionCache(GetCurrentProcess(), thunk, sizeof(code));
        return thunk;
    }
    return NULL;
}

void line_in_menu_install(unsigned char *game_image) {
    image = game_image;
    unsigned char *site = image + RESOURCE_CALL;
    int32_t relative;
    memory_copy(&relative, site + 1, sizeof(relative));
    unsigned char *target = site + 5 + relative;
    // the game's own call, or one Extra Options sent to its thunk (a mov rax / jmp rax)
    if (site[0] != 0xe8 || (target != image + GET_RESOURCE && !(target[0] == 0x48 && target[1] == 0xb8))) {
        log_append("LINE-IN: the Audio options call is not the expected one; no Line-in Radio rows.\r\n");
        return;
    }
    unsigned char *thunk = padding_thunk(site);
    int64_t distance = thunk ? thunk - (site + 5) : 0;
    if (!thunk || distance != (int32_t)distance) {
        log_append("LINE-IN: no room for the Audio options hook; no Line-in Radio rows.\r\n");
        return;
    }
    next_resource = (GetResource)target;
    relative = (int32_t)distance;
    DWORD old;
    VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    memory_copy(site + 1, &relative, sizeof(relative));
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log_append("LINE-IN: Audio options rows ready.\r\n");
}

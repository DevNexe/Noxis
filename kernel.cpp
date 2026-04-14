extern "C" void kmain();

namespace {
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

constexpr int VGA_WIDTH = 80;
constexpr int VGA_HEIGHT = 25;
constexpr int MAX_LINE = 128;
constexpr int HISTORY_SIZE = 16;
constexpr int MAX_EXEC_DEPTH = 4;

constexpr u64 FS_BASE = 0x50000ULL;
constexpr u32 FS_LBA_START = 128;
constexpr u32 FS_SECTORS = 64;
constexpr u32 FS_BYTES = FS_SECTORS * 512;
constexpr u32 FS_MAX_ENTRIES = 32;
constexpr int MAX_MODULES = 16;
constexpr int MAX_APPS = 24;
constexpr u8 NXFS_TYPE_FILE = 1;
constexpr u8 NXFS_TYPE_DIR = 2;
constexpr u8 NXFS_ROOT_PARENT = 0xFF;
constexpr u8 NXFS_PERM_FILE = 066;
constexpr u8 NXFS_PERM_DIR = 077;

volatile u16* const VGA = (volatile u16*)0xB8000;

u8 g_color = 0x0F;
int g_row = 0;
int g_col = 0;

bool g_shift = false;
bool g_ctrl = false;
bool g_alt = false;
bool g_caps = false;

bool g_fs_ready = false;
u8 g_uid = 0;
u8 g_cwd = NXFS_ROOT_PARENT;
u8 g_io_tmp[FS_BYTES];

struct ShellModule {
    bool used;
    char command[24];
    char message[128];
};

ShellModule g_modules[MAX_MODULES];
int g_module_count = 0;
int g_exec_depth = 0;

struct AppRecord {
    bool used;
    char id[24];
    char name[48];
    char version[16];
    char path[96];
    char entry[24];
    char description[80];
};

AppRecord g_apps[MAX_APPS];
int g_app_count = 0;

char g_history[HISTORY_SIZE][MAX_LINE];
int g_history_count = 0;

struct KeyEvent {
    bool valid;
    bool is_char;
    char ch;
    u8 code;
    bool extended;
};

struct __attribute__((packed)) NxfsHeader {
    char magic[4];
    u32 version;
    u32 max_entries;
    u32 data_offset;
    u32 data_size;
    u32 used_entries;
    u32 used_bytes;
    u32 checksum;
    u8 reserved[32];
};

struct __attribute__((packed)) NxfsEntry {
    char name[32];
    u32 offset;
    u32 size;
    u8 used;
    u8 reserved[23];
};

static inline u8 inb(u16 port) {
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(u16 port, u8 value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(u16 port, u16 value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

u16 make_cell(char ch, u8 color) {
    return (u16)ch | ((u16)color << 8);
}

void mem_copy(void* dst, const void* src, u32 n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    for (u32 i = 0; i < n; i++) d[i] = s[i];
}

void mem_move(void* dst, const void* src, u32 n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    if (d == s || n == 0) return;
    if (d < s) {
        for (u32 i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (u32 i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
}

void mem_set(void* dst, u8 value, u32 n) {
    u8* d = (u8*)dst;
    for (u32 i = 0; i < n; i++) d[i] = value;
}

int str_len(const char* s) {
    int n = 0;
    while (s[n] != '\0') n++;
    return n;
}

void str_copy(char* dst, const char* src, int cap) {
    int i = 0;
    while (i < cap - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

bool str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

bool str_ieq(const char* a, const char* b) {
    int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (to_lower(a[i]) != to_lower(b[i])) return false;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

bool str_starts_with(const char* a, const char* b) {
    int i = 0;
    while (b[i] != '\0') {
        if (a[i] != b[i]) return false;
        i++;
    }
    return true;
}

bool str_ends_with(const char* s, const char* suffix) {
    int ls = str_len(s);
    int lf = str_len(suffix);
    if (lf > ls) return false;
    int off = ls - lf;
    for (int i = 0; i < lf; i++) {
        if (s[off + i] != suffix[i]) return false;
    }
    return true;
}

const char* skip_spaces(const char* s) {
    while (*s == ' ') s++;
    return s;
}

void hw_set_cursor(int row, int col) {
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;

    g_row = row;
    g_col = col;

    u16 pos = (u16)(row * VGA_WIDTH + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

void clear_row(int row) {
    if (row < 0 || row >= VGA_HEIGHT) return;
    for (int x = 0; x < VGA_WIDTH; x++) {
        VGA[row * VGA_WIDTH + x] = make_cell(' ', g_color);
    }
}

void clear_screen() {
    for (int y = 0; y < VGA_HEIGHT; y++) clear_row(y);
    hw_set_cursor(0, 0);
}

void scroll_if_needed() {
    if (g_row < VGA_HEIGHT) return;
    for (int y = 1; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            VGA[(y - 1) * VGA_WIDTH + x] = VGA[y * VGA_WIDTH + x];
        }
    }
    clear_row(VGA_HEIGHT - 1);
    g_row = VGA_HEIGHT - 1;
}

void put_char(char ch) {
    if (ch == '\n') {
        g_col = 0;
        g_row++;
        scroll_if_needed();
        hw_set_cursor(g_row, g_col);
        return;
    }
    if (ch == '\r') {
        g_col = 0;
        hw_set_cursor(g_row, g_col);
        return;
    }

    VGA[g_row * VGA_WIDTH + g_col] = make_cell(ch, g_color);
    g_col++;
    if (g_col >= VGA_WIDTH) {
        g_col = 0;
        g_row++;
        scroll_if_needed();
    }
    hw_set_cursor(g_row, g_col);
}

void print(const char* text) {
    for (int i = 0; text[i] != '\0'; i++) put_char(text[i]);
}

void print_line(const char* text) {
    print(text);
    put_char('\n');
}

void print_u32(u32 value) {
    char buf[16];
    int idx = 0;
    if (value == 0) {
        put_char('0');
        return;
    }
    while (value > 0 && idx < 15) {
        buf[idx++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (idx > 0) put_char(buf[--idx]);
}

char letter_from_scancode(u8 code) {
    switch (code) {
        case 0x1E: return 'a'; case 0x30: return 'b'; case 0x2E: return 'c';
        case 0x20: return 'd'; case 0x12: return 'e'; case 0x21: return 'f';
        case 0x22: return 'g'; case 0x23: return 'h'; case 0x17: return 'i';
        case 0x24: return 'j'; case 0x25: return 'k'; case 0x26: return 'l';
        case 0x32: return 'm'; case 0x31: return 'n'; case 0x18: return 'o';
        case 0x19: return 'p'; case 0x10: return 'q'; case 0x13: return 'r';
        case 0x1F: return 's'; case 0x14: return 't'; case 0x16: return 'u';
        case 0x2F: return 'v'; case 0x11: return 'w'; case 0x2D: return 'x';
        case 0x15: return 'y'; case 0x2C: return 'z';
        default: return '\0';
    }
}

char scancode_to_ascii(u8 code) {
    char letter = letter_from_scancode(code);
    if (letter != '\0') {
        bool upper = (g_shift ^ g_caps);
        return upper ? (char)(letter - ('a' - 'A')) : letter;
    }

    switch (code) {
        case 0x02: return g_shift ? '!' : '1';
        case 0x03: return g_shift ? '@' : '2';
        case 0x04: return g_shift ? '#' : '3';
        case 0x05: return g_shift ? '$' : '4';
        case 0x06: return g_shift ? '%' : '5';
        case 0x07: return g_shift ? '^' : '6';
        case 0x08: return g_shift ? '&' : '7';
        case 0x09: return g_shift ? '*' : '8';
        case 0x0A: return g_shift ? '(' : '9';
        case 0x0B: return g_shift ? ')' : '0';
        case 0x0C: return g_shift ? '_' : '-';
        case 0x0D: return g_shift ? '+' : '=';
        case 0x1A: return g_shift ? '{' : '[';
        case 0x1B: return g_shift ? '}' : ']';
        case 0x27: return g_shift ? ':' : ';';
        case 0x28: return g_shift ? '"' : '\'';
        case 0x29: return g_shift ? '~' : '`';
        case 0x2B: return g_shift ? '|' : '\\';
        case 0x33: return g_shift ? '<' : ',';
        case 0x34: return g_shift ? '>' : '.';
        case 0x35: return g_shift ? '?' : '/';
        case 0x39: return ' ';
        default: return '\0';
    }
}

u8 read_scancode_raw() {
    while ((inb(0x64) & 1) == 0) {
    }
    return inb(0x60);
}

KeyEvent next_key_event() {
    bool extended = false;

    while (true) {
        u8 code = read_scancode_raw();
        if (code == 0xE0) {
            extended = true;
            continue;
        }

        bool release = (code & 0x80) != 0;
        u8 make = (u8)(code & 0x7F);

        if (!extended) {
            if (make == 0x2A || make == 0x36) {
                g_shift = !release;
                continue;
            }
            if (make == 0x1D) {
                g_ctrl = !release;
                continue;
            }
            if (make == 0x38) {
                g_alt = !release;
                continue;
            }
            if (make == 0x3A && !release) {
                g_caps = !g_caps;
                continue;
            }
        }

        if (release) {
            extended = false;
            continue;
        }

        KeyEvent ev{};
        ev.valid = true;
        ev.code = make;
        ev.extended = extended;
        ev.is_char = false;
        ev.ch = '\0';

        if (!extended) {
            char ch = scancode_to_ascii(make);
            if (ch != '\0') {
                ev.is_char = true;
                ev.ch = ch;
            }
        }
        return ev;
    }
}

void add_history(const char* line) {
    if (line[0] == '\0') return;
    if (g_history_count > 0 && str_eq(g_history[g_history_count - 1], line)) return;

    if (g_history_count < HISTORY_SIZE) {
        str_copy(g_history[g_history_count], line, MAX_LINE);
        g_history_count++;
        return;
    }

    for (int i = 1; i < HISTORY_SIZE; i++) str_copy(g_history[i - 1], g_history[i], MAX_LINE);
    str_copy(g_history[HISTORY_SIZE - 1], line, MAX_LINE);
}

void render_editor_row(int row, const char* prompt, const char* buf, int len, int cursor) {
    int prompt_len = str_len(prompt);
    clear_row(row);

    int x = 0;
    for (int i = 0; prompt[i] != '\0' && x < VGA_WIDTH; i++, x++) {
        VGA[row * VGA_WIDTH + x] = make_cell(prompt[i], g_color);
    }

    for (int i = 0; i < len && x < VGA_WIDTH; i++, x++) {
        VGA[row * VGA_WIDTH + x] = make_cell(buf[i], g_color);
    }

    int cursor_col = prompt_len + cursor;
    if (cursor_col >= VGA_WIDTH) cursor_col = VGA_WIDTH - 1;
    hw_set_cursor(row, cursor_col);
}

bool read_line(const char* prompt, char* out, int cap) {
    int edit_row = g_row;
    int len = 0;
    int cursor = 0;

    char buf[MAX_LINE];
    char draft[MAX_LINE];
    buf[0] = '\0';
    draft[0] = '\0';

    bool draft_saved = false;
    int hist_pos = g_history_count;

    render_editor_row(edit_row, prompt, buf, len, cursor);

    while (true) {
        KeyEvent ev = next_key_event();
        if (!ev.valid) continue;

        if (ev.extended) {
            if (ev.code == 0x4B) {
                if (cursor > 0) cursor--;
            } else if (ev.code == 0x4D) {
                if (cursor < len) cursor++;
            } else if (ev.code == 0x48) {
                if (g_history_count > 0 && hist_pos > 0) {
                    if (!draft_saved && hist_pos == g_history_count) {
                        str_copy(draft, buf, MAX_LINE);
                        draft_saved = true;
                    }
                    hist_pos--;
                    str_copy(buf, g_history[hist_pos], MAX_LINE);
                    len = str_len(buf);
                    cursor = len;
                }
            } else if (ev.code == 0x50) {
                if (hist_pos < g_history_count - 1) {
                    hist_pos++;
                    str_copy(buf, g_history[hist_pos], MAX_LINE);
                    len = str_len(buf);
                    cursor = len;
                } else if (hist_pos == g_history_count - 1) {
                    hist_pos = g_history_count;
                    if (draft_saved) str_copy(buf, draft, MAX_LINE);
                    else buf[0] = '\0';
                    len = str_len(buf);
                    cursor = len;
                }
            } else if (ev.code == 0x47) {
                cursor = 0;
            } else if (ev.code == 0x4F) {
                cursor = len;
            } else if (ev.code == 0x53) {
                if (cursor < len) {
                    for (int i = cursor; i < len; i++) buf[i] = buf[i + 1];
                    len--;
                }
            }

            render_editor_row(edit_row, prompt, buf, len, cursor);
            continue;
        }

        if (ev.code == 0x1C) {
            str_copy(out, buf, cap);
            put_char('\n');
            return true;
        }

        if (ev.code == 0x0E) {
            if (cursor > 0) {
                for (int i = cursor - 1; i < len; i++) buf[i] = buf[i + 1];
                len--;
                cursor--;
            }
            render_editor_row(edit_row, prompt, buf, len, cursor);
            continue;
        }

        if (ev.code == 0x01) {
            out[0] = '\0';
            put_char('\n');
            return true;
        }

        if (g_ctrl) {
            if (ev.code == 0x1E) cursor = 0;
            else if (ev.code == 0x12) cursor = len;
            else if (ev.code == 0x26) {
                clear_screen();
                edit_row = g_row;
            } else if (ev.code == 0x16 || ev.code == 0x2E) {
                len = 0;
                cursor = 0;
                buf[0] = '\0';
                if (ev.code == 0x2E) {
                    print_line("^C");
                    edit_row = g_row;
                }
            }
            render_editor_row(edit_row, prompt, buf, len, cursor);
            continue;
        }

        if (ev.is_char) {
            if (len >= cap - 1) continue;
            if (str_len(prompt) + len >= VGA_WIDTH - 1) continue;
            for (int i = len; i > cursor; i--) buf[i] = buf[i - 1];
            buf[cursor] = ev.ch;
            len++;
            cursor++;
            buf[len] = '\0';
            render_editor_row(edit_row, prompt, buf, len, cursor);
        }
    }
}

NxfsHeader* fs_header() {
    return (NxfsHeader*)FS_BASE;
}

NxfsEntry* fs_entries() {
    return (NxfsEntry*)(FS_BASE + sizeof(NxfsHeader));
}

u8* fs_data() {
    return (u8*)(FS_BASE + fs_header()->data_offset);
}

bool fs_name_valid(const char* name) {
    int n = str_len(name);
    if (n <= 0 || n >= 32) return false;
    for (int i = 0; i < n; i++) {
        char c = name[i];
        if (c == ' ' || c == '/' || c == '\\' || c == '\t') return false;
    }
    return true;
}

bool fs_entry_name_eq(const NxfsEntry* e, const char* name) {
    return str_eq(e->name, name);
}

u8 fs_type(const NxfsEntry* e) { return e->reserved[0]; }
void fs_set_type(NxfsEntry* e, u8 t) { e->reserved[0] = t; }
u8 fs_perm(const NxfsEntry* e) { return e->reserved[1]; }
void fs_set_perm(NxfsEntry* e, u8 p) { e->reserved[1] = p; }
u8 fs_owner(const NxfsEntry* e) { return e->reserved[2]; }
void fs_set_owner(NxfsEntry* e, u8 id) { e->reserved[2] = id; }
u8 fs_parent(const NxfsEntry* e) { return e->reserved[3]; }
void fs_set_parent(NxfsEntry* e, u8 p) { e->reserved[3] = p; }
bool fs_is_file(const NxfsEntry* e) { return fs_type(e) == NXFS_TYPE_FILE; }
bool fs_is_dir(const NxfsEntry* e) { return fs_type(e) == NXFS_TYPE_DIR; }

bool fs_has_access(const NxfsEntry* e, u8 need) {
    if (g_uid == 0) return true;
    u8 p = fs_perm(e);
    u8 eff = (fs_owner(e) == g_uid) ? ((p >> 3) & 0x7) : (p & 0x7);
    return (eff & need) == need;
}

int fs_find_free_entry() {
    NxfsEntry* entries = fs_entries();
    for (u32 i = 0; i < fs_header()->max_entries; i++) {
        if (!entries[i].used) return (int)i;
    }
    return -1;
}

int fs_find_in_dir(u8 parent, const char* name) {
    NxfsEntry* entries = fs_entries();
    for (u32 i = 0; i < fs_header()->max_entries; i++) {
        if (!entries[i].used) continue;
        if (fs_parent(&entries[i]) != parent) continue;
        if (fs_entry_name_eq(&entries[i], name)) return (int)i;
    }
    return -1;
}

void fs_recount() {
    NxfsHeader* h = fs_header();
    NxfsEntry* entries = fs_entries();
    u32 used_entries = 0;
    u32 used_bytes = 0;

    for (u32 i = 0; i < h->max_entries; i++) {
        if (!entries[i].used) continue;
        used_entries++;
        if (!fs_is_file(&entries[i])) continue;
        u32 end = entries[i].offset + entries[i].size;
        if (end > used_bytes) used_bytes = end;
    }
    h->used_entries = used_entries;
    h->used_bytes = used_bytes;
}

void fs_compact() {
    NxfsHeader* h = fs_header();
    NxfsEntry* entries = fs_entries();
    u8* data = fs_data();

    bool moved[FS_MAX_ENTRIES];
    for (u32 i = 0; i < FS_MAX_ENTRIES; i++) moved[i] = false;

    u32 cursor = 0;
    for (u32 n = 0; n < h->max_entries; n++) {
        int pick = -1;
        u32 min_off = 0xFFFFFFFFu;
        for (u32 i = 0; i < h->max_entries; i++) {
            if (moved[i]) continue;
            if (!entries[i].used || !fs_is_file(&entries[i])) continue;
            if (entries[i].size == 0) {
                moved[i] = true;
                entries[i].offset = cursor;
                continue;
            }
            if (entries[i].offset < min_off) {
                min_off = entries[i].offset;
                pick = (int)i;
            }
        }
        if (pick < 0) break;

        NxfsEntry* e = &entries[pick];
        if (e->offset != cursor) {
            mem_move(data + cursor, data + e->offset, e->size);
            e->offset = cursor;
        }
        cursor += e->size;
        moved[pick] = true;
    }

    h->used_bytes = cursor;
    fs_recount();
}

bool fs_validate() {
    NxfsHeader* h = fs_header();

    if (h->magic[0] != 'N' || h->magic[1] != 'X' || h->magic[2] != 'F' || h->magic[3] != 'S') return false;
    if (h->version != 1) return false;
    if (h->max_entries != FS_MAX_ENTRIES) return false;
    if (h->data_offset < sizeof(NxfsHeader) + sizeof(NxfsEntry) * FS_MAX_ENTRIES) return false;
    if (h->data_offset >= FS_BYTES) return false;
    if (h->data_size > FS_BYTES - h->data_offset) return false;

    NxfsEntry* entries = fs_entries();
    for (u32 i = 0; i < h->max_entries; i++) {
        if (!entries[i].used) continue;
        if (entries[i].name[0] == '\0') return false;
        if (fs_type(&entries[i]) == 0) continue;
        if (fs_type(&entries[i]) != NXFS_TYPE_FILE && fs_type(&entries[i]) != NXFS_TYPE_DIR) return false;
        if (fs_is_file(&entries[i]) && entries[i].offset + entries[i].size > h->data_size) return false;
    }

    return true;
}

bool ata_wait_not_busy() {
    for (u32 i = 0; i < 1000000; i++) {
        u8 st = inb(0x1F7);
        if ((st & 0x80) == 0) return true;
    }
    return false;
}

bool ata_wait_drq() {
    for (u32 i = 0; i < 1000000; i++) {
        u8 st = inb(0x1F7);
        if (st & 0x01) return false;
        if ((st & 0x80) == 0 && (st & 0x08)) return true;
    }
    return false;
}

bool ata_write_sector_lba28(u32 lba, const u8* src512) {
    if (!ata_wait_not_busy()) return false;

    outb(0x1F6, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(0x1F2, 1);
    outb(0x1F3, (u8)(lba & 0xFF));
    outb(0x1F4, (u8)((lba >> 8) & 0xFF));
    outb(0x1F5, (u8)((lba >> 16) & 0xFF));
    outb(0x1F7, 0x30);

    if (!ata_wait_drq()) return false;

    const u16* words = (const u16*)src512;
    for (int i = 0; i < 256; i++) outw(0x1F0, words[i]);

    return ata_wait_not_busy();
}

bool fs_sync_to_disk() {
    const u8* ptr = (const u8*)FS_BASE;

    for (u32 i = 0; i < FS_SECTORS; i++) {
        if (!ata_write_sector_lba28(FS_LBA_START + i, ptr + i * 512)) return false;
    }

    outb(0x1F7, 0xE7);
    return ata_wait_not_busy();
}

bool fs_comp_is_special(const char* c) {
    return str_eq(c, ".") || str_eq(c, "..");
}

bool fs_next_component(const char*& p, char* out, int cap) {
    while (*p == '/') p++;
    if (*p == '\0') return false;
    int i = 0;
    while (*p != '\0' && *p != '/') {
        if (i < cap - 1) out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    return i > 0;
}

bool fs_resolve_dir(const char* path, u8* out_dir) {
    if (path == nullptr || path[0] == '\0') {
        *out_dir = g_cwd;
        return true;
    }
    const char* p = path;
    u8 cur = (p[0] == '/') ? NXFS_ROOT_PARENT : g_cwd;
    char comp[32];
    while (fs_next_component(p, comp, sizeof(comp))) {
        if (str_eq(comp, ".")) continue;
        if (str_eq(comp, "..")) {
            if (cur != NXFS_ROOT_PARENT) cur = fs_parent(&fs_entries()[cur]);
            continue;
        }
        int idx = fs_find_in_dir(cur, comp);
        if (idx < 0 || !fs_is_dir(&fs_entries()[idx])) return false;
        if (!fs_has_access(&fs_entries()[idx], 1)) return false;
        cur = (u8)idx;
    }
    *out_dir = cur;
    return true;
}

bool fs_split_parent_name(const char* path, u8* out_parent, char* out_name) {
    if (path == nullptr || path[0] == '\0') return false;
    const char* p = path;
    u8 cur = (p[0] == '/') ? NXFS_ROOT_PARENT : g_cwd;
    char comp[32];
    bool had = false;

    while (true) {
        if (!fs_next_component(p, comp, sizeof(comp))) break;
        had = true;

        const char* q = p;
        while (*q == '/') q++;
        bool last = (*q == '\0');

        if (!last) {
            if (str_eq(comp, ".")) continue;
            if (str_eq(comp, "..")) {
                if (cur != NXFS_ROOT_PARENT) cur = fs_parent(&fs_entries()[cur]);
                continue;
            }
            int idx = fs_find_in_dir(cur, comp);
            if (idx < 0 || !fs_is_dir(&fs_entries()[idx])) return false;
            if (!fs_has_access(&fs_entries()[idx], 1)) return false;
            cur = (u8)idx;
        } else {
            if (fs_comp_is_special(comp)) return false;
            if (!fs_name_valid(comp)) return false;
            *out_parent = cur;
            str_copy(out_name, comp, 32);
            return true;
        }
    }
    return had ? false : false;
}

int fs_lookup_path(const char* path) {
    u8 parent;
    char name[32];
    if (!fs_split_parent_name(path, &parent, name)) return -1;
    return fs_find_in_dir(parent, name);
}

bool fs_dir_is_empty(u8 dir_idx) {
    NxfsEntry* entries = fs_entries();
    for (u32 i = 0; i < fs_header()->max_entries; i++) {
        if (!entries[i].used) continue;
        if (fs_parent(&entries[i]) == dir_idx) return false;
    }
    return true;
}

bool fs_is_descendant(u8 candidate, u8 ancestor) {
    u8 cur = candidate;
    while (cur != NXFS_ROOT_PARENT) {
        if (cur == ancestor) return true;
        cur = fs_parent(&fs_entries()[cur]);
    }
    return false;
}

bool fs_create_entry(u8 parent, const char* name, u8 type) {
    NxfsHeader* h = fs_header();
    if (!fs_name_valid(name)) return false;
    if (fs_find_in_dir(parent, name) >= 0) return false;
    if (parent != NXFS_ROOT_PARENT && !fs_is_dir(&fs_entries()[parent])) return false;
    if (parent != NXFS_ROOT_PARENT && !fs_has_access(&fs_entries()[parent], 2)) return false;

    int idx = fs_find_free_entry();
    if (idx < 0) return false;

    NxfsEntry* e = &fs_entries()[idx];
    mem_set(e, 0, sizeof(NxfsEntry));
    str_copy(e->name, name, 32);
    e->used = 1;
    e->offset = h->used_bytes;
    e->size = 0;
    fs_set_type(e, type);
    fs_set_perm(e, type == NXFS_TYPE_DIR ? NXFS_PERM_DIR : NXFS_PERM_FILE);
    fs_set_owner(e, g_uid);
    fs_set_parent(e, parent);
    fs_recount();
    return true;
}

bool fs_remove_payload(int idx) {
    NxfsHeader* h = fs_header();
    NxfsEntry* entries = fs_entries();
    NxfsEntry* e = &entries[idx];
    if (!fs_is_file(e) || e->size == 0) return true;

    u32 start = e->offset;
    u32 end = e->offset + e->size;
    u32 tail = h->used_bytes - end;
    if (tail > 0) mem_move(fs_data() + start, fs_data() + end, tail);

    for (u32 i = 0; i < h->max_entries; i++) {
        if (!entries[i].used || !fs_is_file(&entries[i]) || (int)i == idx) continue;
        if (entries[i].offset > start) entries[i].offset -= e->size;
    }
    h->used_bytes -= e->size;
    e->offset = h->used_bytes;
    e->size = 0;
    return true;
}

bool fs_write_entry_data(int idx, const u8* data, u32 len) {
    NxfsHeader* h = fs_header();
    NxfsEntry* e = &fs_entries()[idx];
    if (!fs_is_file(e)) return false;
    if (len > h->data_size) return false;

    fs_compact();
    if (!fs_remove_payload(idx)) return false;
    if (h->used_bytes + len > h->data_size) return false;

    e = &fs_entries()[idx];
    e->offset = h->used_bytes;
    e->size = len;
    if (len > 0) mem_copy(fs_data() + e->offset, data, len);
    h->used_bytes += len;
    fs_recount();
    return true;
}

bool fs_create_file(const char* path) {
    u8 parent;
    char name[32];
    if (!fs_split_parent_name(path, &parent, name)) return false;
    return fs_create_entry(parent, name, NXFS_TYPE_FILE);
}

bool fs_create_dir(const char* path) {
    u8 parent;
    char name[32];
    if (!fs_split_parent_name(path, &parent, name)) return false;
    return fs_create_entry(parent, name, NXFS_TYPE_DIR);
}

bool fs_write_text(const char* path, const char* text) {
    u8 parent;
    char name[32];
    if (!fs_split_parent_name(path, &parent, name)) return false;

    int idx = fs_find_in_dir(parent, name);
    if (idx < 0) {
        if (!fs_create_entry(parent, name, NXFS_TYPE_FILE)) return false;
        idx = fs_find_in_dir(parent, name);
        if (idx < 0) return false;
    }
    if (!fs_is_file(&fs_entries()[idx])) return false;
    if (!fs_has_access(&fs_entries()[idx], 2)) return false;

    return fs_write_entry_data(idx, (const u8*)text, (u32)str_len(text));
}

bool fs_append_text(const char* path, const char* text) {
    int idx = fs_lookup_path(path);
    if (idx < 0) return false;
    NxfsEntry* e = &fs_entries()[idx];
    if (!fs_is_file(e)) return false;
    if (!fs_has_access(e, 2)) return false;

    u32 old_len = e->size;
    u32 add_len = (u32)str_len(text);
    u32 new_len = old_len + add_len;
    if (new_len > sizeof(g_io_tmp)) return false;

    if (old_len > 0) mem_copy(g_io_tmp, fs_data() + e->offset, old_len);
    if (add_len > 0) mem_copy(g_io_tmp + old_len, text, add_len);
    return fs_write_entry_data(idx, g_io_tmp, new_len);
}

bool fs_remove_path(const char* path) {
    int idx = fs_lookup_path(path);
    if (idx < 0) return false;
    NxfsEntry* e = &fs_entries()[idx];
    if (!fs_has_access(e, 2)) return false;
    if (fs_is_dir(e) && !fs_dir_is_empty((u8)idx)) return false;
    if (fs_parent(e) != NXFS_ROOT_PARENT && !fs_has_access(&fs_entries()[fs_parent(e)], 2)) return false;
    if (fs_is_file(e)) fs_remove_payload(idx);
    mem_set(e, 0, sizeof(NxfsEntry));
    fs_compact();
    fs_recount();
    return true;
}

bool fs_move_rename(const char* src_path, const char* dst_path) {
    int src_idx = fs_lookup_path(src_path);
    if (src_idx < 0) return false;

    u8 dst_parent;
    char dst_name[32];
    if (!fs_split_parent_name(dst_path, &dst_parent, dst_name)) return false;
    if (fs_find_in_dir(dst_parent, dst_name) >= 0) return false;

    NxfsEntry* src = &fs_entries()[src_idx];
    if (!fs_has_access(src, 2)) return false;
    if (dst_parent != NXFS_ROOT_PARENT && !fs_has_access(&fs_entries()[dst_parent], 2)) return false;
    if (fs_is_dir(src) && fs_is_descendant(dst_parent, (u8)src_idx)) return false;

    mem_set(src->name, 0, 32);
    str_copy(src->name, dst_name, 32);
    fs_set_parent(src, dst_parent);
    return true;
}

bool fs_chmod_path(const char* mode, const char* path) {
    int idx = fs_lookup_path(path);
    if (idx < 0) return false;
    if (g_uid != 0 && fs_owner(&fs_entries()[idx]) != g_uid) return false;

    int n = str_len(mode);
    if (n < 1 || n > 3) return false;
    u32 value = 0;
    for (int i = 0; i < n; i++) {
        if (mode[i] < '0' || mode[i] > '7') return false;
        value = value * 8 + (u32)(mode[i] - '0');
    }
    fs_set_perm(&fs_entries()[idx], (u8)(value & 0x3F));
    return true;
}

bool fs_chown_path(const char* owner, const char* path) {
    int idx = fs_lookup_path(path);
    if (idx < 0) return false;
    if (g_uid != 0) return false;

    int n = str_len(owner);
    if (n <= 0 || n > 3) return false;
    u32 id = 0;
    for (int i = 0; i < n; i++) {
        if (owner[i] < '0' || owner[i] > '9') return false;
        id = id * 10 + (u32)(owner[i] - '0');
        if (id > 255) return false;
    }
    fs_set_owner(&fs_entries()[idx], (u8)id);
    return true;
}

void fs_print_perm(u8 p) {
    const char rwx[3] = {'r', 'w', 'x'};
    u8 u = (p >> 3) & 0x7;
    u8 o = p & 0x7;
    for (int i = 0; i < 3; i++) put_char((u & (1 << (2 - i))) ? rwx[i] : '-');
    for (int i = 0; i < 3; i++) put_char((o & (1 << (2 - i))) ? rwx[i] : '-');
}

void fs_list_dir(u8 dir) {
    NxfsHeader* h = fs_header();
    NxfsEntry* entries = fs_entries();
    bool any = false;
    for (u32 i = 0; i < h->max_entries; i++) {
        if (!entries[i].used) continue;
        if (fs_parent(&entries[i]) != dir) continue;
        if (!fs_has_access(&entries[i], 4)) continue;
        any = true;

        put_char(fs_is_dir(&entries[i]) ? 'd' : '-');
        fs_print_perm(fs_perm(&entries[i]));
        put_char(' ');
        print(entries[i].name);
        if (fs_is_dir(&entries[i])) put_char('/');
        put_char(' ');
        print_u32(entries[i].size);
        print_line("B");
    }
    if (!any) print_line("(empty)");
}

void fs_list_path(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        fs_list_dir(g_cwd);
        return;
    }
    u8 dir;
    if (fs_resolve_dir(path, &dir)) {
        fs_list_dir(dir);
        return;
    }
    int idx = fs_lookup_path(path);
    if (idx < 0) {
        print_line("ls: path not found");
        return;
    }
    NxfsEntry* e = &fs_entries()[idx];
    put_char(fs_is_dir(e) ? 'd' : '-');
    fs_print_perm(fs_perm(e));
    put_char(' ');
    print(e->name);
    if (fs_is_dir(e)) put_char('/');
    put_char(' ');
    print_u32(e->size);
    print_line("B");
}

bool fs_cat(const char* path) {
    int idx = fs_lookup_path(path);
    if (idx < 0) return false;
    NxfsEntry* e = &fs_entries()[idx];
    if (!fs_is_file(e) || !fs_has_access(e, 4)) return false;
    u8* ptr = fs_data() + e->offset;
    for (u32 i = 0; i < e->size; i++) {
        char ch = (char)ptr[i];
        if (ch == '\0') ch = ' ';
        put_char(ch);
    }
    put_char('\n');
    return true;
}

bool fs_stat(const char* path) {
    int idx = fs_lookup_path(path);
    if (idx < 0) return false;
    NxfsEntry* e = &fs_entries()[idx];
    print("name: ");
    print_line(e->name);
    print("type: ");
    print_line(fs_is_dir(e) ? "directory" : "file");
    print("size: ");
    print_u32(e->size);
    print_line(" bytes");
    print("offset: ");
    print_u32(e->offset);
    put_char('\n');
    print("owner: ");
    print_u32(fs_owner(e));
    put_char('\n');
    print("perms: ");
    fs_print_perm(fs_perm(e));
    put_char('\n');
    return true;
}

void fs_df() {
    NxfsHeader* h = fs_header();
    print("capacity: ");
    print_u32(h->data_size);
    print_line(" bytes");
    print("used: ");
    print_u32(h->used_bytes);
    print_line(" bytes");
    print("free: ");
    print_u32(h->data_size - h->used_bytes);
    print_line(" bytes");
    print("files: ");
    print_u32(h->used_entries);
    put_char('/');
    print_u32(h->max_entries);
    put_char('\n');
}

void fs_pwd() {
    if (g_cwd == NXFS_ROOT_PARENT) {
        print_line("/");
        return;
    }
    u8 stack[FS_MAX_ENTRIES];
    int top = 0;
    u8 cur = g_cwd;
    while (cur != NXFS_ROOT_PARENT && top < (int)FS_MAX_ENTRIES) {
        stack[top++] = cur;
        cur = fs_parent(&fs_entries()[cur]);
    }
    put_char('/');
    for (int i = top - 1; i >= 0; i--) {
        print(fs_entries()[stack[i]].name);
        if (i > 0) put_char('/');
    }
    put_char('\n');
}

void fs_build_prompt(char* out, int cap) {
    int p = 0;
    const char* prefix = "noxis:";
    for (int i = 0; prefix[i] != '\0' && p < cap - 1; i++) out[p++] = prefix[i];

    if (g_cwd == NXFS_ROOT_PARENT) {
        if (p < cap - 1) out[p++] = '/';
    } else {
        u8 stack[FS_MAX_ENTRIES];
        int top = 0;
        u8 cur = g_cwd;
        while (cur != NXFS_ROOT_PARENT && top < (int)FS_MAX_ENTRIES) {
            stack[top++] = cur;
            cur = fs_parent(&fs_entries()[cur]);
        }
        if (p < cap - 1) out[p++] = '/';
        for (int i = top - 1; i >= 0; i--) {
            const char* nm = fs_entries()[stack[i]].name;
            for (int j = 0; nm[j] != '\0' && p < cap - 1; j++) out[p++] = nm[j];
            if (i > 0 && p < cap - 1) out[p++] = '/';
        }
    }

    const char* tail = "$ ";
    for (int i = 0; tail[i] != '\0' && p < cap - 1; i++) out[p++] = tail[i];
    out[p] = '\0';
}

bool fs_read_text_file(const char* path, char* out, int cap) {
    int idx = fs_lookup_path(path);
    if (idx < 0) return false;
    NxfsEntry* e = &fs_entries()[idx];
    if (!fs_is_file(e) || !fs_has_access(e, 4)) return false;
    if (cap <= 0) return false;

    int n = (int)e->size;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = (char)fs_data()[e->offset + (u32)i];
    out[n] = '\0';
    return true;
}

void modules_clear() {
    g_module_count = 0;
    for (int i = 0; i < MAX_MODULES; i++) {
        g_modules[i].used = false;
        g_modules[i].command[0] = '\0';
        g_modules[i].message[0] = '\0';
    }
}

void modules_parse_line(const char* line, char* key, int key_cap, char* value, int val_cap) {
    int k = 0;
    int v = 0;
    bool sep = false;
    for (int i = 0; line[i] != '\0'; i++) {
        char c = line[i];
        if (!sep && c == '=') {
            sep = true;
            continue;
        }
        if (!sep) {
            if (k < key_cap - 1) key[k++] = c;
        } else {
            if (v < val_cap - 1) value[v++] = c;
        }
    }
    key[k] = '\0';
    value[v] = '\0';
}

bool modules_parse_content(const char* text, ShellModule* out) {
    char line[180];
    int li = 0;
    out->command[0] = '\0';
    out->message[0] = '\0';

    for (int i = 0;; i++) {
        char c = text[i];
        if (c != '\n' && c != '\r' && c != '\0') {
            if (li < (int)sizeof(line) - 1) line[li++] = c;
            continue;
        }

        line[li] = '\0';
        if (li > 0) {
            char key[32];
            char value[140];
            modules_parse_line(line, key, sizeof(key), value, sizeof(value));
            if (str_ieq(key, "command")) str_copy(out->command, value, (int)sizeof(out->command));
            if (str_ieq(key, "message")) str_copy(out->message, value, (int)sizeof(out->message));
        }

        li = 0;
        if (c == '\0') break;
    }

    return out->command[0] != '\0' && out->message[0] != '\0';
}

void modules_load() {
    modules_clear();
    u8 modules_dir;
    if (!fs_resolve_dir("/system/modules", &modules_dir)) return;

    NxfsHeader* h = fs_header();
    NxfsEntry* entries = fs_entries();

    for (u32 i = 0; i < h->max_entries && g_module_count < MAX_MODULES; i++) {
        if (!entries[i].used) continue;
        if (fs_parent(&entries[i]) != modules_dir) continue;
        if (!fs_is_file(&entries[i])) continue;

        char path[96];
        str_copy(path, "/system/modules/", (int)sizeof(path));
        int p = str_len(path);
        for (int j = 0; entries[i].name[j] != '\0' && p < (int)sizeof(path) - 1; j++) {
            path[p++] = entries[i].name[j];
        }
        path[p] = '\0';

        char content[256];
        if (!fs_read_text_file(path, content, (int)sizeof(content))) continue;

        ShellModule mod{};
        if (!modules_parse_content(content, &mod)) continue;

        g_modules[g_module_count] = mod;
        g_modules[g_module_count].used = true;
        g_module_count++;
    }
}

bool modules_execute(const char* cmd, const char* args) {
    for (int i = 0; i < g_module_count; i++) {
        if (!g_modules[i].used) continue;
        if (!str_ieq(g_modules[i].command, cmd)) continue;
        print_line(g_modules[i].message);
        if (args != nullptr && args[0] != '\0') {
            print("args: ");
            print_line(args);
        }
        return true;
    }
    return false;
}

void modules_list() {
    if (g_module_count == 0) {
        print_line("modules: none loaded");
        return;
    }
    print("modules loaded: ");
    print_u32((u32)g_module_count);
    put_char('\n');
    for (int i = 0; i < g_module_count; i++) {
        print("- ");
        print_line(g_modules[i].command);
    }
}

void execute_command(const char* line);

void nxapp_trim(char* s) {
    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    int len = str_len(s);
    int end = len;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
    int w = 0;
    for (int i = start; i < end; i++) s[w++] = s[i];
    s[w] = '\0';
}

bool nxapp_next_line(const char* text, int* pos, char* out, int cap) {
    int p = *pos;
    if (text[p] == '\0') return false;
    int o = 0;
    while (text[p] != '\0' && text[p] != '\n' && text[p] != '\r') {
        if (o < cap - 1) out[o++] = text[p];
        p++;
    }
    out[o] = '\0';
    while (text[p] == '\n' || text[p] == '\r') p++;
    *pos = p;
    return true;
}

void nxapp_expand_args(const char* in, const char* args, char* out, int cap) {
    int o = 0;
    for (int i = 0; in[i] != '\0' && o < cap - 1; i++) {
        if (in[i] == '$' && in[i + 1] == 'A' && in[i + 2] == 'R' && in[i + 3] == 'G' && in[i + 4] == 'S') {
            for (int j = 0; args[j] != '\0' && o < cap - 1; j++) out[o++] = args[j];
            i += 4;
            continue;
        }
        out[o++] = in[i];
    }
    out[o] = '\0';
}

bool nxapp_parse_manifest_full(const char* content,
                               int* code_start,
                               char* app_id,
                               int app_id_cap,
                               char* name,
                               int name_cap,
                               char* version,
                               int ver_cap,
                               char* entry,
                               int entry_cap,
                               char* description,
                               int desc_cap) {
    int pos = 0;
    char line[180];
    app_id[0] = '\0';
    name[0] = '\0';
    version[0] = '\0';
    description[0] = '\0';
    str_copy(entry, "main", entry_cap);

    if (!nxapp_next_line(content, &pos, line, sizeof(line))) return false;
    nxapp_trim(line);
    if (!str_eq(line, "NXAPP-1")) return false;

    while (nxapp_next_line(content, &pos, line, sizeof(line))) {
        nxapp_trim(line);
        if (line[0] == '\0') continue;
        if (str_eq(line, "---")) {
            *code_start = pos;
            return true;
        }
        char key[32];
        char val[140];
        modules_parse_line(line, key, sizeof(key), val, sizeof(val));
        nxapp_trim(key);
        nxapp_trim(val);
        if (str_ieq(key, "id")) str_copy(app_id, val, app_id_cap);
        if (str_ieq(key, "name")) str_copy(name, val, name_cap);
        if (str_ieq(key, "version")) str_copy(version, val, ver_cap);
        if (str_ieq(key, "entry")) str_copy(entry, val, entry_cap);
        if (str_ieq(key, "description")) str_copy(description, val, desc_cap);
    }
    return false;
}

bool nxapp_run(const char* path, const char* args, bool info_only) {
    char content[1024];
    if (!fs_read_text_file(path, content, (int)sizeof(content))) {
        print_line("nxapp: cannot read file");
        return false;
    }

    int code_start = 0;
    char app_id[24];
    char name[48];
    char version[24];
    char entry[24];
    char description[80];
    if (!nxapp_parse_manifest_full(content,
                                   &code_start,
                                   app_id,
                                   sizeof(app_id),
                                   name,
                                   sizeof(name),
                                   version,
                                   sizeof(version),
                                   entry,
                                   sizeof(entry),
                                   description,
                                   sizeof(description))) {
        print_line("nxapp: invalid format (need NXAPP-1 + ---)");
        return false;
    }

    if (info_only) {
        print("id: ");
        print_line(app_id[0] ? app_id : "(none)");
        print("name: ");
        print_line(name[0] ? name : "(unnamed)");
        print("version: ");
        print_line(version[0] ? version : "(none)");
        print("entry: ");
        print_line(entry[0] ? entry : "(main)");
        if (description[0] != '\0') {
            print("description: ");
            print_line(description);
        }
        int pos = code_start;
        char line[180];
        u32 count = 0;
        while (nxapp_next_line(content, &pos, line, sizeof(line))) {
            nxapp_trim(line);
            if (line[0] == '\0' || line[0] == '#') continue;
            count++;
        }
        print("steps: ");
        print_u32(count);
        put_char('\n');
        return true;
    }

    if (g_exec_depth >= MAX_EXEC_DEPTH) {
        print_line("nxapp: max execution depth reached");
        return false;
    }

    g_exec_depth++;
    int pos = code_start;
    char line[180];
    char expanded[200];
    while (nxapp_next_line(content, &pos, line, sizeof(line))) {
        nxapp_trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        nxapp_expand_args(line, args ? args : "", expanded, sizeof(expanded));
        execute_command(expanded);
    }
    g_exec_depth--;
    return true;
}

void appmgr_clear() {
    g_app_count = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        g_apps[i].used = false;
        g_apps[i].id[0] = '\0';
        g_apps[i].name[0] = '\0';
        g_apps[i].version[0] = '\0';
        g_apps[i].path[0] = '\0';
        g_apps[i].entry[0] = '\0';
        g_apps[i].description[0] = '\0';
    }
}

bool appmgr_id_valid(const char* id) {
    int n = str_len(id);
    if (n <= 0 || n >= (int)sizeof(g_apps[0].id)) return false;
    for (int i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

void appmgr_derive_id_from_path(const char* path, char* out, int cap) {
    int last = 0;
    for (int i = 0; path[i] != '\0'; i++) if (path[i] == '/') last = i + 1;
    int o = 0;
    for (int i = last; path[i] != '\0' && o < cap - 1; i++) {
        char c = to_lower(path[i]);
        if (c == '.') break;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out[o++] = c;
        else out[o++] = '-';
    }
    out[o] = '\0';
}

int appmgr_find(const char* id) {
    for (int i = 0; i < g_app_count; i++) {
        if (g_apps[i].used && str_ieq(g_apps[i].id, id)) return i;
    }
    return -1;
}

bool appmgr_add_or_update(const AppRecord* in) {
    int idx = appmgr_find(in->id);
    if (idx < 0) {
        if (g_app_count >= MAX_APPS) return false;
        idx = g_app_count++;
    }
    g_apps[idx] = *in;
    g_apps[idx].used = true;
    return true;
}

void appmgr_parse_db_line(const char* line, AppRecord* out) {
    out->used = false;
    int field = 0;
    char* targets[6] = {out->id, out->name, out->version, out->path, out->entry, out->description};
    int caps[6] = {(int)sizeof(out->id), (int)sizeof(out->name), (int)sizeof(out->version), (int)sizeof(out->path), (int)sizeof(out->entry), (int)sizeof(out->description)};
    int pos[6] = {0, 0, 0, 0, 0, 0};

    for (int i = 0; ; i++) {
        char c = line[i];
        if (c == '|' || c == '\0') {
            if (field < 6) targets[field][pos[field]] = '\0';
            field++;
            if (c == '\0') break;
            continue;
        }
        if (field < 6 && pos[field] < caps[field] - 1) targets[field][pos[field]++] = c;
    }

    if (out->id[0] == '\0' || out->path[0] == '\0') return;
    if (out->entry[0] == '\0') str_copy(out->entry, "main", (int)sizeof(out->entry));
    out->used = true;
}

bool appmgr_save_db() {
    char text[4096];
    int o = 0;
    for (int i = 0; i < g_app_count; i++) {
        if (!g_apps[i].used) continue;
        const char* cols[6] = {g_apps[i].id, g_apps[i].name, g_apps[i].version, g_apps[i].path, g_apps[i].entry, g_apps[i].description};
        for (int c = 0; c < 6; c++) {
            for (int j = 0; cols[c][j] != '\0' && o < (int)sizeof(text) - 1; j++) text[o++] = cols[c][j];
            if (c < 5 && o < (int)sizeof(text) - 1) text[o++] = '|';
        }
        if (o < (int)sizeof(text) - 1) text[o++] = '\n';
    }
    text[o] = '\0';
    if (!fs_write_text("/system/appdb/installed.db", text)) return false;
    return fs_sync_to_disk();
}

void appmgr_load_db() {
    appmgr_clear();
    char text[4096];
    if (!fs_read_text_file("/system/appdb/installed.db", text, (int)sizeof(text))) return;

    int pos = 0;
    char line[220];
    while (nxapp_next_line(text, &pos, line, sizeof(line))) {
        nxapp_trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        AppRecord rec{};
        appmgr_parse_db_line(line, &rec);
        if (!rec.used) continue;
        if (g_app_count < MAX_APPS) g_apps[g_app_count++] = rec;
    }
}

bool appmgr_manifest_from_path(const char* path, AppRecord* out) {
    char content[1024];
    if (!fs_read_text_file(path, content, (int)sizeof(content))) return false;

    int code_start = 0;
    char app_id[24];
    char name[48];
    char version[16];
    char entry[24];
    char description[80];
    if (!nxapp_parse_manifest_full(content,
                                   &code_start,
                                   app_id,
                                   sizeof(app_id),
                                   name,
                                   sizeof(name),
                                   version,
                                   sizeof(version),
                                   entry,
                                   sizeof(entry),
                                   description,
                                   sizeof(description))) return false;

    if (app_id[0] == '\0') appmgr_derive_id_from_path(path, app_id, (int)sizeof(app_id));
    if (!appmgr_id_valid(app_id)) return false;

    out->used = true;
    str_copy(out->id, app_id, (int)sizeof(out->id));
    str_copy(out->name, name[0] ? name : app_id, (int)sizeof(out->name));
    str_copy(out->version, version[0] ? version : "0.0", (int)sizeof(out->version));
    str_copy(out->path, path, (int)sizeof(out->path));
    str_copy(out->entry, entry[0] ? entry : "main", (int)sizeof(out->entry));
    str_copy(out->description, description, (int)sizeof(out->description));
    return true;
}

bool appmgr_install(const char* path) {
    AppRecord rec{};
    if (!appmgr_manifest_from_path(path, &rec)) return false;
    if (!appmgr_add_or_update(&rec)) return false;
    return appmgr_save_db();
}

bool appmgr_remove(const char* id) {
    int idx = appmgr_find(id);
    if (idx < 0) return false;
    for (int i = idx + 1; i < g_app_count; i++) g_apps[i - 1] = g_apps[i];
    g_app_count--;
    return appmgr_save_db();
}

bool appmgr_run(const char* id, const char* args) {
    int idx = appmgr_find(id);
    if (idx < 0) return false;
    return nxapp_run(g_apps[idx].path, args, false);
}

bool appmgr_info(const char* id) {
    int idx = appmgr_find(id);
    if (idx < 0) return false;
    AppRecord* a = &g_apps[idx];
    print("id: "); print_line(a->id);
    print("name: "); print_line(a->name);
    print("version: "); print_line(a->version);
    print("path: "); print_line(a->path);
    print("entry: "); print_line(a->entry);
    if (a->description[0] != '\0') {
        print("description: ");
        print_line(a->description);
    }
    return true;
}

void appmgr_list() {
    if (g_app_count == 0) {
        print_line("apps: none installed");
        return;
    }
    for (int i = 0; i < g_app_count; i++) {
        print(g_apps[i].id);
        print("  ");
        print(g_apps[i].version);
        print("  ");
        print_line(g_apps[i].name);
    }
}

u32 appmgr_rescan(const char* dir_path) {
    u8 dir;
    if (!fs_resolve_dir(dir_path, &dir)) return 0;
    NxfsHeader* h = fs_header();
    NxfsEntry* entries = fs_entries();
    u32 installed = 0;
    for (u32 i = 0; i < h->max_entries; i++) {
        if (!entries[i].used) continue;
        if (fs_parent(&entries[i]) != dir) continue;
        if (!fs_is_file(&entries[i])) continue;
        if (!str_ends_with(entries[i].name, ".nxapp")) continue;

        char path[96];
        str_copy(path, dir_path, (int)sizeof(path));
        int p = str_len(path);
        if (p > 0 && path[p - 1] != '/' && p < (int)sizeof(path) - 1) path[p++] = '/';
        for (int j = 0; entries[i].name[j] != '\0' && p < (int)sizeof(path) - 1; j++) path[p++] = entries[i].name[j];
        path[p] = '\0';
        if (appmgr_install(path)) installed++;
    }
    return installed;
}

void reboot_system() {
    while (inb(0x64) & 0x02) {
    }
    outb(0x64, 0xFE);
    while (1) asm volatile("hlt");
}

void show_help() {
    print_line("Commands:");
    print_line("help                     Show this help");
    print_line("clear | cls              Clear screen");
    print_line("echo <text>              Print text");
    print_line("history                  Show command history");
    print_line("uname                    Show system name");
    print_line("ver                      Show shell version");
    print_line("reboot                   Reboot machine");
    print_line("pwd                      Print current directory");
    print_line("cd <path>                Change directory");
    print_line("mkdir <path>             Create directory");
    print_line("ls | dir [path]          List files/directories");
    print_line("cat <path>               Print file contents");
    print_line("touch <path>             Create empty file");
    print_line("write <path> <text>      Overwrite file");
    print_line("append <path> <text>     Append text (full mode)");
    print_line("rm <path>                Delete file/directory");
    print_line("mv <src> <dst>           Rename/move entry");
    print_line("stat <path>              Entry metadata");
    print_line("chmod <mode> <path>      Change mode (octal)");
    print_line("chown <uid> <path>       Change owner (root only)");
    print_line("modules                  List loaded modules");
    print_line("modreload                Reload modules from /system/modules");
    print_line("app list                 List installed apps");
    print_line("app info <id>            Show app metadata");
    print_line("app install <path>       Install/update .nxapp");
    print_line("app remove <id>          Uninstall app");
    print_line("app run <id> [args]      Run installed app");
    print_line("app rescan [dir]         Bulk install .nxapp from dir");
    print_line("<app-id> [args]          Run installed app directly");
    print_line("nxinfo <path>            Show .nxapp metadata");
    print_line("nxrun <path> [args]      Run .nxapp program");
    print_line("df                       Filesystem usage");
    print_line("sync                     Flush FS memory to disk");
}

void show_history() {
    if (g_history_count == 0) {
        print_line("history: empty");
        return;
    }
    for (int i = 0; i < g_history_count; i++) {
        print(g_history[i]);
        put_char('\n');
    }
}

bool next_token(const char*& s, char* out, int cap) {
    s = skip_spaces(s);
    if (*s == '\0') return false;

    int i = 0;
    while (*s != '\0' && *s != ' ' && i < cap - 1) {
        out[i++] = *s++;
    }
    out[i] = '\0';
    return true;
}

void execute_command(const char* line) {
    const char* s = skip_spaces(line);
    if (*s == '\0') return;

    char cmd[24];
    if (!next_token(s, cmd, sizeof(cmd))) return;

    if (str_ieq(cmd, "help") || str_eq(cmd, "?")) {
        show_help();
        return;
    }
    if (str_ieq(cmd, "clear") || str_ieq(cmd, "cls")) {
        clear_screen();
        return;
    }
    if (str_ieq(cmd, "echo")) {
        print_line(skip_spaces(s));
        return;
    }
    if (str_ieq(cmd, "history")) {
        show_history();
        return;
    }
    if (str_ieq(cmd, "uname")) {
        print_line("Noxis Kernel x86_64");
        return;
    }
    if (str_ieq(cmd, "ver") || str_ieq(cmd, "version")) {
        print_line("Noxis 0.4 with directories and permissions");
        return;
    }
    if (str_ieq(cmd, "reboot") || str_ieq(cmd, "restart") || str_ieq(cmd, "shutdown")) {
        print_line("Rebooting...");
        reboot_system();
        return;
    }

    if (!g_fs_ready) {
        print_line("filesystem: not mounted");
        return;
    }

    if (str_ieq(cmd, "pwd")) {
        fs_pwd();
        return;
    }
    if (str_ieq(cmd, "cd")) {
        char path[64];
        if (!next_token(s, path, sizeof(path))) {
            g_cwd = NXFS_ROOT_PARENT;
            return;
        }
        u8 dir;
        if (!fs_resolve_dir(path, &dir)) {
            print_line("cd: no such directory");
            return;
        }
        g_cwd = dir;
        return;
    }
    if (str_ieq(cmd, "mkdir")) {
        char path[64];
        if (!next_token(s, path, sizeof(path))) {
            print_line("mkdir: usage mkdir <path>");
            return;
        }
        if (!fs_create_dir(path)) {
            print_line("mkdir: failed");
            return;
        }
        if (!fs_sync_to_disk()) print_line("mkdir: warning sync failed");
        return;
    }
    if (str_ieq(cmd, "ls") || str_ieq(cmd, "dir")) {
        char path[64];
        if (next_token(s, path, sizeof(path))) fs_list_path(path);
        else fs_list_path(nullptr);
        return;
    }
    if (str_ieq(cmd, "df")) {
        fs_df();
        return;
    }
    if (str_ieq(cmd, "sync")) {
        if (fs_sync_to_disk()) print_line("sync: ok");
        else print_line("sync: disk write error");
        return;
    }
    if (str_ieq(cmd, "modules")) {
        modules_list();
        return;
    }
    if (str_ieq(cmd, "modreload")) {
        modules_load();
        print("modreload: loaded ");
        print_u32((u32)g_module_count);
        print_line(" modules");
        return;
    }
    if (str_ieq(cmd, "app")) {
        char sub[16];
        if (!next_token(s, sub, sizeof(sub))) {
            print_line("app: usage app <list|info|install|remove|run|rescan> ...");
            return;
        }
        if (str_ieq(sub, "list")) {
            appmgr_list();
            return;
        }
        if (str_ieq(sub, "info")) {
            char id[24];
            if (!next_token(s, id, sizeof(id))) {
                print_line("app info: usage app info <id>");
                return;
            }
            if (!appmgr_info(id)) print_line("app info: not found");
            return;
        }
        if (str_ieq(sub, "install")) {
            char path[96];
            if (!next_token(s, path, sizeof(path))) {
                print_line("app install: usage app install <path>");
                return;
            }
            if (!appmgr_install(path)) print_line("app install: failed");
            else print_line("app install: ok");
            return;
        }
        if (str_ieq(sub, "remove")) {
            char id[24];
            if (!next_token(s, id, sizeof(id))) {
                print_line("app remove: usage app remove <id>");
                return;
            }
            if (!appmgr_remove(id)) print_line("app remove: not found/failed");
            else print_line("app remove: ok");
            return;
        }
        if (str_ieq(sub, "run")) {
            char id[24];
            if (!next_token(s, id, sizeof(id))) {
                print_line("app run: usage app run <id> [args]");
                return;
            }
            if (!appmgr_run(id, skip_spaces(s))) print_line("app run: failed");
            return;
        }
        if (str_ieq(sub, "rescan")) {
            char dir[64];
            const char* dir_path = "/system/apps";
            if (next_token(s, dir, sizeof(dir))) dir_path = dir;
            u32 count = appmgr_rescan(dir_path);
            print("app rescan: installed ");
            print_u32(count);
            print_line(" app(s)");
            return;
        }
        print_line("app: unknown subcommand");
        return;
    }
    if (str_ieq(cmd, "nxinfo")) {
        char path[96];
        if (!next_token(s, path, sizeof(path))) {
            print_line("nxinfo: usage nxinfo <path>");
            return;
        }
        if (!nxapp_run(path, "", true)) {
            print_line("nxinfo: failed");
        }
        return;
    }
    if (str_ieq(cmd, "nxrun")) {
        char path[96];
        if (!next_token(s, path, sizeof(path))) {
            print_line("nxrun: usage nxrun <path> [args]");
            return;
        }
        const char* args = skip_spaces(s);
        if (!nxapp_run(path, args, false)) {
            print_line("nxrun: failed");
        }
        return;
    }

    char name1[64];
    if (str_ieq(cmd, "cat")) {
        if (!next_token(s, name1, sizeof(name1))) {
            print_line("cat: usage cat <path>");
            return;
        }
        if (!fs_cat(name1)) print_line("cat: failed");
        return;
    }
    if (str_ieq(cmd, "touch")) {
        if (!next_token(s, name1, sizeof(name1))) {
            print_line("touch: usage touch <path>");
            return;
        }
        if (!fs_create_file(name1)) {
            print_line("touch: cannot create");
            return;
        }
        if (!fs_sync_to_disk()) print_line("touch: warning sync failed");
        return;
    }
    if (str_ieq(cmd, "rm")) {
        if (!next_token(s, name1, sizeof(name1))) {
            print_line("rm: usage rm <path>");
            return;
        }
        if (!fs_remove_path(name1)) {
            print_line("rm: failed");
            return;
        }
        if (!fs_sync_to_disk()) print_line("rm: warning sync failed");
        return;
    }
    if (str_ieq(cmd, "stat")) {
        if (!next_token(s, name1, sizeof(name1))) {
            print_line("stat: usage stat <path>");
            return;
        }
        if (!fs_stat(name1)) print_line("stat: not found");
        return;
    }
    if (str_ieq(cmd, "mv")) {
        char name2[64];
        if (!next_token(s, name1, sizeof(name1)) || !next_token(s, name2, sizeof(name2))) {
            print_line("mv: usage mv <src> <dst>");
            return;
        }
        if (!fs_move_rename(name1, name2)) {
            print_line("mv: failed");
            return;
        }
        if (!fs_sync_to_disk()) print_line("mv: warning sync failed");
        return;
    }
    if (str_ieq(cmd, "write")) {
        if (!next_token(s, name1, sizeof(name1))) {
            print_line("write: usage write <path> <text>");
            return;
        }
        s = skip_spaces(s);
        if (*s == '\0') {
            print_line("write: missing text");
            return;
        }
        if (!fs_write_text(name1, s)) {
            print_line("write: failed");
            return;
        }
        if (!fs_sync_to_disk()) print_line("write: warning sync failed");
        return;
    }
    if (str_ieq(cmd, "append")) {
        if (!next_token(s, name1, sizeof(name1))) {
            print_line("append: usage append <path> <text>");
            return;
        }
        s = skip_spaces(s);
        if (*s == '\0') {
            print_line("append: missing text");
            return;
        }
        if (!fs_append_text(name1, s)) {
            print_line("append: failed");
            return;
        }
        if (!fs_sync_to_disk()) print_line("append: warning sync failed");
        return;
    }
    if (str_ieq(cmd, "chmod")) {
        char mode[8];
        if (!next_token(s, mode, sizeof(mode)) || !next_token(s, name1, sizeof(name1))) {
            print_line("chmod: usage chmod <mode> <path>");
            return;
        }
        if (!fs_chmod_path(mode, name1)) {
            print_line("chmod: failed");
            return;
        }
        if (!fs_sync_to_disk()) print_line("chmod: warning sync failed");
        return;
    }
    if (str_ieq(cmd, "chown")) {
        char owner[8];
        if (!next_token(s, owner, sizeof(owner)) || !next_token(s, name1, sizeof(name1))) {
            print_line("chown: usage chown <uid> <path>");
            return;
        }
        if (!fs_chown_path(owner, name1)) {
            print_line("chown: failed");
            return;
        }
        if (!fs_sync_to_disk()) print_line("chown: warning sync failed");
        return;
    }

    if (modules_execute(cmd, skip_spaces(s))) return;
    if (appmgr_run(cmd, skip_spaces(s))) return;

    print(cmd);
    print_line(": command not found");
}

void fs_mount() {
    if (!fs_validate()) {
        g_fs_ready = false;
        print_line("filesystem: invalid NXFS image");
        return;
    }

    NxfsHeader* h = fs_header();
    NxfsEntry* entries = fs_entries();
    for (u32 i = 0; i < h->max_entries; i++) {
        if (!entries[i].used) continue;
        u8 t = fs_type(&entries[i]);
        if (t != NXFS_TYPE_FILE && t != NXFS_TYPE_DIR) {
            fs_set_type(&entries[i], NXFS_TYPE_FILE);
            fs_set_parent(&entries[i], NXFS_ROOT_PARENT);
        }
        if (fs_perm(&entries[i]) == 0) {
            fs_set_perm(&entries[i], fs_is_dir(&entries[i]) ? NXFS_PERM_DIR : NXFS_PERM_FILE);
        }
        if (fs_parent(&entries[i]) != NXFS_ROOT_PARENT) {
            u8 p = fs_parent(&entries[i]);
            if (p >= h->max_entries || !entries[p].used || !fs_is_dir(&entries[p])) {
                fs_set_parent(&entries[i], NXFS_ROOT_PARENT);
            }
        }
    }

    fs_compact();
    g_fs_ready = true;
    g_cwd = NXFS_ROOT_PARENT;

    print("filesystem: NXFS mounted, entries=");
    print_u32(h->used_entries);
    put_char(',');
    put_char(' ');
    print("used=");
    print_u32(h->used_bytes);
    print_line(" bytes");
}

}  // namespace

extern "C" void kmain() {
    clear_screen();
    print_line("Noxis shell (Windows/Linux style)");
    print_line("Type 'help' for commands");

    fs_mount();
    modules_load();
    appmgr_load_db();
    if (g_app_count == 0) {
        appmgr_rescan("/system/apps");
    }

    char line[MAX_LINE];
    char prompt[96];
    while (1) {
        fs_build_prompt(prompt, sizeof(prompt));
        if (!read_line(prompt, line, MAX_LINE)) continue;
        add_history(line);
        execute_command(line);
    }
}


/*
 * Ugaris Skill Planner
 *
 * A full build calculator on top of the client's own cost function:
 *
 * - Every raisable skill with the exact cost of its next point and how
 *   many points your unused experience can buy right now.
 * - Targets per skill with the exact total cost, a build summary, and an
 *   APPLY button that raises everything for you - one confirmed click,
 *   each raise acknowledged by the server before the next is sent.
 * - Builds are saved and loaded as JSON files keyed by skill NAME, so you
 *   can share them with other players (Discord, wiki, ...) and they can
 *   load them and work toward the same build.
 * - The window is draggable by its title bar and remembers its position.
 *
 * Commands:
 *   #plan                 - Toggle the planner window
 *   #plan save <name>     - Save current targets as a build
 *   #plan load <name>     - Load a build (matched by skill names)
 *   #plan builds          - List saved builds
 *   #plan clear           - Clear all targets
 *   #plan apply           - Apply the build (same as the button)
 *   #plan stop            - Stop applying
 *   #plan help            - Help
 *
 * Mouse: click a skill to select it, [-] / [+] move its target
 * (shift = 10 points), drag the title bar to move the window, wheel
 * scrolls, [x] closes.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "amod/amod.h"

#define PLANNER_VERSION "1.1.0"

#define COL_INFO "\260c2"
#define COL_PLAN "\260c6"

/* exported by the client but missing from the SDK header */
DLL_IMPORT int raise_cost(int v, int n);

#define CL_RAISE_OP 15

/* center alignment exists in the client but is missing from the SDK header */
#ifndef RENDER_TEXT_CENTER
#define RENDER_TEXT_CENTER 1
#endif

#define C_BG     IRGB(2, 2, 4)
#define C_BG2    IRGB(5, 4, 9)
#define C_GOLD   IRGB(31, 26, 8)
#define C_GOLD2  IRGB(24, 18, 4)
#define C_YELLOW IRGB(31, 28, 2)
#define C_GREEN  IRGB(12, 28, 14)
#define C_RED    IRGB(31, 12, 8)
#define C_BTN    IRGB(10, 9, 16)

#define PANEL_W 540
#define ROW_H   15
#define HDR_H   64
#define MAX_TARGET_STEP 1000

/* ---------------------------------------------------------------- state */

static int s_ingame;
static unsigned int s_ticks;
static int s_open;
static int s_scroll;
static int s_sel_v = -1;
static int s_sort_cheap;
static int s_mx, s_my;
static short s_targets[V_MAX];
static char s_build[40];          /* name of the last saved/loaded build */

/* panel position: anchor + user drag offset (persisted) */
static int s_off_x, s_off_y;
static int s_px, s_py, s_ph;
static int s_drag, s_drag_mx, s_drag_my;

/* apply engine */
static int s_apply;               /* 0 idle, 1 armed, 2 running */
static unsigned int s_arm_until;
static int s_wait_v;              /* skill we sent a raise for, -1 = none */
static int s_wait_base;
static unsigned int s_wait_deadline;
static int s_apply_done, s_apply_total;

/* click targets rebuilt every frame */
enum { HIT_CLOSE = 1, HIT_SORT, HIT_CLEAR, HIT_APPLY, HIT_ROW, HIT_MINUS, HIT_PLUS };
struct hit {
    short x0, y0, x1, y1;
    short what, arg;
};
static struct hit s_hits[96];
static int s_hit_count;

/* ---------------------------------------------------------------- utils */

static long long unused_exp(void)
{
    return (long long)experience - (long long)experience_used;
}

static const char *fmt_thousands(long long v)
{
    static char buf[4][32];
    static int slot;
    char raw[24], *out;
    int len, i, o = 0;

    slot = (slot + 1) & 3;
    out = buf[slot];
    if (v < 0) { out[o++] = '-'; v = -v; }
    len = snprintf(raw, sizeof(raw), "%lld", v);
    for (i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = 0;
    return out;
}

static long long cost_range(int v, int from, int to)
{
    long long sum = 0;
    int k;

    for (k = from; k < to; k++) {
        sum += raise_cost(v, k);
        if (sum > (1LL << 60)) break;
    }
    return sum;
}

static int affordable_points(int v, int base)
{
    long long left = unused_exp();
    int n = 0;

    while (n < MAX_TARGET_STEP) {
        long long c = raise_cost(v, base + n);
        if (c > left) break;
        left -= c;
        n++;
    }
    return n;
}

static int base_of(int v)
{
    int i;

    for (i = 0; i < skltab_cnt; i++)
        if (skltab[i].v == v) return skltab[i].base;
    return 0;
}

static const char *name_of(int v)
{
    int i;

    for (i = 0; i < skltab_cnt; i++)
        if (skltab[i].v == v && skltab[i].name[0]) return skltab[i].name;
    return NULL;
}

static void add_hit(int x0, int y0, int x1, int y1, int what, int arg)
{
    if (s_hit_count >= (int)(sizeof(s_hits) / sizeof(s_hits[0]))) return;
    s_hits[s_hit_count].x0 = (short)x0;
    s_hits[s_hit_count].y0 = (short)y0;
    s_hits[s_hit_count].x1 = (short)x1;
    s_hits[s_hit_count].y1 = (short)y1;
    s_hits[s_hit_count].what = (short)what;
    s_hits[s_hit_count].arg = (short)arg;
    s_hit_count++;
}

/* build summary: points and total cost over all targets */
static void build_totals(int *points, long long *total)
{
    int v, base;
    long long sum = 0;
    int pts = 0;

    for (v = 0; v < V_MAX; v++) {
        if (!s_targets[v]) continue;
        base = base_of(v);
        if (s_targets[v] <= base) continue;
        pts += s_targets[v] - base;
        sum += cost_range(v, base, s_targets[v]);
    }
    *points = pts;
    *total = sum;
}

/* ---------------------------------------------------------------- config */

static void cfg_path(char *out, size_t n, const char *file)
{
    const char *dir = client_config_dir();

    snprintf(out, n, "%s%s", dir && *dir ? dir : "", file);
}

static void save_config(void)
{
    char path[512];
    FILE *f;
    int v;

    cfg_path(path, sizeof(path), "skill_planner.cfg");
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "sort=%d\noffx=%d\noffy=%d\n", s_sort_cheap, s_off_x, s_off_y);
    for (v = 0; v < V_MAX; v++)
        if (s_targets[v]) fprintf(f, "target_%d=%d\n", v, s_targets[v]);
    fclose(f);
}

static void load_config(void)
{
    char path[512], line[64];
    FILE *f;

    cfg_path(path, sizeof(path), "skill_planner.cfg");
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "sort=", 5)) s_sort_cheap = atoi(line + 5);
        else if (!strncmp(line, "offx=", 5)) s_off_x = atoi(line + 5);
        else if (!strncmp(line, "offy=", 5)) s_off_y = atoi(line + 5);
        else if (!strncmp(line, "target_", 7)) {
            int v = atoi(line + 7);
            char *eq = strchr(line, '=');
            if (eq && v >= 0 && v < V_MAX) s_targets[v] = (short)atoi(eq + 1);
        }
    }
    fclose(f);
}

/* ------------------------------------------------------- build files */

static void sanitize(const char *in, char *out, size_t n)
{
    size_t o = 0;

    while (*in && o + 1 < n) {
        char c = *in++;
        out[o++] = isalnum((unsigned char)c) ? (char)tolower((unsigned char)c) : '_';
    }
    out[o] = 0;
    if (!out[0]) snprintf(out, n, "build");
}

static void build_index_add(const char *name)
{
    char path[512], line[64];
    FILE *f;

    cfg_path(path, sizeof(path), "skill_builds.txt");
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            if (!strcmp(line, name)) { fclose(f); return; }
        }
        fclose(f);
    }
    f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", name); fclose(f); }
}

static int save_build(const char *rawname)
{
    char name[40], path[512], file[80];
    FILE *f;
    int v, n = 0;

    sanitize(rawname, name, sizeof(name));
    snprintf(file, sizeof(file), "skill_build_%s.json", name);
    cfg_path(path, sizeof(path), file);
    f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "{\n  \"game\": \"ugaris\",\n  \"build\": \"%s\",\n", name);
    fprintf(f, "  \"character\": \"%.39s\",\n  \"level\": %d,\n",
            username[0] ? username : "?", exp2level((int)experience));
    fprintf(f, "  \"targets\": {\n");
    for (v = 0; v < V_MAX; v++) {
        const char *nm;
        if (!s_targets[v]) continue;
        nm = name_of(v);
        if (!nm) continue;
        fprintf(f, "%s    \"%s\": %d", n ? ",\n" : "", nm, s_targets[v]);
        n++;
    }
    fprintf(f, "\n  }\n}\n");
    fclose(f);

    build_index_add(name);
    snprintf(s_build, sizeof(s_build), "%s", name);
    addline(COL_PLAN "Planner: saved build \"%s\" (%d skills) to %s", name, n, path);
    return n;
}

/* Tiny parser for exactly the JSON this mod writes: finds "targets" and
 * then consumes "Name": number pairs until the closing brace. */
static int load_build(const char *rawname)
{
    char name[40], path[512], file[80];
    FILE *f;
    char *data, *p;
    long size;
    int matched = 0, skipped = 0, v;

    sanitize(rawname, name, sizeof(name));
    snprintf(file, sizeof(file), "skill_build_%s.json", name);
    cfg_path(path, sizeof(path), file);
    f = fopen(path, "rb");
    if (!f) {
        addline(COL_INFO "Planner: no build \"%s\" (%s)", name, path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) { fclose(f); return -1; }
    data = malloc((size_t)size + 1);
    if (!data) { fclose(f); return -1; }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return -1;
    }
    data[size] = 0;
    fclose(f);

    p = strstr(data, "\"targets\"");
    if (!p) { free(data); addline(COL_INFO "Planner: %s has no targets", file); return -1; }
    p = strchr(p, '{');
    if (!p) { free(data); return -1; }
    p++;

    for (v = 0; v < V_MAX; v++) s_targets[v] = 0;

    while (*p) {
        char skname[80];
        size_t o = 0;
        long val;
        int i, found;

        while (*p && *p != '"' && *p != '}') p++;
        if (!*p || *p == '}') break;
        p++;
        while (*p && *p != '"' && o + 1 < sizeof(skname)) skname[o++] = *p++;
        skname[o] = 0;
        if (*p != '"') break;
        p++;
        while (*p && (*p == ':' || isspace((unsigned char)*p))) p++;
        val = strtol(p, &p, 10);

        found = 0;
        for (i = 0; i < skltab_cnt; i++) {
            if (skltab[i].v < 0 || !skltab[i].button) continue;
            if (!strcasecmp(skltab[i].name, skname)) {
                if (val > 0 && val < 10000)
                    s_targets[skltab[i].v] = (short)val;
                found = 1;
                matched++;
                break;
            }
        }
        if (!found) skipped++;

        while (*p && *p != ',' && *p != '}') p++;
        if (*p == ',') p++;
    }

    free(data);
    snprintf(s_build, sizeof(s_build), "%s", name);
    save_config();
    if (skipped)
        addline(COL_PLAN "Planner: loaded \"%s\" - %d skills (%d unknown skipped)",
                name, matched, skipped);
    else
        addline(COL_PLAN "Planner: loaded \"%s\" - %d skills", name, matched);
    return matched;
}

static void list_builds(void)
{
    char path[512], line[64];
    FILE *f;
    int n = 0;

    cfg_path(path, sizeof(path), "skill_builds.txt");
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            if (!len) continue;
            addline(COL_PLAN "  %s", line);
            n++;
        }
        fclose(f);
    }
    if (!n) addline(COL_INFO "Planner: no saved builds yet - #plan save <name>");
    else addline(COL_INFO "Planner: %d build%s - #plan load <name>", n, n == 1 ? "" : "s");
}

/* ---------------------------------------------------------- apply engine */

static void apply_stop(const char *why)
{
    if (s_apply == 2 && why)
        addline(COL_INFO "Planner: apply stopped (%s) - %d of %d points raised",
                why, s_apply_done, s_apply_total);
    s_apply = 0;
    s_wait_v = -1;
}

static void apply_start(void)
{
    int points;
    long long total;

    build_totals(&points, &total);
    if (!points) {
        addline(COL_INFO "Planner: no targets to apply");
        return;
    }
    if (total > unused_exp()) {
        addline(COL_INFO "Planner: build needs %s exp, you have %s",
                fmt_thousands(total), fmt_thousands(unused_exp()));
        return;
    }
    s_apply = 2;
    s_wait_v = -1;
    s_apply_done = 0;
    s_apply_total = points;
    addline(COL_PLAN "Planner: raising %d point%s for %s exp...",
            points, points == 1 ? "" : "s", fmt_thousands(total));
}

static void apply_tick(void)
{
    int v, base;

    if (s_apply != 2) return;

    if (s_wait_v >= 0) {
        if (base_of(s_wait_v) > s_wait_base) {
            s_apply_done++;
            s_wait_v = -1;
        } else if (s_ticks > s_wait_deadline) {
            apply_stop("the server did not confirm a raise");
            return;
        } else {
            return;
        }
    }

    for (v = 0; v < V_MAX; v++) {
        if (!s_targets[v]) continue;
        base = base_of(v);
        if (s_targets[v] <= base) { s_targets[v] = 0; continue; }
        if (raise_cost(v, base) > unused_exp()) {
            apply_stop("out of experience");
            save_config();
            return;
        }
        {
            unsigned char pkt[3];
            pkt[0] = CL_RAISE_OP;
            pkt[1] = (unsigned char)(v & 0xFF);
            pkt[2] = (unsigned char)((v >> 8) & 0xFF);
            client_send(pkt, 3);
        }
        s_wait_v = v;
        s_wait_base = base;
        s_wait_deadline = s_ticks + 48;
        return;
    }

    addline(COL_PLAN "Planner: build applied - %d point%s raised.",
            s_apply_done, s_apply_done == 1 ? "" : "s");
    s_apply = 0;
    save_config();
}

/* --------------------------------------------------------------- drawing */

struct row {
    int v;
    int base, curr;
    int cost;
    const char *name;
};

static int collect_rows(struct row *out, int max)
{
    int i, n = 0;

    for (i = 0; i < skltab_cnt && n < max; i++) {
        if (skltab[i].v < 0 || !skltab[i].button) continue;
        out[n].v = skltab[i].v;
        out[n].base = skltab[i].base;
        out[n].curr = skltab[i].curr;
        out[n].cost = raise_cost(skltab[i].v, skltab[i].base);
        out[n].name = skltab[i].name;
        n++;
    }

    if (s_sort_cheap) {
        int j;
        for (i = 1; i < n; i++) {
            struct row tmp = out[i];
            for (j = i; j > 0 && out[j - 1].cost > tmp.cost; j--) out[j] = out[j - 1];
            out[j] = tmp;
        }
    }
    return n;
}

static int panel_height(void)
{
    int map_h = doty(DOT_MBR) - doty(DOT_MTL);
    int h = map_h - 40;

    if (h > 460) h = 460;
    if (h < 280) h = 280;
    return h;
}

static void draw_button(int x0, int y, const char *label, unsigned short col, int what, int arg)
{
    int w = render_text_length(RENDER_TEXT_SMALL, label);

    render_rounded_rect_filled_alpha(x0, y - 2, x0 + w + 12, y + 12, 4, C_BTN, 220);
    render_rounded_rect_alpha(x0, y - 2, x0 + w + 12, y + 12, 4, C_GOLD2, 90);
    render_text(x0 + 6, y, col, RENDER_TEXT_SMALL, label);
    add_hit(x0 - 2, y - 4, x0 + w + 14, y + 14, what, arg);
}

static void draw_panel(void)
{
    struct row rows[64];
    char buf[192];
    int n, i, y, max_rows, skipped = 0, more = 0;
    int points;
    long long total;

    /* anchor + drag offset, clamped so the title bar stays reachable */
    s_px = dotx(DOT_MTL) + 16 + s_off_x;
    s_py = doty(DOT_MTL) + 12 + s_off_y;
    s_ph = panel_height();
    if (s_px < dotx(DOT_MTL) - PANEL_W + 120) s_px = dotx(DOT_MTL) - PANEL_W + 120;
    if (s_px > dotx(DOT_MBR) - 120) s_px = dotx(DOT_MBR) - 120;
    if (s_py < doty(DOT_MTL)) s_py = doty(DOT_MTL);
    if (s_py > doty(DOT_MBR) - 60) s_py = doty(DOT_MBR) - 60;

    s_hit_count = 0;

    render_rounded_rect_filled_alpha(s_px, s_py, s_px + PANEL_W, s_py + s_ph, 8, C_BG, 232);
    render_rounded_rect_filled_alpha(s_px, s_py, s_px + PANEL_W, s_py + 19, 8, C_BG2, 255);
    render_rect_alpha(s_px, s_py + 12, s_px + PANEL_W, s_py + 19, C_BG2, 255);
    render_gradient_rect_h(s_px + 8, s_py + 19, s_px + PANEL_W - 8, s_py + 20, C_GOLD, C_BG, 170);
    render_rounded_rect_alpha(s_px, s_py, s_px + PANEL_W, s_py + s_ph, 8, C_GOLD2, 110);

    if (s_build[0])
        snprintf(buf, sizeof(buf), "Skill Planner - %s", s_build);
    else
        snprintf(buf, sizeof(buf), "Skill Planner");
    render_text(s_px + 8, s_py + 4, C_GOLD, RENDER_TEXT_SMALL | RENDER_TEXT_SHADED, buf);
    render_text(s_px + PANEL_W - 16, s_py + 4, C_RED, RENDER_TEXT_SMALL, "[x]");
    add_hit(s_px + PANEL_W - 28, s_py, s_px + PANEL_W, s_py + 19, HIT_CLOSE, 0);

    snprintf(buf, sizeof(buf), "level %d   unused %s   spent %s",
             exp2level((int)experience), fmt_thousands(unused_exp()),
             fmt_thousands((long long)experience_used));
    render_text(s_px + 8, s_py + 24, textcolor, RENDER_TEXT_SMALL, buf);

    draw_button(s_px + 8, s_py + 40, s_sort_cheap ? "cheapest first" : "table order",
                s_sort_cheap ? C_YELLOW : textcolor, HIT_SORT, 0);
    draw_button(s_px + 128, s_py + 40, "clear targets", textcolor, HIT_CLEAR, 0);
    render_text(s_px + PANEL_W - 8, s_py + 40, graycolor,
                RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "drag title to move");

    n = collect_rows(rows, 64);
    if (!n) {
        render_text(s_px + 8, s_py + HDR_H, graycolor, RENDER_TEXT_SMALL,
                    "No skill data yet - open your skill window once.");
        return;
    }

    y = s_py + HDR_H - 2;
    render_text(s_px + 8, y, graycolor, RENDER_TEXT_SMALL, "skill");
    render_text(s_px + 160, y, graycolor, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "have");
    render_text(s_px + 245, y, graycolor, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "next");
    render_text(s_px + 292, y, graycolor, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "afford");
    render_text(s_px + 310, y, graycolor, RENDER_TEXT_SMALL, "  target");
    render_text(s_px + PANEL_W - 8, y, graycolor, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "build cost");
    y += ROW_H;

    max_rows = (s_ph - HDR_H - ROW_H - 30) / ROW_H;
    if (max_rows > 64) max_rows = 64;

    for (i = 0; i < n; i++) {
        int afford, sel;

        if (skipped < s_scroll) { skipped++; continue; }
        if ((y - (s_py + HDR_H - 2 + ROW_H)) / ROW_H >= max_rows) { more++; continue; }

        sel = (rows[i].v == s_sel_v);
        if (sel)
            render_rect_alpha(s_px + 4, y - 1, s_px + PANEL_W - 4, y + ROW_H - 2, C_GOLD2, 70);
        add_hit(s_px + 4, y - 1, s_px + 300, y + ROW_H - 1, HIT_ROW, rows[i].v);

        if (rows[i].curr != rows[i].base)
            snprintf(buf, sizeof(buf), "%d (%d)", rows[i].base, rows[i].curr);
        else
            snprintf(buf, sizeof(buf), "%d", rows[i].base);

        render_text(s_px + 8, y, sel ? C_GOLD : whitecolor, RENDER_TEXT_SMALL, rows[i].name);
        render_text(s_px + 160, y, whitecolor, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, buf);
        render_text(s_px + 245, y, C_YELLOW, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT,
                    fmt_thousands(rows[i].cost));
        afford = affordable_points(rows[i].v, rows[i].base);
        snprintf(buf, sizeof(buf), "+%d", afford);
        render_text(s_px + 292, y, afford ? C_GREEN : graycolor,
                    RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, buf);

        /* target controls: generous [-] and [+] buttons around the value */
        {
            int has = s_targets[rows[i].v] > rows[i].base;
            int bx = s_px + 316;

            if (sel || has) {
                render_rounded_rect_filled_alpha(bx, y - 1, bx + 20, y + 12, 3, C_BTN, 220);
                render_text(bx + 6, y, C_RED, RENDER_TEXT_SMALL, "-");
                add_hit(bx - 3, y - 3, bx + 23, y + 15, HIT_MINUS, rows[i].v);

                snprintf(buf, sizeof(buf), "%d", has ? s_targets[rows[i].v] : rows[i].base);
                render_text(bx + 42, y, has ? C_GOLD : graycolor,
                            RENDER_TEXT_SMALL | RENDER_TEXT_CENTER, buf);

                render_rounded_rect_filled_alpha(bx + 64, y - 1, bx + 84, y + 12, 3, C_BTN, 220);
                render_text(bx + 70, y, C_GREEN, RENDER_TEXT_SMALL, "+");
                add_hit(bx + 61, y - 3, bx + 87, y + 15, HIT_PLUS, rows[i].v);
            }

            if (has) {
                long long tc = cost_range(rows[i].v, rows[i].base, s_targets[rows[i].v]);
                render_text(s_px + PANEL_W - 8, y,
                            unused_exp() >= tc ? C_GREEN : C_RED,
                            RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, fmt_thousands(tc));
            }
        }

        y += ROW_H;
    }

    if (s_scroll > 0)
        render_text(s_px + PANEL_W - 8, s_py + HDR_H - 14, textcolor,
                    RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "^ more");
    if (more > 0) {
        snprintf(buf, sizeof(buf), "v %d more", more);
        render_text(s_px + PANEL_W - 8, s_py + s_ph - 32, textcolor,
                    RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, buf);
    }

    /* footer: build summary + apply */
    build_totals(&points, &total);
    render_rect_alpha(s_px + 4, s_py + s_ph - 22, s_px + PANEL_W - 4, s_py + s_ph - 21, C_GOLD2, 120);
    if (s_apply == 2) {
        snprintf(buf, sizeof(buf), "raising... %d / %d", s_apply_done, s_apply_total);
        render_text(s_px + 8, s_py + s_ph - 16, C_YELLOW, RENDER_TEXT_SMALL, buf);
        draw_button(s_px + PANEL_W - 60, s_py + s_ph - 16, "stop", C_RED, HIT_APPLY, 0);
    } else if (points) {
        int can = unused_exp() >= total;
        snprintf(buf, sizeof(buf), "build: %d point%s, %s exp %s", points,
                 points == 1 ? "" : "s", fmt_thousands(total),
                 can ? "- affordable" : "- not affordable");
        render_text(s_px + 8, s_py + s_ph - 16, can ? C_GREEN : C_RED, RENDER_TEXT_SMALL, buf);
        if (can) {
            const char *lbl = (s_apply == 1 && s_ticks < s_arm_until)
                                  ? "really raise?" : "apply build";
            draw_button(s_px + PANEL_W - 8 - render_text_length(RENDER_TEXT_SMALL, lbl) - 12,
                        s_py + s_ph - 16, lbl,
                        s_apply == 1 ? C_RED : C_GREEN, HIT_APPLY, 0);
        }
    } else {
        render_text(s_px + 8, s_py + s_ph - 16, graycolor, RENDER_TEXT_SMALL,
                    "no targets - select a skill and use + / #plan save|load <name> for builds");
    }
}

/* ------------------------------------------------------------- mod hooks */

DLL_EXPORT char *amod_version(void)
{
    return "Skill Planner " PLANNER_VERSION;
}

DLL_EXPORT void amod_init(void)
{
}

DLL_EXPORT void amod_exit(void)
{
}

DLL_EXPORT void amod_gamestart(void)
{
    s_ingame = 1;
    s_ticks = 0;
    s_apply = 0;
    s_wait_v = -1;
    load_config();
}

DLL_EXPORT void amod_tick(void)
{
    if (!s_ingame) return;
    s_ticks++;
    if (s_ticks == 24)
        addline("Skill Planner %s loaded. Type #plan to open.", PLANNER_VERSION);
    apply_tick();
}

DLL_EXPORT void amod_frame(void)
{
    if (!s_ingame || !s_open) return;
    draw_panel();
}

DLL_EXPORT void amod_mouse_move(int x, int y)
{
    if (s_drag) {
        s_off_x += x - s_drag_mx;
        s_off_y += y - s_drag_my;
        s_drag_mx = x;
        s_drag_my = y;
    }
    s_mx = x;
    s_my = y;
}

static int inside_panel(int x, int y)
{
    return s_open && x >= s_px && x <= s_px + PANEL_W && y >= s_py && y <= s_py + s_ph;
}

DLL_EXPORT int amod_mouse_over(int x, int y)
{
    return inside_panel(x, y);
}

static void bump_target(int v, int dir)
{
    int base = base_of(v);
    int step = vk_shift ? 10 : 1;

    if (!s_targets[v]) s_targets[v] = (short)base;
    s_targets[v] = (short)(s_targets[v] + dir * step);
    if (s_targets[v] <= base) s_targets[v] = 0;
    if (s_targets[v] > base + MAX_TARGET_STEP) s_targets[v] = (short)(base + MAX_TARGET_STEP);
    save_config();
}

DLL_EXPORT int amod_mouse_click(int x, int y, int what)
{
    if (!s_ingame || !s_open) return 0;

    if (what == SDL_MOUM_LUP && s_drag) {
        s_drag = 0;
        save_config();
        return 1;
    }

    if (what == SDL_MOUM_WHEEL) {
        if (!inside_panel(s_mx, s_my)) return 0;
        s_scroll -= y * 3;
        if (s_scroll < 0) s_scroll = 0;
        if (s_scroll > 60) s_scroll = 60;
        return 1;
    }

    if (!inside_panel(x, y)) return 0;

    if (what == SDL_MOUM_LDOWN) {
        int i;

        for (i = 0; i < s_hit_count; i++) {
            if (x < s_hits[i].x0 || x > s_hits[i].x1 || y < s_hits[i].y0 || y > s_hits[i].y1)
                continue;
            switch (s_hits[i].what) {
            case HIT_CLOSE:
                s_open = 0;
                return 1;
            case HIT_SORT:
                s_sort_cheap = !s_sort_cheap;
                s_scroll = 0;
                save_config();
                return 1;
            case HIT_CLEAR: {
                int v;
                for (v = 0; v < V_MAX; v++) s_targets[v] = 0;
                s_build[0] = 0;
                save_config();
                return 1;
            }
            case HIT_APPLY:
                if (s_apply == 2) {
                    apply_stop("stopped by you");
                } else if (s_apply == 1 && s_ticks < s_arm_until) {
                    apply_start();
                } else {
                    s_apply = 1;
                    s_arm_until = s_ticks + 3 * 24;
                }
                return 1;
            case HIT_ROW:
                s_sel_v = s_hits[i].arg;
                return 1;
            case HIT_MINUS:
                s_sel_v = s_hits[i].arg;
                bump_target(s_hits[i].arg, -1);
                return 1;
            case HIT_PLUS:
                s_sel_v = s_hits[i].arg;
                bump_target(s_hits[i].arg, +1);
                return 1;
            }
        }

        /* title bar: start dragging */
        if (y <= s_py + 19) {
            s_drag = 1;
            s_drag_mx = x;
            s_drag_my = y;
            return 1;
        }
        return 1;
    }

    return 1;
}

/* ---- Options > Mods ------------------------------------------------------
 * The window's own sort button writes the same variable; both save. */
DLL_EXPORT int amod_options_count(void)
{
    return 2;
}

DLL_EXPORT int amod_option_get(int index, struct amod_option *out)
{
    memset(out, 0, sizeof(*out));
    if (index == 0) {
        out->type = AMOD_OPT_HEADER;
        snprintf(out->label, sizeof(out->label), "Skill Planner");
        return 1;
    }
    if (index == 1) {
        out->type = AMOD_OPT_TOGGLE;
        out->value = s_sort_cheap;
        snprintf(out->label, sizeof(out->label), "Sort by cheapest first");
        return 1;
    }
    return 0;
}

DLL_EXPORT void amod_option_set(int index, int value)
{
    if (index != 1) return;
    s_sort_cheap = value;
    save_config();
}

DLL_EXPORT int amod_client_cmd(const char *buf)
{
    if (strncmp(buf, "#plan", 5)) return 0;
    buf += 5;
    while (*buf == ' ') buf++;

    if (!*buf) {
        s_open = !s_open;
        return 1;
    }
    if (!strcmp(buf, "help")) {
        addline(COL_INFO "Skill Planner %s - #plan toggles the window.", PLANNER_VERSION);
        addline(COL_INFO "#plan save/load <name>, #plan builds, #plan clear, #plan apply, #plan stop");
        addline(COL_INFO "Click a skill, [-]/[+] set its target (shift = 10). Builds are JSON files");
        addline(COL_INFO "in your client config dir - share them and load each other's builds.");
        return 1;
    }
    if (!strncmp(buf, "save", 4)) {
        const char *arg = buf + 4;
        while (*arg == ' ') arg++;
        save_build(*arg ? arg : (s_build[0] ? s_build : "mybuild"));
        return 1;
    }
    if (!strncmp(buf, "load", 4)) {
        const char *arg = buf + 4;
        while (*arg == ' ') arg++;
        if (!*arg) { addline(COL_INFO "Planner: #plan load <name>"); return 1; }
        load_build(arg);
        return 1;
    }
    if (!strcmp(buf, "builds")) {
        list_builds();
        return 1;
    }
    if (!strcmp(buf, "clear")) {
        int v;
        for (v = 0; v < V_MAX; v++) s_targets[v] = 0;
        s_build[0] = 0;
        save_config();
        addline(COL_INFO "Planner: targets cleared");
        return 1;
    }
    if (!strcmp(buf, "apply")) {
        if (s_apply == 1 && s_ticks < s_arm_until) apply_start();
        else {
            int points;
            long long total;
            build_totals(&points, &total);
            if (!points) { addline(COL_INFO "Planner: no targets to apply"); return 1; }
            s_apply = 1;
            s_arm_until = s_ticks + 3 * 24;
            addline(COL_PLAN "Planner: %d point%s for %s exp - type #plan apply again within 3s to confirm",
                    points, points == 1 ? "" : "s", fmt_thousands(total));
        }
        return 1;
    }
    if (!strcmp(buf, "stop")) {
        if (s_apply == 2) apply_stop("stopped by you");
        else addline(COL_INFO "Planner: nothing is being applied");
        return 1;
    }
    addline(COL_INFO "Unknown #plan option. Try #plan help");
    return 1;
}

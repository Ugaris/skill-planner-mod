/*
 * Ugaris Skill Planner
 *
 * Makes the raise-cost math visible: every raisable skill with the cost of
 * its next point, how many points your unused experience can buy right
 * now, and per-skill targets with the exact total cost to reach them.
 *
 * Commands:
 *   #plan           - Toggle the planner window
 *   #plan help      - Help
 *
 * Mouse: click a skill to select it, click [-] / [+] to move its target
 * (the row's total updates live), mouse wheel scrolls, [x] closes.
 * Click [cheapest] / [table] in the header to switch sorting.
 *
 * Uses the client's own skill table and raise-cost function, so the
 * numbers match the skill window exactly. Targets persist in
 * <client config dir>/skill_planner.cfg.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "amod/amod.h"

#define PLANNER_VERSION "1.0.0"

#define COL_INFO "\260c2"

/* exported by the client but missing from the SDK header */
DLL_IMPORT int raise_cost(int v, int n);

#define C_BG     IRGB(2, 2, 4)
#define C_BG2    IRGB(5, 4, 9)
#define C_GOLD   IRGB(31, 26, 8)
#define C_GOLD2  IRGB(24, 18, 4)
#define C_YELLOW IRGB(31, 28, 2)
#define C_GREEN  IRGB(12, 28, 14)
#define C_RED    IRGB(31, 12, 8)

#define PANEL_W 470
#define ROW_H   14
#define HDR_H   46
#define MAX_TARGET_STEP 1000

/* ---------------------------------------------------------------- state */

static int s_ingame;
static unsigned int s_ticks;
static int s_open;
static int s_scroll;
static int s_sel_v = -1;          /* selected skill (wire index) */
static int s_sort_cheap;          /* 0 = table order, 1 = by next cost */
static int s_mx, s_my;
static short s_targets[V_MAX];    /* 0 = none */

/* panel position, computed per frame */
static int s_px, s_py, s_ph;

/* rows drawn this frame, for hit testing */
static short s_row_v[64];
static short s_row_y[64];
static int s_row_count;

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

/* total cost to raise skill v from `from` to `to` (base values) */
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

/* how many points of skill v the unused exp can buy right now */
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

/* ---------------------------------------------------------------- config */

static void save_config(void)
{
    const char *dir = client_config_dir();
    char path[512];
    FILE *f;
    int v;

    snprintf(path, sizeof(path), "%sskill_planner.cfg", dir && *dir ? dir : "");
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "sort=%d\n", s_sort_cheap);
    for (v = 0; v < V_MAX; v++)
        if (s_targets[v]) fprintf(f, "target_%d=%d\n", v, s_targets[v]);
    fclose(f);
}

static void load_config(void)
{
    const char *dir = client_config_dir();
    char path[512], line[64];
    FILE *f;

    snprintf(path, sizeof(path), "%sskill_planner.cfg", dir && *dir ? dir : "");
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "sort=", 5)) s_sort_cheap = atoi(line + 5);
        else if (!strncmp(line, "target_", 7)) {
            int v = atoi(line + 7);
            char *eq = strchr(line, '=');
            if (eq && v >= 0 && v < V_MAX) s_targets[v] = (short)atoi(eq + 1);
        }
    }
    fclose(f);
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

    if (h > 430) h = 430;
    if (h < 260) h = 260;
    return h;
}

static void draw_panel(void)
{
    struct row rows[64];
    char buf[192];
    int n, i, y, max_rows, skipped = 0, more = 0;

    s_px = dotx(DOT_MTL) + 16;
    s_py = doty(DOT_MTL) + 12;
    s_ph = panel_height();

    render_rounded_rect_filled_alpha(s_px, s_py, s_px + PANEL_W, s_py + s_ph, 8, C_BG, 232);
    render_rounded_rect_filled_alpha(s_px, s_py, s_px + PANEL_W, s_py + 19, 8, C_BG2, 255);
    render_rect_alpha(s_px, s_py + 12, s_px + PANEL_W, s_py + 19, C_BG2, 255);
    render_gradient_rect_h(s_px + 8, s_py + 19, s_px + PANEL_W - 8, s_py + 20, C_GOLD, C_BG, 170);
    render_rounded_rect_alpha(s_px, s_py, s_px + PANEL_W, s_py + s_ph, 8, C_GOLD2, 110);

    render_text(s_px + 8, s_py + 4, C_GOLD, RENDER_TEXT_SMALL | RENDER_TEXT_SHADED, "Skill Planner");
    render_text(s_px + PANEL_W - 16, s_py + 4, IRGB(31, 12, 8), RENDER_TEXT_SMALL, "[x]");

    snprintf(buf, sizeof(buf), "level %d   unused %s   spent %s",
             exp2level((int)experience), fmt_thousands(unused_exp()),
             fmt_thousands((long long)experience_used));
    render_text(s_px + 8, s_py + 24, textcolor, RENDER_TEXT_SMALL, buf);
    render_text(s_px + PANEL_W - 8, s_py + 24,
                s_sort_cheap ? C_YELLOW : graycolor,
                RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT,
                s_sort_cheap ? "[cheapest first]" : "[table order]");

    n = collect_rows(rows, 64);
    if (!n) {
        render_text(s_px + 8, s_py + HDR_H, graycolor, RENDER_TEXT_SMALL,
                    "No skill data yet - open your skill window once.");
        s_row_count = 0;
        return;
    }

    /* column headers */
    y = s_py + HDR_H - 2;
    render_text(s_px + 8, y, graycolor, RENDER_TEXT_SMALL, "skill");
    render_text(s_px + 168, y, graycolor, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "have");
    render_text(s_px + 258, y, graycolor, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "next point");
    render_text(s_px + 308, y, graycolor, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "afford");
    render_text(s_px + 8 + 320, y, graycolor, RENDER_TEXT_SMALL, "target");
    y += ROW_H;

    max_rows = (s_ph - HDR_H - ROW_H - 8) / ROW_H;
    if (max_rows > 64) max_rows = 64;
    s_row_count = 0;

    for (i = 0; i < n; i++) {
        int afford, sel;

        if (skipped < s_scroll) { skipped++; continue; }
        if (s_row_count >= max_rows) { more++; continue; }

        sel = (rows[i].v == s_sel_v);
        if (sel)
            render_rect_alpha(s_px + 4, y - 1, s_px + PANEL_W - 4, y + ROW_H - 2, C_GOLD2, 70);

        if (rows[i].curr != rows[i].base)
            snprintf(buf, sizeof(buf), "%d (%d)", rows[i].base, rows[i].curr);
        else
            snprintf(buf, sizeof(buf), "%d", rows[i].base);

        render_text(s_px + 8, y, sel ? C_GOLD : whitecolor, RENDER_TEXT_SMALL, rows[i].name);
        render_text(s_px + 168, y, whitecolor, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, buf);
        render_text(s_px + 258, y, C_YELLOW, RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT,
                    fmt_thousands(rows[i].cost));
        afford = affordable_points(rows[i].v, rows[i].base);
        snprintf(buf, sizeof(buf), "+%d", afford);
        render_text(s_px + 308, y, afford ? C_GREEN : graycolor,
                    RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, buf);

        if (s_targets[rows[i].v] > rows[i].base) {
            long long total = cost_range(rows[i].v, rows[i].base, s_targets[rows[i].v]);
            int can = unused_exp() >= total;
            snprintf(buf, sizeof(buf), "%s%d: %s%s",
                     sel ? "[-] " : "", s_targets[rows[i].v], fmt_thousands(total),
                     sel ? " [+]" : "");
            render_text(s_px + 8 + 320, y, can ? C_GREEN : C_RED, RENDER_TEXT_SMALL, buf);
        } else if (sel) {
            render_text(s_px + 8 + 320, y, graycolor, RENDER_TEXT_SMALL, "[-] set [+]");
        }

        s_row_v[s_row_count] = (short)rows[i].v;
        s_row_y[s_row_count] = (short)y;
        s_row_count++;
        y += ROW_H;
    }

    if (s_scroll > 0)
        render_text(s_px + PANEL_W - 8, s_py + HDR_H - 12, textcolor,
                    RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, "^ more");
    if (more > 0) {
        snprintf(buf, sizeof(buf), "v %d more", more);
        render_text(s_px + PANEL_W - 8, s_py + s_ph - 14, textcolor,
                    RENDER_TEXT_SMALL | RENDER_TEXT_RIGHT, buf);
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
    load_config();
}

DLL_EXPORT void amod_tick(void)
{
    if (!s_ingame) return;
    s_ticks++;
    if (s_ticks == 24)
        addline("Skill Planner %s loaded. Type #plan to open.", PLANNER_VERSION);
}

DLL_EXPORT void amod_frame(void)
{
    if (!s_ingame || !s_open) return;
    draw_panel();
}

DLL_EXPORT void amod_mouse_move(int x, int y)
{
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
    int base = 0, i;

    for (i = 0; i < skltab_cnt; i++)
        if (skltab[i].v == v) { base = skltab[i].base; break; }

    if (!s_targets[v]) s_targets[v] = (short)base;
    s_targets[v] = (short)(s_targets[v] + dir * (vk_shift ? 10 : 1));
    if (s_targets[v] <= base) s_targets[v] = 0;      /* back to "no target" */
    if (s_targets[v] > base + MAX_TARGET_STEP) s_targets[v] = (short)(base + MAX_TARGET_STEP);
    save_config();
}

DLL_EXPORT int amod_mouse_click(int x, int y, int what)
{
    if (!s_ingame || !s_open) return 0;

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

        /* [x] close */
        if (y <= s_py + 19 && x >= s_px + PANEL_W - 26) {
            s_open = 0;
            return 1;
        }
        /* sort toggle */
        if (y >= s_py + 22 && y < s_py + 22 + ROW_H && x >= s_px + PANEL_W - 120) {
            s_sort_cheap = !s_sort_cheap;
            s_scroll = 0;
            save_config();
            return 1;
        }
        for (i = 0; i < s_row_count; i++) {
            if (y >= s_row_y[i] - 1 && y < s_row_y[i] + ROW_H - 1) {
                int v = s_row_v[i];
                if (v == s_sel_v && x >= s_px + 320) {
                    /* [-] / [+] around the target text */
                    if (x < s_px + 320 + 22) bump_target(v, -1);
                    else if (x >= s_px + PANEL_W - 40 ||
                             (s_targets[v] == 0 && x >= s_px + 320 + 40))
                        bump_target(v, +1);
                    else if (s_targets[v]) {
                        /* click the middle: nudge up too */
                        bump_target(v, +1);
                    }
                } else {
                    s_sel_v = v;
                }
                return 1;
            }
        }
        return 1;
    }

    return 1;
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
        addline(COL_INFO "Click a skill, then [-]/[+] to set a target (shift = 10 points).");
        return 1;
    }
    addline(COL_INFO "Unknown #plan option. Try #plan help");
    return 1;
}

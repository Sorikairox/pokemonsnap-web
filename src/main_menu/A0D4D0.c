#include "common.h"

#include "main_menu.h"

// .bss
u8 D_800E8330_A0F830[0x4];
GObj* D_800E8334_A0F8C4;
GObj* D_800E8338_A0F8C8;
GObj* D_800E833C_A0F8CC;
extern GObj* D_800E8340_A0F8D0;
extern GObj* D_800E8344_A0F8D4;
extern GObj* D_800E8348_A0F8D8;
extern GObj* D_800E834C_A0F8DC;
extern GObj* D_800E8350_A0F8E0;
extern GObj* D_800E8354_A0F8E4;
extern GObj* D_800E8358_A0F8E8;
extern GObj* D_800E835C_A0F8EC;
extern GObj* D_800E8360_A0F8F0;

extern Sprite D_802DABC0_main_menu_vpk0;
extern Sprite D_80312238_main_menu_vpk0;
extern Sprite D_803128E0_main_menu_vpk0;
extern Sprite D_80316230_main_menu_vpk0;
extern Sprite D_80319B80_main_menu_vpk0;
extern Sprite D_8031D4D0_main_menu_vpk0;
extern Sprite D_80320E20_main_menu_vpk0;
extern Sprite D_80324770_main_menu_vpk0;
extern Sprite D_803280C0_main_menu_vpk0;
extern Sprite D_8032BA10_main_menu_vpk0;
extern Sprite D_8032F360_main_menu_vpk0;
extern Sprite D_80332CB0_main_menu_vpk0;
extern Sprite D_80336600_main_menu_vpk0;
extern Sprite D_80339F50_main_menu_vpk0;
extern Sprite D_8033D8A0_main_menu_vpk0;
extern Sprite D_8033E210_main_menu_vpk0;
extern Sprite D_8033E938_main_menu_vpk0;
extern Sprite D_8033ECD0_main_menu_vpk0;
extern Sprite D_8033ED70_main_menu_vpk0;
extern Sprite D_8033F498_main_menu_vpk0;
extern Sprite D_8033FBB8_main_menu_vpk0;
extern Sprite D_80341160_main_menu_vpk0;
extern Sprite D_80341790_main_menu_vpk0;
extern Sprite D_80341C70_main_menu_vpk0;
extern Sprite D_80342150_main_menu_vpk0;
extern Sprite D_80342630_main_menu_vpk0;
extern Sprite D_80342B10_main_menu_vpk0;
extern Sprite D_80342FF0_main_menu_vpk0;
extern Sprite D_803434D0_main_menu_vpk0;

#ifdef PORT
/* port (F1 + F2 + F5 + F6 + F7; docs/port/notes/F1.md, F2.md, F5.md, F6.md, F7.md): the rows "Snap Button", "Pixels",
 * "Motion Aim", "Mouse Aim" and "Mouse Speed" of the Options menu (port options 1-5, port/ultra/pc_options.c; not in
 * the save). The logic is in A0E190.c (row indices 5-9). The menu's own rows, values and descriptions are pictures (IA16, white, the letters in the alpha,
 * SP_TEXSHUF) in this scene's vpk0 data, drawn in a font of their own (no font of that look exists in the game). F5:
 * the port's rows are composed when the menu opens from the letters of those pictures, already decompressed in memory
 * from the player's ROM: each letter is cut out of a native picture (portGlyphs: which picture, column, width; no
 * pixel is in the source), "O" is the native "C" with its left half mirrored, "x" (no native one) is drawn here. Words
 * are kerned like the native text (letters 3 pixels apart on each row, a space is 4 more); the bullet and the "< >"
 * brackets are copied from "Screen" and "< Mono >". The new pictures are IA16 like the native ones and get the same
 * attributes and colours, so they blink, highlight and blend exactly as the native rows do. F7: the menu pictures have
 * no digits; "0".."9" are the course's film counter's (white, square like this face, a "1" that reads as one; 8 rows
 * on the capitals' baseline), cut from the player's ROM by port_menu_digits (pc_options.c) as coverage, so they take
 * the rows' white / orange like the letters. */
#define PORT_OPT_SNAP_BUTTON 1
#define PORT_OPT_PIXELS 2  /* F2: "Pixels: Smooth / Sharp" (display only) */
#define PORT_OPT_MOTION_AIM 3 /* F5: "Motion Aim: On / Off" (the host reads the gyroscope while it is on) */
#define PORT_OPT_MOUSE_AIM 4  /* F6: "Mouse Aim: On / Off" (the module asks for the mouse in camera control) */
#define PORT_OPT_MOUSE_SPEED 5 /* F7: "Mouse Speed: < N >", 1..10 (how far the view turns for a pixel of mouse motion) */
#define PORT_SPEED_MAX 10
#define PORT_OPT_ROWS 5    /* rows 5, 6, 7, 8, 9 */
/* F6: nine rows (the four native ones, the port's, Return) no longer fit 16 pixels apart above the description box.
 * F7: ten rows are 14 pixels apart from y 59 (the native first row is at 73, 19 pixels below the title's line at 54;
 * now 5, the letters' top 6 below it), so Return stays at y 185 and the box where F5 put it (moved down 32: y 198-231
 * of 240) */
#define PORT_ROW_Y(k) (59 + 14 * (k)) /* screen row k: 0-3 native, 4.. the port's, 4 + PORT_OPT_ROWS Return */
#define PORT_OPT_BOX_DY 32
void* port_menu_alloc(s32 slot, s32 bytes);
void* port_menu_digits(void);
s32 port_option_get(s32 id);
GObj* port_OptValues[PORT_OPT_ROWS]; /* "< On >", "< Off >" (as D_800E8358_A0F8E8 for Sound); Mouse Speed: "< N >" */
GObj* port_OptHelp[PORT_OPT_ROWS];   /* what each value does, shown while choosing (as D_800E834C_A0F8DC) */
Bitmap* port_SpeedBitmaps[PORT_SPEED_MAX]; /* F7: the pictures "< 1 >" .. "< 10 >" (A0E190.c swaps them on the sprite) */

/* the alpha (coverage) of pixel (x, y) of a native IA16 menu picture: its bitmaps lie side by side in bands of
 * bmheight rows; SP_TEXSHUF pictures have the two 32-bit words of every 64-bit word of their odd rows swapped (as
 * TMEM wants them) */
static s32 portNativeAlpha(Sprite* spr, s32 x, s32 y) {
    Bitmap* bm = spr->bitmap;
    s32 i, bx = 0, by = 0, off;

    if (x < 0 || y < 0 || x >= spr->width || y >= spr->height) {
        return 0;
    }
    for (i = 0; i < spr->nbitmaps; i++, bm++) {
        if (y >= by && y < by + bm->actualHeight && x >= bx && x < bx + bm->width) {
            off = ((bm->t + y - by) * bm->width_img + bm->s + x - bx) * 2;
            if ((spr->attr & SP_TEXSHUF) && ((y - by) & 1)) {
                off ^= 4;
            }
            return ((u8*) bm->buf)[off + 1];
        }
        bx += bm->width;
        if (bx >= spr->width) {
            bx = 0;
            by += spr->bmheight;
        }
    }
    return 0;
}

/* where each letter is in the native pictures: the picture, its first column and width, and the top of its line
 * (a letter keeps its rows from there: the native letters sit a row higher or lower, as they were drawn) */
typedef struct {
    char ch;
    Sprite* spr;
    s16 x, w, top;
} PortGlyphSrc;

#define PORT_GLYPH_H 12
static PortGlyphSrc portGlyphs[] = {
    { ',', &D_80324770_main_menu_vpk0, 125, 2, 3 }, /* "Press Z Button to focus, and press" */
    { '.', &D_8032F360_main_menu_vpk0, 130, 1, 3 }, /* "Display setting on screen." */
    { 'A', &D_80332CB0_main_menu_vpk0, 9, 5, 3 },   /* "Adjust orange frame by moving Control Stick." */
    { 'B', &D_803280C0_main_menu_vpk0, 19, 5, 3 },  /* "Z Button setting." */
    { 'C', &D_80316230_main_menu_vpk0, 9, 5, 3 },   /* "Control Stick setting." */
    { 'D', &D_8032F360_main_menu_vpk0, 9, 5, 3 },
    { 'H', &D_80341C70_main_menu_vpk0, 18, 6, 0 },  /* "< Hold >" */
    { 'M', &D_80342150_main_menu_vpk0, 18, 5, 0 },  /* "< Mono >" */
    { 'N', &D_80342630_main_menu_vpk0, 12, 5, 0 },  /* "<Normal>" */
    { 'P', &D_80324770_main_menu_vpk0, 9, 5, 3 },
    { 'R', &D_8032BA10_main_menu_vpk0, 9, 5, 3 },   /* "Return to title screen." */
    { 'S', &D_80342FF0_main_menu_vpk0, 13, 4, 0 },  /* "<Stereo>" */
    { 'Z', &D_803280C0_main_menu_vpk0, 9, 5, 3 },
    { 'a', &D_80342630_main_menu_vpk0, 35, 5, 0 },
    { 'b', &D_80332CB0_main_menu_vpk0, 115, 4, 3 },
    { 'c', &D_803434D0_main_menu_vpk0, 34, 3, 0 },  /* "<Switch>" */
    { 'd', &D_80341C70_main_menu_vpk0, 33, 4, 0 },
    { 'e', &D_80342FF0_main_menu_vpk0, 24, 4, 0 },
    { 'f', &D_80332CB0_main_menu_vpk0, 81, 4, 3 },
    { 'g', &D_8032F360_main_menu_vpk0, 77, 4, 3 },
    { 'h', &D_803434D0_main_menu_vpk0, 39, 4, 0 },
    { 'i', &D_803434D0_main_menu_vpk0, 26, 1, 0 },
    { 'j', &D_80332CB0_main_menu_vpk0, 22, 3, 3 },
    { 'k', &D_80316230_main_menu_vpk0, 69, 4, 3 },
    { 'l', &D_80341C70_main_menu_vpk0, 30, 1, 0 },
    { 'm', &D_80342630_main_menu_vpk0, 28, 5, 0 },
    { 'n', &D_80342150_main_menu_vpk0, 30, 4, 0 },
    { 'o', &D_80342FF0_main_menu_vpk0, 40, 4, 0 },
    { 'p', &D_8032F360_main_menu_vpk0, 24, 3, 3 },
    { 'r', &D_80342FF0_main_menu_vpk0, 30, 3, 0 },
    { 's', &D_8032F360_main_menu_vpk0, 19, 3, 3 },
    { 't', &D_80342FF0_main_menu_vpk0, 19, 4, 0 },
    { 'u', &D_80339F50_main_menu_vpk0, 19, 4, 3 },  /* "Sound setting." */
    { 'v', &D_80332CB0_main_menu_vpk0, 141, 4, 3 },
    { 'w', &D_803434D0_main_menu_vpk0, 19, 5, 0 },
    { 'y', &D_8032F360_main_menu_vpk0, 38, 4, 3 },
};

/* "x": the native text has none; drawn in the font's manner (x-height rows 4-9, soft corners) */
static const u8 portGlyphX[6][4] = {
    { 255, 60, 60, 255 }, { 160, 230, 230, 160 }, { 20, 255, 255, 20 },
    { 20, 255, 255, 20 }, { 160, 230, 230, 160 }, { 255, 60, 60, 255 },
};

/* one letter's coverage, PORT_GLYPH_H rows from the line's top; returns its width (0: no such letter) */
static s32 portGlyph(char ch, u8 out[PORT_GLYPH_H][8]) {
    static const s8 mirrorC[5] = { 0, 1, 2, 1, 0 }; /* "O" = "C" with its left half mirrored */
    PortGlyphSrc* g;
    s32 i, r, k, w;

    for (r = 0; r < PORT_GLYPH_H; r++) {
        for (k = 0; k < 8; k++) {
            out[r][k] = 0;
        }
    }
    if (ch == 'x') {
        for (r = 0; r < 6; r++) {
            for (k = 0; k < 4; k++) {
                out[4 + r][k] = portGlyphX[r][k];
            }
        }
        return 4;
    }
    if (ch >= '0' && ch <= '9') { /* F7: a digit (port_menu_digits: 12 x 8 coverage, on the capitals' baseline), its
                                   * empty columns trimmed */
        u8* d = port_menu_digits();
        s32 c0 = 8, c1 = -1;

        if (d == NULL) {
            return 0;
        }
        d += (ch - '0') * 12 * 8;
        for (r = 0; r < PORT_GLYPH_H; r++) {
            for (k = 0; k < 8; k++) {
                if (d[r * 8 + k] != 0) {
                    c0 = k < c0 ? k : c0;
                    c1 = k > c1 ? k : c1;
                }
            }
        }
        if (c1 < c0) {
            return 0;
        }
        w = c1 - c0 + 1;
        for (r = 0; r < PORT_GLYPH_H; r++) {
            for (k = 0; k < w; k++) {
                out[r][k] = d[r * 8 + c0 + k];
            }
        }
        return w;
    }
    for (i = 0; i < ARRAY_COUNT(portGlyphs); i++) {
        g = &portGlyphs[i];
        if (g->ch == (ch == 'O' ? 'C' : ch)) {
            w = ch == 'O' ? 5 : g->w;
            for (r = 0; r < PORT_GLYPH_H; r++) {
                for (k = 0; k < w; k++) {
                    out[r][k] = portNativeAlpha(g->spr, g->x + (ch == 'O' ? mirrorC[k] : k), g->top + r);
                }
            }
            return w;
        }
    }
    return 0;
}

/* where the next letter goes after one at px: the first column (at least one empty column between them) at which on
 * every row their solid pixels (coverage >= 128) are at least 3 apart, as the native text is set */
static s32 portKern(u8 prev[PORT_GLYPH_H][8], s32 pw, s32 px, u8 next[PORT_GLYPH_H][8], s32 nw) {
    s32 x = px + pw + 1;
    s32 r, k, pr, nl;

    for (r = 0; r < PORT_GLYPH_H; r++) {
        pr = -1;
        nl = -1;
        for (k = 0; k < pw; k++) {
            if (prev[r][k] >= 128) {
                pr = k;
            }
        }
        for (k = nw - 1; k >= 0; k--) {
            if (next[r][k] >= 128) {
                nl = k;
            }
        }
        if (pr >= 0 && nl >= 0 && x + nl - (px + pr) < 3) {
            x = px + pr + 3 - nl;
        }
    }
    return x;
}

/* the width of a line of text ('\n' or the end stops it) */
static s32 portTextWidth(char* s) {
    u8 g[2][PORT_GLYPH_H][8];
    s32 w[2], x = 0, cur = 0, have = 0, space = 0, end = 0;

    for (; *s != 0 && *s != '\n'; s++) {
        if (*s == ' ') {
            space += 4;
            continue;
        }
        w[cur] = portGlyph(*s, g[cur]);
        if (w[cur] == 0) {
            continue;
        }
        x = have ? portKern(g[cur ^ 1], w[cur ^ 1], x, g[cur], w[cur]) + space : 0;
        end = x + w[cur];
        have = 1;
        space = 0;
        cur ^= 1;
    }
    return end;
}

/* text into an IA16 image (white, the letters' coverage as alpha): lines 12 pixels apart from (x, y) */
static void portTextDraw(u16* img, s32 iw, s32 ih, char* s, s32 x0, s32 y) {
    u8 g[2][PORT_GLYPH_H][8];
    s32 w[2], x = x0, cur = 0, have = 0, space = 0, r, k, a;

    for (; *s != 0; s++) {
        if (*s == '\n') {
            y += 12;
            x = x0;
            have = 0;
            space = 0;
            continue;
        }
        if (*s == ' ') {
            space += 4;
            continue;
        }
        w[cur] = portGlyph(*s, g[cur]);
        if (w[cur] == 0) {
            continue;
        }
        if (have) {
            x = portKern(g[cur ^ 1], w[cur ^ 1], x, g[cur], w[cur]) + space;
        }
        for (r = 0; r < PORT_GLYPH_H; r++) {
            for (k = 0; k < w[cur]; k++) {
                a = g[cur][r][k];
                if (a != 0 && x + k >= 0 && x + k < iw && y + r >= 0 && y + r < ih) {
                    img[(y + r) * iw + x + k] = 0xFF00 | a;
                }
            }
        }
        have = 1;
        space = 0;
        cur ^= 1;
    }
}

/* columns x0..x0+w-1, rows 0..h-1 of a native picture copied into an IA16 image at (dx, dy) */
static void portNativeCopy(u16* img, s32 iw, s32 ih, Sprite* spr, s32 x0, s32 w, s32 h, s32 dx, s32 dy) {
    s32 r, k, a;

    for (r = 0; r < h; r++) {
        for (k = 0; k < w; k++) {
            a = portNativeAlpha(spr, x0 + k, r);
            if (a != 0 && dx + k < iw && dy + r < ih) {
                img[(dy + r) * iw + dx + k] = 0xFF00 | a;
            }
        }
    }
}

/* a clear w x h IA16 sprite (w a power of two: the sprite library loads rows with LoadBlock, whose dxt steps exactly
 * only through rows of 2^n words; F1 trap) whose image the caller fills. Fields as UIElement_Create sets them
 * (window/element.c), one bitmap per 4 KiB; attributes as the native pictures' minus SP_TEXSHUF (rows unshuffled). */
static u16* portSprite(Sprite* spr, s32 slot, s32 w, s32 h) {
    s32 tileH = 0x1000 / 2 / w;
    s32 n = (h + tileH - 1) / tileH;
    Bitmap* bm = port_menu_alloc(slot, n * sizeof(Bitmap) + w * h * 2);
    u16* img = (u16*) (bm + n);
    s32 i;

    for (i = 0; i < w * h; i++) {
        img[i] = 0xFF00;
    }
    for (i = 0; i < n; i++) {
        bm[i].width = w;
        bm[i].width_img = w;
        bm[i].s = 0;
        bm[i].t = 0;
        bm[i].buf = img + i * tileH * w;
        bm[i].actualHeight = tileH < h - tileH * i ? tileH : h - tileH * i;
        bm[i].LUToffset = 0;
    }
    spr->x = spr->y = 0;
    spr->width = w;
    spr->height = h;
    spr->scalex = spr->scaley = 1.0f;
    spr->expx = spr->expy = 0;
    spr->attr = SP_TRANSPARENT;
    spr->zdepth = 0;
    spr->red = spr->green = spr->blue = spr->alpha = 0xFF;
    spr->startTLUT = spr->nTLUT = 0;
    spr->LUT = NULL;
    spr->istart = 0;
    spr->istep = 1;
    spr->nbitmaps = n;
    spr->ndisplist = NUM_DL(n);
    spr->bmheight = spr->bmHreal = tileH;
    spr->bmfmt = G_IM_FMT_IA;
    spr->bmsiz = G_IM_SIZ_16b;
    spr->bitmap = bm;
    spr->rsp_dl = spr->rsp_dl_next = NULL;
    spr->frac_s = spr->frac_t = 0;
    return img;
}

/* a row's label: the native bullet ("Screen"'s first 7 columns) and the words from column 8, as "Screen" */
static void portLabelSprite(Sprite* spr, s32 slot, char* label) {
    u16* img = portSprite(spr, slot, 128, PORT_GLYPH_H);

    portNativeCopy(img, 128, PORT_GLYPH_H, &D_8033F498_main_menu_vpk0, 0, 7, 10, 0, 0);
    portTextDraw(img, 128, PORT_GLYPH_H, label, 8, 0);
}

/* a value, 56 pixels like the native "< Mono >": its brackets ("< Mono >"'s columns 0-9 and 45-55) and the word
 * centred on column 28 */
static void portValueSprite(Sprite* spr, s32 slot, char* word) {
    u16* img = portSprite(spr, slot, 64, PORT_GLYPH_H);

    portNativeCopy(img, 64, PORT_GLYPH_H, &D_80342150_main_menu_vpk0, 0, 10, 10, 0, 0);
    portNativeCopy(img, 64, PORT_GLYPH_H, &D_80342150_main_menu_vpk0, 45, 11, 10, 45, 0);
    portTextDraw(img, 64, PORT_GLYPH_H, word, 28 - portTextWidth(word) / 2, 0);
}

/* F7: Mouse Speed's value, one picture over both value columns (x 163-277: the left "<" where "< Stereo >" has it,
 * the right ">" where "< Mono >" has it) with the number centred between them */
static void portSpeedSprite(Sprite* spr, s32 slot, s32 n) {
    u16* img = portSprite(spr, slot, 128, PORT_GLYPH_H);
    char num[3];

    num[0] = n >= 10 ? '0' + n / 10 : '0' + n;
    num[1] = n >= 10 ? '0' + n % 10 : 0;
    num[2] = 0;
    portNativeCopy(img, 128, PORT_GLYPH_H, &D_80342150_main_menu_vpk0, 0, 10, 10, 0, 0);
    portNativeCopy(img, 128, PORT_GLYPH_H, &D_80342150_main_menu_vpk0, 45, 11, 10, 104, 0);
    portTextDraw(img, 128, PORT_GLYPH_H, num, 57 - portTextWidth(num) / 2, 0);
}

/* a description: 238 x 30 like the native ones (their lines start at column 8, rows 3 and 15) */
static void portHelpSprite(Sprite* spr, s32 slot, char* text) {
    u16* img = portSprite(spr, slot, 256, 30);

    portTextDraw(img, 256, 30, text, 8, 3);
}

void func_800E643C_A0D9CC(SObj* sobj, s8 arg1);

/* the port's rows, in screen order below "Control Stick Setup" (row index 5 + i in A0E190.c). Only letters of
 * portGlyphs (and O, x) and the digits (F7) can be written: the native pictures have no E F G I J K L Q T U V W X Y
 * q z. Mouse Speed (an int, F7) has no two values: its number is drawn by portSpeedSprite, one value help. */
typedef struct {
    s32 option;      /* port option id (pc_options.c) */
    s32 leftIsOne;   /* the left value means option value 1 (On / Off) or 0 (Pixels: Smooth / Sharp) */
    char* label;
    char* values[2]; /* left, right */
    char* help;      /* the row's description */
    char* valueHelp[2];
} PortOptRowText;

static PortOptRowText portOptRows[PORT_OPT_ROWS] = {
    { PORT_OPT_SNAP_BUTTON, true, "Snap Button", { "On", "Off" }, "Snap Button setting.",
      { "Hold Snap to focus, and release\nSnap to take a picture.",
        "Snap Button does nothing.\nPress Z and A to take pictures." } },
    { PORT_OPT_PIXELS, false, "Pixels", { "Smooth", "Sharp" }, "Picture sharpness setting.",
      { "Smooth, soft picture.\nPhotos and scores do not change.",
        "Sharp, crisp picture.\nPhotos and scores do not change." } },
    { PORT_OPT_MOTION_AIM, true, "Motion Aim", { "On", "Off" }, "Motion Aim setting.",
      { "Aim by turning your phone, held\nsideways. Allow it if asked.", "Aim with the Control Stick only." } },
    { PORT_OPT_MOUSE_AIM, true, "Mouse Aim", { "On", "Off" }, "Mouse Aim setting.",
      { "Aim with the mouse. Click to take a\npicture, hold the right button to focus.",
        "Mouse is not used. Aim with the\nControl Stick." } },
    { PORT_OPT_MOUSE_SPEED, false, "Mouse Speed", { NULL, NULL }, "Mouse Speed setting.",
      { "How fast the mouse turns the view.\n1 slow, 10 fast. Mouse Aim must be On.", NULL } },
};

static void portMoveY(GObj* gobj, s32 dy) {
    SObj* sobj;

    for (sobj = gobj->data.sobj; sobj != NULL; sobj = sobj->next) {
        sobj->sprite.y += dy;
    }
}

/* F1 + F2 + F5 + F6 + F7: rows 5 ("Snap Button"), 6 ("Pixels"), 7 ("Motion Aim"), 8 ("Mouse Aim") and 9 ("Mouse
 * Speed") between "Control Stick Setup" and "Return"; every row 14 pixels apart from y 59 (PORT_ROW_Y), "Return" at
 * y 185 and the description box 32 pixels down (it then spans y 198-231 of 240, 13 pixels below "Return" as in F2) */
static void portRowsSpread(void) {
    SObj* sobj;
    GObj* values[3];
    s32 k;

    values[0] = D_800E8358_A0F8E8; /* Sound, Z Button Setup, Control Stick Setup: two values each */
    values[1] = D_800E835C_A0F8EC;
    values[2] = D_800E8360_A0F8F0;
    for (k = 0, sobj = D_800E8340_A0F8D0->data.sobj; k < 4; k++, sobj = sobj->next) { /* the native labels */
        sobj->sprite.y = PORT_ROW_Y(k);
    }
    for (k = 0; k < 3; k++) {
        values[k]->data.sobj->sprite.y = PORT_ROW_Y(k + 1);
        values[k]->data.sobj->next->sprite.y = PORT_ROW_Y(k + 1);
    }
    /* their ":" (D_800E8338_A0F8C8 sprites 6-8, 2 pixels below the row) */
    sobj = D_800E8338_A0F8C8->data.sobj->next->next->next->next->next->next;
    for (k = 0; k < 3; k++, sobj = sobj->next) {
        sobj->sprite.y = PORT_ROW_Y(k + 1) + 2;
    }
}

void port_OptionsCreateRows(void) {
    Sprite spr;
    SObj* sobj;
    s32 i, k, y, value;
    PortOptRowText* row;

    portRowsSpread();
    /* "Return" (the 5th label) moves down; the new labels become labels 5.. (port_OptLabels in A0E190.c) */
    sobj = D_800E8340_A0F8D0->data.sobj->next->next->next->next;
    SET_SPRITE_POS(sobj->sprite, 43, PORT_ROW_Y(4 + PORT_OPT_ROWS));
    /* the description box: the frame's lower line, its bottom line and its two sides (D_800E8338_A0F8C8 sprites 2-5),
     * and every description sprite */
    sobj = D_800E8338_A0F8C8->data.sobj->next->next;
    for (k = 0; k < 4; k++, sobj = sobj->next) {
        sobj->sprite.y += PORT_OPT_BOX_DY;
    }
    portMoveY(D_800E8344_A0F8D4, PORT_OPT_BOX_DY);
    portMoveY(D_800E8348_A0F8D8, PORT_OPT_BOX_DY);
    portMoveY(D_800E834C_A0F8DC, PORT_OPT_BOX_DY);
    portMoveY(D_800E8350_A0F8E0, PORT_OPT_BOX_DY);
    portMoveY(D_800E8354_A0F8E4, PORT_OPT_BOX_DY);

    for (i = 0; i < PORT_OPT_ROWS; i++) {
        row = &portOptRows[i];
        y = PORT_ROW_Y(4 + i);
        value = port_option_get(row->option);
        portLabelSprite(&spr, 6 * i, row->label);
        sobj = omGObjAddSprite(D_800E8340_A0F8D0, &spr);
        SET_SPRITE_POS(sobj->sprite, 43, y);
        sobj->sprite.red = sobj->sprite.green = sobj->sprite.blue = 0x80;

        sobj = omGObjAddSprite(D_800E8338_A0F8C8, &D_8033ED70_main_menu_vpk0); /* the row's ":" */
        SET_SPRITE_POS(sobj->sprite, 158, y + 2);
        sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;

        if (row->option == PORT_OPT_MOUSE_SPEED) { /* F7: one value, "< N >", over both value columns */
            for (k = 0; k < PORT_SPEED_MAX; k++) {
                portSpeedSprite(&spr, 30 + k, k + 1);
                port_SpeedBitmaps[k] = spr.bitmap;
            }
            if (value < 1 || value > PORT_SPEED_MAX) {
                value = 5;
            }
            spr.bitmap = port_SpeedBitmaps[value - 1];
            port_OptValues[i] = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                               &spr, 0, NULL, 1);
            sobj = port_OptValues[i]->data.sobj;
            SET_SPRITE_POS(sobj->sprite, 163, y);
            func_800E643C_A0D9CC(sobj, false); /* the current value: orange */

            portHelpSprite(&spr, 6 * i + 3, row->help);
            sobj = omGObjAddSprite(D_800E8344_A0F8D4, &spr);
            SET_SPRITE_POS(sobj->sprite, 41, 168 + PORT_OPT_BOX_DY);
            sobj->sprite.attr |= SP_HIDDEN;
            portHelpSprite(&spr, 6 * i + 4, row->valueHelp[0]);
            port_OptHelp[i] = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                             &spr, 0, NULL, 1);
            sobj = port_OptHelp[i]->data.sobj;
            SET_SPRITE_POS(sobj->sprite, 41, 168 + PORT_OPT_BOX_DY);
            sobj->sprite.attr |= SP_HIDDEN;
            continue;
        }

        /* choice 0 = the left value, as the Sound row's Stereo / Mono; highlighted: the current one */
        portValueSprite(&spr, 6 * i + 1, row->values[0]);
        port_OptValues[i] = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1, &spr,
                                           0, NULL, 1);
        sobj = port_OptValues[i]->data.sobj;
        SET_SPRITE_POS(sobj->sprite, 163, y);
        func_800E643C_A0D9CC(sobj, value != row->leftIsOne);
        portValueSprite(&spr, 6 * i + 2, row->values[1]);
        sobj = omGObjAddSprite(port_OptValues[i], &spr);
        SET_SPRITE_POS(sobj->sprite, 222, y);
        func_800E643C_A0D9CC(sobj, value == row->leftIsOne);

        /* the row's description (D_800E8344_A0F8D4, index 5 + i) and its values' (shown while choosing) */
        portHelpSprite(&spr, 6 * i + 3, row->help);
        sobj = omGObjAddSprite(D_800E8344_A0F8D4, &spr);
        SET_SPRITE_POS(sobj->sprite, 41, 168 + PORT_OPT_BOX_DY);
        sobj->sprite.attr |= SP_HIDDEN;
        for (k = 0; k < 2; k++) {
            portHelpSprite(&spr, 6 * i + 4 + k, row->valueHelp[k]);
            if (k == 0) {
                port_OptHelp[i] = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000,
                                                 -1, &spr, 0, NULL, 1);
                sobj = port_OptHelp[i]->data.sobj;
            } else {
                sobj = omGObjAddSprite(port_OptHelp[i], &spr);
            }
            SET_SPRITE_POS(sobj->sprite, 41, 168 + PORT_OPT_BOX_DY);
            sobj->sprite.attr |= SP_HIDDEN;
        }
    }
}
#endif

void func_800E5F40_A0D4D0(void) {
}

void func_800E5F48_A0D4D8(void) {
    SObj* sobj;

    D_800E8334_A0F8C4 = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                       &D_802DABC0_main_menu_vpk0, 0, NULL, 1);
    sobj = D_800E8334_A0F8C4->data.sobj;
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    sobj->sprite.red = sobj->sprite.green = sobj->sprite.blue = 0x66;
}

void func_800E5FDC_A0D56C(void) {
    GObj* gobj;
    SObj* sobj;

    gobj = D_800E8338_A0F8C8 = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_803128E0_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 40, 36);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_803128E0_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 40, 54);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_803128E0_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 40, 166);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_803128E0_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 40, 197);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80312238_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 39, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80312238_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 278, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_8033ED70_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 158, 91);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_8033ED70_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 158, 107);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_8033ED70_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 158, 123);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
}

void func_800E61C8_A0D758(void) {
    GObj* gobj;
    SObj* sobj;

    gobj = D_800E833C_A0F8CC = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_80341790_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 43, 40);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80341160_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 161, 41);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
}

void func_800E6290_A0D820(void) {
    GObj* gobj;
    SObj* sobj;

    gobj = D_800E8340_A0F8D0 = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_8033F498_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 43, 73);
    sobj->sprite.red = sobj->sprite.green = sobj->sprite.blue = 0x80;
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_8033FBB8_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 43, 89);
    sobj->sprite.red = sobj->sprite.green = sobj->sprite.blue = 0x80;
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_8033E938_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 43, 105);
    sobj->sprite.red = sobj->sprite.green = sobj->sprite.blue = 0x80;
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_8033E210_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 43, 121);
    sobj->sprite.red = sobj->sprite.green = sobj->sprite.blue = 0x80;
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_8033ECD0_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 43, 137);
    sobj->sprite.red = sobj->sprite.green = sobj->sprite.blue = 0x80;
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
}

void func_800E643C_A0D9CC(SObj* sobj, s8 arg1) {
    if (!arg1) {
        sobj->sprite.red = 0xFF;
        sobj->sprite.green = 0x82;
        sobj->sprite.blue = 0x41;
    } else {
        sobj->sprite.red = 0x80;
        sobj->sprite.green = 0x80;
        sobj->sprite.blue = 0x80;
    }
}

void func_800E6480_A0DA10(void) {
    s8 sp5F;
    s8 sp5E;
    s8 sp5D;
    UNUSED s32 pad;
    GObj* gobj;
    SObj* sobj;

    sp5F = checkPlayerFlag(PFID_9);
    sp5E = checkPlayerFlag(PFID_ZOOM_SWITCH);
    sp5D = checkPlayerFlag(PFID_INVERTED_Y);

    gobj = D_800E8358_A0F8E8 = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_80342FF0_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 163, 89);
    func_800E643C_A0D9CC(sobj, sp5F);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80342150_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 222, 89);
    func_800E643C_A0D9CC(sobj, sp5F == 0);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;

    gobj = D_800E835C_A0F8EC = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_80341C70_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 163, 105);
    func_800E643C_A0D9CC(sobj, sp5E);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_803434D0_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 222, 105);
    func_800E643C_A0D9CC(sobj, sp5E == 0);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;

    gobj = D_800E8360_A0F8F0 = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_80342B10_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 163, 121);
    func_800E643C_A0D9CC(sobj, sp5D);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80342630_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 222, 121);
    func_800E643C_A0D9CC(sobj, sp5D == 0);
    sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;
}

void func_800E672C_A0DCBC(void) {
    GObj* gobj;
    SObj* sobj;

    gobj = D_800E8344_A0F8D4 = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_8032F360_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80339F50_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_803280C0_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80316230_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_8032BA10_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;
}

void func_800E6880_A0DE10(void) {
    GObj* gobj;
    SObj* sobj;

    D_800E8348_A0F8D8 = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                       &D_80332CB0_main_menu_vpk0, 0, NULL, 1);
    sobj = D_800E8348_A0F8D8->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;

    gobj = D_800E834C_A0F8DC = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_8033D8A0_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80336600_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;

    gobj = D_800E8350_A0F8E0 = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_80320E20_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80324770_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;

    gobj = D_800E8354_A0F8E4 = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1,
                                              &D_8031D4D0_main_menu_vpk0, 0, NULL, 1);
    sobj = gobj->data.sobj;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;
    omGObjAddSprite(gobj, &D_80319B80_main_menu_vpk0);

    sobj = sobj->next;
    SET_SPRITE_POS(sobj->sprite, 41, 168);
    sobj->sprite.attr = SP_TEXSHUF | SP_HIDDEN | SP_TRANSPARENT;
}

void OptionsMenu_CreateSprites(void) { /* was func_800E6ADC_A0E06C */
    func_800E5F48_A0D4D8();
    func_800E5FDC_A0D56C();
    func_800E61C8_A0D758();
    func_800E6290_A0D820();
    func_800E6480_A0DA10();
    func_800E672C_A0DCBC();
    func_800E6880_A0DE10();
#ifdef PORT
    port_OptionsCreateRows();
#endif
}

s32 OptionsMenu_Main(void) { /* was func_800E6B2C_A0E0BC */
    OptionsMenu_CreateSprites();
    OptionsMenu_Run();
    omDeleteGObj(D_800E8334_A0F8C4);
    omDeleteGObj(D_800E8338_A0F8C8);
    omDeleteGObj(D_800E833C_A0F8CC);
    omDeleteGObj(D_800E8340_A0F8D0);
    omDeleteGObj(D_800E8344_A0F8D4);
    omDeleteGObj(D_800E8348_A0F8D8);
    omDeleteGObj(D_800E834C_A0F8DC);
    omDeleteGObj(D_800E8350_A0F8E0);
    omDeleteGObj(D_800E8354_A0F8E4);
    omDeleteGObj(D_800E8358_A0F8E8);
    omDeleteGObj(D_800E835C_A0F8EC);
    omDeleteGObj(D_800E8360_A0F8F0);
#ifdef PORT
    {
        s32 i;
        for (i = 0; i < PORT_OPT_ROWS; i++) {
            omDeleteGObj(port_OptValues[i]);
            omDeleteGObj(port_OptHelp[i]);
        }
    }
#endif
    return 12;
}

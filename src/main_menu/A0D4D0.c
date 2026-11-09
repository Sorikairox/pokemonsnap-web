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
/* port (F1, docs/port/notes/F1.md; F2, notes/F2.md): the "Snap Button" and "Pixels" rows of the Options menu (port
 * options 1 and 2, port/ultra/pc_options.c; not in the save). The menu's own rows are pictures from the ROM; these
 * rows' words are the port's own, drawn when the menu opens with the game's 12-pixel font (window/text.c's glyphs and
 * widths; the window overlay is not loaded in this scene, so port_menu_font12 copies the glyphs from the player's ROM)
 * into RGBA16 sprites placed and coloured like the menu's own. The rows sit between "Control Stick Setup" and
 * "Return" (which moves down two rows, with the description box); the logic is in A0E190.c (row indices 5, 6). */
#define PORT_OPT_SNAP_BUTTON 1
#define PORT_OPT_PIXELS 2  /* F2: "Pixels: Smooth / Sharp" (display only) */
#define PORT_OPT_ROWS 2    /* rows 5, 6 */
#define PORT_OPT_BOX_DY 16 /* F2: the description box moves down to make room for the second row */
s32 UIText_GetCharIndex(s32 ch);
s32 UIText_Ascii2WideChar(s32 c);
extern u8 UIText_WidthTable[2][370];
void* port_menu_font12(void);
void* port_menu_alloc(s32 slot, s32 bytes);
s32 port_option_get(s32 id);
GObj* port_OptValues[PORT_OPT_ROWS]; /* "< On >", "< Off >" (as D_800E8358_A0F8E8 for Sound) */
GObj* port_OptHelp[PORT_OPT_ROWS];   /* what each value does, shown while choosing (as D_800E834C_A0F8DC) */

#define PORT_TEXT_SHADOW 0x0001 /* black, opaque: window/text.c's shadow palette on a clear image */

/* a glyph code of window/text.c: ASCII, or a two-byte EUC code (bytes >= 0x80) */
static s32 portTextChar(u8** s) {
    u8* p = *s;
    if (p[0] >= 0x80 && p[1] != 0) {
        *s = p + 2;
        return (p[0] << 8) | p[1];
    }
    *s = p + 1;
    return p[0] == ' ' ? 0 : UIText_Ascii2WideChar(p[0]);
}

static s32 portTextWidth(char* str) {
    u8* s = (u8*) str;
    s32 w = 0;
    while (*s != 0 && *s != '\n') {
        s32 ch = portTextChar(&s);
        w += ch == 0 ? 4 : UIText_WidthTable[1][UIText_GetCharIndex(ch)] + 1;
    }
    return w;
}

/* UIText_PrintChar16 (window/text.c) on a clear RGBA16 image, white text, shadow offset 2 */
static void portTextGlyph(u16* img, s32 w, s32 h, s32 x, s32 y, u8* glyph) {
    s32 i, j, k, ci, v;
    u16* p;

    for (i = 0; i < 12; i++) {
        for (j = 0; j < 12; j++) {
            ci = glyph[(i * 12 + j) >> 1];
            ci = (j & 1) ? (ci >> 4) & 0xF : ci & 0xF;
            if (ci == 0 || x + j < 0 || x + j >= w || y + i >= h) {
                continue;
            }
            p = img + (y + i) * w + x + j;
            v = 255 * ci / 15;
            *p = ((v << 8) & 0xF800) | ((v << 3) & 0x7C0) | ((v >> 2) & 0x3E) | 1;
            for (k = 1; k <= 2; k++) {
                if (y + i + k < h && x + j + k < w) {
                    p[k * (w + 1)] = PORT_TEXT_SHADOW;
                }
            }
        }
    }
}

/* a w x h text sprite: str ('\n' starts a line 14 pixels lower) from (x, y); centre: x is the middle of each line.
 * w is a power of two (the sprite library loads rows with LoadBlock, whose dxt steps exactly only through rows of 2^n
 * words: a 240-pixel image came out smeared on its right side). Sprite fields as UIElement_Create sets them
 * (window/element.c), one bitmap per 4 KiB. */
static void portTextSprite(Sprite* spr, s32 slot, s32 w, s32 h, char* str, s32 x, s32 y, s32 centre) {
    u8* font = port_menu_font12();
    s32 tileH = 0x1000 / 2 / w;
    s32 n = (h + tileH - 1) / tileH;
    Bitmap* bm = port_menu_alloc(slot, n * sizeof(Bitmap) + w * h * 2);
    u16* img = (u16*) (bm + n);
    u8* s = (u8*) str;
    s32 i, cx;

    for (i = 0; i < w * h; i++) {
        img[i] = 0;
    }
    cx = centre ? x - portTextWidth(str) / 2 : x;
    while (*s != 0 && font != NULL) {
        s32 ch;
        if (*s == '\n') {
            s++;
            y += 14;
            cx = centre ? x - portTextWidth((char*) s) / 2 : x;
            continue;
        }
        ch = portTextChar(&s);
        if (ch == 0) {
            cx += 4;
        } else {
            i = UIText_GetCharIndex(ch);
            portTextGlyph(img, w, h, cx, y, font + i * 72);
            cx += UIText_WidthTable[1][i] + 1;
        }
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
    spr->bmfmt = G_IM_FMT_RGBA;
    spr->bmsiz = G_IM_SIZ_16b;
    spr->bitmap = bm;
    spr->rsp_dl = spr->rsp_dl_next = NULL;
    spr->frac_s = spr->frac_t = 0;
}

/* "<  On  >": brackets at the edges like the menu's "< Mono >"; the word centred at `centre`, '>' at `right` */
static void portValueSprite(Sprite* spr, s32 slot, char* word, s32 centre, s32 right) {
    portTextSprite(spr, slot, 64, 16, word, centre, 1, true);
    {
        u8* font = port_menu_font12();
        u16* img = (u16*) (spr->bitmap + spr->nbitmaps);
        if (font != NULL) {
            portTextGlyph(img, 64, 16, 0, 1, font + UIText_GetCharIndex(UIText_Ascii2WideChar('<')) * 72);
            portTextGlyph(img, 64, 16, right, 1, font + UIText_GetCharIndex(UIText_Ascii2WideChar('>')) * 72);
        }
    }
}

void func_800E643C_A0D9CC(SObj* sobj, s8 arg1);

/* the port's rows, in screen order below "Control Stick Setup" (row index 5 + i in A0E190.c) */
typedef struct {
    s32 option;      /* port option id (pc_options.c) */
    s32 leftIsOne;   /* the left value means option value 1 (Snap: On / Off) or 0 (Pixels: Smooth / Sharp) */
    char* label;
    char* values[2]; /* left, right */
    s32 centre, right; /* value sprites: the word's centre, the '>' */
    char* help;      /* the row's description */
    char* valueHelp[2];
} PortOptRowText;

static PortOptRowText portOptRows[PORT_OPT_ROWS] = {
    { PORT_OPT_SNAP_BUTTON, true, "Snap Button", { "On", "Off" }, 28, 51,
      "Snap: an extra touch button (key 1).\nHold to aim, let go to take a picture.",
      { "On: hold Snap to zoom in and aim,\nlet go of it to take a picture.",
        "Off: the Snap button does nothing.\nUse Z and A to take pictures." } },
    /* F2: "Smooth" is 44 pixels wide: the brackets sit 1 pixel further out than Snap's (the native "<Reverse>" is as
     * tight) */
    { PORT_OPT_PIXELS, false, "Pixels", { "Smooth", "Sharp" }, 29, 52,
      "Pixels: how the game's picture is\nenlarged to fit your screen.",
      { "Smooth: soft, blended pixels,\nlike the picture on a TV.",
        "Sharp: crisp, square pixels.\nPhotos and scores do not change." } },
};

static void portMoveY(GObj* gobj, s32 dy) {
    SObj* sobj;

    for (sobj = gobj->data.sobj; sobj != NULL; sobj = sobj->next) {
        sobj->sprite.y += dy;
    }
}

/* F1 + F2: rows 5 ("Snap Button", option 1) and 6 ("Pixels", option 2) between "Control Stick Setup" (y 121) and
 * "Return", 16 pixels apart like the native rows; "Return" and the description box below them move down 32 pixels
 * (the box then spans y 182-213 of 240: inside the picture, clear of the rows) */
void port_OptionsCreateRows(void) {
    Sprite spr;
    SObj* sobj;
    s32 i, k, y, value;
    PortOptRowText* row;

    /* "Return" (the 5th label) moves down; the new labels become labels 5, 6 (D_800E8378_A0F908[5], [6]) */
    sobj = D_800E8340_A0F8D0->data.sobj->next->next->next->next;
    SET_SPRITE_POS(sobj->sprite, 43, 137 + 16 * PORT_OPT_ROWS);
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
        y = 136 + 16 * i;
        value = port_option_get(row->option);
        portTextSprite(&spr, 6 * i, 128, 16, row->label, 7, 1, false);
        {
            /* the rows' bullet: a 2 x 2 dot with its shadow */
            u16* img = (u16*) (spr.bitmap + spr.nbitmaps);
            img[6 * 128 + 2] = img[6 * 128 + 3] = img[7 * 128 + 2] = img[7 * 128 + 3] = 0xFFFF;
            img[7 * 128 + 4] = img[8 * 128 + 4] = img[8 * 128 + 3] = PORT_TEXT_SHADOW;
        }
        sobj = omGObjAddSprite(D_800E8340_A0F8D0, &spr);
        SET_SPRITE_POS(sobj->sprite, 43, y);
        sobj->sprite.red = sobj->sprite.green = sobj->sprite.blue = 0x80;

        sobj = omGObjAddSprite(D_800E8338_A0F8C8, &D_8033ED70_main_menu_vpk0); /* the row's ":" */
        SET_SPRITE_POS(sobj->sprite, 158, y + 3);
        sobj->sprite.attr = SP_TEXSHUF | SP_TRANSPARENT;

        /* choice 0 = the left value, as the Sound row's Stereo / Mono; highlighted: the current one */
        portValueSprite(&spr, 6 * i + 1, row->values[0], row->centre, row->right);
        port_OptValues[i] = ohCreateSprite(0xE, ohUpdateDefault, 0, 0x80000000, renDrawSprite, 1, 0x80000000, -1, &spr,
                                           0, NULL, 1);
        sobj = port_OptValues[i]->data.sobj;
        SET_SPRITE_POS(sobj->sprite, 162, y);
        func_800E643C_A0D9CC(sobj, value != row->leftIsOne);
        portValueSprite(&spr, 6 * i + 2, row->values[1], row->centre, row->right);
        sobj = omGObjAddSprite(port_OptValues[i], &spr);
        SET_SPRITE_POS(sobj->sprite, 221, y);
        func_800E643C_A0D9CC(sobj, value == row->leftIsOne);

        /* the row's description (D_800E8344_A0F8D4, index 5 + i) and its values' (shown while choosing) */
        portTextSprite(&spr, 6 * i + 3, 256, 30, row->help, 8, 3, false);
        sobj = omGObjAddSprite(D_800E8344_A0F8D4, &spr);
        SET_SPRITE_POS(sobj->sprite, 41, 168 + PORT_OPT_BOX_DY);
        sobj->sprite.attr |= SP_HIDDEN;
        for (k = 0; k < 2; k++) {
            portTextSprite(&spr, 6 * i + 4 + k, 256, 30, row->valueHelp[k], 8, 3, false);
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
    omDeleteGObj(port_OptValues[0]);
    omDeleteGObj(port_OptHelp[0]);
    omDeleteGObj(port_OptValues[1]);
    omDeleteGObj(port_OptHelp[1]);
#endif
    return 12;
}

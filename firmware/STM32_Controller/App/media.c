/**
 * @file media.c
 * 用途：SD 相册、表情、文本与 GUI 的统一显示入口。
 * 使用：LCD、FatFs 和字库就绪后初始化；主循环按当前页面和重绘请求调用绘制。
 * 时序与边界：文件句柄、当前表情和字幕由媒体层管理；忙期避免整屏刷新，横竖屏变化需重新布局。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "camera_link.h"
/* BMP reader adapted from zProject/03 App/photo.c: retain BGR->RGB565 and
 * FAT root scanning, use this board's portrait LCD and bounded row reads.
 * Everything here runs in main, never in an interrupt. No card writes. */
#include "media.h"
#include "lcd.h"
#include "font.h"
#include "ff.h"
#include "time.h"
#include "ailink.h"
#include "robot_gui.h"
#include "iap_app.h"
#include "micaud.h"
#include "miclink.h"
#include "face_anim.h"
#include "font_store.h"
#include "font_upload.h"
#include <stdio.h>
#include <string.h>

#define MAX_PHOTOS 64u
#define MAX_BMP_DIM 8192u
static FATFS fs;
static FIL file;
static char names[MAX_PHOTOS][13];
static char chosen[13];
static char text[256] = "Hello STM32!";
static uint16_t count, selected;
static uint8_t sd_ready;
static uint8_t raw[LCD_MAX_WIDTH * 4];
static uint16_t pixels[LCD_MAX_WIDTH];
static const char *photo_error = "NO BMP FILE";

/* ---------- 表情库（SD 卡 FACE\ 目录，只读）----------
 * 名字一律**大写 8.3**（HAPPY.BMP 这种）。5 个名字正好对上
 * 决策协议v2.md 里 expression.face 的取值：happy/sad/sleepy/angry/normal。
 *  MAX_FACES*13 = 416 字节 RAM。本工程只剩 52KB，不要往这儿加大的。 */
#define MAX_FACES 32u
static char faces[MAX_FACES][13];
static uint16_t face_count;
static char face_name[16]; /* 当前表情裸名（协议里的 "happy"） */
static char drawn_face[16], drawn_caption[256];
static uint8_t drawn_listening, drawn_ota;
static char caption[256]; /* 当前 AI 文字（UTF-8） */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void message(const char *title, const char *hint)
{
    LCD_Clear(LCD_BLACK);
    LCD_ShowString(8, 80, title, LCD_YELLOW, LCD_BLACK, LCD_FONT_16, LCD_MODE_NORMAL);
    LCD_ShowString(8, 112, hint, LCD_WHITE, LCD_BLACK, LCD_FONT_16, LCD_MODE_NORMAL);
}
static int bmp_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext && strlen(ext) == 4 && (ext[1] == 'b' || ext[1] == 'B') &&
           (ext[2] == 'm' || ext[2] == 'M') && (ext[3] == 'p' || ext[3] == 'P');
}
static void scan(void)
{
    DIR dir;
    FILINFO info;
    FRESULT fr;
    uint16_t i, j;
    char tmp[13];
    count = selected = 0;
    chosen[0] = 0;
    photo_error = "NO SD CARD";
    if (!sd_ready)
        return;
    f_mount(0, &fs); /* R0.09 defers the actual mount until opendir. */
    fr = f_opendir(&dir, "/");
    while (fr == FR_OK && count < MAX_PHOTOS)
    {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || !info.fname[0])
            break;
        if (!(info.fattrib & (AM_DIR | AM_HID | AM_SYS)) && bmp_name(info.fname))
        {
            memcpy(names[count], info.fname, sizeof(names[count]));
            names[count++][12] = 0;
        }
    }
    if (fr != FR_OK)
    {
        count = 0;
        photo_error = fr == FR_NO_FILESYSTEM ? "NOT FAT16/FAT32" : "SD FILESYSTEM ERROR";
        printf("[BMP] root failed FR=%u (13=unsupported filesystem, exFAT not supported)\r\n",
               (unsigned)fr);
        return;
    }
    /* Stable filename order rather than creation order. */
    for (i = 0; i < count; i++)
        for (j = (uint16_t)(i + 1); j < count; j++)
            if (strcmp(names[i], names[j]) > 0)
            {
                strcpy(tmp, names[i]);
                strcpy(names[i], names[j]);
                strcpy(names[j], tmp);
            }
    for (i = 0; i < count; i++)
        printf("[BMP] %u: %s\r\n", (unsigned)(i + 1), names[i]);
    if (count)
        strcpy(chosen, names[0]);
    photo_error = "NO BMP FILE";
    printf("[BMP] found %u file(s) in root (limit 64)\r\n", (unsigned)count);
}

/* 大写化一个 ASCII 字符。FatFs 自己比较时也是折大写的（ff.c 的 create_name
 * 会把输入折成大写、dir_find 逐字节比 11 字节 SFN），但这里路径一律**写出**
 * 字面大写最省心 —— 万一哪天换成长文件名支持，也还是对的。 */
static char up(char c)
{
    return (char)((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
}

/* 由协议里的表情名（"happy"）拼出 SD 路径（"FACE/HAPPY.BMP"）。
 * 返回 1 = 名字合法；0 = 不合法（空 / 太长 / 带斜杠冒号）。out 至少 24 字节。
 * 允许调用者写成 "happy.bmp"，扩展名会被丢掉重拼。 */
static uint8_t face_build_path(const char *name, char *out)
{
    uint8_t i = 0u;

    if (name == 0 || name[0] == 0)
        return 0u;

    out[i++] = 'F';
    out[i++] = 'A';
    out[i++] = 'C';
    out[i++] = 'E';
    out[i++] = '/';

    while (name[0] != 0)
    {
        char c = name[0];
        if (c == '.')
            break; /* 后面跟的是扩展名，不要 */
        if (i >= 13u)
            return 0u; /* 5 + 8 = 13 是主名上限 */
        if (c == '/' || c == '\\' || c == ':')
            return 0u;
        out[i++] = up(c);
        name++;
    }
    if (i == 5u)
        return 0u; /* 只有 "/FACE/" 没有名字 */

    out[i++] = '.';
    out[i++] = 'B';
    out[i++] = 'M';
    out[i++] = 'P';
    out[i] = 0;
    return 1u;
}

/* 扫 SD 卡上的 FACE\ 目录，把 .bmp 短名填进 faces[]。
 *  不用改任何 FatFs 配置：_FS_MINIMIZE 0 已经开了 f_opendir/f_readdir，
 *   _USE_LFN 0 正好是 8.3 短名，_FS_RPATH 0 也不影响绝对路径 "/FACE"
 *   （RPATH 只管 f_chdir 那一族）。 */
void Media_ScanFaces(void)
{
    DIR dir;
    FILINFO info;
    FRESULT fr;
    uint16_t i;

    face_count = 0u;
    if (!sd_ready)
    {
        printf("[FACE] no SD card, skip scan\r\n");
        return;
    }

    f_mount(0, &fs);
    fr = f_opendir(&dir, FACE_DIR);
    if (fr != FR_OK)
    {
        /* 5=FR_NO_PATH(卡上没这个目录) 4=FR_NO_FILE 13=FR_NO_FILESYSTEM(exFAT) */
        printf("[FACE] opendir /" FACE_DIR " failed FR=%u\r\n", (unsigned)fr);
        printf("[FACE] Need /FACE on the card with 8.3 short-name BMPs\r\n");
        return;
    }

    while (face_count < MAX_FACES)
    {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || !info.fname[0])
            break;
        if (!(info.fattrib & (AM_DIR | AM_HID | AM_SYS)) && bmp_name(info.fname))
        {
            uint8_t k;
            for (k = 0; k < 12u && info.fname[k]; k++)
                faces[face_count][k] = up(info.fname[k]);
            faces[face_count][k] = 0;
            face_count++;
        }
    }
    if (fr != FR_OK)
    {
        printf("[FACE] readdir failed FR=%u\r\n", (unsigned)fr);
        face_count = 0u;
        return;
    }
    /*  R0.09 **没有 f_closedir**（ff.h 里只有 f_mount/f_opendir/f_readdir）。
     *   想关闭/复位目录对象就是 f_readdir(&dir, 0)。本项目原来的 scan() 是
     *   干脆不关（R0.09 容忍），这里按规矩关掉。 */
    f_readdir(&dir, 0);
    for (i = 0; i < face_count; i++)
        printf("[FACE] %u: %s\r\n", (unsigned)(i + 1u), faces[i]);
    printf("[FACE] found %u face(s) in /" FACE_DIR " (limit %u)\r\n", (unsigned)face_count,
           (unsigned)MAX_FACES);
}

/* ------------------------------------------------------------------
 * 把 BMP 居中裁剪后画进 (bx,by,bw,bh) 这个框。
 *   返回 0 = 成功；1 = 失败（*err 指向错误串，框里清黑并写一行提示）。
 *
 *  这段就是**原来 Media_ShowPhoto 的读图逻辑**，逐字搬过来的，只把
 *   "整屏" 换成了 "任意框" ⇒ 相册和表情**共用同一份已验证的代码**。
 *   表情库因此**不引入任何新的读图路径** —— 那是这次改动最大的风险来源。
 *
 * 裁剪规则：图比框大就居中裁，比框小就居中留黑。**绝不缩放**
 *   （F407 逐像素缩放太慢，屏上会明显卡）。
 * ------------------------------------------------------------------ */
static uint8_t bmp_blit(const char *fname, uint16_t bx, uint16_t by, uint16_t bw, uint16_t bh,
                        const char **err)
{
    uint8_t hdr[54], bpp, top_down;
    uint32_t offset, dib, width, height, stride, signed_h;
    uint32_t crop_x, crop_y, file_row, pos;
    uint16_t vw, vh, dx, dy, row, col;
    UINT got;
    FRESULT fr;
    uint32_t start;
    const char *e = 0;

    *err = 0;
    if (bw == 0u || bh == 0u)
    {
        *err = "BAD BOX";
        return 1u;
    }
    /* raw 只有 LCD_W*4 = 960 字节，而这里每行最多读 vw*bpp 字节
     * （vw <= bw，bpp 最大 4）⇒ bw 超过 LCD_W 就会溢出。挡一道。 */
    if (((uint32_t)bw * 4u) > (uint32_t)sizeof(raw))
    {
        *err = "BOX TOO WIDE";
        return 1u;
    }

    start = GetTick_ms();
    fr = f_open(&file, fname, FA_READ);
    if (fr != FR_OK)
    {
        printf("[BMP] open %s failed FR=%u (4=NO_FILE 5=NO_PATH)\r\n", fname, (unsigned)fr);
        *err = (fr == FR_NO_PATH)   ? "NO FACE DIR"
               : (fr == FR_NO_FILE) ? "NO SUCH BMP"
                                    : "BMP OPEN ERROR";
        return 1u;
    }
    fr = f_read(&file, hdr, sizeof(hdr), &got);
    if (fr != FR_OK || got != sizeof(hdr) || hdr[0] != 'B' || hdr[1] != 'M')
    {
        e = "BAD BMP HEADER";
        goto done;
    }
    offset = le32(hdr + 10);
    dib = le32(hdr + 14);
    width = le32(hdr + 18);
    signed_h = le32(hdr + 22);
    top_down = (signed_h & 0x80000000u) != 0;
    height = top_down ? 0u - signed_h : signed_h;
    if (dib < 40 || dib > 124 || offset < 14u + dib || le16(hdr + 26) != 1)
    {
        e = "UNSUPPORTED BMP HEADER";
        goto done;
    }
    if (le32(hdr + 30) != 0 || (le16(hdr + 28) != 24 && le16(hdr + 28) != 32))
    {
        e = "USE 24/32BIT RGB BMP";
        goto done;
    }
    if (!width || !height || width > MAX_BMP_DIM || height > MAX_BMP_DIM)
    {
        e = "INVALID BMP SIZE";
        goto done;
    }
    bpp = (uint8_t)(le16(hdr + 28) / 8);
    stride = (width * bpp + 3u) & ~3u;
    /* 先卡大小再乘，防溢出；顺带把截断文件在画之前就挡掉 */
    if (offset > f_size(&file) || height > (f_size(&file) - offset) / stride)
    {
        e = "TRUNCATED BMP";
        goto done;
    }

    vw = (uint16_t)(width > bw ? bw : width);
    vh = (uint16_t)(height > bh ? bh : height);
    crop_x = (width - vw) / 2u;
    crop_y = (height - vh) / 2u;
    dx = (uint16_t)(bx + (bw - vw) / 2u);
    dy = (uint16_t)(by + (bh - vh) / 2u);

    /* 先把整个框清黑：图比框小时四周留黑，也顺手擦掉上一张的残影 */
    /* A full-size image replaces every pixel; do not flash a black square first. */
    if (vw < bw || vh < bh)
        LCD_Fill(bx, by, (uint16_t)(bx + bw - 1u), (uint16_t)(by + bh - 1u), LCD_BLACK);

    /* 按文件行正序读，自下而上的 BMP 画到反过来的屏幕行上；
     * 只有可见像素进 RAM，填充/超出的部分直接跳过。
     *  这里只读 vw*bpp 而不是整行 stride —— 因为裁剪起点是用 f_lseek
     *    跳过去的。这正是 raw[] 只要 LCD_W*4 就够的原因，别改成整行读。 */
    for (row = 0; row < vh; row++)
    {
        file_row = (top_down ? crop_y : height - crop_y - vh) + row;
        pos = offset + file_row * stride + crop_x * bpp;
        fr = f_lseek(&file, pos);
        if (fr != FR_OK)
        {
            e = "BMP SEEK ERROR";
            goto done;
        }
        fr = f_read(&file, raw, (UINT)(vw * bpp), &got);
        if (fr != FR_OK || got != vw * bpp)
        {
            e = "BMP READ ERROR";
            goto done;
        }
        for (col = 0; col < vw; col++)
        {
            const uint8_t *px = raw + col * bpp;
            pixels[col] = LCD_RGB(px[2], px[1], px[0]);
        }
        LCD_ShowBuf(dx, (uint16_t)(dy + (top_down ? row : vh - 1u - row)), vw, 1, pixels);
        /* Only service PWM targets here: no recursive UART/timeline dispatch. */
        AILink_ServiceMotion();
    }
    printf("[BMP] %s %lux%lu %u-bit %s -> box(%u,%u %ux%u) OK %lums\r\n", fname,
           (unsigned long)width, (unsigned long)height, (unsigned)(bpp * 8u),
           top_down ? "top-down" : "bottom-up", (unsigned)bx, (unsigned)by, (unsigned)bw,
           (unsigned)bh, (unsigned long)(GetTick_ms() - start));
done:
    f_close(&file);
    if (e)
    {
        printf("[BMP] %s: %s (FR=%u)\r\n", fname, e, (unsigned)fr);
        LCD_Fill(bx, by, (uint16_t)(bx + bw - 1u), (uint16_t)(by + bh - 1u), LCD_BLACK);
        /* 错误串全是 ASCII，直接画在框中间。 本文件不许在字符串字面量里写汉字
         * （会触发 #870-D），下面这些提示也一样。 */
        LCD_ShowString((uint16_t)(bx + 4u), (uint16_t)(by + bh / 2u), e, LCD_YELLOW, LCD_BLACK,
                       LCD_FONT_16, LCD_MODE_NORMAL);
        *err = e;
        return 1u;
    }
    return 0u;
}

void Media_Init(uint8_t sd_ok)
{
    sd_ready = sd_ok;
    scan();
    Media_ScanFaces();
    printf("[MEDIA] Send text (UTF-8); CR/LF or 150ms idle ends a line\r\n");
    printf("[MEDIA] /bmp /next /rescan /text /face /cap /ai /status /color /help\r\n");
    printf("[FONT] ASCII 8x16 + Flash %u hanzi (App/font_data.h); unknown shows '?'\r\n",
           (unsigned)Font_Count());
}
uint16_t Media_PhotoCount(void)
{
    return count;
}

void Media_ShowPhoto(void)
{
    const char *error = 0;

    if (!chosen[0])
    {
        message(photo_error, "Root: TEST.BMP\nUART: /rescan");
        return;
    }
    if (bmp_blit(chosen, 0u, 0u, LCD_W, LCD_H, &error) != 0u)
    {
        message(error, chosen);
    }
}

/* Decode UTF-8 so one unknown Unicode character becomes one '?'. */
static uint32_t next_codepoint(const unsigned char **cursor)
{
    const unsigned char *p = *cursor;
    uint32_t code, min;
    uint8_t extra, i, first = *p++;
    if (first < 0x80)
    {
        *cursor = p;
        return first;
    }
    if (first >= 0xC2 && first <= 0xDF)
    {
        extra = 1;
        code = first & 31u;
        min = 0x80;
    }
    else if (first >= 0xE0 && first <= 0xEF)
    {
        extra = 2;
        code = first & 15u;
        min = 0x800;
    }
    else if (first >= 0xF0 && first <= 0xF4)
    {
        extra = 3;
        code = first & 7u;
        min = 0x10000;
    }
    else
    {
        *cursor = p;
        return '?';
    }
    for (i = 0; i < extra; i++)
    {
        if ((p[i] & 0xC0) != 0x80)
        {
            *cursor = p;
            return '?';
        }
        code = (code << 6) | (p[i] & 63u);
    }
    *cursor = p + extra;
    if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
        return '?';
    return code;
}
/* ------------------------------------------------------------------
 * 在 (x0,y0) 起、宽 w 高 h 的区域里逐字画 UTF-8 文本，自动换行/翻行。
 *   *truncated 回填 1 = 区域装不下、后面还有字没画。
 *   返回**缺字个数**（字库查不到 / 是非 ASCII 非汉字码点）。
 * 行高 20（8x16 字模 + 4 行距），换行/翻行判据与原来 Media_ShowText 逐字一致。
 * ------------------------------------------------------------------ */
static uint16_t draw_text(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const char *txt,
                          uint16_t color, uint16_t background, uint8_t *truncated)
{
    const unsigned char *p = (const unsigned char *)txt;
    const uint8_t *glyph;
    uint32_t cp;
    uint16_t x = x0, y = y0, missing = 0;
    uint8_t width;

    *truncated = 0u;
    while (*p)
    {
        cp = next_codepoint(&p);
        if (cp == '\n')
        {
            x = x0;
            y = (uint16_t)(y + 20);
            continue;
        }
        /* The 3755-Hanzi font need not contain CJK punctuation. */
        switch (cp)
        {
        case 0xFF0Cu:
            cp = ',';
            break;
        case 0x3002u:
            cp = '.';
            break;
        case 0xFF01u:
            cp = '!';
            break;
        case 0xFF1Fu:
            cp = '?';
            break;
        case 0xFF1Au:
            cp = ':';
            break;
        case 0xFF1Bu:
            cp = ';';
            break;
        case 0xFF08u:
            cp = '(';
            break;
        case 0xFF09u:
            cp = ')';
            break;
        default:
            break;
        }
        glyph = 0;
        if (cp >= 32u && cp <= 126u)
            width = 8u; /* 可打印 ASCII 走内置 8x16 */
        else
        {
            /* Flash 字库的索引是 2 字节码点。next_codepoint() 能解到 U+10FFFF，
               不在这里挡一道，(uint16_t)cp 会把 U+1F600 截成 0xF600 —— 万一
               那个码点恰好在字库里，屏上就会画出一个毫不相干的字，而且不报错。 */
            if (cp <= 0xFFFFu)
                glyph = Font_Glyph16((uint16_t)cp);
            width = 16u;
        }
        if ((uint16_t)(x + width) > (uint16_t)(x0 + w))
        {
            x = x0;
            y = (uint16_t)(y + 20);
        }
        if ((uint16_t)(y + 16u) > (uint16_t)(y0 + h))
        {
            *truncated = 1u;
            break;
        }
        if (glyph)
            LCD_ShowGlyph16(x, y, glyph, color, background, LCD_MODE_NORMAL);
        else
        {
            if (cp < 32u || cp > 126u)
            {
                cp = '?';
                missing++;
            }
            LCD_ShowChar(x, y, (uint8_t)cp, color, background, LCD_FONT_16, LCD_MODE_NORMAL);
        }
        x = (uint16_t)(x + width);
    }
    return missing;
}

void Media_DrawTextBox(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *value,
                       uint16_t foreground, uint16_t background)
{
    uint8_t truncated;
    (void)draw_text(x, y, w, h, value, foreground, background, &truncated);
}

uint8_t Media_NextPage(uint8_t page)
{
    switch (page)
    {
    case PAGE_AI:
        return PAGE_STATUS;
    case PAGE_STATUS:
        return PAGE_MIC_SETTINGS;
    case PAGE_MIC_SETTINGS:
        return PAGE_SERVO_SETTINGS;
    case PAGE_SERVO_SETTINGS:
        return PAGE_CAMERA;
    default:
        return PAGE_AI;
    }
}

void Media_ShowText(void)
{
    uint8_t truncated = 0u;
    uint16_t missing;

    LCD_Clear(LCD_BLACK);
    LCD_ShowString(8, 8, "UART TEXT / UTF-8", LCD_CYAN, LCD_BLACK, LCD_FONT_16, LCD_MODE_NORMAL);
    missing = draw_text(8u, 40u, (uint16_t)(LCD_W - 16u), (uint16_t)(LCD_H - 40u - 24u), text,
                        LCD_WHITE, LCD_BLACK, &truncated);
    LCD_ShowString(8, LCD_H - 16, "ASCII + FLASH CN; ?=missing", LCD_GRAY, LCD_BLACK, LCD_FONT_8,
                   LCD_MODE_NORMAL);
    printf("[TEXT] displayed; font=%u glyphs, missing=%u%s\r\n", (unsigned)Font_Count(),
           (unsigned)missing, truncated ? "; screen full" : "");
}

/* ==================================================================
 * AI 页：上方表情框 + 下方文字区
 * ------------------------------------------------------------------
 * 状态（face_name / caption）和绘制是分开的，理由见 media.h 里那段注释。
 * ================================================================== */

const char *Media_GetFace(void)
{
    return face_name;
}
const char *Media_GetCaption(void)
{
    return caption;
}

void Media_ClearFace(void)
{
    LCD_Fill(FACE_BOX_X, FACE_BOX_Y, (uint16_t)(FACE_BOX_X + FACE_BOX_SIZE - 1u),
             (uint16_t)(FACE_BOX_Y + FACE_BOX_SIZE - 1u), LCD_BLACK);
}

void Media_SetFace(const char *name)
{
    if (name == 0)
    {
        face_name[0] = 0;
        return;
    }
    strncpy(face_name, name, sizeof(face_name) - 1u);
    face_name[sizeof(face_name) - 1u] = 0;
}

void Media_SetCaption(const char *text_in)
{
    if (text_in == 0)
    {
        caption[0] = 0;
        return;
    }
    strncpy(caption, text_in, sizeof(caption) - 1u);
    caption[sizeof(caption) - 1u] = 0;
}

/* Reuse the same SD BMP decoder as existing expressions and photos. */
void Gui_DrawHead(uint8_t listening)
{
    const char *path = listening ? GUI_HEAD_LISTEN_BMP : GUI_HEAD_NORMAL_BMP;
    const char *error = 0;
    /* The ARMED pass loads the head before MicAud_Start. Never read SD while
     * DMA capture is running, even if another caller requests a repaint. */
    if (MicAud_IsCapturing())
        return;
    if (bmp_blit(path, FACE_BOX_X, FACE_BOX_Y, FACE_BOX_SIZE, FACE_BOX_SIZE, &error) == 0u)
        return;
    LCD_Fill(FACE_BOX_X, FACE_BOX_Y, FACE_BOX_X + FACE_BOX_SIZE - 1u,
             FACE_BOX_Y + FACE_BOX_SIZE - 1u, GUI_BG);
    LCD_ShowString(FACE_BOX_X + 8u, FACE_BOX_Y + 48u, "SD IMAGE UNAVAILABLE", GUI_INK, GUI_BG,
                   LCD_FONT_8, LCD_MODE_NORMAL);
    LCD_ShowString(FACE_BOX_X + 8u, FACE_BOX_Y + 68u, path, GUI_INK, GUI_BG, LCD_FONT_8,
                   LCD_MODE_NORMAL);
    Media_DrawTextBox(FACE_BOX_X + 8u, FACE_BOX_Y + 88u, FACE_BOX_SIZE - 16u, 40u,
                      error ? error : "BMP ERROR", GUI_INK, GUI_BG);
    printf("[GUI] SD head unavailable: %s (%s)\r\n", path, error ? error : "unknown");
}

static void draw_face(void)
{
    char path[24];
    const char *error = 0;
    if (MicAud_IsCapturing())
        return;
    FaceAnim_Stop();
    if (Gui_IsListening())
    {
        Gui_DrawHead(1u);
        return;
    }
    if (FaceAnim_Start(face_name, FACE_BOX_X, FACE_BOX_Y, GetTick_ms()))
        return;
    if (face_name[0] && strcmp(face_name, "normal") && face_build_path(face_name, path))
    {
        if (bmp_blit(path, FACE_BOX_X, FACE_BOX_Y, FACE_BOX_SIZE, FACE_BOX_SIZE, &error) == 0u)
            return;
        printf("[GUI] face %s unavailable: %s; default SD head used\r\n", face_name,
               error ? error : "unknown");
    }
    Gui_DrawHead(0u);
}

static void draw_caption(void)
{
    uint8_t truncated = 0u;
    uint16_t missing;

    LCD_Fill(CAPTION_X, CAPTION_Y, (uint16_t)(CAPTION_X + CAPTION_W - 1u),
             (uint16_t)(CAPTION_Y + CAPTION_H - 1u), GUI_BG);
    missing = draw_text((uint16_t)(CAPTION_X + 4u), (uint16_t)(CAPTION_Y + 2u),
                        (uint16_t)(CAPTION_W - 8u), (uint16_t)(CAPTION_H - 4u),
                        Gui_IsListening() ? "你说，我在听。"
                                          : (caption[0] ? caption : "你好呀，我在这里。"),
                        GUI_INK, GUI_BG, &truncated);
    printf("[CAP] %u bytes shown, missing=%u%s\r\n", (unsigned)strlen(caption), (unsigned)missing,
           truncated ? "; AREA FULL" : "");
}

void Media_DrawAIPage(void)
{
    FaceAnim_Stop();
    drawn_ota = (uint8_t)(AILink_OtaPending() || IAP_IsBusy() || !strncmp(caption, "OTA", 3u));
    Gui_Frame(drawn_ota ? "固件更新 / OTA" : "小伴 / COMPANION", 0u);
    if (drawn_ota)
        Media_DrawTextBox(12u, 40u, LCD_W - 24u, LCD_H - 72u, caption, GUI_INK, GUI_BG);
    else
    {
        draw_face();
        draw_caption();
        LCD_DrawRect(FACE_BOX_X - 2u, FACE_BOX_Y - 2u, FACE_BOX_X + FACE_BOX_SIZE + 1u,
                     FACE_BOX_Y + FACE_BOX_SIZE + 1u, GUI_INK);
        Gui_AudioTick(PAGE_AI, GetTick_ms(), 1u);
    }
    drawn_listening = Gui_IsListening();
    strcpy(drawn_face, face_name);
    strcpy(drawn_caption, caption);
}

void Media_UpdateAIPage(void)
{
    uint8_t ota = (uint8_t)(AILink_OtaPending() || IAP_IsBusy() || !strncmp(caption, "OTA", 3u));
    if (ota != drawn_ota)
    {
        Media_DrawAIPage();
        return;
    }
    if (ota)
    {
        if (strcmp(drawn_caption, caption))
        {
            LCD_Fill(12u, 40u, LCD_W - 13u, LCD_H - 33u, GUI_BG);
            Media_DrawTextBox(12u, 40u, LCD_W - 24u, LCD_H - 72u, caption, GUI_INK, GUI_BG);
            strcpy(drawn_caption, caption);
        }
        return;
    }
    if (strcmp(drawn_face, face_name) || drawn_listening != Gui_IsListening())
    {
        draw_face();
        strcpy(drawn_face, face_name);
    }
    if (strcmp(drawn_caption, caption) || drawn_listening != Gui_IsListening())
    {
        draw_caption();
        strcpy(drawn_caption, caption);
    }
    drawn_listening = Gui_IsListening();
    Gui_AudioTick(PAGE_AI, GetTick_ms(), 1u);
}

void Media_AnimationTick(uint8_t visible_page, uint32_t now)
{
    FaceAnim_Info info;
    /* A stopped/faulted animation is dealt with only when idle. */
    if (CameraLink_Busy() || MicLink_IsRecording() || MicAud_IsStreaming() || IAP_IsBusy() ||
        AILink_OtaPending() || FontUpload_Active())
        return;
    if (visible_page != PAGE_AI || drawn_ota)
    {
        FaceAnim_Stop();
        return;
    }
    FaceAnim_Tick(now, 1u);
    FaceAnim_GetInfo(&info);
    if (info.error && !info.active)
    {
        Gui_DrawHead(0u);
        /* Clear the error without repeatedly retrying a missing SD frame. */
        (void)FaceAnim_Start("NONE", FACE_BOX_X, FACE_BOX_Y, now);
    }
}

uint8_t Media_Command(const char *line)
{
    const char *name;
    size_t n;
    uint16_t i;
    if (!strcmp(line, "/help"))
    {
        printf("[MEDIA] /bmp [NAME.BMP] /next /rescan /text TEXT /status /color\r\n");
        printf("[MEDIA] /face <happy|sad|sleepy|angry|normal>  /cap TEXT  /ai  /faces\r\n");
        /* 录音那三条命令其实归 Hardware\19MICREC 管（main.c 里先截 micrec
         * 再给 media），这里只负责让人知道它们存在。 同样不写汉字。 */
        printf("[REC] /rec [1..2] /dump /mic\r\n");
        /* 别在这行里写汉字：本文件虽有 BOM，但字符串里的中文一旦被别的工具
           改编码就是 #870-D。字表清单看 App/font_data.h。 */
        printf("[FONT] ASCII 8x16 built in; %u hanzi in Flash (App/font_data.h)\r\n",
               (unsigned)Font_Count());
        return PAGE_KEEP;
    }
    if (!strcmp(line, "/anim on") || !strcmp(line, "/anim off"))
    {
        (void)FaceAnim_SetEnabled((uint8_t)(!strcmp(line, "/anim on")));
        FaceAnim_Stop();
        drawn_face[0] = 0;
        return PAGE_AI;
    }
    if (!strncmp(line, "/anim fps ", 10u))
    {
        unsigned fps = 0u;
        char extra;
        if (sscanf(line + 10, "%u %c", &fps, &extra) == 1 && fps <= 255u &&
            FaceAnim_SetFPS((uint8_t)fps))
            printf("[ANIM] requested fps=%u (actual SD/display throughput dependent)\r\n", fps);
        else
            printf("[ANIM] FPS must be 1..25\r\n");
        return PAGE_KEEP;
    }
    if (!strcmp(line, "/anim"))
    {
        FaceAnim_Info a;
        FaceAnim_GetInfo(&a);
        printf("[ANIM] %s frame=%u/%u rows=%u completed=%lu interval=%u active=%u err=%u\r\n",
               a.directory, (unsigned)a.frame, (unsigned)a.frames, (unsigned)a.rows,
               (unsigned long)a.completed, (unsigned)a.interval_ms, (unsigned)a.active,
               (unsigned)a.error);
        return PAGE_KEEP;
    }
    if (!strcmp(line, "/status"))
        return PAGE_STATUS;
    if (!strcmp(line, "/settings mic"))
        return PAGE_MIC_SETTINGS;
    if (!strcmp(line, "/settings servo"))
        return PAGE_SERVO_SETTINGS;
    if (!strcmp(line, "/camera"))
        return PAGE_CAMERA;
    if (!strcmp(line, "/color"))
        return PAGE_COLOR;
    if (!strcmp(line, "/text"))
        return PAGE_TEXT;
    if (!strcmp(line, "/rescan"))
    {
        scan();
        return PAGE_PHOTO;
    }
    if (!strcmp(line, "/next"))
    {
        if (count)
        {
            selected = (uint16_t)((selected + 1u) % count);
            strcpy(chosen, names[selected]);
        }
        return PAGE_PHOTO;
    }
    if (!strcmp(line, "/bmp"))
        return PAGE_PHOTO;
    if (!strncmp(line, "/bmp ", 5))
    {
        name = line + 5;
        n = strlen(name);
        if (!n || n > 12 || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':') ||
            !bmp_name(name))
        {
            printf("[BMP] Use root short name, e.g. /bmp TEST.BMP\r\n");
            return PAGE_KEEP;
        }
        strcpy(chosen, name);
        for (i = 0; i < count; i++)
            if (!strcmp(chosen, names[i]))
            {
                selected = i;
                break;
            }
        return PAGE_PHOTO;
    }
    /* ---------- AI 页相关的四条 ----------
     *  必须插在下面那个 "/text " / else-if 链**之前**，否则 /face、/cap
     *    会被那条链当成"不认识 /xxx"吃掉，回一句 Unknown command。 */
    if (!strcmp(line, "/ai"))
        return PAGE_AI;
    if (!strcmp(line, "/faces"))
    {
        Media_ScanFaces();
        return PAGE_KEEP;
    }
    if (!strncmp(line, "/face", 5) && (line[5] == 0 || line[5] == ' '))
    {
        const char *n = line + 5;
        while (*n == ' ')
            n++;
        if (*n == 0)
        {
            printf("[FACE] usage: /face happy|sad|sleepy|angry|normal\r\n");
            return PAGE_KEEP;
        }
        Media_SetFace(n);
        return PAGE_AI;
    }
    if (!strncmp(line, "/cap", 4) && (line[4] == 0 || line[4] == ' '))
    {
        const char *n = line + 4;
        while (*n == ' ')
            n++;
        Media_SetCaption(n); /* 不带参数 = 清空 */
        return PAGE_AI;
    }
    if (!strncmp(line, "/text ", 6))
        line += 6;
    else if (line[0] == '/')
    {
        printf("[MEDIA] Unknown command; /help\r\n");
        return PAGE_KEEP;
    }
    strncpy(text, line, sizeof(text) - 1u);
    text[sizeof(text) - 1u] = 0;
    return PAGE_TEXT;
}

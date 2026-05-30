/*
 * app_usbstick.cpp — USB mass-storage drive with on-screen file browser
 *
 * Portrait 368×448, canvas. Like Filehub but stripped of WiFi/WebDAV/HTTP:
 * the device only exposes the SD card to a connected PC over USB MSC.
 *
 * Controls:
 *   Touch tap   – enter folder / go up (first row is "..")
 *   Touch drag  – scroll file list / text viewer
 *   BOOT short  – toggle WRITE PROTECT (or exit text viewer back to list)
 *   PWR  short  – (unused; long-press PWR exits to launcher)
 *
 * Write-protect: when enabled, the USB MSC write callback returns an error,
 * so the connected PC sees write failures while reads keep working. Toggle
 * before plugging in for the cleanest experience; toggling mid-transfer can
 * leave the host with a half-written file.
 */

#include "app_usbstick.h"
#include "app_common.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include "TouchDrvFT6X36.hpp"

// USB MSC is only available in TinyUSB mode (USBMode=default + CDCOnBoot=cdc).
#if !ARDUINO_USB_MODE && SOC_USB_OTG_SUPPORTED
  #define US_HAS_USB_MSC 1
  #include <USB.h>
  #include <USBMSC.h>
#else
  #define US_HAS_USB_MSC 0
#endif

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;
extern TouchDrvFT6X36  touch;

// ── Constants ────────────────────────────────────────────────────────────────
#define BOOT_BTN        0
#define SWIPE_THRESH    20
#define TAP_MAX_MS      300
#define TAP_MAX_DIST    20
// UI geometry — match Filehub for visual consistency
#define ROW_H           28
#define LIST_TOP        96
#define LIST_BOTTOM    326
#define PATH_BAR_Y      68
#define PATH_BAR_H      22
#define MAX_ENTRIES     512
#define NAME_COL_MAX    24

#define TEXT_MAX_BYTES  (256 * 1024)
#define TEXT_MAX_LINES  20000
#define TEXT_COLS       29
#define TEXT_TOP        46
#define TEXT_BOTTOM     320
#define TEXT_LINE_H     20

enum UsState { US_BOOT, US_NO_SD, US_READY };
enum UsView  { UV_FILES, UV_TEXT };

// ── State ────────────────────────────────────────────────────────────────────
static Arduino_Canvas *canvas = nullptr;
static UsState   s_state      = US_BOOT;
static UsView    s_view       = UV_FILES;

// USB-MSC state (driven from USB event task; consumed in loop)
static volatile bool s_usbConnected = false;
static volatile bool s_usbDirty     = false;   // UI needs a redraw on connect/disconnect
static volatile bool s_writeProtect = false;
static volatile uint64_t s_bytesIn  = 0;
static volatile uint64_t s_bytesOut = 0;
#if US_HAS_USB_MSC
static USBMSC s_msc;
#endif

// Text viewer state
static char     *s_textBuf        = nullptr;
static uint32_t  s_textLen        = 0;
static bool      s_textTruncated  = false;
static char      s_textName[64]   = {};
static uint32_t *s_textLineOff    = nullptr;
static int       s_textLineCount  = 0;
static int       s_textScroll     = 0;

// File browser state
static char      s_path[256] = "/";
static struct UsEntry {
    char name[96];
    uint32_t size;
    bool isDir;
} *s_entries = nullptr;
static int       s_entryCount = 0;
static int       s_scroll     = 0;

// Input state
static bool      s_bootWas     = false;
static bool      s_touchWas    = false;
static int16_t   s_touchStartY = 0;
static int16_t   s_touchLastY  = 0;
static uint32_t  s_touchDownMs = 0;
static bool      s_touchDrag   = false;
static uint32_t  s_lastDraw    = 0;
static uint32_t  s_lastStatus  = 0;

// ── Forward decls ────────────────────────────────────────────────────────────
static void drawAll();
static void drawText();
static void refreshListing();

// ── Path helpers ─────────────────────────────────────────────────────────────
static void joinSd(char *out, size_t cap, const char *rel) {
    if (!rel || !*rel) { if (cap > 1) { out[0] = '/'; out[1] = '\0'; } return; }
    if (rel[0] == '/') { strncpy(out, rel, cap - 1); out[cap - 1] = '\0'; }
    else               { snprintf(out, cap, "/%s", rel); }
}

static bool sanitizeRelPath(char *p) {
    if (!p) return false;
    if (strstr(p, "..")) return false;
    for (char *c = p; *c; c++) if (*c == '\\') return false;
    return true;
}

static void humanSize(uint32_t b, char *buf, size_t cap) {
    if (b < 1024)                       snprintf(buf, cap, "%u B",   (unsigned)b);
    else if (b < 1024UL * 1024)         snprintf(buf, cap, "%.1f KB", b / 1024.0);
    else if (b < 1024UL * 1024 * 1024)  snprintf(buf, cap, "%.1f MB", b / (1024.0 * 1024));
    else                                snprintf(buf, cap, "%.2f GB", b / (1024.0 * 1024 * 1024));
}

// ── Listing ──────────────────────────────────────────────────────────────────
static int cmpEntry(const void *a, const void *b) {
    const UsEntry *x = (const UsEntry *)a, *y = (const UsEntry *)b;
    if (x->isDir != y->isDir) return x->isDir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void refreshListing() {
    if (!s_entries) s_entries = (UsEntry *)ps_malloc(sizeof(UsEntry) * MAX_ENTRIES);
    if (!s_entries) return;
    s_entryCount = 0;

    char full[300]; joinSd(full, sizeof(full), s_path);
    File d = SD_MMC.open(full);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }

    File c;
    while ((c = d.openNextFile()) && s_entryCount < MAX_ENTRIES) {
        const char *nm = c.name();
        const char *base = strrchr(nm, '/'); base = base ? base + 1 : nm;
        UsEntry &e = s_entries[s_entryCount++];
        strncpy(e.name, base, sizeof(e.name) - 1); e.name[sizeof(e.name) - 1] = '\0';
        e.size  = (uint32_t)c.size();
        e.isDir = c.isDirectory();
        c.close();
    }
    d.close();
    qsort(s_entries, s_entryCount, sizeof(UsEntry), cmpEntry);
    s_scroll = 0;
}

static void pathUp() {
    if (strcmp(s_path, "/") == 0) return;
    char *slash = strrchr(s_path, '/');
    if (!slash) return;
    if (slash == s_path) { s_path[1] = '\0'; }
    else                 { *slash    = '\0'; }
    refreshListing();
}

static void pathEnter(const char *sub) {
    char np[256];
    if (strcmp(s_path, "/") == 0) snprintf(np, sizeof(np), "/%s", sub);
    else                          snprintf(np, sizeof(np), "%s/%s", s_path, sub);
    if (!sanitizeRelPath(np)) return;
    strncpy(s_path, np, sizeof(s_path) - 1); s_path[sizeof(s_path) - 1] = '\0';
    refreshListing();
}

// ── Text viewer ──────────────────────────────────────────────────────────────
static bool isTextFile(const char *name) {
    static const char *exts[] = {
        ".txt", ".md", ".gcode", ".nc", ".csv", ".log", ".json", ".ini",
        ".cfg", ".conf", ".sh", ".py", ".js", ".html", ".htm", ".css",
        ".xml", ".yaml", ".yml", ".c", ".cpp", ".h", ".hpp", ".ino", nullptr
    };
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[12]; size_t n = strlen(dot);
    if (n >= sizeof(ext)) return false;
    for (size_t i = 0; i <= n; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    for (int i = 0; exts[i]; i++) if (!strcmp(ext, exts[i])) return true;
    return false;
}

static void freeText() {
    if (s_textBuf)     { heap_caps_free(s_textBuf);     s_textBuf = nullptr; }
    if (s_textLineOff) { heap_caps_free(s_textLineOff); s_textLineOff = nullptr; }
    s_textLen = 0; s_textLineCount = 0; s_textScroll = 0; s_textTruncated = false;
    s_textName[0] = '\0';
}

static void buildTextLines() {
    if (!s_textBuf || !s_textLineOff) return;
    s_textLineCount = 0;
    uint32_t i = 0;
    while (i < s_textLen && s_textLineCount < TEXT_MAX_LINES) {
        s_textLineOff[s_textLineCount++] = i;
        uint32_t lineStart = i;
        uint32_t lastSpace = 0; bool haveSpace = false;
        while (i < s_textLen) {
            char c = s_textBuf[i];
            if (c == '\n') { i++; break; }
            if (c == ' ') { lastSpace = i; haveSpace = true; }
            uint32_t col = i - lineStart;
            if (col >= TEXT_COLS) {
                if (haveSpace && (i - lastSpace) < TEXT_COLS / 2) i = lastSpace + 1;
                break;
            }
            i++;
        }
    }
}

static bool openTextFile(const char *sdPath, const char *displayName) {
    freeText();
    File f = SD_MMC.open(sdPath, FILE_READ);
    if (!f || f.isDirectory()) { if (f) f.close(); return false; }

    uint32_t fsize = f.size();
    uint32_t toRead = fsize;
    if (toRead > TEXT_MAX_BYTES) { toRead = TEXT_MAX_BYTES; s_textTruncated = true; }

    s_textBuf = (char *)heap_caps_malloc(toRead + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_textBuf) { f.close(); return false; }

    uint32_t got = 0;
    while (got < toRead) {
        int n = f.read((uint8_t *)s_textBuf + got, toRead - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    f.close();
    s_textLen = got;
    s_textBuf[s_textLen] = '\0';

    for (uint32_t k = 0; k < s_textLen; k++) if (s_textBuf[k] == '\r') s_textBuf[k] = ' ';

    s_textLineOff = (uint32_t *)heap_caps_malloc(TEXT_MAX_LINES * sizeof(uint32_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_textLineOff) { heap_caps_free(s_textBuf); s_textBuf = nullptr; return false; }

    buildTextLines();

    strncpy(s_textName, displayName, sizeof(s_textName) - 1);
    s_textName[sizeof(s_textName) - 1] = '\0';
    s_textScroll = 0;
    return true;
}

// ── UI ───────────────────────────────────────────────────────────────────────
static void drawTitle() {
    const char *title = "USB Stick";
    int tw = (int)strlen(title) * 18;
    canvas->setTextSize(3);
    canvas->setTextColor(0x07FF);
    canvas->setCursor((LCD_WIDTH - tw) / 2, 10);
    canvas->print(title);
}

static void drawFiles() {
    canvas->fillScreen(0x0000);

    drawTitle();

    // Status line: USB connected? / write-protect?
    canvas->setTextSize(2);
    canvas->setCursor(12, 40);
    if (s_usbConnected) { canvas->setTextColor(0x07E0); canvas->print("USB: linked"); }
    else                { canvas->setTextColor(0x8410); canvas->print("USB: idle"); }
    canvas->print("  ");
    if (s_writeProtect) { canvas->setTextColor(0xF800); canvas->print("WP: ON");  }
    else                { canvas->setTextColor(0x07E0); canvas->print("WP: off"); }

    // Path bar
    canvas->fillRect(0, PATH_BAR_Y, LCD_WIDTH, PATH_BAR_H, HUD_PILL_BG);
    canvas->drawFastHLine(0, PATH_BAR_Y,              LCD_WIDTH, HUD_PILL_BD);
    canvas->drawFastHLine(0, PATH_BAR_Y + PATH_BAR_H, LCD_WIDTH, HUD_PILL_BD);
    canvas->setTextSize(2);
    canvas->setTextColor(0xDEFB);
    canvas->setCursor(8, PATH_BAR_Y + 4);
    char pathTrim[32];
    size_t pl = strlen(s_path);
    if (pl > 26) { strcpy(pathTrim, "..."); strcat(pathTrim, s_path + pl - 23); }
    else         { strncpy(pathTrim, s_path, sizeof(pathTrim) - 1); pathTrim[sizeof(pathTrim) - 1] = '\0'; }
    canvas->print(pathTrim);

    // List
    int listH = LIST_BOTTOM - LIST_TOP;
    int rowsVisible = listH / ROW_H;
    int atRoot = (strcmp(s_path, "/") == 0);
    int totalRows = s_entryCount + (atRoot ? 0 : 1);

    if (totalRows > rowsVisible && s_scroll > totalRows - rowsVisible)
        s_scroll = totalRows - rowsVisible;
    if (s_scroll < 0) s_scroll = 0;

    for (int row = 0; row < rowsVisible; row++) {
        int idx = s_scroll + row;
        if (idx >= totalRows) break;
        int16_t y = LIST_TOP + row * ROW_H;

        canvas->fillRect(0, y, LCD_WIDTH, ROW_H - 2, (row & 1) ? 0x0861 : 0x0000);

        const char *name;
        char szBuf[16] = "";
        bool isDir;
        if (!atRoot && idx == 0) { name = ".."; isDir = true; }
        else {
            const UsEntry &e = s_entries[idx - (atRoot ? 0 : 1)];
            name = e.name;
            isDir = e.isDir;
            if (!e.isDir) humanSize(e.size, szBuf, sizeof(szBuf));
        }

        canvas->setTextSize(2);
        canvas->setCursor(10, y + 5);
        if (isDir) { canvas->setTextColor(0x07FF); canvas->print("/ "); }
        else       { canvas->setTextColor(0x2104); canvas->print("  "); }

        char trimmed[NAME_COL_MAX + 1];
        strncpy(trimmed, name, NAME_COL_MAX);
        trimmed[NAME_COL_MAX] = '\0';
        canvas->setTextColor(isDir ? 0xDEFB : 0xFFFF);
        printUtf8(canvas, trimmed);

        if (szBuf[0]) {
            canvas->setTextSize(1);
            canvas->setTextColor(0x8410);
            int16_t szW = (int16_t)strlen(szBuf) * 6;
            canvas->setCursor(LCD_WIDTH - 32 - szW, y + 10);
            canvas->print(szBuf);
        }
    }

    // Counters
    canvas->setTextSize(2);
    canvas->setTextColor(0x4208);
    char up[16], dn[16], counters[48];
    humanSize((uint32_t)(s_bytesIn  & 0xFFFFFFFF), up, sizeof(up));
    humanSize((uint32_t)(s_bytesOut & 0xFFFFFFFF), dn, sizeof(dn));
    snprintf(counters, sizeof(counters), "in %s  out %s", up, dn);
    canvas->setCursor(12, 370);
    canvas->print(counters);

    // Pills
    draw_pill_label(canvas, 0, 0, s_writeProtect ? "unlock" : "lock");
    draw_pill_label(canvas, 0, 1, "exit");

    draw_battery_g  (canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    canvas->flush();
}

static void drawText() {
    canvas->fillScreen(0x0000);

    canvas->setTextSize(2);
    canvas->setTextColor(0x07FF);
    char title[30];
    size_t nl = strlen(s_textName);
    if (nl > 26) { memcpy(title, s_textName, 23); title[23] = '.'; title[24] = '.'; title[25] = '.'; title[26] = '\0'; }
    else         { strcpy(title, s_textName); }
    int16_t tw = (int16_t)strlen(title) * 12;
    canvas->setCursor((LCD_WIDTH - tw) / 2, 14);
    canvas->print(title);

    int linesPerPage = (TEXT_BOTTOM - TEXT_TOP) / TEXT_LINE_H;
    if (s_textScroll < 0) s_textScroll = 0;
    if (s_textScroll > s_textLineCount - 1) s_textScroll = s_textLineCount - 1;
    if (s_textScroll < 0) s_textScroll = 0;

    canvas->setTextSize(2);
    canvas->setTextColor(0xFFFF);
    for (int row = 0; row < linesPerPage; row++) {
        int idx = s_textScroll + row;
        if (idx >= s_textLineCount) break;
        uint32_t start = s_textLineOff[idx];
        uint32_t end   = (idx + 1 < s_textLineCount) ? s_textLineOff[idx + 1] : s_textLen;
        while (end > start && (s_textBuf[end - 1] == '\n' || s_textBuf[end - 1] == ' ')) end--;
        uint32_t len = end - start;
        if (len > TEXT_COLS) len = TEXT_COLS;
        char buf[TEXT_COLS + 1];
        memcpy(buf, s_textBuf + start, len);
        for (uint32_t k = 0; k < len; k++) {
            unsigned char c = (unsigned char)buf[k];
            if (c == '\t') buf[k] = ' ';
            else if (c < 0x20 || c >= 0x7F) buf[k] = '.';
        }
        buf[len] = '\0';
        canvas->setCursor(8, TEXT_TOP + row * TEXT_LINE_H);
        canvas->print(buf);
    }

    canvas->setTextSize(2);
    canvas->setTextColor(0x8410);
    char st[48];
    int endLine = s_textScroll + linesPerPage;
    if (endLine > s_textLineCount) endLine = s_textLineCount;
    snprintf(st, sizeof(st), "lines %d-%d / %d%s",
             s_textScroll + 1, endLine, s_textLineCount,
             s_textTruncated ? " (truncated)" : "");
    int sw = (int)strlen(st) * 12;
    canvas->setCursor((LCD_WIDTH - sw) / 2, 404);
    canvas->print(st);

    draw_pill_label(canvas, 0, 0, "back");
    draw_battery_g  (canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    canvas->flush();
}

static void drawCenteredBanner(const char *title, const char *sub, uint16_t titleCol) {
    canvas->fillScreen(0x0000);
    canvas->setTextSize(3);
    canvas->setTextColor(titleCol);
    int tw = (int)strlen(title) * 18;
    canvas->setCursor((LCD_WIDTH - tw) / 2, 180);
    canvas->print(title);
    if (sub) {
        canvas->setTextSize(2);
        canvas->setTextColor(0xDEFB);
        int16_t sw = (int16_t)strlen(sub) * 12;
        canvas->setCursor((LCD_WIDTH - sw) / 2, 230);
        canvas->print(sub);
    }
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    canvas->flush();
}

static void drawNoSd() { drawCenteredBanner("No SD card", "insert and reset", 0xFD20); }
static void drawBoot() { drawCenteredBanner("USB Stick", "starting...",      0x07FF); }

static void drawAll() {
    if      (s_view == UV_TEXT)  drawText();
    else if (s_state == US_NO_SD) drawNoSd();
    else                          drawFiles();
}

// ── USB Mass Storage ────────────────────────────────────────────────────────
#if US_HAS_USB_MSC
extern "C" {
#include "diskio.h"
}
static constexpr uint8_t US_PDRV = 0;

static int32_t mscOnWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)offset;
    if (s_writeProtect) return -1;            // pretend the medium is locked
    uint32_t secSize = SD_MMC.sectorSize();
    if (!secSize) return -1;
    uint32_t count = bufsize / secSize;
    if (disk_write(US_PDRV, buffer, lba, count) != RES_OK) return -1;
    s_bytesIn += bufsize;
    return bufsize;
}

static int32_t mscOnRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)offset;
    uint32_t secSize = SD_MMC.sectorSize();
    if (!secSize) return -1;
    uint32_t count = bufsize / secSize;
    if (disk_read(US_PDRV, (uint8_t *)buffer, lba, count) != RES_OK) return -1;
    s_bytesOut += bufsize;
    return bufsize;
}

static bool mscOnStartStop(uint8_t, bool, bool) { return true; }

static void usbEventCallback(void *, esp_event_base_t base, int32_t id, void *) {
    if (base != ARDUINO_USB_EVENTS) return;
    if (id == ARDUINO_USB_STARTED_EVENT) { s_usbConnected = true;  s_usbDirty = true; }
    if (id == ARDUINO_USB_STOPPED_EVENT) { s_usbConnected = false; s_usbDirty = true; }
}

static void initUsbMsc() {
    s_msc.vendorID("Pixels");
    s_msc.productID("USBStick");
    s_msc.productRevision("1.0");
    s_msc.onRead(mscOnRead);
    s_msc.onWrite(mscOnWrite);
    s_msc.onStartStop(mscOnStartStop);
    s_msc.begin(SD_MMC.numSectors(), SD_MMC.sectorSize());
    s_msc.mediaPresent(true);
    USB.onEvent(usbEventCallback);
    USB.begin();
}
#else
static void initUsbMsc() {}
#endif

// ── Lifecycle ────────────────────────────────────────────────────────────────
void app_usbstick_setup(Arduino_SH8601 *gfx_unused) {
    (void)gfx_unused;
    canvas = g_canvas;

    s_view          = UV_FILES;
    s_bytesIn       = s_bytesOut = 0;
    s_scroll        = 0;
    s_writeProtect  = false;
    s_usbConnected  = false;
    s_usbDirty      = false;
    s_bootWas       = false;
    s_touchWas      = false;
    s_touchDrag     = false;
    s_lastDraw      = 0;
    s_lastStatus    = 0;
    strcpy(s_path, "/");

    drawBoot();

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    if (!SD_MMC.begin("/sdcard", true)) {
        s_state = US_NO_SD;
        drawNoSd();
        return;
    }

    initUsbMsc();

    refreshListing();
    s_state = US_READY;
    drawAll();
    s_lastDraw = millis();
}

void app_usbstick_loop() {
    common_activity();      // background-service app: never auto-off
    common_tick();

    if (s_state == US_NO_SD) {
        delay(1500);
        if (SD_MMC.begin("/sdcard", true)) {
            s_state = US_BOOT;
            initUsbMsc();
            refreshListing();
            s_state = US_READY;
            drawAll();
        }
        return;
    }

    uint32_t now = millis();

    // BOOT short = toggle write-protect (or back-to-files from text viewer)
    bool boot = (digitalRead(BOOT_BTN) == LOW);
    if (boot && !s_bootWas) {
        common_activity();
        if (s_view == UV_TEXT) {
            freeText();
            s_view = UV_FILES;
        } else {
            s_writeProtect = !s_writeProtect;
        }
        drawAll();
        s_lastDraw = now;
    }
    s_bootWas = boot;

    // Drain any PWR short press (long-press is handled by common_tick → exit)
    (void)common_consume_pwr_short();

    // USB connect/disconnect — refresh the status line
    if (s_usbDirty) {
        s_usbDirty = false;
        // Re-read folder contents in case the host wrote files while connected.
        if (!s_usbConnected && s_view == UV_FILES) refreshListing();
        drawAll();
        s_lastDraw = now;
    }

    // Touch
    int16_t tx[1], ty[1];
    bool touching = touch.getPoint(tx, ty, 1);

    if (touching && !s_touchWas) {
        s_touchStartY = ty[0];
        s_touchLastY  = ty[0];
        s_touchDownMs = now;
        s_touchDrag   = false;
        common_activity();
    }

    if (touching && s_touchWas && s_view == UV_FILES) {
        int16_t dy = ty[0] - s_touchLastY;
        if (dy > SWIPE_THRESH) {
            s_scroll -= 1;
            if (s_scroll < 0) s_scroll = 0;
            s_touchLastY = ty[0];
            s_touchDrag = true;
            drawAll();
            s_lastDraw = now;
        } else if (dy < -SWIPE_THRESH) {
            s_scroll += 1;
            s_touchLastY = ty[0];
            s_touchDrag = true;
            drawAll();
            s_lastDraw = now;
        }
    }

    if (touching && s_touchWas && s_view == UV_TEXT) {
        int16_t dy = ty[0] - s_touchLastY;
        int linesPerPage = (TEXT_BOTTOM - TEXT_TOP) / TEXT_LINE_H;
        int step = linesPerPage / 3; if (step < 1) step = 1;
        if (dy > SWIPE_THRESH) {
            s_textScroll -= step;
            if (s_textScroll < 0) s_textScroll = 0;
            s_touchLastY = ty[0];
            s_touchDrag = true;
            drawAll();
            s_lastDraw = now;
        } else if (dy < -SWIPE_THRESH) {
            int maxScroll = s_textLineCount - linesPerPage;
            if (maxScroll < 0) maxScroll = 0;
            if (s_textScroll < maxScroll) s_textScroll += step;
            if (s_textScroll > maxScroll) s_textScroll = maxScroll;
            s_touchLastY = ty[0];
            s_touchDrag = true;
            drawAll();
            s_lastDraw = now;
        }
    }

    if (!touching && s_touchWas && s_view == UV_FILES) {
        int16_t totDy = abs((int)ty[0] - (int)s_touchStartY);
        uint32_t dur = now - s_touchDownMs;
        if (!s_touchDrag && dur < TAP_MAX_MS && totDy < TAP_MAX_DIST) {
            int yAtTouch = s_touchStartY;
            if (yAtTouch >= LIST_TOP && yAtTouch < LIST_BOTTOM) {
                int row = (yAtTouch - LIST_TOP) / ROW_H;
                int idx = s_scroll + row;
                int atRoot = (strcmp(s_path, "/") == 0);
                int totalRows = s_entryCount + (atRoot ? 0 : 1);
                if (idx >= 0 && idx < totalRows) {
                    if (!atRoot && idx == 0) pathUp();
                    else {
                        const UsEntry &e = s_entries[idx - (atRoot ? 0 : 1)];
                        if (e.isDir) {
                            pathEnter(e.name);
                        } else if (isTextFile(e.name)) {
                            char full[512];
                            if (strcmp(s_path, "/") == 0) snprintf(full, sizeof(full), "/%s", e.name);
                            else                          snprintf(full, sizeof(full), "%s/%s", s_path, e.name);
                            if (openTextFile(full, e.name)) s_view = UV_TEXT;
                        }
                    }
                    drawAll();
                    s_lastDraw = now;
                }
            }
        }
    }
    s_touchWas = touching;

    // Periodic counter refresh while host is busy
    if (now - s_lastStatus > 500) {
        s_lastStatus = now;
        static uint64_t lastIn = 0, lastOut = 0;
        if (s_bytesIn != lastIn || s_bytesOut != lastOut) {
            lastIn = s_bytesIn; lastOut = s_bytesOut;
            drawAll();
            s_lastDraw = now;
        }
    }

    delay(10);
}

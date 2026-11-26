// Top (external) screen drawing target — included ONCE from src/main.cpp, right
// after the display objects. The cone, ZONES and HEALTH views draw through `tg`
// in a 240x180 layout, between topFrameStart() and topFramePresent():
//
//   * ST7789V build (default): `tg` is the 240x180 M5Canvas `topCanvas`, pushed
//     to the Waveshare 2" panel scaled x4/3 (pushRotateZoom), as before.
//   * ScreenDriver build (-DEXT_SCREENDRIVER=1): `tg` is a TopGfx adapter for the
//     XIAO ESP32-C6 + 2.8" ILI9341 (RemoteDisplay `ext`, 320x240), with the
//     coordinates scaled x4/3 so the layout is unchanged. Areas (rects,
//     horizontal/vertical lines) map to the pixels they cover on 320x240, so the
//     row-by-row cone fill has no gaps. Text uses the C6's GLCD font (the same 6x8
//     font) at native size, about the same physical size on the 2.8" panel;
//     textWidth() returns its width in 240x180 units.
//
//     Only what changed is sent. A frame's calls are recorded into a list; at
//     topFramePresent() the screen is split into 8 bands of 30 rows, each band
//     gets a hash of the calls that touch it, and only bands whose hash changed
//     are redrawn (clipped to the band, the frame's calls replayed). The C6 then
//     flushes just those rows. Redrawing and flushing the whole 320x240 screen
//     8 times a second made the picture jump 1-2 px sideways now and then.
//     After a C6 reboot or a lost command the library calls onRedraw, which calls
//     tg.invalidate(): the next frame sends every band. If a frame does not fit
//     in the list, it is drawn in full (direct mode) and the next one too.
#ifndef TOP_GFX_H
#define TOP_GFX_H

#if EXT_SCREENDRIVER

class TopGfx {
 public:
  explicit TopGfx(RemoteDisplay& d) : d_(d) {}

  int width() const { return 240; }
  int height() const { return 180; }
  static constexpr uint16_t color565(uint8_t r, uint8_t g, uint8_t b) { return RemoteDisplay::color565(r, g, b); }

  // ---- drawing (240x180 coordinates; recorded in 320x240 coordinates)
  void fillSprite(uint16_t c) {
    Op o = mk(OP_SCREEN, 0, 0, SD_SCREEN_W, SD_SCREEN_H);
    o.col = c;
    add(o);
  }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    const int x0 = sx(x), y0 = sx(y), x1 = sx(x + w), y1 = sx(y + h);
    if (x1 <= x0 || y1 <= y0) return;
    Op o = mk(OP_FILL, x0, y0, x1 - x0, y1 - y0);
    o.col = c;
    add(o);
  }
  void drawFastHLine(int x, int y, int w, uint16_t c) { fillRect(x, y, w, 1, c); }
  void drawFastVLine(int x, int y, int h, uint16_t c) { fillRect(x, y, 1, h, c); }
  void drawRect(int x, int y, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    fillRect(x, y, w, 1, c);
    fillRect(x, y + h - 1, w, 1, c);
    fillRect(x, y, 1, h, c);
    fillRect(x + w - 1, y, 1, h, c);
  }
  void drawLine(int x0, int y0, int x1, int y1, uint16_t c) {
    const int X0 = px(x0), Y0 = px(y0), X1 = px(x1), Y1 = px(y1);
    Op o = {};
    o.kind = OP_LINE;
    o.a = X0; o.b = Y0; o.c = X1; o.d = Y1;
    o.col = c;
    o.x0 = min(X0, X1) - 1; o.y0 = min(Y0, Y1) - 1;
    o.x1 = max(X0, X1) + 2; o.y1 = max(Y0, Y1) + 2;
    add(o);
  }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) { roundRect(OP_FROUND, x, y, w, h, r, c); }
  void drawRoundRect(int x, int y, int w, int h, int r, uint16_t c) { roundRect(OP_DROUND, x, y, w, h, r, c); }

  void setTextSize(uint8_t s) { size_ = s ? s : 1; }
  void setTextColor(uint16_t fg) { fg_ = fg; hasBg_ = false; }
  void setTextColor(uint16_t fg, uint16_t bg) { fg_ = fg; bg_ = bg; hasBg_ = true; }
  // Width in 240x180 units of the GLCD text as drawn on 320x240 (6 px per char x size).
  int textWidth(const char* s) const { return ((int)strlen(s) * 6 * size_ * 3 + 3) / 4; }
  // Same top-left as the canvas text; the smaller glyphs are centred in the old line height.
  void drawString(const char* s, int x, int y) {
    int len = (int)strlen(s);
    if (len == 0) return;
    if (len > kMaxText) len = kMaxText;
    const int X = sx(x), Y = sx(y) + size_;
    Op o = mk(OP_TEXT, X - 1, Y - 1, len * 6 * size_ + 2, 8 * size_ + 2);   // bbox with a 1 px margin
    o.a = X; o.b = Y;
    o.col = fg_; o.bg = bg_; o.hasBg = hasBg_; o.size = size_; o.len = (uint8_t)len;
    add(o, s);
  }

  // ---- frames
  void frameStart() {
    n_ = 0;
    strUsed_ = 0;
    direct_ = false;
    size_ = 1;
    fg_ = TFT_WHITE; bg_ = TFT_BLACK; hasBg_ = false;
    fillSprite(TFT_BLACK);
  }
  void framePresent() {
    if (direct_) {                    // the list overflowed: this frame was drawn in full
      d_.endFrame();
      invalidate();
      return;
    }
    d_.startFrame();                  // after a recovery this runs onRedraw -> invalidate()
    uint32_t h[kBands];
    for (int b = 0; b < kBands; ++b) h[b] = 2166136261u;
    for (int i = 0; i < n_; ++i) {
      Op o = ops_[i];
      o.str = 0;                      // the pool offset moves when earlier text changes: hash the text instead
      uint32_t oh = 2166136261u;
      const uint8_t* p = (const uint8_t*)&o;
      for (size_t k = 0; k < sizeof(Op); ++k) oh = (oh ^ p[k]) * 16777619u;
      if (o.kind == OP_TEXT)
        for (int k = 0; k < o.len; ++k) oh = (oh ^ (uint8_t)pool_[ops_[i].str + k]) * 16777619u;
      for (int b = bandOf(o.y0); b <= bandOf(o.y1 - 1); ++b) h[b] = (h[b] ^ oh) * 16777619u;
    }
    bool clipped = false;
    for (int b = 0; b < kBands;) {
      h[b] |= 1;                      // never 0: 0 = "not sent"
      if (h[b] == prev_[b]) { ++b; continue; }
      int e = b;                      // merge a run of changed bands into one clip rect
      while (e + 1 < kBands && (h[e + 1] | 1) != prev_[e + 1]) { ++e; h[e] |= 1; }
      d_.setClipRect(0, b * kBandH, SD_SCREEN_W, (e - b + 1) * kBandH);
      clipped = true;
      replay(b * kBandH, (e + 1) * kBandH);
      b = e + 1;
    }
    if (clipped) d_.clearClipRect();
    memcpy(prev_, h, sizeof(prev_));
    d_.endFrame();                    // nothing changed: an empty frame (keeps the link checks)
  }
  void invalidate() { memset(prev_, 0, sizeof(prev_)); }

 private:
  enum : uint8_t { OP_SCREEN, OP_FILL, OP_LINE, OP_FROUND, OP_DROUND, OP_TEXT };
  // 28 bytes, no padding (hashed as raw bytes). Coordinates are 320x240.
  struct Op {
    int16_t a, b, c, d, e;            // x, y, w, h, r  (line: x0, y0, x1, y1; text: x, y)
    uint16_t col, bg, str;            // colour, text background, text offset in pool_
    int16_t x0, y0, x1, y1;           // bounding box, end exclusive
    uint8_t kind, size, hasBg, len;
  };
  static_assert(sizeof(Op) == 28, "Op is hashed as raw bytes: keep it free of padding");
  static const int kMaxOps = 1000;    // HEALTH with 6 nodes is ~700
  static const int kPool = 3072;
  static const int kMaxText = 80;
  static const int kBands = 8, kBandH = SD_SCREEN_H / kBands;   // 30 rows

  static int sx(int v) { return v * 4 / 3; }          // area edge on 320x240
  static int px(int v) { return (v * 4 + 2) / 3; }    // pixel centre on 320x240
  static int bandOf(int y) { return y < 0 ? 0 : (y >= SD_SCREEN_H ? kBands - 1 : y / kBandH); }

  static Op mk(uint8_t kind, int x, int y, int w, int h) {
    Op o = {};
    o.kind = kind;
    o.a = x; o.b = y; o.c = w; o.d = h;
    o.x0 = x; o.y0 = y; o.x1 = x + w; o.y1 = y + h;
    return o;
  }
  void roundRect(uint8_t kind, int x, int y, int w, int h, int r, uint16_t c) {
    const int X = sx(x), Y = sx(y);
    Op o = mk(kind, X, Y, sx(x + w) - X, sx(y + h) - Y);
    o.e = sx(r);
    o.col = c;
    add(o);
  }

  void add(Op o, const char* s = nullptr) {
    if (!direct_ && (n_ >= kMaxOps || (s && strUsed_ + o.len > kPool))) goDirect();
    if (direct_) { send(o, s); return; }
    if (s) {
      memcpy(pool_ + strUsed_, s, o.len);
      o.str = (uint16_t)strUsed_;
      strUsed_ += o.len;
    }
    ops_[n_++] = o;
  }

  // The list is full: draw what is recorded now and send the rest of the frame directly.
  void goDirect() {
    d_.startFrame();
    d_.clearClipRect();
    for (int i = 0; i < n_; ++i) send(ops_[i], pool_ + ops_[i].str);
    direct_ = true;
  }

  // Replays the frame's calls that touch rows [y0, y1) (the clip rect is set by the caller).
  void replay(int y0, int y1) {
    for (int i = 0; i < n_; ++i) {
      const Op& o = ops_[i];
      if (o.y1 > y0 && o.y0 < y1) send(o, pool_ + o.str);
    }
  }

  void send(const Op& o, const char* s) {
    switch (o.kind) {
      case OP_SCREEN: d_.fillScreen(o.col); break;    // respects the clip rect on the C6
      case OP_FILL:   d_.fillRect(o.a, o.b, o.c, o.d, o.col); break;
      case OP_LINE:   d_.drawLine(o.a, o.b, o.c, o.d, o.col); break;
      case OP_FROUND: d_.fillRoundRect(o.a, o.b, o.c, o.d, o.e, o.col); break;
      case OP_DROUND: d_.drawRoundRect(o.a, o.b, o.c, o.d, o.e, o.col); break;
      case OP_TEXT: {
        char buf[kMaxText + 1];
        memcpy(buf, s, o.len);
        buf[o.len] = '\0';
        // Style every time: the library sends it lazily and re-sends it after a C6 reboot.
        d_.setFont(RD_FONT_GLCD);
        d_.setTextDatum(0);
        d_.setTextSize(o.size);
        if (o.hasBg) d_.setTextColor(o.col, o.bg);
        else         d_.setTextColor(o.col);
        d_.drawString(buf, o.a, o.b);
        break;
      }
    }
  }

  RemoteDisplay& d_;
  Op ops_[kMaxOps];
  char pool_[kPool];
  int n_ = 0, strUsed_ = 0;
  bool direct_ = false;
  uint32_t prev_[kBands] = {};
  uint8_t size_ = 1;
  uint16_t fg_ = TFT_WHITE, bg_ = TFT_BLACK;
  bool hasBg_ = false;
};

static TopGfx tg(ext);

static void topFrameStart()   { tg.frameStart(); }
static void topFramePresent() { tg.framePresent(); }

#else  // ST7789V: draw into the 240x180 canvas, scale it onto the panel

static M5Canvas& tg = topCanvas;
static void applyExtBrightness();     // main.cpp (soft-dim fallback)

static void topFrameStart() { tg.fillSprite(TFT_BLACK); }
static void topFramePresent() {
  applyExtBrightness();
  topCanvas.pushRotateZoom(160, 120, 0.0f, EXT_ZOOM, EXT_ZOOM);   // 240x180 * 4/3 = 320x240
}

#endif
#endif  // TOP_GFX_H

// MODULE: image
// Purpose : inline image rendering — stb_image decode, 2×2 block quantization, 24-bit color output
// Exports : is_image_ext() cat_image() | imgpush_cell() (used by video.h)
// Depends : common.h, stb_image.h (must be included before this via zcmd.cpp)

static bool is_image_ext(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext==".jpg"||ext==".jpeg"||ext==".png"||ext==".bmp"||ext==".gif"||ext==".tga"||ext==".psd";
}

struct imgpixel { uint8_t r, g, b; };

static imgpixel imgsample(const uint8_t* img, int w, int x, int y) {
    const uint8_t* p = img + (y * w + x) * 3;
    return { p[0], p[1], p[2] };
}

static char* imgwrite_u8(char* p, uint8_t v) {
    if (v >= 100) { *p++ = '0' + v/100; *p++ = '0' + (v/10)%10; }
    else if (v >= 10) { *p++ = '0' + v/10; }
    *p++ = '0' + v%10;
    return p;
}

static constexpr const char* imgquad[16] = {
    " ","\xE2\x96\x98","\xE2\x96\x9D","\xE2\x96\x80",
    "\xE2\x96\x96","\xE2\x96\x8C","\xE2\x96\x9E","\xE2\x96\x9B",
    "\xE2\x96\x97","\xE2\x96\x9A","\xE2\x96\x90","\xE2\x96\x9C",
    "\xE2\x96\x84","\xE2\x96\x99","\xE2\x96\x9F","\xE2\x96\x88",
};

static float imgluma(imgpixel p) { return 0.299f*p.r + 0.587f*p.g + 0.114f*p.b; }
static float imgdist2(imgpixel a, imgpixel b) {
    float dr=a.r-b.r, dg=a.g-b.g, db=a.b-b.b;
    return dr*dr + dg*dg + db*db;
}

// Encodes a 2×2 pixel block as a Unicode block character with fg/bg 24-bit colors.
static char* imgpush_cell(char* p, imgpixel tl, imgpixel tr, imgpixel bl, imgpixel br) {
    imgpixel px[4] = { tl, tr, bl, br };
    float lu[4] = { imgluma(px[0]), imgluma(px[1]), imgluma(px[2]), imgluma(px[3]) };
    int lo=0, hi=0;
    for (int i=1; i<4; ++i) {
        if (lu[i] < lu[lo]) lo = i;
        if (lu[i] > lu[hi]) hi = i;
    }
    imgpixel c0=px[lo], c1=px[hi];
    int mask = 0;
    for (int iter=0; iter<3; ++iter) {
        mask = 0;
        for (int i=0; i<4; ++i)
            if (imgdist2(px[i], c1) < imgdist2(px[i], c0))
                mask |= (1 << i);
        int r0=0,g0=0,b0=0,n0=0, r1=0,g1=0,b1=0,n1=0;
        for (int i=0; i<4; ++i) {
            if (mask & (1<<i)) { r1+=px[i].r; g1+=px[i].g; b1+=px[i].b; ++n1; }
            else               { r0+=px[i].r; g0+=px[i].g; b0+=px[i].b; ++n0; }
        }
        if (n0) c0 = { (uint8_t)(r0/n0), (uint8_t)(g0/n0), (uint8_t)(b0/n0) };
        if (n1) c1 = { (uint8_t)(r1/n1), (uint8_t)(g1/n1), (uint8_t)(b1/n1) };
    }
    *p++='\033'; *p++='['; *p++='3'; *p++='8'; *p++=';'; *p++='2'; *p++=';';
    p=imgwrite_u8(p,c1.r); *p++=';'; p=imgwrite_u8(p,c1.g); *p++=';'; p=imgwrite_u8(p,c1.b); *p++='m';
    *p++='\033'; *p++='['; *p++='4'; *p++='8'; *p++=';'; *p++='2'; *p++=';';
    p=imgwrite_u8(p,c0.r); *p++=';'; p=imgwrite_u8(p,c0.g); *p++=';'; p=imgwrite_u8(p,c0.b); *p++='m';
    for (const char* g=imgquad[mask]; *g;) *p++=*g++;
    return p;
}

static int cat_image(const std::string& path) {
    int img_w, img_h;
    uint8_t* img = stbi_load(path.c_str(), &img_w, &img_h, nullptr, 3);
    if (!img) { out("cat: cannot load image '" + path + "'\r\n"); return 1; }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int term_w = 80, term_h = 24;
    if (GetConsoleScreenBufferInfo(out_h, &csbi)) {
        term_w = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        term_h = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    }
    --term_h;

    int out_w = term_w;
    int out_h2 = (int)((double)img_h / img_w * out_w / 2.0 + 0.5);
    if (out_h2 > term_h) {
        out_h2 = term_h;
        out_w = (int)((double)img_w / img_h * out_h2 * 2.0 + 0.5);
    }
    if (out_w < 1) out_w = 1;
    if (out_h2 < 1) out_h2 = 1;

    std::vector<char> buf((size_t)out_h2 * (out_w * 41 + 6));
    char* p = buf.data();

    const double x_scale = (double)img_w / (out_w * 2);
    const double y_scale = (double)img_h / (out_h2 * 2);

    for (int row=0; row<out_h2; ++row) {
        int y0 = (int)((2*row)   * y_scale); if (y0 >= img_h) y0 = img_h-1;
        int y1 = (int)((2*row+1) * y_scale); if (y1 >= img_h) y1 = img_h-1;
        for (int col=0; col<out_w; ++col) {
            int x0 = (int)((2*col)   * x_scale); if (x0 >= img_w) x0 = img_w-1;
            int x1 = (int)((2*col+1) * x_scale); if (x1 >= img_w) x1 = img_w-1;
            p = imgpush_cell(p,
                imgsample(img, img_w, x0, y0), imgsample(img, img_w, x1, y0),
                imgsample(img, img_w, x0, y1), imgsample(img, img_w, x1, y1));
        }
        *p++='\033'; *p++='['; *p++='0'; *p++='m'; *p++='\r'; *p++='\n';
    }

    DWORD written;
    WriteConsoleA(out_h, buf.data(), (DWORD)(p - buf.data()), &written, nullptr);
    stbi_image_free(img);
    return 0;
}

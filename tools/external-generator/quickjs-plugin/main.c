/*
 * Optional standalone QuickJS external generator plugin for v-simple-editor (Plan B).
 * NOT linked into v-simple-editor.exe (0 bytes added to main application binary).
 *
 * Protocol: --stdio-rgba
 *   stdin : one UTF-8 JSON line:
 *           {"entry":"...","frame":0,"fps":30.0,"width":1280,"height":720,"params":{...}}
 *   stdout: 16-byte header ("VGEN" + u32le width + u32le height + u32le byteLen)
 *           followed by width*height*4 raw RGBA8888 pixels.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "quickjs.h"

typedef struct {
    uint8_t *rgba;
    int width;
    int height;
} CanvasBuf;

static CanvasBuf g_canvas = { NULL, 0, 0 };

static JSValue js_canvas_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int32_t r = 0, g = 0, b = 0, a = 0;
    if (argc >= 4) {
        JS_ToInt32(ctx, &r, argv[0]);
        JS_ToInt32(ctx, &g, argv[1]);
        JS_ToInt32(ctx, &b, argv[2]);
        JS_ToInt32(ctx, &a, argv[3]);
    }
    size_t n = (size_t)g_canvas.width * (size_t)g_canvas.height;
    for (size_t i = 0; i < n; ++i) {
        g_canvas.rgba[i * 4 + 0] = (uint8_t)(r & 0xFF);
        g_canvas.rgba[i * 4 + 1] = (uint8_t)(g & 0xFF);
        g_canvas.rgba[i * 4 + 2] = (uint8_t)(b & 0xFF);
        g_canvas.rgba[i * 4 + 3] = (uint8_t)(a & 0xFF);
    }
    return JS_UNDEFINED;
}

static JSValue js_canvas_fill_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 8) return JS_UNDEFINED;
    int32_t x = 0, y = 0, w = 0, h = 0, r = 255, g = 255, b = 255, a = 255;
    JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]); JS_ToInt32(ctx, &h, argv[3]);
    JS_ToInt32(ctx, &r, argv[4]); JS_ToInt32(ctx, &g, argv[5]);
    JS_ToInt32(ctx, &b, argv[6]); JS_ToInt32(ctx, &a, argv[7]);

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_canvas.width ? g_canvas.width : x + w;
    int y1 = y + h > g_canvas.height ? g_canvas.height : y + h;
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            size_t idx = ((size_t)py * (size_t)g_canvas.width + (size_t)px) * 4;
            g_canvas.rgba[idx + 0] = (uint8_t)(r & 0xFF);
            g_canvas.rgba[idx + 1] = (uint8_t)(g & 0xFF);
            g_canvas.rgba[idx + 2] = (uint8_t)(b & 0xFF);
            g_canvas.rgba[idx + 3] = (uint8_t)(a & 0xFF);
        }
    }
    return JS_UNDEFINED;
}

static void write_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static char *read_entire_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) *len_out = got;
    return buf;
}

int main(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    char line[65536];
    if (!fgets(line, sizeof(line), stdin)) {
        fprintf(stderr, "vse-quickjs-gen: empty stdin\n");
        return 1;
    }

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue req = JS_ParseJSON(ctx, line, strlen(line), "<stdin>");
    if (JS_IsException(req)) {
        fprintf(stderr, "vse-quickjs-gen: invalid request JSON\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 2;
    }

    JSValue vEntry = JS_GetPropertyStr(ctx, req, "entry");
    JSValue vFrame = JS_GetPropertyStr(ctx, req, "frame");
    JSValue vFps = JS_GetPropertyStr(ctx, req, "fps");
    JSValue vWidth = JS_GetPropertyStr(ctx, req, "width");
    JSValue vHeight = JS_GetPropertyStr(ctx, req, "height");
    JSValue vParams = JS_GetPropertyStr(ctx, req, "params");

    const char *entry = JS_ToCString(ctx, vEntry);
    int32_t frame = 0, width = 640, height = 360;
    double fps = 30.0;
    JS_ToInt32(ctx, &frame, vFrame);
    JS_ToFloat64(ctx, &fps, vFps);
    JS_ToInt32(ctx, &width, vWidth);
    JS_ToInt32(ctx, &height, vHeight);

    size_t scriptLen = 0;
    char *script = entry ? read_entire_file(entry, &scriptLen) : NULL;
    if (!script) {
        fprintf(stderr, "vse-quickjs-gen: cannot read entry script\n");
        return 3;
    }

    g_canvas.width = width > 0 ? width : 640;
    g_canvas.height = height > 0 ? height : 360;
    uint32_t byteLen = (uint32_t)(g_canvas.width * g_canvas.height * 4);
    g_canvas.rgba = (uint8_t *)calloc(1, byteLen);

    JSValue evalRes = JS_Eval(ctx, script, scriptLen, entry, JS_EVAL_TYPE_GLOBAL);
    free(script);
    if (JS_IsException(evalRes)) {
        fprintf(stderr, "vse-quickjs-gen: script evaluation failed\n");
        return 4;
    }
    JS_FreeValue(ctx, evalRes);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue renderFn = JS_GetPropertyStr(ctx, global, "renderFrame");
    if (JS_IsFunction(ctx, renderFn)) {
        JSValue canvasObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, canvasObj, "width", JS_NewInt32(ctx, g_canvas.width));
        JS_SetPropertyStr(ctx, canvasObj, "height", JS_NewInt32(ctx, g_canvas.height));
        JS_SetPropertyStr(ctx, canvasObj, "clear", JS_NewCFunction(ctx, js_canvas_clear, "clear", 4));
        JS_SetPropertyStr(ctx, canvasObj, "fillRect", JS_NewCFunction(ctx, js_canvas_fill_rect, "fillRect", 8));

        JSValue argv[4] = { canvasObj, JS_NewInt32(ctx, frame), JS_NewFloat64(ctx, fps), vParams };
        JSValue callRes = JS_Call(ctx, renderFn, global, 4, argv);
        JS_FreeValue(ctx, callRes);
        JS_FreeValue(ctx, canvasObj);
    }

    uint8_t hdr[16];
    hdr[0] = 'V'; hdr[1] = 'G'; hdr[2] = 'E'; hdr[3] = 'N';
    write_u32_le(hdr + 4, (uint32_t)g_canvas.width);
    write_u32_le(hdr + 8, (uint32_t)g_canvas.height);
    write_u32_le(hdr + 12, byteLen);
    fwrite(hdr, 1, 16, stdout);
    fwrite(g_canvas.rgba, 1, byteLen, stdout);
    fflush(stdout);

    free(g_canvas.rgba);
    JS_FreeCString(ctx, entry);
    JS_FreeValue(ctx, renderFn);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, vEntry); JS_FreeValue(ctx, vFrame); JS_FreeValue(ctx, vFps);
    JS_FreeValue(ctx, vWidth); JS_FreeValue(ctx, vHeight); JS_FreeValue(ctx, vParams);
    JS_FreeValue(ctx, req);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}

#include "engine.h"
#include <math.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HWND window;
static HDC window_dc;
static HDC frame_dc;
static HBITMAP frame_bitmap;
static HGDIOBJ original_bitmap;
static int frame_width, frame_height;
static bool closing;
static bool keys[CGF_KEY_COUNT];
static bool pressed[CGF_KEY_COUNT];
static LARGE_INTEGER frequency, last_time;

static void log_message(const char *message) {
  puts(message);
  fflush(stdout);
}

static int map_key(WPARAM key) {
  switch (key) {
  case VK_LEFT:
    return CGF_KEY_LEFT;
  case VK_RIGHT:
    return CGF_KEY_RIGHT;
  case VK_UP:
    return CGF_KEY_UP;
  case VK_DOWN:
    return CGF_KEY_DOWN;
  case VK_F5:
    return CGF_KEY_F5;
  case VK_F6:
    return CGF_KEY_F6;
  case 'W':
    return CGF_KEY_W;
  case 'A':
    return CGF_KEY_A;
  case 'S':
    return CGF_KEY_S;
  case 'D':
    return CGF_KEY_D;
  case 'Q':
    return CGF_KEY_Q;
  case 'E':
    return CGF_KEY_E;
  case VK_SHIFT:
    return CGF_KEY_SHIFT;
  case 'P':
    return CGF_KEY_P;
  default:
    return -1;
  }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
  switch (message) {
  case WM_CLOSE:
    closing = true;
    return 0;
  case WM_DESTROY:
    closing = true;
    return 0;
  case WM_KILLFOCUS:
    ZeroMemory(keys, sizeof(keys));
    ZeroMemory(pressed, sizeof(pressed));
    return 0;
  case WM_KEYDOWN:
  case WM_KEYUP: {
    int key = map_key(wparam);
    bool down = message == WM_KEYDOWN;
    if (wparam == VK_ESCAPE && down)
      closing = true;
    if (key >= 0) {
      if (down && !keys[key])
        pressed[key] = true;
      keys[key] = down;
    }
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    /* The window is presented every frame (by GDI or by the renderer), so
       painting only validates the region. */
    PAINTSTRUCT paint;
    BeginPaint(hwnd, &paint);
    EndPaint(hwnd, &paint);
    return 0;
  }
  default:
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

static BYTE color_channel(float value) {
  if (!isfinite(value) || value < 0.0f)
    return 0;
  if (value > 1.0f)
    return 255;
  return (BYTE)(value * 255.0f);
}

static bool key_down(CGF_Key key) {
  return key >= 0 && key < CGF_KEY_COUNT && keys[key];
}

static void draw_rect(float x, float y, float w, float h, float r, float g,
                      float b) {
  if (!frame_dc)
    return;
  RECT rect = {(LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h)};
  HBRUSH brush = CreateSolidBrush(
      RGB(color_channel(r), color_channel(g), color_channel(b)));
  if (brush) {
    FillRect(frame_dc, &rect, brush);
    DeleteObject(brush);
  }
}

/* Text colors are packed as 0xRRGGBB. */
static void draw_text(const char *text, int x, int y, int size,
                      uint32_t color) {
  if (!frame_dc || !text || size <= 0)
    return;
  HFONT font =
      CreateFontA(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
  if (!font)
    return;
  HGDIOBJ previous = SelectObject(frame_dc, font);
  SetBkMode(frame_dc, TRANSPARENT);
  SetTextColor(frame_dc,
               RGB((color >> 16) & 255, (color >> 8) & 255, color & 255));
  TextOutA(frame_dc, x, y, text, lstrlenA(text));
  SelectObject(frame_dc, previous);
  DeleteObject(font);
}

static void window_size(uint32_t *width, uint32_t *height) {
  RECT rect = {0};
  if (!window || IsIconic(window) || !GetClientRect(window, &rect))
    rect = (RECT){0};
  if (width)
    *width = (uint32_t)(rect.right - rect.left);
  if (height)
    *height = (uint32_t)(rect.bottom - rect.top);
}

static void native_window(void **instance, void **hwnd) {
  if (instance)
    *instance = window ? (void *)GetModuleHandleW(NULL) : NULL;
  if (hwnd)
    *hwnd = (void *)window;
}

bool engine_register_api(const CGF_APIRegistry *registry) {
  static const CGF_EngineAPI api = {
      .struct_size = sizeof(CGF_EngineAPI),
      .log = log_message,
      .key_down = key_down,
      .draw_rect = draw_rect,
      .draw_text = draw_text,
      .window_size = window_size,
      .native_window = native_window,
  };
  return registry && registry->set(CGF_ENGINE_API_NAME, sizeof(api), &api);
}

void engine_reset_clock(void) {
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&last_time);
}

float engine_delta_time(void) {
  LARGE_INTEGER now;
  if (!frequency.QuadPart)
    engine_reset_clock();
  QueryPerformanceCounter(&now);
  double dt =
      (double)(now.QuadPart - last_time.QuadPart) / (double)frequency.QuadPart;
  last_time = now;
  /* Avoid a large simulation step after a debugger pause or window drag. */
  return (float)(dt > 0.1 ? 0.1 : dt);
}

bool engine_open_window(int width, int height, const char *title) {
  if (window || width <= 0 || height <= 0)
    return false;
  HINSTANCE instance = GetModuleHandleW(NULL);
  WNDCLASSW wc = {0};
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.lpszClassName = L"CGFWindow";
  wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return false;

  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT bounds = {0, 0, width, height};
  if (!AdjustWindowRect(&bounds, style, FALSE))
    return false;
  closing = false;
  ZeroMemory(keys, sizeof(keys));
  ZeroMemory(pressed, sizeof(pressed));
  window =
      CreateWindowExW(0, wc.lpszClassName, L"Chibi Game Framework", style,
                      CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                      bounds.bottom - bounds.top, NULL, NULL, instance, NULL);
  if (!window)
    return false;
  SetWindowTextA(window, title);
  window_dc = GetDC(window);
  if (!window_dc)
    goto fail;
  frame_dc = CreateCompatibleDC(window_dc);
  frame_bitmap = CreateCompatibleBitmap(window_dc, width, height);
  if (!frame_dc || !frame_bitmap)
    goto fail;
  original_bitmap = SelectObject(frame_dc, frame_bitmap);
  if (!original_bitmap || original_bitmap == HGDI_ERROR)
    goto fail;
  frame_width = width;
  frame_height = height;
  engine_begin_frame(0, 0, 0);
  engine_reset_clock();
  ShowWindow(window, SW_SHOW);
  return true;
fail:
  engine_close_window();
  return false;
}

bool engine_should_close(void) {
  ZeroMemory(pressed, sizeof(pressed));
  MSG message;
  while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
    if (message.message == WM_QUIT)
      closing = true;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return closing;
}

bool engine_key_pressed(CGF_Key key) {
  return key >= 0 && key < CGF_KEY_COUNT && pressed[key];
}

void engine_begin_frame(float r, float g, float b) {
  draw_rect(0, 0, (float)frame_width, (float)frame_height, r, g, b);
}

void engine_end_frame(void) {
  if (window_dc && frame_dc)
    BitBlt(window_dc, 0, 0, frame_width, frame_height, frame_dc, 0, 0, SRCCOPY);
}

void engine_close_window(void) {
  if (frame_dc && original_bitmap && original_bitmap != HGDI_ERROR)
    SelectObject(frame_dc, original_bitmap);
  if (frame_bitmap)
    DeleteObject(frame_bitmap);
  if (frame_dc)
    DeleteDC(frame_dc);
  if (window_dc && window)
    ReleaseDC(window, window_dc);
  if (window)
    DestroyWindow(window);
  window = NULL;
  window_dc = frame_dc = NULL;
  frame_bitmap = NULL;
  original_bitmap = NULL;
  closing = true;
}

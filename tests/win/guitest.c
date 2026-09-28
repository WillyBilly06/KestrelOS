/* guitest - a graphical Windows program.
 *
 * A window, a class, a procedure and a message loop: the shape every Windows
 * program with a user interface has.  What makes this checkable rather than
 * merely watchable is that it reads its own pixels back after painting them,
 * so it can say whether the drawing actually landed rather than leaving a
 * human to squint at a screenshot.
 */
#include "winapi.h"

static int  painted;
static int  timer_ticks;
static HWND window;

/* The colours are picked to be unmistakable when read back. */
#define BACKGROUND RGB(24, 32, 48)
#define BOX        RGB(220, 60, 60)
#define DISC       RGB(60, 200, 120)
#define LINE       RGB(250, 220, 40)

static void verify_drawing(HDC dc) {
    check(GetPixel(dc, 5, 5) == BACKGROUND, "the background is where it was painted");
    check(GetPixel(dc, 60, 60) == BOX, "the rectangle filled its area");
    check(GetPixel(dc, 5, 60) != BOX, "and did not fill outside it");
    check(GetPixel(dc, 220, 60) == DISC, "the ellipse filled its middle");
    check(GetPixel(dc, 160, 61) != DISC, "and left its corner alone");
    check(GetPixel(dc, 100, 160) == LINE, "the line was drawn");
}

static void paint(HDC dc, const RECT *client) {
    HBRUSH background = CreateSolidBrush(BACKGROUND);
    FillRect(dc, client, background);
    DeleteObject(background);

    /* A filled rectangle, drawn with a brush and no outline. */
    HBRUSH box = CreateSolidBrush(BOX);
    HPEN edge = CreatePen(0, 1, BOX);
    HGDIOBJ old_brush = SelectObject(dc, box);
    HGDIOBJ old_pen = SelectObject(dc, edge);
    Rectangle(dc, 30, 30, 120, 100);

    /* An ellipse in a different colour, to prove the shape is filled rather
     * than merely outlined. */
    HBRUSH disc = CreateSolidBrush(DISC);
    SelectObject(dc, disc);
    HPEN disc_edge = CreatePen(0, 1, DISC);
    SelectObject(dc, disc_edge);
    Ellipse(dc, 160, 30, 280, 100);

    /* A line across the middle. */
    HPEN line = CreatePen(0, 3, LINE);
    SelectObject(dc, line);
    MoveToEx(dc, 30, 160, NULL);
    LineTo(dc, 300, 160);

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(box);
    DeleteObject(disc);
    DeleteObject(edge);
    DeleteObject(disc_edge);
    DeleteObject(line);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 240));
    TextOutA(dc, 30, 200, "A Windows program, running on KestrelOS.", -1);

    SIZE extent;
    GetTextExtentPoint32A(dc, "measure me", -1, &extent);
    if (!painted) {
        check(extent.cx > 0 && extent.cy > 0, "text can be measured");
        check(extent.cx > extent.cy, "and \"measure me\" is wider than it is tall");
    }
}

static LRESULT CALLBACK window_proc(HWND h, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT client;
        GetClientRect(h, &client);
        paint(dc, &client);
        if (!painted) {
            printf(" checking what was drawn\n");
            verify_drawing(dc);
            painted = 1;
        }
        EndPaint(h, &ps);
        return 0;
    }

    case WM_TIMER:
        /* Two ticks is enough to show the loop is turning; then it leaves. */
        if (++timer_ticks >= 2) PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, message, wp, lp);
}

int main(void) {
    printf("guitest: a graphical program\n");

    WNDCLASSEXA cls;
    memset(&cls, 0, sizeof cls);
    cls.cbSize = sizeof cls;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.hCursor = LoadCursorA(NULL, (const char *)32512);
    cls.lpszClassName = "KestrelGuiTest";
    check(RegisterClassExA(&cls) != 0, "the window class was registered");

    window = CreateWindowExA(0, "KestrelGuiTest", "guitest", WS_OVERLAPPEDWINDOW,
                             80, 60, 360, 240, NULL, NULL, cls.hInstance, NULL);
    check(window != NULL, "the window was created");

    RECT client;
    check(GetClientRect(window, &client), "GetClientRect answered");
    check(client.right == 360 && client.bottom == 240, "with the size that was asked for");
    if (client.right != 360) printf("       got %dx%d\n", (int)client.right, (int)client.bottom);

    check(GetSystemMetrics(0) > 0, "the screen has a width");

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetTimer(window, 1, 1500, NULL);

    printf(" running the message loop\n");
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    check(painted, "the window received WM_PAINT");
    check(timer_ticks >= 2, "the timer fired and the loop kept turning");
    check(msg.message == WM_QUIT, "the loop ended on WM_QUIT");

    DestroyWindow(window);
    return report("guitest");
}

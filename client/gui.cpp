/*
 * gui.cpp — Dear ImGui + SDL2 frontend for Lateralus (Windows).
 *
 * macOS uses gui_macos.m (native AppKit).
 * Linux  uses gui_gtk.c  (native GTK3).
 * Windows uses this file (SDL2 + ImGui — no native Win32 dep needed,
 *          Direct3D is selected automatically by SDL_Renderer).
 *
 * Style: pure black-on-white (light) or white-on-black (dark).
 *        Dark mode is detected from the Windows registry at startup.
 */

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef _WIN32
#  include <windows.h>
#  include <shlobj.h>   /* SHGetFolderPathA */
#endif

extern "C" {
#include "../common/vc_api.h"
}

/* ── Engine thread ──────────────────────────────────────────────────────── */

struct EngineArgs { char host[256], room[64], user[64], psk[256]; };

static pthread_t    s_engine_thread;
static volatile int s_engine_active = 0;
static EngineArgs   s_args;

static void *engine_thread_fn(void *arg)
{
    EngineArgs *a = (EngineArgs *)arg;
    vc_run(a->host, a->room, a->user, a->psk);
    s_engine_active = 0;
    return NULL;
}

static void engine_start()
{
    s_engine_active = 1;
    pthread_create(&s_engine_thread, NULL, engine_thread_fn, &s_args);
}

static void engine_stop()
{
    if (s_engine_active) {
        vc_quit();
        pthread_join(s_engine_thread, NULL);
        s_engine_active = 0;
    }
}

/* ── Settings persistence ───────────────────────────────────────────────── */

static char s_cfg_path[512];

static void cfg_init()
{
#ifdef _WIN32
    char appdata[MAX_PATH] = "";
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata);
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\voicechat", appdata);
    CreateDirectoryA(dir, NULL);
    snprintf(s_cfg_path, sizeof s_cfg_path, "%s\\settings", dir);
#else
    const char *home = getenv("HOME");
    if (home) snprintf(s_cfg_path, sizeof s_cfg_path,
                       "%s/.config/voicechat/settings", home);
    else      snprintf(s_cfg_path, sizeof s_cfg_path, "/tmp/vc-settings");
#endif
}

static void cfg_load(char *srv, size_t srv_sz,
                     char *room, size_t room_sz,
                     char *user, size_t user_sz)
{
    FILE *f = fopen(s_cfg_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "server=", 7) == 0) snprintf(srv,  srv_sz,  "%s", line+7);
        if (strncmp(line, "room=",   5) == 0) snprintf(room, room_sz, "%s", line+5);
        if (strncmp(line, "user=",   5) == 0) snprintf(user, user_sz, "%s", line+5);
    }
    fclose(f);
}

static void cfg_save(const char *srv, const char *room, const char *user)
{
    FILE *f = fopen(s_cfg_path, "w");
    if (!f) return;
    fprintf(f, "server=%s\nroom=%s\nuser=%s\n", srv, room, user);
    fclose(f);
}

/* ── UI state ───────────────────────────────────────────────────────────── */

static char g_host[256]        = "";
static char g_room[64]         = "general";
static char g_user[64]         = "";
static char g_psk[256]         = "";
static char s_shown_error[256] = "";

/* Update button state */
static bool s_updating       = false;
static bool s_update_done    = false;
static bool s_update_ok      = false;
static char s_update_msg[128]= "";
static pthread_mutex_t s_upd_lock = PTHREAD_MUTEX_INITIALIZER;

static void *update_bg(void *)
{
    int rc = do_update();
    pthread_mutex_lock(&s_upd_lock);
    s_update_ok   = (rc == 0);
    s_update_done = true;
    snprintf(s_update_msg, sizeof s_update_msg,
             rc == 0 ? "Updated! Restart to apply." : "Update failed.");
    s_updating = false;
    pthread_mutex_unlock(&s_upd_lock);
    return NULL;
}

/* ── Dark mode detection ────────────────────────────────────────────────── */

static bool g_dark = false;

#ifdef _WIN32
static bool detect_dark_mode()
{
    DWORD val = 1, sz = sizeof(val);
    RegGetValueA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        "AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &val, &sz);
    return val == 0; /* 0 = dark mode */
}
#endif

/* ── Pure 2-color style ─────────────────────────────────────────────────── */

static void apply_style(bool dark)
{
    ImGuiStyle &st = ImGui::GetStyle();
    ImVec4 *c = st.Colors;

    st.WindowRounding   = st.FrameRounding = st.GrabRounding = 2.0f;
    st.WindowPadding    = {12, 12};
    st.FramePadding     = {6,  4};
    st.ItemSpacing      = {8,  6};
    st.ScrollbarSize    = 12.0f;
    st.WindowBorderSize = 0.0f;
    st.FrameBorderSize  = 1.0f;

    ImVec4 bg  = dark ? ImVec4{0.00f, 0.00f, 0.00f, 1.0f}
                      : ImVec4{1.00f, 1.00f, 1.00f, 1.0f};
    ImVec4 fg  = dark ? ImVec4{1.00f, 1.00f, 1.00f, 1.0f}
                      : ImVec4{0.00f, 0.00f, 0.00f, 1.0f};
    ImVec4 hov = dark ? ImVec4{0.12f, 0.12f, 0.12f, 1.0f}
                      : ImVec4{0.88f, 0.88f, 0.88f, 1.0f};
    ImVec4 brd = {0.50f, 0.50f, 0.50f, 1.0f};

    for (int i = 0; i < ImGuiCol_COUNT; i++) c[i] = bg;

    c[ImGuiCol_Text]                 = fg;
    c[ImGuiCol_TextDisabled]         = brd;
    c[ImGuiCol_WindowBg]             = bg;
    c[ImGuiCol_ChildBg]              = bg;
    c[ImGuiCol_PopupBg]              = bg;
    c[ImGuiCol_Border]               = brd;
    c[ImGuiCol_BorderShadow]         = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg]              = bg;
    c[ImGuiCol_FrameBgHovered]       = hov;
    c[ImGuiCol_FrameBgActive]        = hov;
    c[ImGuiCol_TitleBg]              = bg;
    c[ImGuiCol_TitleBgActive]        = bg;
    c[ImGuiCol_TitleBgCollapsed]     = bg;
    c[ImGuiCol_MenuBarBg]            = bg;
    c[ImGuiCol_ScrollbarBg]          = bg;
    c[ImGuiCol_ScrollbarGrab]        = brd;
    c[ImGuiCol_ScrollbarGrabHovered] = fg;
    c[ImGuiCol_ScrollbarGrabActive]  = fg;
    c[ImGuiCol_CheckMark]            = fg;
    c[ImGuiCol_SliderGrab]           = fg;
    c[ImGuiCol_SliderGrabActive]     = fg;
    c[ImGuiCol_Button]               = bg;
    c[ImGuiCol_ButtonHovered]        = hov;
    c[ImGuiCol_ButtonActive]         = hov;
    c[ImGuiCol_Header]               = hov;
    c[ImGuiCol_HeaderHovered]        = hov;
    c[ImGuiCol_HeaderActive]         = hov;
    c[ImGuiCol_Separator]            = brd;
    c[ImGuiCol_SeparatorHovered]     = brd;
    c[ImGuiCol_SeparatorActive]      = fg;
    c[ImGuiCol_ResizeGrip]           = {0, 0, 0, 0};
    c[ImGuiCol_ResizeGripHovered]    = brd;
    c[ImGuiCol_ResizeGripActive]     = fg;
    c[ImGuiCol_NavHighlight]         = fg;
    c[ImGuiCol_TextSelectedBg]       = hov;
    c[ImGuiCol_ModalWindowDimBg]     = {0, 0, 0, 0.5f};
}

/* ── Peer row ───────────────────────────────────────────────────────────── */

static void peer_row(const char *name, bool speaking, bool self, bool muted,
                     const char *path_tag, int jb_ms, int jb_tgt_ms)
{
    const char *dot = speaking ? "\xe2\x97\x8f" : "\xe2\x97\x8b"; /* ● / ○ */
    if (self) {
        ImGui::Text("%s  %s  %s", dot, name, muted ? "(you, muted)" : "(you)");
    } else {
        ImGui::Text("%s  %s  [%s]  jb=%d/%dms",
                    dot, name, path_tag, jb_ms, jb_tgt_ms);
    }
}

/* ── Connect form ───────────────────────────────────────────────────────── */

static void render_connect(float win_w)
{
    /* Version stamp — top-left, faint */
#ifndef VC_COMMIT_COUNT
#  define VC_COMMIT_COUNT "0"
#  define VC_COMMIT_HASH  "dev"
#endif
    ImGui::SetCursorPos({4.0f, 4.0f});
    ImGui::PushStyleColor(ImGuiCol_Text, g_dark
        ? ImVec4{1,1,1,0.18f} : ImVec4{0,0,0,0.18f});
    ImGui::Text("#" VC_COMMIT_COUNT "  " VC_COMMIT_HASH);
    ImGui::PopStyleColor();

    /* Title */
    ImGui::SetCursorPosY(24.0f);
    const char *title = "Lateralus";
    float tw = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX((win_w - tw) * 0.5f);
    ImGui::Text("%s", title);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
    ImGui::Separator();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);

    const float lw = 68.0f;
    const float iw = win_w - lw - 28.0f;

    struct { const char *label, *id; char *buf; size_t sz;
             ImGuiInputTextFlags fl; } rows[] = {
        { "Server", "##host", g_host, sizeof g_host, 0 },
        { "Room",   "##room", g_room, sizeof g_room, 0 },
        { "Name",   "##user", g_user, sizeof g_user, 0 },
        { "PSK",    "##psk",  g_psk,  sizeof g_psk,
          ImGuiInputTextFlags_Password },
    };
    for (auto &row : rows) {
        ImGui::Text("%s", row.label);
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(iw);
        ImGui::InputText(row.id, row.buf, row.sz, row.fl);
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0f);

    /* Connect button */
    const float bw = 110.0f;
    ImGui::SetCursorPosX((win_w - bw) * 0.5f);
    bool go = ImGui::Button("Connect", {bw, 0});
    go |= ImGui::IsKeyPressed(ImGuiKey_Enter);

    if (go && g_host[0] && g_user[0]) {
        s_shown_error[0] = '\0';
        cfg_save(g_host, g_room, g_user);
        snprintf(s_args.host, sizeof s_args.host, "%s", g_host);
        snprintf(s_args.room, sizeof s_args.room, "%s",
                 g_room[0] ? g_room : "general");
        snprintf(s_args.user, sizeof s_args.user, "%s", g_user);
        snprintf(s_args.psk,  sizeof s_args.psk,  "%s", g_psk);
        engine_start();
    }

    /* Update button */
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
    ImGui::SetCursorPosX((win_w - bw) * 0.5f);

    pthread_mutex_lock(&s_upd_lock);
    bool upd_busy = s_updating;
    bool upd_done = s_update_done;
    bool upd_ok   = s_update_ok;
    pthread_mutex_unlock(&s_upd_lock);

    const char *upd_label = upd_busy ? "Updating\xe2\x80\xa6"
                          : upd_done ? (upd_ok ? "\xe2\x9c\x93 Restart to apply" : "Update failed")
                          : "Check for Updates";

    if (upd_busy) ImGui::BeginDisabled();
    if (ImGui::Button(upd_label, {bw, 0}) && !upd_busy && !upd_done) {
        s_updating = true;
        pthread_t t; pthread_create(&t, NULL, update_bg, NULL); pthread_detach(t);
    }
    if (upd_busy) ImGui::EndDisabled();

    if (s_shown_error[0]) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
        float ew = ImGui::CalcTextSize(s_shown_error).x;
        ImGui::SetCursorPosX((win_w - ew) * 0.5f);
        ImGui::Text("%s", s_shown_error);
    }
    if (upd_done && s_update_msg[0]) {
        float mw = ImGui::CalcTextSize(s_update_msg).x;
        ImGui::SetCursorPosX((win_w - mw) * 0.5f);
        ImGui::Text("%s", s_update_msg);
    }

    /* Debug log checkbox — centered at bottom */
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
    bool dbg = vc_get_debug() != 0;
    float cbw = ImGui::CalcTextSize("Debug logs").x + ImGui::GetFrameHeight() + 4.0f;
    ImGui::SetCursorPosX((win_w - cbw) * 0.5f);
    if (ImGui::Checkbox("Debug logs", &dbg))
        vc_set_debug(dbg ? 1 : 0);
}

/* ── Room view ──────────────────────────────────────────────────────────── */

static void render_room(float win_w, float win_h)
{
    bool connected = (vc_is_connected() != 0);
    bool muted     = (vc_get_muted()    != 0);

    if (connected)
        ImGui::Text("# %s", vc_room_name());
    else
        ImGui::Text("  Connecting to %s\xe2\x80\xa6", s_args.host);
    ImGui::Separator();

    float list_h = win_h - 84.0f;
    ImGui::BeginChild("##peers", {0, list_h}, true);

    if (connected) {
        peer_row(vc_username(), false, true, muted, "", 0, 0);
        vc_peer_snapshot_t peers[32];
        int n = vc_snapshot_peers(peers, 32);
        for (int i = 0; i < n; i++) {
            auto &p = peers[i];
            const char *path = p.direct_ok    ? "P2P"  :
                               p.direct_known ? "~P2P" : "relay";
            peer_row(p.name, p.speaking != 0, false, false,
                     path, p.jb_ms, p.jb_target_ms);
        }
        if (n == 0)
            ImGui::TextDisabled("  Waiting for others to join\xe2\x80\xa6");
    } else {
        ImGui::TextDisabled("  Connecting\xe2\x80\xa6");
    }
    ImGui::EndChild();

    ImGui::Separator();

    if (ImGui::Button(muted ? "Unmute [M]" : "Mute [M]",
                      {win_w * 0.46f - 12.0f, 0}))
        vc_set_muted(!muted);

    ImGui::SameLine();
    if (ImGui::Button("Disconnect", {-1.0f, 0})) engine_stop();
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--update") == 0)
            return do_update();
        if (strcmp(argv[i], "--token") == 0 && i + 1 < argc)
            return save_token(argv[i + 1]);
    }

    cfg_init();

#ifdef _WIN32
    g_dark = detect_dark_mode();
#endif

    /* Load persisted connection settings */
    cfg_load(g_host, sizeof g_host, g_room, sizeof g_room, g_user, sizeof g_user);

    /* Default username */
    if (!g_user[0]) {
        const char *u = getenv("USER");
        if (!u || !u[0]) u = getenv("USERNAME");
        if (u && u[0]) snprintf(g_user, sizeof g_user, "%s", u);
        else           snprintf(g_user, sizeof g_user, "user");
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

    SDL_Window *window = SDL_CreateWindow(
        "Lateralus",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        380, 520,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io   = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = NULL;

    /* Try to load Segoe UI (Windows system font) at a crisp size */
    bool font_loaded = false;
    const char *font_paths[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
    };
    for (auto *fp : font_paths) {
        if (FILE *f = fopen(fp, "rb")) {
            fclose(f);
            io.Fonts->AddFontFromFileTTF(fp, 15.0f);
            font_loaded = true;
            break;
        }
    }
    if (!font_loaded) {
        ImFontConfig cfg;
        cfg.SizePixels = 15.0f;
        io.Fonts->AddFontDefault(&cfg);
    }

    apply_style(g_dark);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    Uint8 clear = g_dark ? 0 : 255;
    bool prev_active = false;

    for (bool running = true; running; ) {

        /* Block up to 8 ms for next event — avoids busy-spin on Windows too */
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 8)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window))
                running = false;
        }
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        /* M key mute toggle (room view, not typing) */
        if (s_engine_active && !io.WantCaptureKeyboard) {
            static bool m_prev = false;
            bool m_now = (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_M] != 0);
            if (m_now && !m_prev) vc_set_muted(!vc_get_muted());
            m_prev = m_now;
        }

        /* Detect engine stop */
        bool cur_active = (s_engine_active != 0);
        if (prev_active && !cur_active) {
            const char *err = vc_last_error();
            if (err && err[0])
                snprintf(s_shown_error, sizeof s_shown_error, "%s", err);
        }
        prev_active = cur_active;

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##root", NULL,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (!s_engine_active)
            render_connect(io.DisplaySize.x);
        else
            render_room(io.DisplaySize.x, io.DisplaySize.y);

        ImGui::End();

        ImGui::Render();
        SDL_RenderSetScale(renderer,
                           io.DisplayFramebufferScale.x,
                           io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, clear, clear, clear, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    engine_stop();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

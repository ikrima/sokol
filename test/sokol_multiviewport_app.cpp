#include "imgui.h"
#include "imgui_internal.h"

#include <Windows.h>
#ifndef SOKOL_LOG
#define SOKOL_LOG(s) OutputDebugStringA(s)
#define SOKOL_NO_DEPRECATED
#define SOKOL_WIN32_FORCE_MAIN
#define SOKOL_TRACE_HOOKS
#define SOKOL_DEBUG
#define SOKOL_D3D11
#endif
#define SOKOL_IMPL
#define SOKOL_IMGUI_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_time.h"
#include "util/sokol_imgui.h"



#define SOKOL_GFX_IMGUI_IMPL
#include "util/sokol_gfx_imgui.h"

static bool       show_quit_dialog = false;
static sg_context main_sg_ctx;
static sg_imgui_t sg_imgui;


void smv_appviewport_init();
void smv_appviewport_updatemonitors();
void smv_appviewport_draw();

void smv_apphost_init()
{
  // setup sokol-gfx and sokol-time
  main_sg_ctx = sg_setup(&(sg_desc{
    .gl_force_gles2               = sapp_gles2(),
    .mtl_device                   = sapp_metal_get_device(),
    .mtl_renderpass_descriptor_cb = sapp_metal_get_renderpass_descriptor,
    .mtl_drawable_cb              = sapp_metal_get_drawable,
    .d3d11_device                 = sapp_d3d11_get_device(),
    .d3d11_device_context         = sapp_d3d11_get_device_context(),
    .d3d11_render_target_view_cb =
      [] { return sapp_d3d11_get_render_target_view_window(sapp_window{.id = (uint32_t)(uintptr_t) sg_active_context_userdata()}); },
    .d3d11_depth_stencil_view_cb =
      [] { return sapp_d3d11_get_depth_stencil_view_window(sapp_window{.id = (uint32_t)(uintptr_t) sg_active_context_userdata()}); },
    .context_userdata = (void*) (uintptr_t) sapp_main_window().id,
  }));
  stm_setup();

  sg_imgui_init(&sg_imgui);
  simgui_setup(&(simgui_desc_t{
    .sample_count    = 4,
    .dpi_scale       = sapp_dpi_scale(),
    .no_default_font = false,
  }));

  {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;      // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;    // FIXME-DPI: THIS CURRENTLY DOESN'T WORK AS EXPECTED. DON'T USE IN USER APP!
    io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;    // FIXME-DPI
    io.ConfigWindowsMoveFromTitleBarOnly = false;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;    // Enable Docking
    io.ConfigDockingWithShift = false;
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;    // We can honor GetMouseCursor() values (optional)
    io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;     // We can honor io.WantSetMousePos requests (optional, rarely used)
    io.BackendPlatformName = "sokol-app";
    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    ImGuiStyle& style                 = ImGui::GetStyle();
    style.WindowRounding              = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  }

  smv_appviewport_init();
}

static void update_mouse_pos()
{
  const float dpi_scale = _simgui.desc.dpi_scale;
  ImGuiIO*    io        = &ImGui::GetIO();

#if TEMPDISABLE
  // Set OS mouse position if requested (rarely used, only when ImGuiConfigFlags_NavEnableSetMousePos is enabled by user)
  // (When multi-viewports are enabled, all imgui positions are same as OS positions)
  if (io.WantSetMousePos) {
    POINT pos = {(int) io.MousePos.x, (int) io.MousePos.y};
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0) ::ClientToScreen(g_hWnd, &pos);
    ::SetCursorPos(pos.x, pos.y);
  }
#endif

  io->MousePos             = ImVec2(-FLT_MAX, -FLT_MAX);
  io->MouseHoveredViewport = 0;

  // Set imgui mouse position
  POINT mouse_screen_pos;

  if (!::GetCursorPos(&mouse_screen_pos)) return;

  HWND mainHwnd = (HWND) sapp_win32_get_hwnd();
  if (HWND focused_hwnd = ::GetForegroundWindow()) {
    if (::IsChild(focused_hwnd, mainHwnd)) focused_hwnd = mainHwnd;

    // Multi-viewport mode: mouse position in OS absolute coordinates (io.MousePos is (0,0) when the mouse is on the upper-left of the
    // primary monitor) This is the position you can get with GetCursorPos(). In theory adding viewport->Pos is also the reverse operation
    // of doing ScreenToClient().
    if (_simgui_find_viewport_by_platformhandleraw((void*) (uintptr_t)(focused_hwnd)) != NULL)
      io->MousePos = ImVec2(float(mouse_screen_pos.x / dpi_scale), float(mouse_screen_pos.y / dpi_scale));
  }

  // (Optional) When using multiple viewports: set io.MouseHoveredViewport to the viewport the OS mouse cursor is hovering.
  // Important: this information is not easy to provide and many high-level windowing library won't be able to provide it correctly, because
  // - This is _ignoring_ viewports with the ImGuiViewportFlags_NoInputs flag (pass-through windows).
  // - This is _regardless_ of whether another viewport is focused or being dragged from.
  // If ImGuiBackendFlags_HasMouseHoveredViewport is not set by the back-end, imgui will ignore this field and infer the information by
  // relying on the rectangles and last focused time of every viewports it knows about. It will be unaware of foreign windows that may be
  // sitting between or over your windows.
  if (HWND hovered_hwnd = ::WindowFromPoint(mouse_screen_pos))
    if (ImGuiViewport* viewport = _simgui_find_viewport_by_platformhandleraw((void*) hovered_hwnd))
      if ((viewport->Flags & ImGuiViewportFlags_NoInputs)
          == 0)    // FIXME: We still get our NoInputs window with WM_NCHITTEST/HTTRANSPARENT code when decorated?
        io->MouseHoveredViewport = viewport->ID;
}

void smv_apphost_tick()
{
  static uint64_t last_time = 0;

  const int    main_width  = sapp_width();
  const int    main_height = sapp_height();
  const double delta_time  = stm_sec(stm_laptime(&last_time));

  smv_appviewport_updatemonitors();
  update_mouse_pos();
  simgui_new_frame(main_width, main_height, delta_time);


  /* Draw Sokol Debug Toolbar*/ {
    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("sokol-gfx")) {
        ImGui::MenuItem("Capabilities", 0, &sg_imgui.caps.open);
        ImGui::MenuItem("Buffers", 0, &sg_imgui.buffers.open);
        ImGui::MenuItem("Images", 0, &sg_imgui.images.open);
        ImGui::MenuItem("Shaders", 0, &sg_imgui.shaders.open);
        ImGui::MenuItem("Pipelines", 0, &sg_imgui.pipelines.open);
        ImGui::MenuItem("Passes", 0, &sg_imgui.passes.open);
        ImGui::MenuItem("Calls", 0, &sg_imgui.capture.open);
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }
    sg_imgui_draw(&sg_imgui);
  }



  static ImGuiDockNodeFlags dockspaceFlags   = ImGuiDockNodeFlags_None;
  static bool               bShowAppMetrics  = false;
  static bool               bShowDemoWindow  = false;
  static bool               bShowStyleEditor = false;
  {
    // Create MainWindowHost that everything will dock to
    constexpr bool opt_fullscreen = true;

    // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
    // because it would be confusing to have two docking targets within each others.
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    if (opt_fullscreen) {
      ImGuiViewport* viewport = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(viewport->Pos);
      ImGui::SetNextWindowSize(viewport->Size);
      ImGui::SetNextWindowViewport(viewport->ID);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
      window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    }

    // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
    // and handle the pass-thru hole, so we ask Begin() to not render a background.
    if (dockspaceFlags & ImGuiDockNodeFlags_PassthruCentralNode) window_flags |= ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainWindowHost", nullptr, window_flags);
    {
      ImGui::PopStyleVar();

      if (opt_fullscreen) ImGui::PopStyleVar(2);

      // DockSpace
      {
        ImGuiID dockspace_id = ImGui::GetID("MainWindowHostDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspaceFlags);
      }

      if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
          if (ImGui::MenuItem("New")) {
          }
          if (ImGui::MenuItem("Open", "Ctrl+O")) {
          }
          if (ImGui::MenuItem("Save", "Ctrl+S")) {
          }
          if (ImGui::MenuItem("Close")) {
          }
          ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
      }

      if (bShowAppMetrics) {
        ImGui::ShowMetricsWindow(&bShowAppMetrics);
      }
      if (bShowDemoWindow) {
        ImGui::ShowDemoWindow(&bShowDemoWindow);
      }
      if (bShowStyleEditor) {
        if (ImGui::Begin("Style Editor", &bShowStyleEditor)) {
          ImGui::ShowStyleEditor();
        }
        ImGui::End();
      }
    }
    ImGui::End();
  }


  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Debug")) {
      ImGui::MenuItem("Metrics", NULL, &bShowAppMetrics);
      ImGui::MenuItem("DemoWindow", NULL, &bShowDemoWindow);
      ImGui::MenuItem("StyleEditor", NULL, &bShowStyleEditor);
      ImGui::Separator();

      if (ImGui::MenuItem("Flag: NoSplit", "", (dockspaceFlags & ImGuiDockNodeFlags_NoSplit) != 0)) {
        dockspaceFlags ^= ImGuiDockNodeFlags_NoSplit;
      }
      if (ImGui::MenuItem("Flag: NoResize", "", (dockspaceFlags & ImGuiDockNodeFlags_NoResize) != 0)) {
        dockspaceFlags ^= ImGuiDockNodeFlags_NoResize;
      }
      if (ImGui::MenuItem("Flag: NoDockingInCentralNode", "", (dockspaceFlags & ImGuiDockNodeFlags_NoDockingInCentralNode) != 0)) {
        dockspaceFlags ^= ImGuiDockNodeFlags_NoDockingInCentralNode;
      }
      if (ImGui::MenuItem("Flag: PassthruCentralNode", "", (dockspaceFlags & ImGuiDockNodeFlags_PassthruCentralNode) != 0)) {
        dockspaceFlags ^= ImGuiDockNodeFlags_PassthruCentralNode;
      }
      if (ImGui::MenuItem("Flag: AutoHideTabBar", "", (dockspaceFlags & ImGuiDockNodeFlags_AutoHideTabBar) != 0)) {
        dockspaceFlags ^= ImGuiDockNodeFlags_AutoHideTabBar;
      }
      ImGui::Separator();

      ImGui::EndMenu();
    }



    ImGui::Text("%05.2fms (%03.0fFPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    ImGui::EndMainMenuBar();
  }



  if (show_quit_dialog) {
    sapp_quit();
    ImGui::OpenPopup("Really Quit?");
    show_quit_dialog = false;
  }
  if (ImGui::BeginPopupModal("Really Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Do you really want to quit?\n");
    ImGui::Separator();
    if (ImGui::Button("OK", ImVec2(120, 0))) {
      sapp_quit();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  smv_appviewport_draw();
}

void smv_apphost_shutdown()
{
  simgui_shutdown();
  sg_imgui_discard(&sg_imgui);
  sg_shutdown();
}

void smv_apphost_input(const sapp_event* evt)
{
  if (evt->type == SAPP_EVENTTYPE_QUIT_REQUESTED && evt->window.id == sapp_main_window().id) {
    show_quit_dialog = true;
    sapp_cancel_quit();
  }
  else {
    simgui_handle_event(evt);
  }
}

sapp_desc sokol_main(int argc, char* argv[])
{
  return sapp_desc{
    .init_cb                     = smv_apphost_init,
    .frame_cb                    = smv_apphost_tick,
    .cleanup_cb                  = smv_apphost_shutdown,
    .event_cb                    = smv_apphost_input,
    .width                       = 1280,
    .height                      = 800,
    .high_dpi                    = true,
    .fullscreen                  = false,
    .window_title                = "Tolva: AnimTool",
    .ios_keyboard_resizes_canvas = false,
    .gl_force_gles2              = false,
    .num_windows                 = 32,
  };
}





struct ImGuiSokolViewportData {
  sapp_window w;
  sg_context  c;
  DWORD       dwStyle   = 0;
  DWORD       dwExStyle = 0;

  HWND GetHwnd() const
  {
    HWND ret = (HWND) sapp_win32_get_hwnd_window(w);
    IM_ASSERT(ret != 0);
    return ret;
  }
};


static void smv_appviewport_wndwgetstyle(ImGuiViewportFlags flags, DWORD* out_style, DWORD* out_ex_style)
{
  if (flags & ImGuiViewportFlags_NoDecoration)
    *out_style = WS_POPUP;
  else
    *out_style = WS_OVERLAPPEDWINDOW;

  if (flags & ImGuiViewportFlags_NoTaskBarIcon)
    *out_ex_style = WS_EX_TOOLWINDOW;
  else
    *out_ex_style = WS_EX_APPWINDOW;

  if (flags & ImGuiViewportFlags_TopMost) *out_ex_style |= WS_EX_TOPMOST;
}

void smv_appviewport_updatemonitors()
{
  static bool g_WantUpdateMonitors = true;

  auto smv_appviewport_updatemonitors_enum = [](HMONITOR monitor, HDC, LPRECT, LPARAM) {
    MONITORINFO info = {0};
    info.cbSize      = sizeof(MONITORINFO);
    if (!::GetMonitorInfo(monitor, &info)) return TRUE;
    ImGuiPlatformMonitor imgui_monitor;
    imgui_monitor.MainPos = ImVec2((float) info.rcMonitor.left, (float) info.rcMonitor.top);
    imgui_monitor.MainSize =
      ImVec2((float) (info.rcMonitor.right - info.rcMonitor.left), (float) (info.rcMonitor.bottom - info.rcMonitor.top));
    imgui_monitor.WorkPos  = ImVec2((float) info.rcWork.left, (float) info.rcWork.top);
    imgui_monitor.WorkSize = ImVec2((float) (info.rcWork.right - info.rcWork.left), (float) (info.rcWork.bottom - info.rcWork.top));
    imgui_monitor.DpiScale = sapp_dpi_scale();
    ImGuiPlatformIO& io    = ImGui::GetPlatformIO();
    if (info.dwFlags & MONITORINFOF_PRIMARY)
      io.Monitors.push_front(imgui_monitor);
    else
      io.Monitors.push_back(imgui_monitor);
    return TRUE;
  };

  if (g_WantUpdateMonitors) {
    ImGui::GetPlatformIO().Monitors.resize(0);
    ::EnumDisplayMonitors(NULL, NULL, smv_appviewport_updatemonitors_enum, NULL);
    g_WantUpdateMonitors = false;
  }
}

void smv_appviewport_init()
{
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;              // Enable Multi-Viewport / Platform Windows
  io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;       // We can create multi-viewports on the Platform side (optional)
  io.BackendFlags |= ImGuiBackendFlags_HasMouseHoveredViewport;    // We can set io.MouseHoveredViewport correctly (optional, not easy)
  io.BackendRendererName = "sokol-gfx";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;    // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;    // We can create multi-viewports on the Renderer side (optional)
  ImGuiViewport* main_viewport = ImGui::GetMainViewport();

  // Our mouse update function expect PlatformHandle to be filled for the main viewport
  ImGuiSokolViewportData* data     = IM_NEW(ImGuiSokolViewportData)();
  data->w                          = sapp_main_window();
  data->c                          = main_sg_ctx;
  main_viewport->PlatformUserData  = data;
  main_viewport->PlatformHandle    = (void*) (uintptr_t)(data->w.id);
  main_viewport->PlatformHandleRaw = (void*) (uintptr_t)(data->GetHwnd());

  smv_appviewport_updatemonitors();

  // Register platform interface (will be coupled with a renderer interface)
  ImGuiPlatformIO& platform_io      = ImGui::GetPlatformIO();
  platform_io.Platform_CreateWindow = [](ImGuiViewport* viewport) {
    ImGuiSokolViewportData* data    = IM_NEW(ImGuiSokolViewportData)();
    data->w                         = sapp_create_window(&(sapp_window_desc{.window_title = "sokol: not main window"}));
    data->c                         = sg_setup_context(&(sg_context_desc{
      .d3d11_render_target_view_cb =
        [] { return sapp_d3d11_get_render_target_view_window(sapp_window{.id = (uint32_t)(uintptr_t) sg_active_context_userdata()}); },
      .d3d11_depth_stencil_view_cb =
        [] { return sapp_d3d11_get_depth_stencil_view_window(sapp_window{.id = (uint32_t)(uintptr_t) sg_active_context_userdata()}); },
      .context_userdata = (void*) (uintptr_t) data->w.id,
    }));
    viewport->PlatformUserData      = data;
    viewport->PlatformRequestResize = false;
    viewport->PlatformHandle        = (void*) (uintptr_t)(data->w.id);
    viewport->PlatformHandleRaw     = (void*) (uintptr_t)(data->GetHwnd());

    // Select style and parent window
    smv_appviewport_wndwgetstyle(viewport->Flags, &data->dwStyle, &data->dwExStyle);
    // Only reapply the flags that have been changed from our point of view (as other flags are being modified by Windows)
    // if (data->dwStyle != new_style || data->dwExStyle != new_ex_style)
    {
      HWND hwnd = data->GetHwnd();
      ::SetWindowLong(hwnd, GWL_STYLE, data->dwStyle);
      ::SetWindowLong(hwnd, GWL_EXSTYLE, data->dwExStyle);
      RECT rect = {(LONG) viewport->Pos.x, (LONG) viewport->Pos.y, (LONG)(viewport->Pos.x + viewport->Size.x),
        (LONG)(viewport->Pos.y + viewport->Size.y)};
      ::AdjustWindowRectEx(&rect, data->dwStyle, FALSE, data->dwExStyle);    // Client to Screen
      ::SetWindowPos(
        hwnd, NULL, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      ::ShowWindow(hwnd, SW_SHOWNA);    // This is necessary when we alter the style
      viewport->PlatformRequestMove = viewport->PlatformRequestResize = true;
    }
  };
  platform_io.Platform_DestroyWindow = [](ImGuiViewport* viewport) {
    if (ImGuiSokolViewportData* data = (ImGuiSokolViewportData*) viewport->PlatformUserData) {
      sapp_destroy_window(data->w);
      sg_discard_context(data->c);

      if (::GetCapture() == data->GetHwnd()) {
        // Transfer capture so if we started dragging from a window that later disappears, we'll still receive the MOUSEUP event.
        ::ReleaseCapture();
        ::SetCapture(HWND(ImGui::GetMainViewport()->PlatformHandleRaw));
      }
      IM_DELETE(data);
    }

    viewport->PlatformUserData = viewport->PlatformHandle = nullptr;
  };
  platform_io.Platform_ShowWindow = [](ImGuiViewport* viewport) {
    HWND hwnd = ((ImGuiSokolViewportData*) viewport->PlatformUserData)->GetHwnd();
    if (viewport->Flags & ImGuiViewportFlags_NoFocusOnAppearing)
      ::ShowWindow(hwnd, SW_SHOWNA);
    else
      ::ShowWindow(hwnd, SW_SHOW);
  };
  platform_io.Platform_SetWindowPos = [](ImGuiViewport* viewport, ImVec2 pos) {
    ImGuiSokolViewportData* data = (ImGuiSokolViewportData*) viewport->PlatformUserData;
    HWND                    hwnd = data->GetHwnd();
    RECT                    rect = {(LONG) pos.x, (LONG) pos.y, (LONG) pos.x, (LONG) pos.y};
    ::AdjustWindowRectEx(&rect, data->dwStyle, FALSE, data->dwExStyle);
    ::SetWindowPos(hwnd, NULL, rect.left, rect.top, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
  };
  platform_io.Platform_GetWindowPos = [](ImGuiViewport* viewport) {
    HWND  hwnd = ((ImGuiSokolViewportData*) viewport->PlatformUserData)->GetHwnd();
    POINT pos  = {0, 0};
    ::ClientToScreen(hwnd, &pos);
    return ImVec2((float) pos.x, (float) pos.y);
  };
  platform_io.Platform_SetWindowSize = [](ImGuiViewport* viewport, ImVec2 size) {
    ImGuiSokolViewportData* data = (ImGuiSokolViewportData*) viewport->PlatformUserData;
    HWND                    hwnd = data->GetHwnd();
    RECT                    rect = {0, 0, (LONG) size.x, (LONG) size.y};
    ::AdjustWindowRectEx(&rect, data->dwStyle, FALSE, data->dwExStyle);    // Client to Screen
    ::SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
  };
  platform_io.Platform_GetWindowSize = [](ImGuiViewport* viewport) {
    HWND hwnd = ((ImGuiSokolViewportData*) viewport->PlatformUserData)->GetHwnd();
    RECT rect;
    ::GetClientRect(hwnd, &rect);
    return ImVec2(float(rect.right - rect.left), float(rect.bottom - rect.top));
  };
  platform_io.Platform_SetWindowFocus = [](ImGuiViewport* viewport) {
    HWND hwnd = ((ImGuiSokolViewportData*) viewport->PlatformUserData)->GetHwnd();
    ::BringWindowToTop(hwnd);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);
  };
  platform_io.Platform_GetWindowFocus = [](ImGuiViewport* viewport) {
    HWND hwnd = ((ImGuiSokolViewportData*) viewport->PlatformUserData)->GetHwnd();
    return ::GetForegroundWindow() == hwnd;
  };
  platform_io.Platform_GetWindowMinimized = [](ImGuiViewport* viewport) {
    HWND hwnd = ((ImGuiSokolViewportData*) viewport->PlatformUserData)->GetHwnd();
    return ::IsIconic(hwnd) != 0;
  };
  platform_io.Platform_SetWindowTitle = [](ImGuiViewport* viewport, const char* title) {
    // ::SetWindowTextA() doesn't properly handle UTF-8 so we explicitely convert our string.
    HWND              hwnd = ((ImGuiSokolViewportData*) viewport->PlatformUserData)->GetHwnd();
    int               n    = ::MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
    ImVector<wchar_t> title_w;
    title_w.resize(n);
    ::MultiByteToWideChar(CP_UTF8, 0, title, -1, title_w.Data, n);
    ::SetWindowTextW(hwnd, title_w.Data);
  };

  platform_io.Platform_UpdateWindow = [](ImGuiViewport* viewport) {
    // (Optional) Update Win32 style if it changed _after_ creation.
    // Generally they won't change unless configuration flags are changed, but advanced uses (such as manually rewriting viewport flags)
    // make this useful.
    ImGuiSokolViewportData* data = ((ImGuiSokolViewportData*) viewport->PlatformUserData);
    HWND                    hwnd = data->GetHwnd();
    DWORD                   new_style;
    DWORD                   new_ex_style;
    smv_appviewport_wndwgetstyle(viewport->Flags, &new_style, &new_ex_style);

    // Only reapply the flags that have been changed from our point of view (as other flags are being modified by Windows)
    if (data->dwStyle != new_style || data->dwExStyle != new_ex_style) {
      data->dwStyle   = new_style;
      data->dwExStyle = new_ex_style;
      ::SetWindowLong(hwnd, GWL_STYLE, data->dwStyle);
      ::SetWindowLong(hwnd, GWL_EXSTYLE, data->dwExStyle);
      RECT rect = {(LONG) viewport->Pos.x, (LONG) viewport->Pos.y, (LONG)(viewport->Pos.x + viewport->Size.x),
        (LONG)(viewport->Pos.y + viewport->Size.y)};
      ::AdjustWindowRectEx(&rect, data->dwStyle, FALSE, data->dwExStyle);    // Client to Screen
      ::SetWindowPos(
        hwnd, NULL, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      ::ShowWindow(hwnd, SW_SHOWNA);    // This is necessary when we alter the style
      viewport->PlatformRequestMove = viewport->PlatformRequestResize = true;
    }
  };
  platform_io.Platform_SetWindowAlpha = [](ImGuiViewport* viewport, float alpha) {
    HWND hwnd = ((ImGuiSokolViewportData*) viewport->PlatformUserData)->GetHwnd();
    IM_ASSERT(alpha >= 0.0f && alpha <= 1.0f);
    if (alpha < 1.0f) {
      DWORD style = ::GetWindowLongW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED;
      ::SetWindowLongW(hwnd, GWL_EXSTYLE, style);
      ::SetLayeredWindowAttributes(hwnd, 0, (BYTE)(255 * alpha), LWA_ALPHA);
    }
    else {
      DWORD style = ::GetWindowLongW(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED;
      ::SetWindowLongW(hwnd, GWL_EXSTYLE, style);
    }
  };
  platform_io.Platform_GetWindowDpiScale = [](ImGuiViewport* viewport) { return sapp_dpi_scale(); };

  // platform_io.Platform_OnChangedViewport  = ImGui_ImplWin32_OnChangedViewport;    // FIXME-DPI
  // platform_io.Renderer_CreateWindow = ImGui_ImplDX11_CreateWindow;
  // platform_io.Renderer_DestroyWindow = ImGui_ImplDX11_DestroyWindow;
  // platform_io.Renderer_SetWindowSize = ImGui_ImplDX11_SetWindowSize;
  // platform_io.Renderer_RenderWindow = ImGui_ImplDX11_RenderWindow;
  // platform_io.Renderer_SwapBuffers = ImGui_ImplDX11_SwapBuffers;
#if HAS_WIN32_IME
  // platform_io.Platform_SetImeInputPos = ImGui_ImplWin32_SetImeInputPos;
#endif
}

void smv_appviewport_draw()
{
  static sg_pass_action pass_action = sg_pass_action{
    .colors =
      {
        sg_color_attachment_action{
          .action = SG_ACTION_CLEAR,
          .val    = {0.5f, 0.5f, 0.5f, 1.0f},
        },
      },
  };

  ImGui::Render();
  ImGui::UpdatePlatformWindows();
  ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
  for (int i = 0; i < platform_io.Viewports.Size; i++) {
    ImGuiViewport* viewport = platform_io.Viewports[i];
    if (viewport->Flags & ImGuiViewportFlags_Minimized) continue;

    ImGuiSokolViewportData* viewCtx = (ImGuiSokolViewportData*) viewport->PlatformUserData;
    sg_activate_context(viewCtx->c);
    sg_begin_default_pass(&pass_action, viewport->Size.x, viewport->Size.y);
    simgui_render(viewport->DrawData);
    sg_end_pass();
    sg_commit();
  }
}

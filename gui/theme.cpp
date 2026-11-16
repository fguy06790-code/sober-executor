#include "theme.h"
#include "imgui.h"

namespace sober::gui {

// Palette (placeholder — replace with figma tokens when exported)
namespace c {
    constexpr ImVec4 bg      = { 0.07f, 0.07f, 0.09f, 1.0f };
    constexpr ImVec4 panel   = { 0.10f, 0.10f, 0.13f, 1.0f };
    constexpr ImVec4 panel2  = { 0.13f, 0.13f, 0.17f, 1.0f };
    constexpr ImVec4 border  = { 0.18f, 0.18f, 0.22f, 1.0f };
    constexpr ImVec4 text    = { 0.86f, 0.87f, 0.90f, 1.0f };
    constexpr ImVec4 muted   = { 0.52f, 0.54f, 0.60f, 1.0f };
    constexpr ImVec4 accent  = { 0.40f, 0.55f, 0.95f, 1.0f };
    constexpr ImVec4 accent2 = { 0.30f, 0.44f, 0.82f, 1.0f };
    constexpr ImVec4 good    = { 0.35f, 0.80f, 0.45f, 1.0f };
    constexpr ImVec4 bad     = { 0.90f, 0.35f, 0.40f, 1.0f };
    constexpr ImVec4 hilite  = { 0.20f, 0.22f, 0.28f, 1.0f };
}

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 8.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 5.0f;
    s.GrabRounding      = 4.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 4.0f;
    s.TabRounding       = 5.0f;

    s.WindowPadding    = ImVec2(12, 12);
    s.FramePadding     = ImVec2(10, 6);
    s.ItemSpacing      = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing    = 16.0f;
    s.ScrollbarSize    = 10.0f;
    s.GrabMinSize      = 8.0f;

    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize  = 1.0f;
    s.FrameBorderSize  = 0.0f;
    s.TabBorderSize    = 0.0f;

    ImVec4* k = s.Colors;
    k[ImGuiCol_WindowBg]            = c::bg;
    k[ImGuiCol_ChildBg]             = c::panel;
    k[ImGuiCol_PopupBg]             = c::panel2;
    k[ImGuiCol_Border]              = c::border;
    k[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);

    k[ImGuiCol_Text]                = c::text;
    k[ImGuiCol_TextDisabled]        = c::muted;

    k[ImGuiCol_FrameBg]             = c::panel2;
    k[ImGuiCol_FrameBgHovered]      = c::hilite;
    k[ImGuiCol_FrameBgActive]       = c::accent2;

    k[ImGuiCol_TitleBg]             = c::panel;
    k[ImGuiCol_TitleBgActive]       = c::panel;
    k[ImGuiCol_TitleBgCollapsed]    = c::panel;

    k[ImGuiCol_MenuBarBg]           = c::panel;

    k[ImGuiCol_ScrollbarBg]         = c::panel;
    k[ImGuiCol_ScrollbarGrab]       = c::hilite;
    k[ImGuiCol_ScrollbarGrabHovered]= c::border;
    k[ImGuiCol_ScrollbarGrabActive] = c::accent2;

    k[ImGuiCol_CheckMark]           = c::accent;
    k[ImGuiCol_SliderGrab]          = c::accent;
    k[ImGuiCol_SliderGrabActive]    = c::accent2;

    k[ImGuiCol_Button]              = c::panel2;
    k[ImGuiCol_ButtonHovered]       = c::hilite;
    k[ImGuiCol_ButtonActive]        = c::accent2;

    k[ImGuiCol_Header]              = c::panel2;
    k[ImGuiCol_HeaderHovered]       = c::hilite;
    k[ImGuiCol_HeaderActive]        = c::accent2;

    k[ImGuiCol_Separator]           = c::border;
    k[ImGuiCol_SeparatorHovered]    = c::accent;
    k[ImGuiCol_SeparatorActive]     = c::accent;

    k[ImGuiCol_ResizeGrip]          = c::border;
    k[ImGuiCol_ResizeGripHovered]   = c::accent;
    k[ImGuiCol_ResizeGripActive]    = c::accent;

    k[ImGuiCol_Tab]                  = c::panel;
    k[ImGuiCol_TabHovered]           = c::hilite;
    k[ImGuiCol_TabSelected]          = c::panel2;
    k[ImGuiCol_TabDimmed]            = c::panel;
    k[ImGuiCol_TabDimmedSelected]    = c::panel2;

    k[ImGuiCol_PlotLines]           = c::accent;
    k[ImGuiCol_PlotLinesHovered]    = c::accent;
    k[ImGuiCol_PlotHistogram]       = c::accent;
    k[ImGuiCol_PlotHistogramHovered]= c::accent;

    k[ImGuiCol_TextSelectedBg]      = ImVec4(c::accent.x, c::accent.y, c::accent.z, 0.35f);
    k[ImGuiCol_NavHighlight]        = c::accent;
    k[ImGuiCol_ModalWindowDimBg]    = ImVec4(0, 0, 0, 0.55f);
}

} // namespace sober::gui

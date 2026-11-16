#include "app.h"
#include "ipc.h"
#include "theme.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "TextEditor.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace sober::gui {

namespace fs = std::filesystem;

static std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? h : "/tmp";
}

static std::string default_workspace() {
    // Host-visible workspace that we sync to the sandbox-visible path when
    // scripts actually run (file-IO globals in the payload pull from the
    // sandbox-side path).
    std::string base = std::getenv("XDG_DATA_HOME") ? std::getenv("XDG_DATA_HOME")
                                                     : home_dir() + "/.local/share";
    return base + "/sober-exec/workspace";
}

static std::unique_ptr<TextEditor> make_editor() {
    auto ed = std::make_unique<TextEditor>();
    ed->SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    ed->SetShowWhitespaces(false);
    ed->SetTabSize(4);

    TextEditor::Palette pal = TextEditor::GetDarkPalette();
    pal[(int)TextEditor::PaletteIndex::Background] = 0xff1a1a21;
    pal[(int)TextEditor::PaletteIndex::Default]    = 0xffdcdddc;
    pal[(int)TextEditor::PaletteIndex::Keyword]    = 0xfff39b6b;
    pal[(int)TextEditor::PaletteIndex::Number]     = 0xff9bc96b;
    pal[(int)TextEditor::PaletteIndex::String]     = 0xff8fcaf0;
    pal[(int)TextEditor::PaletteIndex::Comment]    = 0xff666876;
    pal[(int)TextEditor::PaletteIndex::Identifier] = 0xffdcdddc;
    pal[(int)TextEditor::PaletteIndex::LineNumber] = 0xff4a4d58;
    ed->SetPalette(pal);
    return ed;
}

static std::string read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream s; s << f.rdbuf();
    return s.str();
}

static void write_text(const std::string& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path);
    f << body;
}

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void log_line(AppState& s, const std::string& line) {
    std::time_t t = std::time(nullptr);
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    s.output_log.push_back(std::string("[") + ts + "] " + line);
    if (s.output_log.size() > 1000) s.output_log.erase(s.output_log.begin(), s.output_log.begin() + 200);
    s.scroll_output_to_bottom = true;
}

// --- tab operations ---------------------------------------------------------

static void new_tab(AppState& s, const std::string& name = "untitled.lua",
                    const std::string& path = "", const std::string& body = "") {
    ScriptTab t;
    t.name = name;
    t.path = path;
    t.editor = make_editor();
    t.editor->SetText(body);
    s.tabs.push_back(std::move(t));
    s.active_tab = (int)s.tabs.size() - 1;
}

static void save_current(AppState& s) {
    if (s.tabs.empty()) return;
    auto& t = s.tabs[s.active_tab];
    if (t.path.empty()) {
        t.path = s.workspace_dir + "/" + t.name;
    }
    write_text(t.path, t.editor->GetText());
    t.dirty = false;
    log_line(s, "saved: " + t.path);
}

static void execute_current(AppState& s) {
    if (s.tabs.empty()) { log_line(s, "no tab"); return; }
    auto& t = s.tabs[s.active_tab];
    std::string src = t.editor->GetText();
    if (src.empty()) { log_line(s, "empty script"); return; }
    std::string err;
    if (send_script(src, err)) {
        log_line(s, "executed [" + t.name + "] (" + std::to_string(src.size()) + " bytes)");
    } else {
        log_line(s, "exec failed: " + err);
    }
}

// --- workspace scan ---------------------------------------------------------

static void scan_workspace(AppState& s) {
    s.workspace_files.clear();
    std::error_code ec;
    fs::create_directories(s.workspace_dir, ec);
    for (const auto& de : fs::directory_iterator(s.workspace_dir, ec)) {
        if (de.is_regular_file()) {
            auto p = de.path();
            auto ext = p.extension().string();
            if (ext == ".lua" || ext == ".luau" || ext == ".txt") {
                s.workspace_files.push_back(p.filename().string());
            }
        }
    }
    std::sort(s.workspace_files.begin(), s.workspace_files.end());
    s.last_workspace_scan = now_s();
}

// --- UI pieces --------------------------------------------------------------

static void draw_menubar(AppState& s) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New tab",  "Ctrl+N")) new_tab(s);
            if (ImGui::MenuItem("Save",     "Ctrl+S")) save_current(s);
            if (ImGui::MenuItem("Execute",  "F5"))     execute_current(s);
            ImGui::Separator();
            if (ImGui::MenuItem("Rescan workspace")) scan_workspace(s);
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) std::exit(0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Executor API",   nullptr, &s.show_api_window);
            ImGui::MenuItem("About",          nullptr, &s.show_about_window);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Options")) {
            ImGui::MenuItem("Auto-exec on attach", nullptr, &s.autoexec_on_attach);
            ImGui::EndMenu();
        }

        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(avail - 140);
        ImVec4 col = s.attached ? ImVec4(0.35f, 0.80f, 0.45f, 1.0f)
                                : ImVec4(0.90f, 0.35f, 0.40f, 1.0f);
        ImGui::TextColored(col, s.attached ? "● attached" : "○ not attached");

        ImGui::EndMenuBar();
    }
}

static void draw_sidebar(AppState& s) {
    ImGui::BeginChild("sidebar", ImVec2(220, 0), true);

    ImGui::TextDisabled("WORKSPACE");
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - 28, 0));
    ImGui::SameLine();
    if (ImGui::SmallButton("⟲")) scan_workspace(s);

    ImGui::Separator();

    for (const auto& name : s.workspace_files) {
        if (ImGui::Selectable(name.c_str())) {
            std::string path = s.workspace_dir + "/" + name;
            new_tab(s, name, path, read_text(path));
        }
    }

    if (s.workspace_files.empty()) {
        ImGui::TextDisabled("(no scripts)");
        ImGui::TextDisabled("%s", s.workspace_dir.c_str());
    }

    ImGui::EndChild();
}

static void draw_editor_area(AppState& s) {
    ImGui::BeginChild("editor_area", ImVec2(0, 0), false);

    if (ImGui::Button("▶ Execute")) execute_current(s);
    ImGui::SameLine();
    if (ImGui::Button("Save")) save_current(s);
    ImGui::SameLine();
    if (ImGui::Button("+ New")) new_tab(s);
    ImGui::SameLine();
    if (ImGui::Button("Clear output")) s.output_log.clear();
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(20, 0));
    ImGui::SameLine();
    ImGui::TextDisabled("socket: %s", socket_path().c_str());

    ImGui::Separator();

    // Editor pane (tabbed)
    float out_h = 170.0f;
    float ed_h  = ImGui::GetContentRegionAvail().y - out_h - ImGui::GetStyle().ItemSpacing.y;

    ImGui::BeginChild("edtabs", ImVec2(0, ed_h), true);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
        for (int i = 0; i < (int)s.tabs.size(); ++i) {
            auto& t = s.tabs[i];
            bool open = true;
            std::string label = (t.dirty ? "● " : "") + t.name + "###tab" + std::to_string(i);
            if (ImGui::BeginTabItem(label.c_str(), &open)) {
                s.active_tab = i;
                ImVec2 sz = ImGui::GetContentRegionAvail();
                t.editor->Render("##ed", sz, false);
                if (t.editor->IsTextChanged()) t.dirty = true;
                ImGui::EndTabItem();
            }
            if (!open) t.pending_close = true;
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
            new_tab(s);
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // Output pane
    ImGui::BeginChild("output", ImVec2(0, out_h), true);
    ImGui::TextDisabled("OUTPUT");
    ImGui::Separator();
    for (const auto& ln : s.output_log) {
        ImGui::TextUnformatted(ln.c_str());
    }
    if (s.scroll_output_to_bottom) {
        ImGui::SetScrollHereY(1.0f);
        s.scroll_output_to_bottom = false;
    }
    ImGui::EndChild();

    ImGui::EndChild();

    // deferred close pass
    for (int i = (int)s.tabs.size() - 1; i >= 0; --i) {
        if (s.tabs[i].pending_close) {
            s.tabs.erase(s.tabs.begin() + i);
            if (s.active_tab >= (int)s.tabs.size()) s.active_tab = (int)s.tabs.size() - 1;
        }
    }
}

static void draw_api_window(AppState& s) {
    if (!s.show_api_window) return;
    ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Executor API", &s.show_api_window)) {
        ImGui::TextWrapped("Bound into the elevated thread globals once the "
                           "payload resolves Luau and a script is executed.");
        ImGui::Separator();
        static const char* fns[] = {
            "loadstring(src[, chunk])",
            "identifyexecutor() / getexecutorname()",
            "getgenv() / getrenv()",
            "checkcaller()",
            "setreadonly(t, b) / isreadonly(t)",
            "getrawmetatable(t) / setrawmetatable(t, mt)",
            "getnamecallmethod() / setnamecallmethod(s)",
            "hookfunction(f, h) / replaceclosure / restorefunction",
            "hookmetamethod(obj, name, h)",
            "newcclosure(f) / iscclosure(f) / islclosure(f)",
            "gethui()",
            "HttpGet(url) / httpget(url)",
            "queue_on_teleport(src) / queueonteleport(src)",
            "writefile(p, b) / readfile(p) / isfile(p) / delfile(p)",
            "getscripts() / getloadedmodules() / getconnections()",
            "getinstances() / getnilinstances()",
            "crypt.base64_encode / base64_decode / generatebytes",
            "base64.encode / base64.decode",
        };
        for (const char* f : fns) ImGui::BulletText("%s", f);
    }
    ImGui::End();
}

static void draw_about_window(AppState& s) {
    if (!s.show_about_window) return;
    ImGui::SetNextWindowSize(ImVec2(360, 160), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About sober-exec", &s.show_about_window)) {
        ImGui::Text("sober-exec 0.1");
        ImGui::TextDisabled("Linux Roblox executor targeting the Sober flatpak.");
        ImGui::Separator();
        ImGui::Text("github.com/fguy06790-code/sober-executor");
        ImGui::TextDisabled("Payload: libsoberpay.so");
        ImGui::TextDisabled("Socket:  %s", socket_path().c_str());
    }
    ImGui::End();
}

static void handle_shortcuts(AppState& s) {
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save_current(s);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) new_tab(s);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) std::exit(0);
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))        execute_current(s);
}

// --- main loop --------------------------------------------------------------

static void glfw_error(int code, const char* msg) {
    std::fprintf(stderr, "[gui] glfw %d: %s\n", code, msg);
}

void run() {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "[gui] glfwInit failed\n");
        return;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* win = glfwCreateWindow(1180, 760, "sober-exec", nullptr, nullptr);
    if (!win) { glfwTerminate(); return; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    apply_theme();

    AppState s;
    s.workspace_dir = default_workspace();
    scan_workspace(s);
    new_tab(s, "scratch.lua", "", "-- sober-exec scratch\nprint(identifyexecutor())\n");

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        if (now_s() - s.last_attach_check > 0.5) {
            s.attached = is_attached();
            s.last_attach_check = now_s();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int ww, wh; glfwGetFramebufferSize(win, &ww, &wh);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)ww, (float)wh));
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoSavedSettings);

        draw_menubar(s);
        handle_shortcuts(s);
        draw_sidebar(s);
        ImGui::SameLine();
        draw_editor_area(s);

        ImGui::End();

        draw_api_window(s);
        draw_about_window(s);

        ImGui::Render();
        int dw, dh; glfwGetFramebufferSize(win, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.03f, 0.03f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
}

} // namespace sober::gui

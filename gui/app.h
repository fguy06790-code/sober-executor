#pragma once

#include <string>
#include <vector>
#include <memory>

class TextEditor;

namespace sober::gui {

struct ScriptTab {
    std::string name;
    std::string path;
    std::unique_ptr<TextEditor> editor;
    bool dirty = false;
    bool pending_close = false;
};

struct AppState {
    std::vector<ScriptTab> tabs;
    int active_tab = 0;

    std::vector<std::string> output_log;  // most recent last
    bool scroll_output_to_bottom = true;

    std::string workspace_dir;            // host-side scripts folder
    std::vector<std::string> workspace_files;
    double last_workspace_scan = 0.0;

    bool show_api_window   = false;
    bool show_about_window = false;
    bool autoexec_on_attach = false;

    double last_attach_check = 0.0;
    bool   attached = false;
};

void run();  // builds window, enters main loop

} // namespace sober::gui

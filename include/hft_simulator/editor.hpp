#pragma once
#include <gtkmm.h>
#include <string>
#include <mutex>
#include <atomic>

namespace hft {

class EditorPanel : public Gtk::Box {
public:
    EditorPanel();
    ~EditorPanel();
    void set_dark(bool dark);

private:
    // Layout
    Gtk::Paned           main_paned_;   // horiz: file list | editor+console
    Gtk::Paned           right_paned_;  // vert:  editor | console

    // ── Left: file browser ─────────────────────────────────────────
    Gtk::Box             left_box_;
    Gtk::Label           files_lbl_;
    Gtk::ScrolledWindow  file_scroll_;
    Gtk::ListBox         file_list_;
    Gtk::Box             file_btn_box_;
    Gtk::Button          new_btn_;
    Gtk::Button          open_btn_;

    // ── Center: editor ─────────────────────────────────────────────
    Gtk::Box             editor_box_;
    Gtk::Box             toolbar_;
    Gtk::Label           file_label_;
    Gtk::Button          save_btn_;
    Gtk::Button          run_btn_;
    Gtk::Button          stop_btn_;
    Gtk::Button          ext_btn_;
    Gtk::ScrolledWindow  editor_scroll_;
    Gtk::TextView        editor_view_;
    Glib::RefPtr<Gtk::TextBuffer> editor_buf_;

    // ── Bottom: console ────────────────────────────────────────────
    Gtk::Box             console_box_;
    Gtk::Box             console_hdr_;
    Gtk::Label           console_lbl_;
    Gtk::Button          clear_btn_;
    Gtk::ScrolledWindow  console_scroll_;
    Gtk::TextView        console_view_;
    Glib::RefPtr<Gtk::TextBuffer>      console_buf_;
    Glib::RefPtr<Gtk::TextBuffer::Tag> tag_err_;
    Glib::RefPtr<Gtk::TextBuffer::Tag> tag_info_;

    // ── State ──────────────────────────────────────────────────────
    std::string       current_file_;
    bool              dirty_{false};
    bool              dark_{true};
    std::atomic<bool> running_{false};

    // Thread → UI output pipe
    Glib::Dispatcher  output_dispatcher_;
    std::mutex        output_mutex_;
    std::string       pending_output_;
    bool              output_is_done_{false};

    // ── Helpers ────────────────────────────────────────────────────
    void new_file();
    void open_file_dialog();
    void save_file();
    void run_file();
    void stop_run();
    void open_external();
    void load_file(const std::string& path);
    void set_current_file(const std::string& path);
    void mark_dirty(bool d);
    void populate_file_list();
    void append_console(const std::string& text, bool error = false);
    void clear_console();
    void on_output_ready();

    static std::string strategies_dir();
};

} // namespace hft

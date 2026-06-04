#include "hft_simulator/editor.hpp"
#include <glibmm.h>
#include <giomm.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <cstdio>
#include <sys/wait.h>
#include <csignal>

namespace fs = std::filesystem;
using namespace hft;

// ── Strategy template ────────────────────────────────────────────────────────

static const char* STRATEGY_TEMPLATE = R"("""
QuantSim Strategy — edit, save, then press  ▶ Run
"""
import sys, os

# Resolve the QuantSim package regardless of where this file lives
_HERE  = os.path.dirname(os.path.abspath(__file__))
_ROOT  = os.path.abspath(os.path.join(_HERE, "../../.."))   # HFT/
_PYLIB = os.path.join(_ROOT, "python")
for _p in (_ROOT, _PYLIB):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from quantsim.strategy   import Strategy, BookUpdate, FillEvent
from quantsim.backtester import BacktestEngine
from quantsim.data.synthetic import GBMFeed

# ── Strategy ──────────────────────────────────────────────────────────────────

class MyStrategy(Strategy):
    """Minimal market-making stub."""

    def __init__(self, gw: Gateway):
        super().__init__(gw)
        self.position = 0
        self.pnl      = 0.0

    def on_start(self):
        print("Strategy started")

    def on_book_update(self, u: BookUpdate):
        # u.symbol, u.bid, u.ask, u.bid_sz, u.ask_sz, u.ts_ns
        mid = (u.bid + u.ask) / 2
        # Example: print mid every 100 ticks
        if u.ts_ns % 100 == 0:
            print(f"  mid={mid:.4f}  pos={self.position:+d}  pnl={self.pnl:.2f}")

    def on_fill(self, f: FillEvent):
        sign = 1 if f.side == 'B' else -1
        self.position += sign * f.qty
        self.pnl      -= sign * f.qty * f.price
        print(f"Fill: {f.side} {f.qty} @ {f.price:.4f}")

    def on_stop(self):
        print(f"Done.  position={self.position:+d}  pnl={self.pnl:.2f}")

# ── Run ───────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    feed   = GBMFeed("SYNTH", initial_price=100.0, sigma=0.0005, n_ticks=500)
    engine = BacktestEngine()
    result = engine.run(feed, MyStrategy, initial_cash=100_000.0)
    print(f"\nBacktest PnL: ${result.final_pnl:,.2f}")
    print(f"Sharpe:       {result.sharpe:.3f}")
)";

// ── Paths ────────────────────────────────────────────────────────────────────

std::string EditorPanel::strategies_dir() {
    auto dir = Glib::get_home_dir() + "/.quantsim/strategies";
    fs::create_directories(dir);
    return dir;
}

// ── Constructor ──────────────────────────────────────────────────────────────

EditorPanel::EditorPanel()
    : Gtk::Box(Gtk::Orientation::VERTICAL)
    , main_paned_(Gtk::Orientation::HORIZONTAL)
    , right_paned_(Gtk::Orientation::VERTICAL)
    , left_box_(Gtk::Orientation::VERTICAL)
    , editor_box_(Gtk::Orientation::VERTICAL)
    , toolbar_(Gtk::Orientation::HORIZONTAL)
    , console_box_(Gtk::Orientation::VERTICAL)
    , console_hdr_(Gtk::Orientation::HORIZONTAL)
    , file_btn_box_(Gtk::Orientation::HORIZONTAL)
    , new_btn_("+ New")
    , open_btn_("Open…")
    , save_btn_("💾 Save")
    , run_btn_("▶ Run")
    , stop_btn_("■ Stop")
    , ext_btn_("⎋ Open in Editor")
    , files_lbl_("Strategy Files")
    , file_label_("(no file open)")
    , console_lbl_("Console Output")
    , clear_btn_("Clear")
{
    // ── Left: file browser ─────────────────────────────────────────
    files_lbl_.add_css_class("sidebar-title");
    files_lbl_.set_margin(8);
    files_lbl_.set_xalign(0.0f);

    file_list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    file_list_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        if (!row) return;
        auto* lbl = dynamic_cast<Gtk::Label*>(row->get_child());
        if (lbl) load_file(lbl->get_tooltip_text());
    });

    file_scroll_.set_child(file_list_);
    file_scroll_.set_vexpand(true);
    file_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);

    new_btn_.signal_clicked().connect([this]{ new_file(); });
    open_btn_.signal_clicked().connect([this]{ open_file_dialog(); });
    new_btn_.add_css_class("run-btn");
    file_btn_box_.append(new_btn_);
    file_btn_box_.append(open_btn_);
    file_btn_box_.set_spacing(4);
    file_btn_box_.set_margin(6);

    left_box_.add_css_class("sidebar");
    left_box_.append(files_lbl_);
    left_box_.append(file_scroll_);
    left_box_.append(file_btn_box_);
    left_box_.set_size_request(200, -1);

    // ── Toolbar ────────────────────────────────────────────────────
    file_label_.set_hexpand(true);
    file_label_.set_xalign(0.0f);
    file_label_.set_margin_start(10);
    file_label_.add_css_class("metric-key");

    run_btn_.add_css_class("run-btn");
    stop_btn_.add_css_class("stop-btn");
    stop_btn_.set_sensitive(false);

    save_btn_.signal_clicked().connect([this]{ save_file(); });
    run_btn_.signal_clicked().connect([this]{ run_file(); });
    stop_btn_.signal_clicked().connect([this]{ stop_run(); });
    ext_btn_.signal_clicked().connect([this]{ open_external(); });

    toolbar_.append(file_label_);
    toolbar_.append(save_btn_);
    toolbar_.append(run_btn_);
    toolbar_.append(stop_btn_);
    toolbar_.append(ext_btn_);
    toolbar_.set_spacing(4);
    toolbar_.set_margin(6);

    // ── Editor ─────────────────────────────────────────────────────
    editor_buf_ = editor_view_.get_buffer();
    editor_view_.set_monospace(true);
    editor_view_.set_left_margin(10);
    editor_view_.set_top_margin(6);
    editor_view_.set_right_margin(10);
    editor_buf_->signal_changed().connect([this]{ mark_dirty(true); });

    editor_scroll_.set_child(editor_view_);
    editor_scroll_.set_vexpand(true);
    editor_scroll_.set_hexpand(true);
    editor_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);

    editor_box_.append(toolbar_);
    editor_box_.append(editor_scroll_);

    // ── Console ────────────────────────────────────────────────────
    console_lbl_.set_hexpand(true);
    console_lbl_.set_xalign(0.0f);
    console_lbl_.add_css_class("section-label");
    console_lbl_.set_margin_start(8);

    clear_btn_.signal_clicked().connect([this]{ clear_console(); });

    console_hdr_.append(console_lbl_);
    console_hdr_.append(clear_btn_);
    console_hdr_.set_margin_top(4);
    console_hdr_.set_margin_end(6);

    console_buf_  = console_view_.get_buffer();
    tag_err_  = console_buf_->create_tag("err");
    tag_err_->property_foreground()  = "#ff6b6b";
    tag_info_ = console_buf_->create_tag("info");
    tag_info_->property_foreground() = "#74c0fc";

    console_view_.set_monospace(true);
    console_view_.set_editable(false);
    console_view_.set_cursor_visible(false);
    console_view_.set_left_margin(8);
    console_view_.set_top_margin(4);

    console_scroll_.set_child(console_view_);
    console_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    console_scroll_.set_size_request(-1, 180);

    console_box_.append(console_hdr_);
    console_box_.append(console_scroll_);

    // ── Assemble ────────────────────────────────────────────────────
    right_paned_.set_orientation(Gtk::Orientation::VERTICAL);
    right_paned_.set_start_child(editor_box_);
    right_paned_.set_end_child(console_box_);
    right_paned_.set_position(420);
    right_paned_.set_resize_start_child(true);
    right_paned_.set_resize_end_child(false);
    right_paned_.set_shrink_start_child(false);
    right_paned_.set_shrink_end_child(false);

    main_paned_.set_start_child(left_box_);
    main_paned_.set_end_child(right_paned_);
    main_paned_.set_position(210);
    main_paned_.set_resize_start_child(false);
    main_paned_.set_resize_end_child(true);
    main_paned_.set_shrink_start_child(false);

    set_vexpand(true);
    set_hexpand(true);
    append(main_paned_);

    // Dispatcher: background thread → UI
    output_dispatcher_.connect([this]{ on_output_ready(); });

    populate_file_list();
    new_file();
}

EditorPanel::~EditorPanel() {
    // Signal any running process (popen pipe — can't easily kill)
    running_ = false;
}

// ── set_dark ─────────────────────────────────────────────────────────────────

void EditorPanel::set_dark(bool dark) {
    dark_ = dark;
    // Tag colors stay readable on both themes
}

// ── File browser ─────────────────────────────────────────────────────────────

void EditorPanel::populate_file_list() {
    while (true) {
        auto* row = file_list_.get_row_at_index(0);
        if (!row) break;
        file_list_.remove(*row);
    }

    auto add_row = [this](const std::string& path, const std::string& label) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(label);
        lbl->set_xalign(0.0f);
        lbl->set_tooltip_text(path);
        lbl->set_margin_start(10);
        lbl->set_margin_top(4);
        lbl->set_margin_bottom(4);
        file_list_.append(*lbl);
    };

    // Built-in examples
    if (fs::exists("examples")) {
        for (auto& e : fs::directory_iterator("examples")) {
            if (e.path().extension() == ".py")
                add_row(e.path().string(), e.path().filename().string());
        }
    }

    // User strategies
    auto sdir = strategies_dir();
    for (auto& e : fs::directory_iterator(sdir)) {
        if (e.path().extension() == ".py")
            add_row(e.path().string(), "[mine] " + e.path().filename().string());
    }
}

// ── New / Open ───────────────────────────────────────────────────────────────

void EditorPanel::new_file() {
    static int n = 1;
    std::string path = strategies_dir() + "/strategy_" + std::to_string(n++) + ".py";
    { std::ofstream f(path); f << STRATEGY_TEMPLATE; }
    load_file(path);
    populate_file_list();
}

void EditorPanel::open_file_dialog() {
    auto native = Gtk::FileChooserNative::create(
        "Open Strategy", *dynamic_cast<Gtk::Window*>(get_root()),
        Gtk::FileChooser::Action::OPEN, "Open", "Cancel");

    auto filter = Gtk::FileFilter::create();
    filter->set_name("Python (*.py)");
    filter->add_pattern("*.py");
    native->add_filter(filter);

    native->signal_response().connect([this, native](int r){
        if (r == static_cast<int>(Gtk::ResponseType::ACCEPT)) {
            auto f = native->get_file();
            if (f) load_file(f->get_path());
        }
    });
    native->show();
}

void EditorPanel::load_file(const std::string& path) {
    if (path.empty() || !fs::exists(path)) return;
    std::ifstream f(path);
    if (!f) return;
    std::string txt((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    editor_buf_->set_text(txt);
    set_current_file(path);
    mark_dirty(false);
}

// ── Save ─────────────────────────────────────────────────────────────────────

void EditorPanel::save_file() {
    if (current_file_.empty()) return;
    std::ofstream f(current_file_);
    if (!f) { append_console("Error: cannot write " + current_file_ + "\n", true); return; }
    f << editor_buf_->get_text();
    mark_dirty(false);
    append_console("Saved  " + current_file_ + "\n", false);
}

void EditorPanel::set_current_file(const std::string& path) {
    current_file_ = path;
    file_label_.set_text(fs::path(path).filename().string());
}

void EditorPanel::mark_dirty(bool d) {
    if (dirty_ == d) return;
    dirty_ = d;
    auto name = current_file_.empty() ? "(no file)" : fs::path(current_file_).filename().string();
    file_label_.set_text(d ? ("● " + name) : name);
}

// ── Run / Stop ───────────────────────────────────────────────────────────────

void EditorPanel::run_file() {
    if (running_ || current_file_.empty()) return;
    save_file();
    clear_console();
    append_console("▶  python3 " + fs::path(current_file_).filename().string() + "\n", false);

    running_ = true;
    run_btn_.set_sensitive(false);
    stop_btn_.set_sensitive(true);

    std::string file = current_file_;
    std::thread([this, file]{
        std::string cmd = "python3 \"" + file + "\" 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            std::lock_guard<std::mutex> lk(output_mutex_);
            pending_output_ += "Error: popen failed\n";
            output_is_done_ = true;
            output_dispatcher_.emit();
            running_ = false;
            return;
        }
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe)) {
            std::lock_guard<std::mutex> lk(output_mutex_);
            pending_output_ += buf;
            output_dispatcher_.emit();
        }
        int rc = pclose(pipe);
        {
            std::lock_guard<std::mutex> lk(output_mutex_);
            pending_output_ += "\n──── exit " + std::to_string(WEXITSTATUS(rc)) + " ────\n";
            output_is_done_ = true;
        }
        output_dispatcher_.emit();
        running_ = false;
    }).detach();
}

void EditorPanel::stop_run() {
    // popen doesn't expose PID; set flag and let thread drain
    running_ = false;
    run_btn_.set_sensitive(true);
    stop_btn_.set_sensitive(false);
    append_console("\n[stopped]\n", true);
}

void EditorPanel::on_output_ready() {
    std::string text;
    bool done = false;
    {
        std::lock_guard<std::mutex> lk(output_mutex_);
        text = std::move(pending_output_);
        pending_output_.clear();
        done = output_is_done_;
        output_is_done_ = false;
    }
    if (!text.empty()) append_console(text);
    if (done) {
        run_btn_.set_sensitive(true);
        stop_btn_.set_sensitive(false);
    }
}

// ── Open External ─────────────────────────────────────────────────────────────

void EditorPanel::open_external() {
    if (current_file_.empty()) return;
    save_file();

    std::string path = current_file_;
    std::string cmd;
#ifdef __APPLE__
    // Try VSCode, then default system app
    cmd = "code \"" + path + "\" 2>/dev/null || open -t \"" + path + "\"";
#else
    cmd = "code \"" + path + "\" 2>/dev/null || "
          "gedit \"" + path + "\" 2>/dev/null || "
          "xdg-open \"" + path + "\"";
#endif

    try {
        Glib::spawn_command_line_async("sh -c \"" + cmd + "\"");
        append_console("Opened in external editor\n", false);
    } catch (const Glib::Error& e) {
        append_console("Open failed: " + std::string(e.what()) + "\n", true);
    }
}

// ── Console ──────────────────────────────────────────────────────────────────

void EditorPanel::append_console(const std::string& text, bool error) {
    auto end = console_buf_->end();
    if (error)
        console_buf_->insert_with_tag(end, text, tag_err_);
    else
        console_buf_->insert(end, text);

    // Auto-scroll to bottom
    auto adj = console_scroll_.get_vadjustment();
    if (adj) adj->set_value(adj->get_upper() - adj->get_page_size());
}

void EditorPanel::clear_console() {
    console_buf_->set_text("");
}


#include "hft_simulator/sim_core.hpp"
#include "hft_simulator/report.hpp"

#include "imgui.h"
#include "implot.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_stdlib.h"
#include "TextEditor.h"
#include "IconsFontAwesome6.h"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <fstream>
#include <csignal>
#include <filesystem>
#include <cstdlib>
#include <cctype>

namespace fs = std::filesystem;

namespace hft {

class ProcRunner {
public:
    ~ProcRunner() { stop(); }
    bool running() const { return running_.load(); }

    void start(const std::string& cmd) {
        stop();
        running_.store(true);
        th_ = std::thread([this, cmd]{
            FILE* p = popen(cmd.c_str(), "r");
            if (!p) { append("[error] launch failed\n"); running_.store(false); return; }
            char buf[2048];
            while (fgets(buf, sizeof buf, p)) { if (!running_.load()) break; append(buf); }
            pclose(p);
            running_.store(false);
        });
    }
    void stop() {
        running_.store(false);
        if (th_.joinable()) th_.detach();
    }
    void append(const std::string& s) { std::lock_guard lk(mu_); out_ += s; }
    std::string drain() { std::lock_guard lk(mu_); std::string s; s.swap(out_); return s; }

private:
    std::atomic<bool> running_{false};
    std::thread       th_;
    std::mutex        mu_;
    std::string       out_;
};

class LiveRunner {
public:
    explicit LiveRunner(SimState& s) : state_(s) {}
    ~LiveRunner() { stop(); }

    bool running() const { return running_.load(); }

    void start(const std::string& symbol, const std::string& strat, double cash,
               const std::string& cpp_path = "") {
        stop();

        std::string lstrat, extra;
        if (strat == "custom_cpp") { lstrat = "custom"; extra = " --dylib \"" + cpp_path + "\""; }
        else if (strat == "mm")        lstrat = "mm";
        else if (strat == "momentum")  lstrat = "momentum";
        else if (strat == "mean_rev" || strat == "stat_arb") lstrat = "meanrev";
        else if (strat == "twap")      lstrat = "twap";
        else                           lstrat = "mm";

        std::ostringstream c;
        c << "./build/live_trader"
          << " --symbol "   << symbol
          << " --strategy " << lstrat
          << extra
          << " --cash "     << (long long)cash
          << " 2>>hft_gui_run.log"
          << " & echo $! > .quantsim_live.pid; wait";
        std::string cmd = c.str();

        { std::lock_guard lk(log_mu_); log_.clear(); }
        log_line("[LIVE] Binance " + symbol + " -> " + lstrat +
                 (cpp_path.empty() ? "" : " (" + cpp_path + ")") + "  (C++ native, paper)");
        running_.store(true);
        th_ = std::thread([this, cmd]{ run(cmd); });
    }

    void stop() {
        if (!running_.load() && !th_.joinable()) return;

        std::ifstream pf(".quantsim_live.pid");
        long pid = 0; if (pf) pf >> pid;
        if (pid > 0) ::kill((pid_t)pid, SIGTERM);
        std::remove(".quantsim_live.pid");
        if (th_.joinable()) th_.join();
        running_.store(false);
    }

    std::string drain_log() {
        std::lock_guard lk(log_mu_);
        std::string out; out.swap(log_); return out;
    }

private:
    void log_line(const std::string& s) { std::lock_guard lk(log_mu_); log_ += s + "\n"; }

    void run(const std::string& cmd) {
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) { log_line("[ERROR] failed to launch python3"); running_.store(false);
                  std::lock_guard lk(state_.mu); state_.done = true; state_.running = false; return; }
        char buf[2048];
        while (fgets(buf, sizeof buf, p)) parse_line(std::string(buf));
        pclose(p);
        running_.store(false);
        std::lock_guard lk(state_.mu);
        state_.running = false; state_.done = true;
    }

    static std::vector<std::pair<double,double>> parse_side(const std::string& s) {
        std::vector<std::pair<double,double>> out;
        size_t i = 0;
        while (i < s.size()) {
            size_t bar = s.find('|', i);
            std::string tok = (bar==std::string::npos) ? s.substr(i) : s.substr(i, bar-i);
            auto colon = tok.find(':');
            if (colon != std::string::npos)
                try { out.emplace_back(std::stod(tok.substr(0,colon)), std::stod(tok.substr(colon+1))); }
                catch (...) {}
            if (bar == std::string::npos) break;
            i = bar + 1;
        }
        return out;
    }

    void parse_line(std::string line) {
        while (!line.empty() && (line.back()=='\n' || line.back()=='\r')) line.pop_back();
        if (line.empty()) return;

        if (line.rfind("EQ:",0)==0) {
            try { state_.push_equity(std::stod(line.substr(3)));
                  std::lock_guard lk(state_.mu); state_.tick_num++; } catch (...) {}
        } else if (line.rfind("BA:",0)==0) {
            auto comma = line.find(',',3);
            if (comma!=std::string::npos) try {
                double bid=std::stod(line.substr(3,comma-3)), ask=std::stod(line.substr(comma+1));
                std::lock_guard lk(state_.mu); state_.best_bid=bid; state_.best_ask=ask;
            } catch (...) {}
        } else if (line.rfind("DEPTH:",0)==0) {
            auto semi = line.find(';',6);
            if (semi!=std::string::npos) {
                auto b = parse_side(line.substr(6, semi-6));
                auto a = parse_side(line.substr(semi+1));
                double bb=b.empty()?0:b.front().first, ba=a.empty()?0:a.front().first;
                state_.set_live_book(bb, ba, std::move(b), std::move(a));

                { std::lock_guard lk(state_.mu); state_.tick_num++; }
            }

        } else if (line.rfind("FILL:",0)==0) {
            std::vector<std::string> t; std::istringstream ps(line.substr(5)); std::string x;
            while (std::getline(ps,x,':')) t.push_back(x);
            if (t.size()>=4) {
                SimState::FillRow r; r.id = ++fill_id_; r.side=t[0];
                try { r.qty=std::stod(t[1]); } catch(...){ r.qty=0; }
                try { r.price=std::stod(t[2]); } catch(...){ r.price=0; }
                r.sym=t[3]; r.maker=(t.size()>=5 && t[4]=="1"); r.ts_ns=0;
                state_.push_fill(std::move(r));
            }
        } else if (line.rfind("POS:",0)==0) {
            std::vector<std::string> t; std::istringstream ps(line.substr(4)); std::string x;
            while (std::getline(ps,x,':')) t.push_back(x);
            if (t.size()>=4) try { std::lock_guard lk(state_.mu);
                state_.pos_net=std::stod(t[0]); state_.pos_avg=std::stod(t[1]);
                state_.pos_realized=std::stod(t[2]); state_.pos_unreal=std::stod(t[3]); } catch(...){}
        } else if (line.rfind("STAT:",0)==0) {
            std::istringstream ss(line.substr(5)); std::string kv; std::lock_guard lk(state_.mu);
            while (std::getline(ss,kv,';')) {
                auto e=kv.find('='); if (e==std::string::npos) continue;
                std::string k=kv.substr(0,e),v=kv.substr(e+1);
                try {
                    if      (k=="sharpe")      state_.sharpe=std::stod(v);
                    else if (k=="sortino")     state_.sortino=std::stod(v);
                    else if (k=="max_dd")      state_.max_dd=std::stod(v);
                    else if (k=="vol")         state_.vol=std::stod(v);
                    else if (k=="fills")       state_.fills_total=std::stoull(v);
                    else if (k=="orders")      state_.orders_sent=std::stoull(v);
                    else if (k=="blocked")     state_.orders_blocked=std::stoull(v);
                    else if (k=="maker_ratio") state_.maker_ratio=std::stod(v);
                } catch(...){}
            }
        } else if (line.rfind("RISK:",0)==0) {
            auto c=line.find(':',5); std::lock_guard lk(state_.mu);
            state_.risk_state = line.substr(5, c==std::string::npos?std::string::npos:c-5);
            state_.risk_detail = (c==std::string::npos)?"":line.substr(c+1);
            state_.kill_armed = (state_.risk_state=="ARMED"||state_.risk_state=="BLOCKED");
        } else if (line.rfind("LAT:",0)==0) {
            try { std::lock_guard lk(state_.mu); state_.feed_lat_us=std::stod(line.substr(4)); } catch(...){}
        } else if (line.rfind("MID:",0)==0) {

        } else if (line.rfind("LOG:",0)==0) {
            log_line(line.substr(4));
        } else {
            log_line(line);
        }
    }

    SimState&         state_;
    std::atomic<bool> running_{false};
    std::thread       th_;
    std::mutex        log_mu_;
    std::string       log_;
    uint64_t          fill_id_{0};
};

static const TextEditor::LanguageDefinition& PythonLang() {
    static bool inited = false;
    static TextEditor::LanguageDefinition lang;
    if (inited) return lang;
    const char* kws[] = {
        "def","class","import","from","as","return","if","elif","else","for","while",
        "break","continue","pass","with","try","except","finally","raise","yield","lambda",
        "global","nonlocal","del","assert","in","is","not","and","or","None","True","False",
        "self","async","await","print","range","len","int","float","str","dict","list","super"
    };
    for (auto k : kws) lang.mKeywords.insert(k);
    using PI = TextEditor::PaletteIndex;
    lang.mTokenRegexStrings.push_back({ "\\\"(\\\\.|[^\\\"])*\\\"", PI::String });
    lang.mTokenRegexStrings.push_back({ "\\'(\\\\.|[^\\'])*\\'",     PI::String });
    lang.mTokenRegexStrings.push_back({ "[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)", PI::Number });
    lang.mTokenRegexStrings.push_back({ "[a-zA-Z_][a-zA-Z0-9_]*",    PI::Identifier });
    lang.mTokenRegexStrings.push_back({ "[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.:]", PI::Punctuation });
    lang.mSingleLineComment = "#";
    lang.mCommentStart = "\"\"\"";
    lang.mCommentEnd   = "\"\"\"";
    lang.mCaseSensitive = true;
    lang.mAutoIndentation = true;
    lang.mName = "Python";
    inited = true;
    return lang;
}

static void apply_theme(bool dark) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 8.0f;
    s.FrameRounding     = 7.0f;
    s.GrabRounding      = 7.0f;
    s.PopupRounding     = 7.0f;
    s.ChildRounding     = 8.0f;
    s.TabRounding       = 7.0f;
    s.ScrollbarRounding = 7.0f;
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(10, 8);
    s.WindowPadding     = ImVec2(14, 12);
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;

    ImVec4* c = s.Colors;

    const ImVec4 bg    = dark ? ImVec4(0.07f,0.07f,0.09f,1) : ImVec4(0.94f,0.95f,0.97f,1);
    const ImVec4 panel = dark ? ImVec4(0.11f,0.11f,0.14f,1) : ImVec4(1.00f,1.00f,1.00f,1);
    const ImVec4 panel2= dark ? ImVec4(0.15f,0.15f,0.19f,1) : ImVec4(0.90f,0.91f,0.94f,1);
    const ImVec4 indigo= ImVec4(0.39f, 0.40f, 0.95f, 1.00f);
    const ImVec4 indigoD=ImVec4(0.31f, 0.31f, 0.78f, 1.00f);
    const ImVec4 text  = dark ? ImVec4(0.90f,0.91f,0.95f,1) : ImVec4(0.11f,0.12f,0.16f,1);
    const ImVec4 textd = dark ? ImVec4(0.50f,0.52f,0.60f,1) : ImVec4(0.42f,0.45f,0.52f,1);
    const float  bw    = dark ? 0.06f : 0.10f;
    const ImVec4 border= dark ? ImVec4(1,1,1,bw) : ImVec4(0,0,0,bw);

    c[ImGuiCol_WindowBg]          = bg;
    c[ImGuiCol_ChildBg]           = panel;
    c[ImGuiCol_PopupBg]           = panel;
    c[ImGuiCol_Border]            = border;
    c[ImGuiCol_FrameBg]           = panel2;
    c[ImGuiCol_FrameBgHovered]    = dark ? ImVec4(0.20f,0.20f,0.25f,1):ImVec4(0.85f,0.86f,0.90f,1);
    c[ImGuiCol_FrameBgActive]     = dark ? ImVec4(0.23f,0.23f,0.29f,1):ImVec4(0.80f,0.82f,0.88f,1);
    c[ImGuiCol_TitleBg]           = bg;
    c[ImGuiCol_TitleBgActive]     = bg;
    c[ImGuiCol_Text]              = text;
    c[ImGuiCol_TextDisabled]      = textd;
    c[ImGuiCol_Button]            = indigoD;
    c[ImGuiCol_ButtonHovered]     = indigo;
    c[ImGuiCol_ButtonActive]      = ImVec4(0.27f,0.27f,0.70f,1.0f);
    c[ImGuiCol_Header]            = panel2;
    c[ImGuiCol_HeaderHovered]     = dark ? ImVec4(0.22f,0.22f,0.28f,1):ImVec4(0.84f,0.85f,0.90f,1);
    c[ImGuiCol_HeaderActive]      = indigoD;
    c[ImGuiCol_CheckMark]         = indigo;
    c[ImGuiCol_SliderGrab]        = indigo;
    c[ImGuiCol_SliderGrabActive]  = indigoD;
    c[ImGuiCol_Tab]               = panel;
    c[ImGuiCol_TabHovered]        = indigo;
    c[ImGuiCol_TabActive]         = panel2;
    c[ImGuiCol_TabUnfocused]      = panel;
    c[ImGuiCol_TabUnfocusedActive]= panel2;
    c[ImGuiCol_ScrollbarBg]       = dark ? ImVec4(0.07f,0.07f,0.09f,0.30f) : ImVec4(0.94f,0.95f,0.97f,0.30f);
    c[ImGuiCol_ScrollbarGrab]     = dark ? ImVec4(0.25f,0.25f,0.30f,1.00f) : ImVec4(0.75f,0.76f,0.80f,1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = dark ? ImVec4(0.35f,0.35f,0.40f,1.00f) : ImVec4(0.65f,0.66f,0.70f,1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = indigo;
    c[ImGuiCol_Separator]         = border;
    c[ImGuiCol_PlotLines]         = indigo;
    c[ImGuiCol_TableHeaderBg]     = panel2;
    c[ImGuiCol_TableRowBg]        = panel;
    c[ImGuiCol_TableRowBgAlt]     = dark ? ImVec4(0.13f,0.13f,0.16f,1):ImVec4(0.96f,0.96f,0.98f,1);
    c[ImGuiCol_TableBorderLight]  = border;
}

class App {
public:
    App() : runner_(state_), live_(state_) {}

    void frame() {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
        ImGui::Begin("##root", nullptr, flags);

        header();
        ImGui::Separator();

        float side_w = 320.0f;
        ImGui::BeginChild("sidebar", ImVec2(side_w, 0), ImGuiChildFlags_Border);
        sidebar();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_NoScrollbar);
        content();
        ImGui::EndChild();

        ImGui::End();

        render_toasts();
    }

private:

    void header() {
        ImGui::PushFont(font_big_);
        ImGui::TextColored(ImVec4(0.39f,0.40f,0.95f,1.0f), ICON_FA_BOLT " QuantSim");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("  HFT Simulation Platform");

        bool running = state_.running || live_.running();
        ImGui::SameLine(ImGui::GetWindowWidth() - 230);
        ImGui::TextColored(running ? ImVec4(0.20f,0.80f,0.45f,1) : ImVec4(0.5f,0.52f,0.6f,1),
                           ICON_FA_CIRCLE " %s", running ? "running" : "idle");
        ImGui::SameLine(ImGui::GetWindowWidth() - 120);
        if (ImGui::Button(dark_ ? ICON_FA_SUN "  Light" : ICON_FA_MOON "  Dark")) {
            dark_ = !dark_;
            apply_theme(dark_);
            ed_te_.SetPalette(dark_ ? TextEditor::GetDarkPalette() : TextEditor::GetLightPalette());
        }
    }

    void sidebar() {

        static int dylib_frame_cnt = 0;
        if (dylib_frame_cnt++ % 60 == 0) {
            refresh_dylibs();
        }

        ImGui::TextDisabled("STRATEGY");
        std::vector<std::string> combo_items;
        std::vector<std::string> combo_ids;

        combo_items.push_back("Market Making (A-S)"); combo_ids.push_back("mm");
        combo_items.push_back("Statistical Arbitrage"); combo_ids.push_back("stat_arb");
        combo_items.push_back("TWAP Execution"); combo_ids.push_back("twap");
        combo_items.push_back("Momentum"); combo_ids.push_back("momentum");
        combo_items.push_back("Mean Reversion (BB)"); combo_ids.push_back("mean_rev");
        combo_items.push_back("RSI Reversal"); combo_ids.push_back("rsi");
        combo_items.push_back("Breakout (Donchian)"); combo_ids.push_back("breakout");
        combo_items.push_back("Portfolio (N-Asset)"); combo_ids.push_back("portfolio");
        combo_items.push_back("Options MM (BS + Greeks)"); combo_ids.push_back("options");

        for (const auto& dylib : dylibs_) {
            combo_items.push_back("Compiled: " + dylib);
            combo_ids.push_back("custom_cpp");
        }

        combo_items.push_back("Browse Custom C++ Strategy...");
        combo_ids.push_back("custom_cpp");

        std::vector<const char*> combo_cstrs;
        for (const auto& item : combo_items) {
            combo_cstrs.push_back(item.c_str());
        }

        if (strat_idx_ < 0 || strat_idx_ >= (int)combo_items.size()) {
            strat_idx_ = 0;
        }

        int old_strat = strat_idx_;
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##strat", &strat_idx_, combo_cstrs.data(), (int)combo_cstrs.size());
        if (old_strat != strat_idx_) {
            save_as_compare_ = false;
        }

        int num_builtins = 9;
        int num_dylibs = (int)dylibs_.size();
        bool is_custom_browse = (strat_idx_ == num_builtins + num_dylibs);
        bool is_dynamic_dylib = (strat_idx_ >= num_builtins && strat_idx_ < num_builtins + num_dylibs);

        if (is_dynamic_dylib) {
            std::string path = "strategies/" + dylibs_[strat_idx_ - num_builtins];
            strncpy(dylib_path_, path.c_str(), sizeof(dylib_path_) - 1);
            std::lock_guard lk(state_.mu);
            state_.custom_dylib_path = dylib_path_;
        }

        if (is_custom_browse) {
            ImGui::Dummy(ImVec2(0, 3));
            ImGui::TextDisabled("Strategy .dylib Path");
            ImGui::SetNextItemWidth(-80);
            ImGui::InputText("##dylibpath", dylib_path_, sizeof(dylib_path_));
            ImGui::SameLine();
            if (ImGui::Button("Browse")) {

                FILE* fp = popen("osascript -e 'POSIX path of (choose file of type {\"dylib\"})' 2>/dev/null", "r");
                if (fp) {
                    char buf[512] = {};
                    if (fgets(buf, sizeof buf, fp)) {

                        size_t n = strlen(buf);
                        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
                        strncpy(dylib_path_, buf, sizeof(dylib_path_)-1);
                        std::lock_guard lk(state_.mu);
                        state_.custom_dylib_path = dylib_path_;
                    }
                    pclose(fp);
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pick a compiled .dylib file (clang++ -shared output)");

            { std::lock_guard lk(state_.mu); state_.custom_dylib_path = dylib_path_; }
        }

        ImGui::Dummy(ImVec2(0,4));
        ImGui::TextDisabled("DATA SOURCE");
        const char* sources[] = { "Synthetic (C++ engine)", "Historical Replay (CSV)", "Crypto — Binance Live" };
        ImGui::SetNextItemWidth(-1);
        int old_src = source_idx_;
        ImGui::Combo("##src", &source_idx_, sources, IM_ARRAYSIZE(sources));
        if (old_src != source_idx_) {
            save_as_compare_ = false;
        }

        bool crypto = source_idx_ == 2;
        bool csv_replay = source_idx_ == 1;
        if (crypto) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Symbol", symbol_, sizeof symbol_);
        } else if (csv_replay) {
            ImGui::SetNextItemWidth(-1);
            char csv_buf[256];
            {
                std::lock_guard lk(state_.mu);
                strncpy(csv_buf, state_.csv_path.c_str(), sizeof(csv_buf));
            }
            csv_buf[sizeof(csv_buf) - 1] = '\0';
            if (ImGui::InputText("CSV Path", csv_buf, sizeof(csv_buf))) {
                std::lock_guard lk(state_.mu);
                state_.csv_path = csv_buf;
            }
        }

        ImGui::Dummy(ImVec2(0,4));
        ImGui::TextDisabled("PARAMETERS");
        if (source_idx_ == 0) {
            ImGui::TextDisabled("Ticks");
            ImGui::SetNextItemWidth(-1); ImGui::InputInt("##Ticks",  &ticks_, 500, 5000);
            ImGui::TextDisabled("Volatility (Sigma)");
            ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##Sigma", &sigma_, 0.0001, 0.001, "%.4f");
            ImGui::TextDisabled("Seed");
            ImGui::SetNextItemWidth(-1); ImGui::InputInt("##Seed",   &seed_);
        }

        {
            const std::string sel_id = combo_ids[strat_idx_];
            const auto& specs = param_spec(sel_id);
            if (!specs.empty()) {
                render_param_sliders(sel_id, specs);
            }
        }

        if (combo_ids[strat_idx_] == "portfolio") {
            ImGui::Dummy(ImVec2(0,4));
            ImGui::TextDisabled("PORTFOLIO");
            ImGui::TextDisabled("# Assets");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##NAssets", &n_assets_, 2, 10)) {}
            ImGui::TextDisabled("Common Factor Vol");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputDouble("##SigCommon", &sigma_common_, 0.0001, 0.001, "%.4f");
            ImGui::TextDisabled("Idiosyncratic Vol");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputDouble("##SigIdio", &sigma_idio_, 0.0001, 0.001, "%.4f");
            if (source_idx_ == 1) {
                ImGui::TextDisabled("CSV Files (one path per line)");
                std::lock_guard lk(state_.mu);
                ImGui::InputTextMultiline("##pfcsv", &state_.csv_paths_multi,
                                          ImVec2(-1, 70));
            }
        }

        if (combo_ids[strat_idx_] == "options") {
            ImGui::Dummy(ImVec2(0,4));
            ImGui::TextDisabled("OPTIONS (ATM call)");
            std::lock_guard lk(state_.mu);
            ImGui::TextDisabled("Tenor (years)");
            ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##OptT", &state_.opt_tenor, 0.05, 0.25, "%.2f");
            ImGui::TextDisabled("Implied Vol");
            ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##OptIV", &state_.opt_iv, 0.01, 0.05, "%.2f");
            ImGui::TextDisabled("Risk-free Rate");
            ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##OptR", &state_.opt_rate, 0.005, 0.01, "%.3f");
            ImGui::Checkbox("Auto Delta-Hedge", &state_.opt_hedge);
            if (state_.opt_tenor < 0.01) state_.opt_tenor = 0.01;
            if (state_.opt_iv < 0.01) state_.opt_iv = 0.01;
        }

        ImGui::Dummy(ImVec2(0,4));
        {
            std::lock_guard lk(state_.mu);
            ImGui::Checkbox("Realistic Fills (latency+queue)", &state_.latency_enabled);
            if (state_.latency_enabled) {
                ImGui::TextDisabled("Latency Min (ms)");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##LatMin", &state_.latency_min_ms, 1.0, 5.0, "%.1f");
                ImGui::TextDisabled("Latency Max (ms)");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##LatMax", &state_.latency_max_ms, 1.0, 5.0, "%.1f");
                if (state_.latency_min_ms < 0) state_.latency_min_ms = 0;
                if (state_.latency_max_ms < state_.latency_min_ms)
                    state_.latency_max_ms = state_.latency_min_ms;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Log-normal network latency delays order arrival;\n"
                              "FIFO queue-position + slippage are tracked.");

        {
            std::lock_guard lk(state_.mu);
            ImGui::Checkbox("Transaction Costs (fees)", &state_.fees_enabled);
            if (state_.fees_enabled) {
                ImGui::TextDisabled("Taker Fee (bps)");
                ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##TakerFee", &state_.taker_fee_bps, 0.1, 1.0, "%.2f");
                ImGui::TextDisabled("Maker Rebate (bps)");
                ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##MakerReb", &state_.maker_rebate_bps, 0.1, 1.0, "%.2f");
                ImGui::TextDisabled("Short Borrow (bps/tick)");
                ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##Borrow", &state_.borrow_bps_per_tick, 0.001, 0.01, "%.4f");
                if (state_.taker_fee_bps < 0) state_.taker_fee_bps = 0;
                if (state_.maker_rebate_bps < 0) state_.maker_rebate_bps = 0;
                if (state_.borrow_bps_per_tick < 0) state_.borrow_bps_per_tick = 0;
            }
        }

        {
            std::lock_guard lk(state_.mu);
            ImGui::Checkbox("Pre-Trade Risk Limits", &state_.risk_enabled);
            if (state_.risk_enabled) {
                ImGui::TextDisabled("Max Position (abs, 0=off)");
                ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##MaxPos", &state_.max_position, 50, 500, "%.0f");
                ImGui::TextDisabled("Max Notional ($, 0=off)");
                ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##MaxNot", &state_.max_notional, 10000, 100000, "%.0f");
                ImGui::TextDisabled("Max Loss ($, 0=off)");
                ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##MaxLoss", &state_.max_loss, 1000, 10000, "%.0f");
                if (state_.max_position < 0) state_.max_position = 0;
                if (state_.max_notional < 0) state_.max_notional = 0;
                if (state_.max_loss < 0) state_.max_loss = 0;
            }
        }

        ImGui::TextDisabled("Cash");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##Cash", &cash_, 10000.0, 100000.0, "%.0f");
        if (!crypto) {
            ImGui::TextDisabled("Tick Delay (us)");
            ImGui::SetNextItemWidth(-1); ImGui::SliderInt("##TickDelay", &state_.delay_us, 0, 5000);
        }
        bool running = state_.running || live_.running();
        if (running) ImGui::BeginDisabled();
        ImGui::Checkbox("Enable Bot Trading", &enable_bots_);
        if (running) ImGui::EndDisabled();
        ticks_ = std::clamp(ticks_, 100, 200000);
        if (sigma_ < 0.00001) sigma_ = 0.00001;

        ImGui::Dummy(ImVec2(0, 4));
        {
            ImGui::TextDisabled("Target Compare Slot");
            ImGui::SetNextItemWidth(-1);
            const char* slots_list[] = { "Slot 1 (Amber)", "Slot 2 (Teal)", "Slot 3 (Purple)", "Slot 4 (Blue)", "Slot 5 (Yellow)" };
            ImGui::Combo("##CompareSlot", &save_slot_idx_, slots_list, IM_ARRAYSIZE(slots_list));

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(8, 4));
            if (save_as_compare_) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.75f, 0.50f, 0.10f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.62f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.38f, 0.05f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.32f, 0.32f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
            }
            char cmp_lbl[128];
            snprintf(cmp_lbl, sizeof(cmp_lbl), save_as_compare_
                ? ICON_FA_FLAG "  Arm Slot %d  [ARMED]"
                : ICON_FA_FLAG "  Set as Compare Slot %d", save_slot_idx_ + 1, save_slot_idx_ + 1);
            if (ImGui::Button(cmp_lbl, ImVec2(-1, 0)))
                save_as_compare_ = !save_as_compare_;
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("When ARMED, this run's results will be saved\nto the Compare tab under the selected Slot.");
        }

        ImGui::Dummy(ImVec2(0,4));
        bool is_custom = (strat_idx_ >= num_builtins);

        bool disable_run = crypto && is_custom && dylib_path_[0] == '\0';
        if (running || disable_run) ImGui::BeginDisabled();
        if (ImGui::Button(crypto ? ICON_FA_BOLT "  Go Live" : ICON_FA_PLAY "  Run", ImVec2(-1, 38)))
            on_run(combo_ids[strat_idx_].c_str(), crypto);
        if (running || disable_run) ImGui::EndDisabled();
        if (disable_run && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Select or compile a custom C++ .dylib first, then Go Live.");
        }

        if (!running) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f,0.25f,0.30f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f,0.30f,0.36f,1.0f));
        if (ImGui::Button(ICON_FA_STOP "  Stop", ImVec2(-1, 32))) { runner_.stop(); live_.stop(); }
        ImGui::PopStyleColor(2);
        if (!running) ImGui::EndDisabled();

        {
            uint64_t tk = state_.tick_num, ttl = state_.total_ticks;
            float frac = ttl ? (float)tk / ttl : 0.0f;
            char ov[48]; snprintf(ov, sizeof ov, "%llu / %llu",
                                  (unsigned long long)tk, (unsigned long long)ttl);
            ImGui::ProgressBar(frac, ImVec2(-1, 0), ov);
        }

        ImGui::Dummy(ImVec2(0,8));
        ImGui::TextDisabled("LIVE METRICS");
        double pnl, sharpe, sortino, calmar, dd, vol; uint64_t fills, orders, bot_fills; double bot_vol;
        {
            std::lock_guard lk(state_.mu);
            pnl = state_.equity.empty() ? 0 : state_.equity.back() - state_.initial_cash;
            sharpe=state_.sharpe; sortino=state_.sortino; calmar=state_.calmar;
            dd=state_.max_dd; vol=state_.vol; fills=state_.fills_total; orders=state_.orders_sent;
            bot_fills=state_.bot_fills_total; bot_vol=state_.bot_volume;
        }
        metric("PnL",      pnl, true,  "$%+.2f");
        metric("Sharpe",   sharpe);
        metric("Sortino",  sortino);
        metric("Calmar",   calmar);
        metric("Max DD",   dd*100, false, "%.2f%%");
        metric("Vol",      vol*100, false, "%.2f%%");
        metric_u("Fills",  fills);
        metric_u("Orders", orders);
        metric_u("Bot Trades", bot_fills);
        metric("Bot Volume", bot_vol, false, "$%.0f");

        bool lat_on; double slp, qp;
        { std::lock_guard lk(state_.mu); lat_on=state_.latency_enabled; slp=state_.avg_slippage; qp=state_.avg_queue_pos; }
        if (lat_on) {
            metric("Avg Slippage", slp, false, "%.4f");
            metric("Avg Queue", qp, false, "%.0f");
        }

        bool live_mode; double pnet, pavg, preal, punr, lat, mratio;
        std::string rstate, rdetail;
        {
            std::lock_guard lk(state_.mu);
            live_mode = state_.live_mode;
            pnet=state_.pos_net; pavg=state_.pos_avg; preal=state_.pos_realized;
            punr=state_.pos_unreal; lat=state_.feed_lat_us; mratio=state_.maker_ratio;
            rstate=state_.risk_state; rdetail=state_.risk_detail;
        }
        if (live_mode) {
            ImGui::Dummy(ImVec2(0,8));
            ImGui::TextDisabled("LIVE — PAPER");
            metric("Position", pnet, false, "%.5f");
            metric("Avg Px",   pavg, false, "%.2f");
            metric("Realized", preal, true, "$%+.2f");
            metric("Unreal",   punr,  true, "$%+.2f");
            metric("Feed Lat", lat,  false, "%.0f us");
            metric("Maker %",  mratio*100, false, "%.0f%%");

            ImGui::Dummy(ImVec2(0,4));
            ImVec4 rc = rstate=="OK" ? ImVec4(0.20f,0.80f,0.45f,1.0f)
                      : rstate=="ARMED" ? ImVec4(0.95f,0.65f,0.20f,1.0f)
                                        : ImVec4(0.95f,0.30f,0.36f,1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(rc.x,rc.y,rc.z,0.18f));
            ImGui::PushStyleColor(ImGuiCol_Text, rc);
            std::string badge = ICON_FA_SHIELD_HALVED " RISK: " + rstate + (rdetail.empty()?"":("  "+rdetail));
            ImGui::Button(badge.c_str(), ImVec2(-1, 0));
            ImGui::PopStyleColor(2);
        }
    }

    void metric(const char* k, double v, bool color=false, const char* fmt="%.3f") {
        ImGui::TextDisabled("%s", k);
        ImGui::SameLine(140);
        char buf[48]; snprintf(buf, sizeof buf, fmt, v);
        ImVec4 col = !color ? ImVec4(0.90f,0.91f,0.95f,1.0f)
                            : (v >= 0 ? ImVec4(0.20f,0.80f,0.45f,1.0f) : ImVec4(0.95f,0.30f,0.36f,1.0f));
        ImGui::TextColored(col, "%s", buf);
    }
    void metric_u(const char* k, uint64_t v) {
        ImGui::TextDisabled("%s", k);
        ImGui::SameLine(140);
        ImGui::Text("%llu", (unsigned long long)v);
    }

    void render_param_sliders(const std::string& sel_id, const std::vector<ParamSpec>& specs) {
        ImGui::Dummy(ImVec2(0,4));
        ImGui::TextDisabled("STRATEGY PARAMETERS");
        std::lock_guard lk(state_.mu);
        auto& pm = state_.params[sel_id];
        fill_defaults(sel_id, pm);
        for (const auto& ps : specs) {
            ImGui::TextDisabled("%s", ps.label);
            ImGui::SetNextItemWidth(-1);
            std::string id = "##p_" + sel_id + "_" + ps.key;
            if (ps.integer) {
                int iv = (int)(pm[ps.key] + 0.5);
                if (ImGui::SliderInt(id.c_str(), &iv, (int)ps.min, (int)ps.max))
                    pm[ps.key] = (double)iv;
            } else {
                float v = (float)pm[ps.key];
                if (ImGui::SliderFloat(id.c_str(), &v, (float)ps.min, (float)ps.max, "%.3f"))
                    pm[ps.key] = (double)v;
            }
        }
    }

    void content() {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Dashboard")) { tab_dashboard(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Charts"))    { tab_charts();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Fills"))     { tab_fills();     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Bot Fills")) { tab_bot_fills(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Results"))   { tab_results();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Editor"))    { tab_editor();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Terminal"))  { tab_terminal();  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Compare"))   { tab_compare();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Optimizer")) { tab_optimizer(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Risk"))      { tab_risk();      ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Options"))   { tab_options();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("TCA"))       { tab_tca();       ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Log"))       { tab_log();       ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }

    void tab_dashboard() {
        bool live; double bb, ba; std::vector<std::pair<double,double>> lb, la;
        std::vector<DepthLevel> bids, asks;
        {
            std::lock_guard lk(state_.mu);
            live = state_.live_mode; bb = state_.best_bid; ba = state_.best_ask;
            lb = state_.live_bids; la = state_.live_asks; bids = state_.bids; asks = state_.asks;
        }
        double mid = (bb>0 && ba>0) ? (bb+ba)/2 : 0;
        double spread = (bb>0 && ba>0) ? ba-bb : 0;
        ImGui::Text("Best Bid"); ImGui::SameLine(120);
        ImGui::TextColored(ImVec4(0.20f,0.80f,0.45f,1), "%.2f", bb); ImGui::SameLine(260);
        ImGui::Text("Best Ask"); ImGui::SameLine(380);
        ImGui::TextColored(ImVec4(0.95f,0.30f,0.36f,1), "%.2f", ba);
        ImGui::Text("Mid %.2f", mid); ImGui::SameLine(200);
        ImGui::TextDisabled("Spread %.4f  (%.1f bps)", spread, mid>0?spread/mid*1e4:0);

        std::vector<std::pair<double,double>> B, A;
        if (live) { B = lb; A = la; }
        else { for (auto& d:bids) B.emplace_back(d.price,(double)d.qty);
               for (auto& d:asks) A.emplace_back(d.price,(double)d.qty); }
        std::sort(A.begin(),A.end(),[](auto&a,auto&b){return a.first<b.first;});
        std::sort(B.begin(),B.end(),[](auto&a,auto&b){return a.first>b.first;});
        int n = (int)std::max(A.size(), B.size()); n = std::min(n, 12);

        if (!B.empty() && !A.empty()) {
            double bq = B[0].second, aq = A[0].second, tot = bq+aq;
            double micro = tot>0 ? (bb*aq + ba*bq)/tot : mid;
            double imb   = tot>0 ? (bq-aq)/tot : 0;
            ImGui::Text("Microprice %.2f", micro); ImGui::SameLine(220);
            ImGui::Text("Imbalance"); ImGui::SameLine(330);
            ImVec4 ic = imb>=0 ? ImVec4(0.20f,0.80f,0.45f,1):ImVec4(0.95f,0.30f,0.36f,1);
            ImGui::TextColored(ic, "%+.2f", imb);
            ImGui::SameLine(440);
            ImGui::SetNextItemWidth(160);
            ImGui::ProgressBar((float)((imb+1)/2), ImVec2(160,0), "");
        }
        ImGui::Separator();

        ImGuiTableFlags f = ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchSame;
        if (ImGui::BeginTable("ladder", 4, f)) {
            ImGui::TableSetupColumn("Bid Qty"); ImGui::TableSetupColumn("Bid");
            ImGui::TableSetupColumn("Ask"); ImGui::TableSetupColumn("Ask Qty");
            ImGui::TableHeadersRow();
            for (int i=0;i<n;++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); if (i<(int)B.size()) ImGui::Text("%.4g", B[i].second);
                ImGui::TableNextColumn();
                if (i<(int)B.size()) ImGui::TextColored(ImVec4(0.20f,0.80f,0.45f,1),"%.2f",B[i].first);
                ImGui::TableNextColumn();
                if (i<(int)A.size()) ImGui::TextColored(ImVec4(0.95f,0.30f,0.36f,1),"%.2f",A[i].first);
                ImGui::TableNextColumn(); if (i<(int)A.size()) ImGui::Text("%.4g", A[i].second);
            }
            ImGui::EndTable();
        }
    }

    void tab_compare() {
        ImGui::BeginChild("##CompareScroll", ImVec2(0, 0), false, ImGuiWindowFlags_None);
        struct Slot {
            std::vector<double> equity;
            double sharpe, sortino, calmar, max_dd, vol;
            double final_pnl, init_cash;
            uint64_t fills, win_trades, loss_trades;
            double win_ratio, profit_factor;
            std::string name;
            bool valid;
        };
        Slot slots[5];
        Slot current;

        bool has_any_valid_slot = false;
        {
            std::lock_guard lk(state_.mu);
            for (int i = 0; i < 5; ++i) {
                auto& src = state_.cmp_slots[i];
                auto& dst = slots[i];
                dst.valid = src.valid;
                if (dst.valid) {
                    dst.equity       = src.equity;
                    dst.sharpe       = src.sharpe;   dst.sortino  = src.sortino;
                    dst.calmar       = src.calmar;   dst.max_dd   = src.max_dd;
                    dst.vol          = src.vol;      dst.final_pnl= src.final_pnl;
                    dst.init_cash    = src.initial_cash;
                    dst.fills        = src.fills_total;
                    dst.win_trades   = src.win_trades;
                    dst.loss_trades  = src.loss_trades;
                    dst.win_ratio    = src.win_ratio;
                    dst.profit_factor= src.profit_factor;
                    dst.name         = src.strategy_name;
                    has_any_valid_slot = true;
                }
            }

            current.valid = !state_.equity.empty();
            if (current.valid) {
                current.equity       = state_.equity;
                current.sharpe       = state_.sharpe;   current.sortino  = state_.sortino;
                current.calmar       = state_.calmar;   current.max_dd   = state_.max_dd;
                current.vol          = state_.vol;      current.final_pnl= current.equity.back() - current.init_cash;
                current.init_cash    = state_.initial_cash;
                current.fills        = state_.fills_total;
                current.win_trades   = state_.win_trades;
                current.loss_trades  = state_.loss_trades;
                current.win_ratio    = state_.win_ratio;
                current.profit_factor= state_.profit_factor;
                current.name         = state_.strategy_name;
            }
        }

        bool batch_running = (run_queue_idx_ >= 0);

        ImGui::TextColored(ImVec4(0.40f, 0.70f, 0.95f, 1.0f), "Batch Strategy Runner");
        ImGui::TextDisabled("Select up to 5 strategies to compare side-by-side:");

        const char* batch_names[] = {
            "Market Making (A-S)", "Statistical Arbitrage", "TWAP Execution",
            "Momentum", "Mean Reversion (BB)", "RSI Reversal", "Breakout (Donchian)"
        };
        const char* batch_ids[] = { "mm", "stat_arb", "twap", "momentum", "mean_rev", "rsi", "breakout" };

        if (batch_running) ImGui::BeginDisabled();
        if (ImGui::BeginTable("##BatchSelectGrid", 4, ImGuiTableFlags_None)) {
            for (int i = 0; i < 7; ++i) {
                ImGui::TableNextColumn();
                ImGui::Checkbox(batch_names[i], &batch_select_[i]);
            }
            for (const auto& dylib : dylibs_) {
                ImGui::TableNextColumn();
                bool& sel = batch_custom_select_[dylib];
                std::string label = "C++: " + dylib;
                ImGui::Checkbox(label.c_str(), &sel);
            }
            ImGui::EndTable();
        }
        if (batch_running) ImGui::EndDisabled();

        ImGui::Dummy(ImVec2(0, 2));
        if (batch_running) ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_PLAY "  Run Selected & Compare", ImVec2(240, 26))) {
            run_queue_.clear();
            {
                std::lock_guard lk(state_.mu);
                for (int i = 0; i < 5; ++i) state_.cmp_slots[i].valid = false;
            }
            for (int i = 0; i < 7; ++i) {
                if (batch_select_[i]) {
                    run_queue_.push_back({ batch_ids[i], "" });
                    if (run_queue_.size() >= 5) break;
                }
            }
            if (run_queue_.size() < 5) {
                for (const auto& dylib : dylibs_) {
                    if (batch_custom_select_[dylib]) {
                        run_queue_.push_back({ "custom_cpp", "strategies/" + dylib });
                        if (run_queue_.size() >= 5) break;
                    }
                }
            }
            if (!run_queue_.empty()) {
                run_queue_idx_ = 0;
            } else {
                toast("Please select at least one strategy to run.", ImVec4(0.95f, 0.30f, 0.36f, 1.0f));
            }
        }
        if (batch_running) ImGui::EndDisabled();

        if (batch_running) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.20f, 0.80f, 0.45f, 1.0f), "Running batch: %d / %d...", run_queue_idx_ + 1, (int)run_queue_.size());
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FILE_EXPORT "  Export Current Run")) export_report();
        ImGui::Separator();
        ImGui::Spacing();

        if (!has_any_valid_slot && !current.valid) {
            ImGui::TextDisabled("Select strategies above and click 'Run Selected & Compare' to generate a comparison report.");
            ImGui::EndChild();
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.80f, 1.0f, 1.0f));
        ImGui::Text("Strategy Comparison Report (up to 5 strategies)");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        ImVec4 slot_colors[5] = {
            ImVec4(0.95f, 0.60f, 0.10f, 1.0f),
            ImVec4(0.10f, 0.75f, 0.75f, 1.0f),
            ImVec4(0.75f, 0.20f, 0.80f, 1.0f),
            ImVec4(0.20f, 0.50f, 0.95f, 1.0f),
            ImVec4(0.85f, 0.85f, 0.10f, 1.0f)
        };

        if (ImPlot::BeginPlot("Equity Curves", ImVec2(-1, 200.0f),
                              ImPlotFlags_Crosshairs)) {
            ImPlot::SetupAxes("tick", "PnL", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.0f");

            for (int i = 0; i < 5; ++i) {
                auto& s = slots[i];
                if (s.valid && s.equity.size() >= 2) {
                    int n = (int)s.equity.size();
                    std::vector<double> axs(n), apnl(n);
                    for (int j = 0; j < n; ++j) { axs[j]=j; apnl[j]=s.equity[j]-s.init_cash; }
                    std::string label = "Slot " + std::to_string(i + 1) + ": " + s.name;
                    ImPlot::SetNextLineStyle(slot_colors[i], 2.0f);
                    ImPlot::PlotLine(label.c_str(), axs.data(), apnl.data(), n);
                }
            }

            if (current.valid && current.equity.size() >= 2) {
                int n = (int)current.equity.size();
                std::vector<double> cur_xs(n), cur_pnl(n);
                for (int j = 0; j < n; ++j) { cur_xs[j]=j; cur_pnl[j]=current.equity[j]-current.init_cash; }
                std::string label = "Current: " + current.name;
                bool up = cur_pnl.back() >= 0;
                ImVec4 bc = up ? ImVec4(0.20f,0.80f,0.45f,1) : ImVec4(0.95f,0.30f,0.36f,1);
                ImPlot::SetNextLineStyle(bc, 2.0f);
                ImPlot::PlotLine(label.c_str(), cur_xs.data(), cur_pnl.data(), n);
            }
            ImPlot::EndPlot();
        }
        ImGui::TextDisabled("Equity Curves — Slots 1-5 (custom colors) vs Current (green/red)");

        if (ImPlot::BeginPlot("##DDCompare", ImVec2(-1, 150.0f),
                              ImPlotFlags_Crosshairs | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("", "DD %", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "%.3f%%");

            for (int i = 0; i < 5; ++i) {
                auto& s = slots[i];
                if (s.valid && s.equity.size() >= 2) {
                    int n = (int)s.equity.size();
                    std::vector<double> axs_dd(n), add(n);
                    double peak = s.equity[0];
                    for (int j = 0; j < n; ++j) {
                        if (s.equity[j] > peak) peak = s.equity[j];
                        axs_dd[j] = j;
                        add[j] = peak > 0 ? (s.equity[j] - peak) / peak * 100.0 : 0.0;
                    }
                    std::string label = "Slot " + std::to_string(i + 1) + ": " + s.name;
                    ImPlot::SetNextLineStyle(slot_colors[i], 1.5f);
                    ImPlot::PlotLine(label.c_str(), axs_dd.data(), add.data(), n);
                }
            }

            if (current.valid && current.equity.size() >= 2) {
                int n = (int)current.equity.size();
                std::vector<double> cur_xs_dd(n), cur_dd(n);
                double peak = current.equity[0];
                for (int j = 0; j < n; ++j) {
                    if (current.equity[j] > peak) peak = current.equity[j];
                    cur_xs_dd[j] = j;
                    cur_dd[j] = peak > 0 ? (current.equity[j] - peak) / peak * 100.0 : 0.0;
                }
                std::string label = "Current: " + current.name;
                ImVec4 col = current.final_pnl >= 0 ? ImVec4(0.20f,0.80f,0.45f,1) : ImVec4(0.95f,0.30f,0.36f,1);
                ImPlot::SetNextLineStyle(col, 1.5f);
                ImPlot::PlotLine(label.c_str(), cur_xs_dd.data(), cur_dd.data(), n);
            }
            ImPlot::EndPlot();
        }
        ImGui::TextDisabled("Drawdown Comparison (%%)");

        ImGui::Spacing();
        ImGuiTableFlags tbl = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders
                            | ImGuiTableFlags_SizingStretchProp;

        int cols = 1;
        std::vector<int> active_slot_indices;
        for (int i = 0; i < 5; ++i) {
            if (slots[i].valid) {
                cols++;
                active_slot_indices.push_back(i);
            }
        }
        bool show_current = current.valid;
        if (show_current) cols++;

        if (ImGui::BeginTable("##CmpMetrics", cols, tbl, ImVec2(-1, 240.0f))) {
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 0.22f);
            for (int idx : active_slot_indices) {
                std::string col_hdr = "Slot " + std::to_string(idx + 1) + ": " + slots[idx].name;
                ImGui::TableSetupColumn(col_hdr.c_str(), ImGuiTableColumnFlags_WidthStretch, 0.78f / (cols - 1));
            }
            if (show_current) {
                std::string col_hdr = "Current: " + current.name;
                ImGui::TableSetupColumn(col_hdr.c_str(), ImGuiTableColumnFlags_WidthStretch, 0.78f / (cols - 1));
            }
            ImGui::TableHeadersRow();

            auto mrow = [&](const char* name,
                            std::function<double(const Slot&)> selector,
                            const char* fmt,
                            bool higher_is_better = true) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);

                std::vector<double> vals;
                double best_val = higher_is_better ? -999999999.0 : 999999999.0;
                int best_col_idx = -1;

                int col_idx = 0;
                for (int idx : active_slot_indices) {
                    double val = selector(slots[idx]);
                    vals.push_back(val);
                    if (higher_is_better) {
                        if (val > best_val) { best_val = val; best_col_idx = col_idx; }
                    } else {
                        if (val < best_val) { best_val = val; best_col_idx = col_idx; }
                    }
                    col_idx++;
                }
                if (show_current) {
                    double val = selector(current);
                    vals.push_back(val);
                    if (higher_is_better) {
                        if (val > best_val) { best_val = val; best_col_idx = col_idx; }
                    } else {
                        if (val < best_val) { best_val = val; best_col_idx = col_idx; }
                    }
                }

                for (size_t i = 0; i < vals.size(); ++i) {
                    ImGui::TableNextColumn();
                    char buf[64];
                    snprintf(buf, sizeof buf, fmt, vals[i]);

                    if ((int)i == best_col_idx && vals.size() > 1) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f,0.80f,0.45f,1.0f));
                        ImGui::TextUnformatted(buf);
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::TextUnformatted(buf);
                    }
                }
            };

            mrow("Total Return (%)", [](const Slot& s){ return std::max(s.init_cash, 1.0) > 1.0 ? (s.final_pnl / s.init_cash) * 100.0 : 0.0; }, "%.2f%%");
            mrow("Final PnL", [](const Slot& s){ return s.final_pnl; }, "$%+.0f");
            mrow("Sharpe (ann)", [](const Slot& s){ return s.sharpe; }, "%.3f");
            mrow("Sortino", [](const Slot& s){ return s.sortino; }, "%.3f");
            mrow("Calmar", [](const Slot& s){ return s.calmar; }, "%.3f");
            mrow("Max Drawdown", [](const Slot& s){ return s.max_dd * 100.0; }, "%.2f%%", false);
            mrow("Volatility", [](const Slot& s){ return s.vol * 100.0; }, "%.2f%%", false);
            mrow("Profit Factor", [](const Slot& s){ return s.profit_factor; }, "%.3f");
            mrow("Win Rate", [](const Slot& s){ return s.win_ratio * 100.0; }, "%.1f%%");
            mrow("Total Fills", [](const Slot& s){ return (double)s.fills; }, "%.0f");
            mrow("Win Trades", [](const Slot& s){ return (double)s.win_trades; }, "%.0f");
            mrow("Loss Trades", [](const Slot& s){ return (double)s.loss_trades; }, "%.0f", false);

            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    static const std::vector<std::pair<std::string,std::string>>& sweepable() {
        static const std::vector<std::pair<std::string,std::string>> S = {
            {"mm","Market Making (A-S)"}, {"momentum","Momentum"},
            {"mean_rev","Mean Reversion (BB)"}, {"rsi","RSI Reversal"},
            {"breakout","Breakout (Donchian)"} };
        return S;
    }

    void start_sweep(const std::string& sid) {
        auto& axes = opt_axes_[sid];
        std::vector<std::string> keys;
        for (const auto& ps : param_spec(sid)) if (axes[ps.key].on) keys.push_back(ps.key);
        if (keys.empty() || keys.size() > 2) {
            toast("Select 1 or 2 parameters to sweep.", ImVec4(0.95f,0.30f,0.36f,1)); return;
        }
        ParamMap base = state_.param_snapshot(sid);
        auto axis_vals = [&](const std::string& k){
            auto& ax = axes[k]; std::vector<double> v;
            int st = std::max(2, ax.steps);
            bool integer = false;
            for (const auto& ps : param_spec(sid)) if (k==ps.key) integer = ps.integer;
            for (int i=0;i<st;++i){ double t=(double)i/(st-1); double x=ax.lo + t*(ax.hi-ax.lo);
                                    v.push_back(integer ? std::round(x) : x); }
            return v;
        };
        sweep_strat_ = sid; sweep_keys_ = keys;
        sweep_av_ = axis_vals(keys[0]);
        if (keys.size()==2) { sweep_bv_ = axis_vals(keys[1]); sweep_nb_ = (int)sweep_bv_.size(); }
        else                { sweep_bv_ = {0.0}; sweep_nb_ = 1; }
        sweep_na_ = (int)sweep_av_.size();
        if (sweep_na_ * sweep_nb_ > 256) {
            toast("Grid too large (>256 runs). Reduce steps.", ImVec4(0.95f,0.30f,0.36f,1)); return;
        }
        sweep_queue_.clear();
        for (int a=0;a<sweep_na_;++a) for (int b=0;b<sweep_nb_;++b) {
            ParamMap p = base; p[keys[0]] = sweep_av_[a];
            if (keys.size()==2) p[keys[1]] = sweep_bv_[b];
            sweep_queue_.push_back(p);
        }
        sweep_results_.assign(sweep_na_*sweep_nb_, 0.0f);
        sweep_best_ = -1; sweep_objhib_ = (opt_objective_ != 2);
        sweep_idx_ = 0; sweep_pending_ = -1;
    }

    void render_sweep_results() {
        if (sweep_results_.empty()) { ImGui::TextDisabled("No sweep results yet — configure axes and Run Sweep."); return; }
        ImGui::Separator();
        const char* objname = opt_objective_==0 ? "Sharpe"
                            : opt_objective_==1 ? "Final PnL" : "Max DD %";
        auto fmt = [](double v){ char b[32]; snprintf(b,sizeof b,"%.3g",v); return std::string(b); };

        if (sweep_nb_ <= 1) {
            if (ImPlot::BeginPlot("Sweep Result", ImVec2(-1, 300), ImPlotFlags_Crosshairs)) {
                ImPlot::SetupAxes(sweep_keys_[0].c_str(), objname,
                                  ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                std::vector<double> ys(sweep_na_);
                for (int i=0;i<sweep_na_;++i) ys[i]=sweep_results_[i];
                ImPlot::SetNextLineStyle(ImVec4(0.39f,0.55f,0.97f,1), 2.0f);
                ImPlot::PlotLine(objname, sweep_av_.data(), ys.data(), sweep_na_);
                if (sweep_best_>=0) {
                    double bx=sweep_av_[sweep_best_], by=sweep_results_[sweep_best_];
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6, ImVec4(0.20f,0.80f,0.45f,1));
                    ImPlot::PlotScatter("best", &bx, &by, 1);
                }
                ImPlot::EndPlot();
            }
        } else {
            float mn=1e30f, mx=-1e30f;
            for (float v:sweep_results_){ mn=std::min(mn,v); mx=std::max(mx,v); }
            if (mn==mx) mx=mn+1;
            ImPlot::PushColormap(ImPlotColormap_Viridis);
            if (ImPlot::BeginPlot("Sweep Heatmap", ImVec2(-70, 340))) {
                ImPlot::SetupAxes(sweep_keys_[1].c_str(), sweep_keys_[0].c_str());
                ImPlot::PlotHeatmap(objname, sweep_results_.data(), sweep_na_, sweep_nb_,
                                    mn, mx, "%.2f", ImPlotPoint(0,0), ImPlotPoint(sweep_nb_, sweep_na_));
                ImPlot::EndPlot();
            }
            ImGui::SameLine();
            ImPlot::ColormapScale("##sweepscale", mn, mx, ImVec2(60, 340), "%.2f");
            ImPlot::PopColormap();
            ImGui::TextDisabled("X = %s   Y = %s   color = %s",
                                sweep_keys_[1].c_str(), sweep_keys_[0].c_str(), objname);
        }

        if (sweep_best_ >= 0) {
            int a = sweep_best_ / sweep_nb_, b = sweep_best_ % sweep_nb_;
            std::string txt = "Best: " + sweep_keys_[0] + "=" + fmt(sweep_av_[a]);
            if (sweep_nb_ > 1) txt += ", " + sweep_keys_[1] + "=" + fmt(sweep_bv_[b]);
            txt += "   ->   " + std::string(objname) + " = " + fmt(sweep_results_[sweep_best_]);
            ImGui::TextColored(ImVec4(0.20f,0.80f,0.45f,1), "%s", txt.c_str());
            if (ImGui::Button(ICON_FA_CHECK "  Apply Best Params")) {
                { std::lock_guard lk(state_.mu); state_.params[sweep_strat_] = sweep_queue_[sweep_best_]; }
                toast("Applied best params to " + sweep_strat_, ImVec4(0.20f,0.80f,0.45f,1));
            }
        }
    }

    void tab_optimizer() {
        const auto& S = sweepable();
        bool busy = (sweep_idx_ >= 0) || state_.running || live_.running();

        ImGui::TextColored(ImVec4(0.40f,0.70f,0.95f,1), "Parameter Sweep / Grid Search Optimizer");
        ImGui::TextDisabled("Check 1-2 parameters to sweep; the engine runs a backtest across the grid and maps the objective.");
        ImGui::Spacing();

        if (busy) ImGui::BeginDisabled();
        std::vector<const char*> snames; for (auto& p : S) snames.push_back(p.second.c_str());
        ImGui::SetNextItemWidth(240);
        ImGui::Combo("Strategy##opt", &opt_strat_idx_, snames.data(), (int)snames.size());
        const std::string sid = S[opt_strat_idx_].first;
        ImGui::SameLine();
        const char* objs[] = { "Maximize Sharpe", "Maximize Final PnL", "Minimize Max Drawdown" };
        ImGui::SetNextItemWidth(240);
        ImGui::Combo("Objective##opt", &opt_objective_, objs, 3);
        ImGui::Separator();

        auto& axes = opt_axes_[sid];
        int swept = 0;
        for (const auto& ps : param_spec(sid)) {
            auto& ax = axes[ps.key];
            if (!ax.inited) { ax.lo=(float)ps.min; ax.hi=(float)ps.max; ax.inited=true; }
            ImGui::PushID(ps.key);
            ImGui::Checkbox(ps.label, &ax.on);
            if (ax.on) {
                ++swept;
                ImGui::SameLine(230); ImGui::SetNextItemWidth(90);  ImGui::InputFloat("min", &ax.lo);
                ImGui::SameLine();    ImGui::SetNextItemWidth(90);  ImGui::InputFloat("max", &ax.hi);
                ImGui::SameLine();    ImGui::SetNextItemWidth(130); ImGui::SliderInt("steps", &ax.steps, 2, 16);
            }
            ImGui::PopID();
        }
        if (busy) ImGui::EndDisabled();

        ImGui::Separator();
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_PLAY "  Run Sweep", ImVec2(160, 28))) start_sweep(sid);
        if (busy) ImGui::EndDisabled();
        ImGui::SameLine();
        if (swept == 0)      ImGui::TextDisabled("Select at least one parameter.");
        else if (swept > 2)  ImGui::TextColored(ImVec4(0.95f,0.30f,0.36f,1), "Select at most 2 parameters.");
        else {
            int total = 1;
            for (const auto& ps : param_spec(sid)) if (axes[ps.key].on) total *= std::max(2, axes[ps.key].steps);
            ImGui::TextDisabled("%d backtests", total);
        }
        if (sweep_idx_ >= 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.20f,0.80f,0.45f,1), "Running %d / %d ...",
                               sweep_idx_, (int)sweep_queue_.size());
        }

        render_sweep_results();
        render_robustness();
    }

    void start_robust() {
        if (sweep_best_ < 0 || sweep_queue_.empty()) { toast("Run a sweep first.", ImVec4(0.95f,0.30f,0.36f,1)); return; }
        robust_strat_  = sweep_strat_;
        robust_params_ = sweep_queue_[sweep_best_];
        robust_n_ = std::clamp(robust_n_, 2, 64);
        robust_sharpe_.assign(robust_n_, 0.0f);
        robust_pnl_.assign(robust_n_, 0.0f);
        robust_idx_ = 0; robust_pending_ = -1;
    }

    void render_robustness() {
        if (sweep_best_ < 0) return;
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.40f,0.70f,0.95f,1), "Out-of-Sample Robustness (best params re-run across seeds)");
        ImGui::TextDisabled("Tests overfitting: stable Sharpe across seeds = robust; wild swings = curve-fit.");
        bool rbusy = (robust_idx_ >= 0) || state_.running;
        if (rbusy) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(120); ImGui::InputInt("seeds##rob", &robust_n_);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLAY "  Validate Best")) start_robust();
        if (rbusy) ImGui::EndDisabled();
        if (robust_idx_ >= 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.20f,0.80f,0.45f,1), "Running %d / %d ...", robust_idx_, robust_n_);
        }
        if (robust_sharpe_.empty() || robust_idx_ >= 0) return;

        double m=0; for (float v:robust_sharpe_) m+=v; m/=robust_sharpe_.size();
        double sd=0; for (float v:robust_sharpe_) sd+=(v-m)*(v-m); sd=std::sqrt(sd/robust_sharpe_.size());
        int win=0; for (float v:robust_pnl_) if (v>0) ++win;
        double hit = 100.0*win/robust_pnl_.size();
        ImGui::Text("Sharpe: mean %.3f  +/-  %.3f      Profitable seeds: %.0f%%  (%d/%d)",
                    m, sd, hit, win, (int)robust_pnl_.size());
        if (ImPlot::BeginPlot("Sharpe by Seed", ImVec2(-1, 220))) {
            ImPlot::SetupAxes("seed #", "Sharpe", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            std::vector<double> xs(robust_sharpe_.size()), ys(robust_sharpe_.size());
            for (size_t i=0;i<xs.size();++i){ xs[i]=(double)i; ys[i]=robust_sharpe_[i]; }
            ImPlot::PlotBars("Sharpe", xs.data(), ys.data(), (int)xs.size(), 0.6);
            double mm[2]={m,m}, mx[2]={-0.5,(double)xs.size()-0.5};
            ImPlot::SetNextLineStyle(ImVec4(1,0.8f,0,1), 1.5f);
            ImPlot::PlotLine("mean", mx, mm, 2);
            ImPlot::EndPlot();
        }
    }

    void tab_options() {
        double S, theo, iv, strike, opt_pos, hpos, d, gm, vg, th, pnl, init;
        std::vector<double> dser, gsurf, gstk;
        {
            std::lock_guard lk(state_.mu);
            if (state_.strategy_name != "options" || state_.equity.empty()) {
                ImGui::TextDisabled("Select 'Options MM (BS + Greeks)' and Run to see the Greeks monitor.");
                return;
            }
            S=state_.opt_underlying; theo=state_.opt_theo; iv=state_.opt_iv_live; strike=state_.opt_strike;
            opt_pos=state_.opt_position; hpos=state_.opt_hedge_pos;
            d=state_.net_delta; gm=state_.net_gamma; vg=state_.net_vega; th=state_.net_theta;
            dser=state_.delta_series; init=state_.initial_cash; pnl=state_.equity.back()-init;
            gsurf=state_.greeks_surface; gstk=state_.greeks_strikes;
        }
        ImGui::TextColored(ImVec4(0.40f,0.70f,0.95f,1), "Options Market-Making — ATM Call (Black-Scholes)");
        ImGui::Separator();

        auto stat = [&](const char* k, const char* fmt, double v, bool col=false){
            ImGui::TextDisabled("%s", k); ImGui::SameLine(150);
            char b[48]; snprintf(b,sizeof b,fmt,v);
            if (col) ImGui::TextColored(v>=0?ImVec4(0.25f,0.85f,0.5f,1):ImVec4(0.95f,0.35f,0.35f,1),"%s",b);
            else ImGui::TextUnformatted(b);
        };
        if (ImGui::BeginTable("##opttop", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            stat("Underlying", "%.2f", S);        stat("Strike", "%.2f", strike);
            stat("Theo (BS)", "%.3f", theo);      stat("Implied Vol", "%.1f%%", iv*100);
            ImGui::TableNextColumn();
            stat("Option Pos", "%+.0f", opt_pos); stat("Hedge (underlying)", "%+.0f", hpos);
            stat("Total PnL", "$%+.2f", pnl, true);
            ImGui::EndTable();
        }
        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.70f,0.80f,1.0f,1), "Net Greeks (position-weighted)");
        if (ImGui::BeginTable("##greeks", 4, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("Delta"); ImGui::TableSetupColumn("Gamma");
            ImGui::TableSetupColumn("Vega");  ImGui::TableSetupColumn("Theta");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            auto cell=[&](double v){ ImGui::TableNextColumn();
                ImGui::TextColored(std::abs(v)<1e-9?ImVec4(0.6f,0.6f,0.6f,1):ImVec4(0.9f,0.91f,0.95f,1),"%+.4f",v); };
            cell(d); cell(gm); cell(vg); cell(th);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Delta near 0 => well delta-hedged. Gamma/Vega/Theta are the residual option risk.");
        ImGui::Spacing();

        if (gstk.size() >= 1 && gsurf.size() == gstk.size() * hft::G_NCOLS) {
            ImGui::TextColored(ImVec4(0.70f,0.80f,1.0f,1), "Greeks Surface (per strike, mdspan)");
            auto gv = hft::greeks_view(gsurf, gstk.size());
            if (ImGui::BeginTable("##gsurf", 6, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableSetupColumn("Strike"); ImGui::TableSetupColumn("Price");
                ImGui::TableSetupColumn("Delta");  ImGui::TableSetupColumn("Gamma");
                ImGui::TableSetupColumn("Vega");   ImGui::TableSetupColumn("Theta");
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < gstk.size(); ++i) {
                    ImGui::TableNextRow(); ImGui::TableNextColumn();
                    bool atm = std::abs(gstk[i]-strike) < strike*0.011;
                    ImGui::TextColored(atm?ImVec4(1,0.85f,0.3f,1):ImVec4(0.85f,0.86f,0.9f,1), "%.2f%s", gstk[i], atm?" *":"");
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", gv[i, hft::G_PRICE]);
                    ImGui::TableNextColumn(); ImGui::Text("%+.3f", gv[i, hft::G_DELTA]);
                    ImGui::TableNextColumn(); ImGui::Text("%.4f", gv[i, hft::G_GAMMA]);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", gv[i, hft::G_VEGA]);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", gv[i, hft::G_THETA]);
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
        }

        if (dser.size() >= 2 && ImPlot::BeginPlot("Net Delta Over Time (hedge tracking)", ImVec2(-1, 240),
                                                  ImPlotFlags_Crosshairs)) {
            ImPlot::SetupAxes("tick", "net delta", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            std::vector<double> xs(dser.size());
            for (size_t i=0;i<xs.size();++i) xs[i]=(double)i;
            ImPlot::SetNextLineStyle(ImVec4(0.39f,0.55f,0.97f,1), 1.6f);
            ImPlot::PlotLine("net delta", xs.data(), dser.data(), (int)dser.size());
            double zx[2]={0,(double)dser.size()}, zy[2]={0,0};
            ImPlot::SetNextLineStyle(ImVec4(1,0.8f,0,0.7f), 1.0f);
            ImPlot::PlotLine("flat", zx, zy, 2);
            ImPlot::EndPlot();
        }
    }

    void tab_risk() {
        bool en, halted; double maxpos, maxnot, maxloss, var, gross, net, real, unreal, pnl;
        uint64_t blocked;
        {
            std::lock_guard lk(state_.mu);
            if (state_.equity.empty()) { ImGui::TextDisabled("Run a strategy to see the risk dashboard."); return; }
            en=state_.risk_enabled; halted=state_.risk_halted;
            maxpos=state_.max_position; maxnot=state_.max_notional; maxloss=state_.max_loss;
            var=state_.var95; gross=state_.gross_exposure; net=state_.net_exposure;
            real=state_.realized_pnl; unreal=state_.unrealized_pnl; blocked=state_.orders_blocked;
            pnl=state_.equity.back()-state_.initial_cash;
        }

        ImVec4 col = halted ? ImVec4(0.95f,0.30f,0.36f,1)
                   : en     ? ImVec4(0.20f,0.80f,0.45f,1)
                            : ImVec4(0.6f,0.62f,0.7f,1);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x,col.y,col.z,0.18f));
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        std::string badge = ICON_FA_SHIELD_HALVED " RISK ";
        badge += halted ? "HALTED (max loss hit)" : en ? "ENFORCED" : "OFF (enable in sidebar)";
        ImGui::Button(badge.c_str(), ImVec2(-1, 0));
        ImGui::PopStyleColor(2);
        ImGui::Spacing();

        auto bar = [&](const char* label, double used, double limit, const char* fmt){
            ImGui::TextDisabled("%s", label);
            float frac = limit > 0 ? (float)std::clamp(used/limit, 0.0, 1.0) : 0.0f;
            ImVec4 bc = frac > 0.9f ? ImVec4(0.95f,0.30f,0.36f,1)
                      : frac > 0.7f ? ImVec4(0.95f,0.65f,0.20f,1)
                                    : ImVec4(0.20f,0.80f,0.45f,1);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bc);
            char ov[64]; snprintf(ov, sizeof ov, limit>0?fmt:"no limit", used, limit);
            ImGui::ProgressBar(frac, ImVec2(-1, 0), ov);
            ImGui::PopStyleColor();
        };
        bar("Net Notional Exposure vs Max", std::abs(net), maxnot, "$%.0f / $%.0f");
        bar("Drawdown Loss vs Max Loss",    std::max(0.0,-pnl), maxloss, "$%.0f / $%.0f");
        ImGui::Spacing();

        if (ImPlot::BeginPlot("PnL Attribution", ImVec2(-1, 200))) {
            ImPlot::SetupAxes("", "$", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            const char* labels[3] = { "Realized", "Unrealized", "Total" };
            ImPlot::SetupAxisTicks(ImAxis_X1, 0, 2, 3, labels);
            double xs[3] = {0,1,2}; double vals[3] = { real, unreal, pnl };
            ImPlot::PlotBars("PnL", xs, vals, 3, 0.6);
            ImPlot::EndPlot();
        }

        ImGuiTableFlags tbl = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame;
        if (ImGui::BeginTable("##risk", 2, tbl)) {
            ImGui::TableSetupColumn("Metric"); ImGui::TableSetupColumn("Value"); ImGui::TableHeadersRow();
            auto row = [&](const char* k, const char* fmt, double v, bool c=false, bool g=true){
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn(); char b[48]; snprintf(b,sizeof b,fmt,v);
                if (c) ImGui::TextColored(g?ImVec4(0.25f,0.85f,0.50f,1):ImVec4(0.95f,0.35f,0.35f,1),"%s",b);
                else ImGui::TextUnformatted(b);
            };
            row("95% VaR (1-tick)",   "$%.2f", var);
            row("Gross Exposure",     "$%.0f", gross);
            row("Net Exposure",       "$%+.0f", net);
            row("Realized PnL",       "$%+.2f", real, true, real>=0);
            row("Unrealized PnL",     "$%+.2f", unreal, true, unreal>=0);
            row("Total PnL",          "$%+.2f", pnl, true, pnl>=0);
            row("Orders Blocked",     "%.0f",   (double)blocked, blocked>0, false);
            ImGui::EndTable();
        }
    }

    void tab_tca() {
        double fees, rebates, borrow, slip, final_pnl, init; uint64_t mk, tk, fills; bool feon;
        {
            std::lock_guard lk(state_.mu);
            if (state_.equity.empty()) { ImGui::TextDisabled("Run a strategy to see transaction-cost analysis."); return; }
            fees=state_.total_fees; rebates=state_.total_rebates; borrow=state_.total_borrow;
            slip=state_.avg_slippage; final_pnl=state_.equity.back()-state_.initial_cash;
            init=state_.initial_cash; mk=state_.maker_fills; tk=state_.taker_fills;
            fills=state_.fills_total; feon=state_.fees_enabled;
        }
        double net_drag = fees + borrow - rebates;
        double gross_pnl = final_pnl + net_drag;
        double maker_ratio = (mk+tk) > 0 ? (double)mk/(mk+tk) : 0.0;

        if (!feon)
            ImGui::TextColored(ImVec4(0.95f,0.65f,0.20f,1),
                "Fees were OFF for this run — enable 'Transaction Costs' in the sidebar for cost drag.");

        ImGui::TextColored(ImVec4(0.40f,0.70f,0.95f,1), "Execution Quality / Cost Drag");
        ImGui::Separator();

        if (ImPlot::BeginPlot("PnL Bridge (Gross -> Net)", ImVec2(-1, 240))) {
            ImPlot::SetupAxes("", "$", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            const char* labels[5] = { "Gross", "Taker Fees", "Rebates", "Borrow", "Net" };
            ImPlot::SetupAxisTicks(ImAxis_X1, 0, 4, 5, labels);
            double vals[5] = { gross_pnl, -fees, rebates, -borrow, final_pnl };
            double xs[5]   = { 0,1,2,3,4 };
            ImPlot::PlotBars("PnL", xs, vals, 5, 0.6);
            ImPlot::EndPlot();
        }

        ImGuiTableFlags tbl = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame;
        if (ImGui::BeginTable("##tca", 2, tbl)) {
            ImGui::TableSetupColumn("Metric"); ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();
            auto row = [&](const char* k, const char* fmt, double v, bool col=false, bool good=true){
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn(); char b[48]; snprintf(b,sizeof b,fmt,v);
                if (col) ImGui::TextColored(good?ImVec4(0.25f,0.85f,0.50f,1):ImVec4(0.95f,0.35f,0.35f,1),"%s",b);
                else ImGui::TextUnformatted(b);
            };
            row("Gross PnL (pre-cost)", "$%+.2f", gross_pnl, true, gross_pnl>=0);
            row("Taker Fees Paid",      "$%.2f",  fees);
            row("Maker Rebates Earned", "$%.2f",  rebates);
            row("Short Borrow Cost",    "$%.2f",  borrow);
            row("Net Cost Drag",        "$%.2f",  net_drag, true, net_drag<=0);
            row("Net PnL (post-cost)",  "$%+.2f", final_pnl, true, final_pnl>=0);
            row("Cost Drag / Gross",    "%.2f%%", gross_pnl!=0 ? net_drag/std::abs(gross_pnl)*100.0 : 0.0);
            row("Maker Fill Ratio",     "%.1f%%", maker_ratio*100.0);
            row("Taker Fills",          "%.0f",   (double)tk);
            row("Maker Fills",          "%.0f",   (double)mk);
            row("Total Fills",          "%.0f",   (double)fills);
            row("Avg Slippage / fill",  "%.4f",   slip);
            ImGui::EndTable();
        }
    }

    void tab_log() {
        if (ImGui::Button("Clear")) log_text_.clear();
        ImGui::SameLine(); ImGui::TextDisabled("%zu chars", log_text_.size());
        ImGui::Separator();
        ImGui::BeginChild("logscroll");
        ImGui::TextUnformatted(log_text_.c_str(), log_text_.c_str()+log_text_.size());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    void tab_charts() {

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(10, 4));

        {
            bool hm = show_heatmap_;
            if (hm) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.55f, 0.85f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.65f, 0.95f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.05f, 0.40f, 0.70f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
            }
            const char* hm_label = hm ? "  Liquidity Heatmap  ON " : "  Liquidity Heatmap  OFF";
            if (ImGui::Button(hm_label)) show_heatmap_ = !show_heatmap_;
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toggle Bookmap-style order book liquidity heatmap");
        }

        ImGui::SameLine(0, 10);

        {
            bool bp = show_bot_pnl_chart_;
            if (bp) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.35f, 0.22f, 0.75f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.30f, 0.88f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.25f, 0.15f, 0.60f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
            }
            const char* bp_label = bp ? "  Bot P&L Overlay  ON " : "  Bot P&L Overlay  OFF";
            if (ImGui::Button(bp_label)) show_bot_pnl_chart_ = !show_bot_pnl_chart_;
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Overlay bot P&L on the unified strategy P&L chart");
        }

        ImGui::PopStyleVar(2);
        ImGui::Separator();
        ImGui::Spacing();

        std::vector<double> eq; std::vector<double> bot_eq; double init; bool live;
        std::vector<std::pair<double,double>> Bp, Ap;
        std::string strat_name;
        {
            std::lock_guard lk(state_.mu);
            eq = state_.equity; bot_eq = state_.bot_equity; init = state_.initial_cash; live = state_.live_mode;
            strat_name = state_.strategy_name;
            if (live) { Bp = state_.live_bids; Ap = state_.live_asks; }
            else {
                for (auto& d:state_.bids) Bp.emplace_back(d.price,(double)d.qty);
                for (auto& d:state_.asks) Ap.emplace_back(d.price,(double)d.qty);
            }
        }
        float h = ImGui::GetContentRegionAvail().y;
        bool is_sa = (strat_name == "stat_arb");
        bool is_pf = (strat_name == "portfolio");
        float pnl_height = is_sa ? h * 0.32f : h * 0.50f;
        float depth_height = is_sa ? h * 0.28f : h * 0.40f;

        bool show_bot_line = show_bot_pnl_chart_ && source_idx_ < 2 && enable_bots_ && bot_eq.size() >= 2;
        ImPlotFlags plot_flags = ImPlotFlags_Crosshairs;
        if (show_bot_line) plot_flags |= ImPlotFlags_YAxis2;

        if (ImPlot::BeginPlot("P&L", ImVec2(-1, pnl_height), plot_flags)) {

            ImPlot::SetupAxis(ImAxis_X1, "tick", ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxis(ImAxis_Y1, "Strategy P&L", ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.2f");

            if (show_bot_line) {

                ImPlot::SetupAxis(ImAxis_Y2, "Bot P&L",
                                  ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Opposite);
                ImPlot::SetupAxisFormat(ImAxis_Y2, "$%.2f");
            }

            if (eq.size() >= 2) {
                int n = (int)eq.size();
                std::vector<double> xs(n), pnl(n), bpnl(n);
                for (int i = 0; i < n; ++i) {
                    xs[i]   = i;
                    pnl[i]  = eq[i] - init;
                    bpnl[i] = (i < (int)bot_eq.size()) ? bot_eq[i] - init : 0.0;
                }

                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                bool up = pnl.back() >= 0;
                ImVec4 col = up ? ImVec4(0.20f,0.80f,0.45f,1) : ImVec4(0.95f,0.30f,0.36f,1);
                ImPlot::SetNextFillStyle(col, 0.18f);
                ImPlot::PlotShaded("Strategy P&L", xs.data(), pnl.data(), n, 0.0);
                ImPlot::SetNextLineStyle(col, 2.0f);
                ImPlot::PlotLine("Strategy P&L", xs.data(), pnl.data(), n);

                if (show_bot_line) {

                    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                    ImVec4 bot_col = ImVec4(0.39f, 0.55f, 0.97f, 1.0f);
                    ImPlot::SetNextLineStyle(bot_col, 1.8f);
                    ImPlot::PlotLine("Bot P&L", xs.data(), bpnl.data(),
                                     std::min(n, (int)bot_eq.size()));
                }
            }
            ImPlot::EndPlot();
        }

        bool has_heatmap = false;
        std::vector<float> hm_vals;
        int hm_rows = 0, hm_cols = 0;
        double hm_min_y = 0.0, hm_max_y = 0.0;
        uint64_t tk = 0;
        {
            std::lock_guard lk(state_.mu);
            if (!state_.heatmap_values.empty()) {
                has_heatmap = true;
                hm_vals = state_.heatmap_values;
                hm_rows = state_.heatmap_rows;
                hm_cols = state_.heatmap_cols;
                hm_min_y = state_.heatmap_min_y;
                hm_max_y = state_.heatmap_max_y;
                tk = state_.tick_num;
            }
        }

        if (is_pf) {

        } else if (show_heatmap_ && has_heatmap) {
            ImPlot::PushColormap(ImPlotColormap_Plasma);
            if (ImPlot::BeginPlot("Order Book Liquidity Heatmap (Bookmap-Style)", ImVec2(-80.0f, depth_height))) {
                double min_x = tk > (uint64_t)hm_cols ? (double)(tk - hm_cols) : 0.0;
                double max_x = (double)tk;
                ImPlot::SetupAxes("tick", "price", 0, 0);
                ImPlot::SetupAxisLimits(ImAxis_X1, min_x, max_x, ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, hm_min_y, hm_max_y, ImGuiCond_Always);

                float max_val = 0.0f;
                for (float v : hm_vals) { if (v > max_val) max_val = v; }
                if (max_val < 1.0f) max_val = 1.0f;

                ImPlotPoint bounds_min(min_x, hm_min_y);
                ImPlotPoint bounds_max(max_x, hm_max_y);

                ImPlot::PlotHeatmap("Liquidity Density", hm_vals.data(), hm_rows, hm_cols,
                                    0.0, (double)max_val, nullptr, bounds_min, bounds_max);
                ImPlot::EndPlot();
            }
            ImGui::SameLine();
            float max_val = 0.0f;
            for (float v : hm_vals) { if (v > max_val) max_val = v; }
            if (max_val < 1.0f) max_val = 1.0f;
            ImPlot::ColormapScale("##HeatScale", 0.0, (double)max_val, ImVec2(60.0f, depth_height), "%.0f");
            ImPlot::PopColormap();
        } else {
            if (ImPlot::BeginPlot("Order Book Depth", ImVec2(-1, depth_height))) {
                ImPlot::SetupAxes("price", "cum qty", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal|ImPlotLegendFlags_Outside);
                auto cum = [](std::vector<std::pair<double,double>> lv, bool asc,
                              std::vector<double>& px, std::vector<double>& qy){
                    std::sort(lv.begin(), lv.end(), [&](auto&a,auto&b){ return asc?a.first<b.first:a.first>b.first; });
                    double run=0; for (auto&l:lv){ run+=l.second; px.push_back(l.first); qy.push_back(run); }
                };
                std::vector<double> bx,by,ax,ay;
                cum(Bp,false,bx,by); cum(Ap,true,ax,ay);
                std::reverse(bx.begin(),bx.end()); std::reverse(by.begin(),by.end());
                ImVec4 g(0.20f,0.80f,0.45f,1), r(0.95f,0.30f,0.36f,1);
                if (bx.size()>=2){ ImPlot::SetNextFillStyle(g,0.30f); ImPlot::PlotShaded("Bids",bx.data(),by.data(),(int)bx.size());
                                   ImPlot::SetNextLineStyle(g,1.8f); ImPlot::PlotLine("Bids",bx.data(),by.data(),(int)bx.size()); }
                if (ax.size()>=2){ ImPlot::SetNextFillStyle(r,0.30f); ImPlot::PlotShaded("Asks",ax.data(),ay.data(),(int)ax.size());
                                   ImPlot::SetNextLineStyle(r,1.8f); ImPlot::PlotLine("Asks",ax.data(),ay.data(),(int)ax.size()); }
                ImPlot::EndPlot();
            }
        }

        if (is_sa) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "Portfolio Leg Allocation & Correlation");

            double spy_p = 0.0, qqq_p = 0.0;
            std::vector<double> corr_data;
            {
                std::lock_guard lk(state_.mu);
                spy_p = state_.sa_spy_pos;
                qqq_p = state_.sa_qqq_pos;
                corr_data = state_.correlation;
            }

            ImGuiTableFlags tbl_flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoHostExtendX;
            if (ImGui::BeginTable("PortfolioTable", 2, tbl_flags)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                ImGui::Text("Current Leg Allocation (Shares)");
                if (ImPlot::BeginPlot("Leg Allocation", ImVec2(-1, h * 0.22f))) {
                    ImPlot::SetupAxes("", "Shares", 0, ImPlotAxisFlags_AutoFit);
                    double positions[2] = { spy_p, qqq_p };
                    const char* labels[2] = { "SPY Leg", "QQQ Leg" };
                    ImPlot::SetupAxisTicks(ImAxis_X1, 0.0, 1.0, 2, labels);
                    ImPlot::PlotBarGroups(labels, positions, 1, 2, 0.4);
                    ImPlot::EndPlot();
                }

                ImGui::TableNextColumn();

                ImGui::Text("Rolling Leg Correlation (60 Ticks)");
                if (ImPlot::BeginPlot("Leg Correlation", ImVec2(-1, h * 0.22f))) {
                    ImPlot::SetupAxes("tick", "Correlation", ImPlotAxisFlags_AutoFit, 0);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, -1.1, 1.1, ImGuiCond_Always);
                    if (!corr_data.empty()) {
                        std::vector<double> xs(corr_data.size());
                        for (size_t i = 0; i < xs.size(); ++i) xs[i] = i;
                        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), 2.0f);
                        ImPlot::PlotLine("Pearson r", xs.data(), corr_data.data(), (int)corr_data.size());
                    }
                    ImPlot::EndPlot();
                }

                ImGui::EndTable();
            }
        }

        if (strat_name == "portfolio") {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "Asset Correlation Matrix (rolling returns)");
            int n; std::vector<float> cm; std::vector<std::string> names; std::vector<double> apos;
            {
                std::lock_guard lk(state_.mu);
                n = state_.n_assets; cm = state_.corr_matrix;
                names = state_.asset_names; apos = state_.asset_pos;
            }
            if (n >= 2 && (int)cm.size() == n * n) {
                std::vector<double> tpos(n);
                std::vector<const char*> labels(n);
                for (int k = 0; k < n; ++k) { tpos[k] = k + 0.5; labels[k] = names[k].c_str(); }
                ImPlot::PushColormap(ImPlotColormap_RdBu);
                if (ImPlot::BeginPlot("##corr", ImVec2(-70.0f, h * 0.5f),
                                      ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
                    ImPlot::SetupAxes(nullptr, nullptr,
                        ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoGridLines,
                        ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoGridLines);
                    ImPlot::SetupAxisTicks(ImAxis_X1, tpos.data(), n, labels.data());
                    ImPlot::SetupAxisTicks(ImAxis_Y1, tpos.data(), n, labels.data());
                    ImPlot::SetupAxisLimits(ImAxis_X1, 0, n, ImGuiCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, n, ImGuiCond_Always);
                    ImPlot::PlotHeatmap("corr", cm.data(), n, n, -1.0, 1.0, "%.2f",
                                        ImPlotPoint(0, 0), ImPlotPoint(n, n));
                    ImPlot::EndPlot();
                }
                ImGui::SameLine();
                ImPlot::ColormapScale("##cs", -1.0, 1.0, ImVec2(60.0f, h * 0.5f), "%.1f");
                ImPlot::PopColormap();
                ImGui::TextDisabled("Net positions:");
                for (int k = 0; k < n && k < (int)apos.size(); ++k) {
                    ImGui::SameLine();
                    ImVec4 pc = apos[k] >= 0 ? ImVec4(0.20f,0.80f,0.45f,1) : ImVec4(0.95f,0.30f,0.36f,1);
                    ImGui::TextColored(pc, "%s %+.0f", names[k].c_str(), apos[k]);
                }
            } else {
                ImGui::TextDisabled("Accumulating rolling-return window for correlation...");
            }
        }
    }

    void tab_fills() {
        std::vector<SimState::FillRow> rows;
        {
            std::lock_guard lk(state_.mu);
            rows.assign(state_.fill_log.begin(), state_.fill_log.end());
        }
        std::reverse(rows.begin(), rows.end());
        ImGuiTableFlags f = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("fills", 5, f)) {
            ImGui::TableSetupScrollFreeze(0,1);
            ImGui::TableSetupColumn("ID");  ImGui::TableSetupColumn("Symbol");
            ImGui::TableSetupColumn("Side"); ImGui::TableSetupColumn("Qty");
            ImGui::TableSetupColumn("Price");
            ImGui::TableHeadersRow();
            for (auto& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.id);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.sym.c_str());
                ImGui::TableNextColumn();
                bool buy = r.side=="BUY";
                ImGui::TextColored(buy?ImVec4(0.20f,0.80f,0.45f,1):ImVec4(0.95f,0.30f,0.36f,1), "%s", r.side.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.4g", r.qty);
                ImGui::TableNextColumn(); ImGui::Text("%.4f", r.price);
            }
            ImGui::EndTable();
        }
    }

    void tab_bot_fills() {
        std::vector<SimState::BotFillRow> rows;
        {
            std::lock_guard lk(state_.mu);
            rows.assign(state_.bot_fill_log.begin(), state_.bot_fill_log.end());
        }
        std::reverse(rows.begin(), rows.end());
        ImGuiTableFlags f = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("bot_fills", 5, f)) {
            ImGui::TableSetupScrollFreeze(0,1);
            ImGui::TableSetupColumn("ID");  ImGui::TableSetupColumn("Symbol");
            ImGui::TableSetupColumn("Side"); ImGui::TableSetupColumn("Qty");
            ImGui::TableSetupColumn("Price");
            ImGui::TableHeadersRow();
            for (auto& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.id);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.sym.c_str());
                ImGui::TableNextColumn();
                bool buy = r.side=="BUY";
                ImGui::TextColored(buy?ImVec4(0.20f,0.80f,0.45f,1):ImVec4(0.95f,0.30f,0.36f,1), "%s", r.side.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.4g", r.qty);
                ImGui::TableNextColumn(); ImGui::Text("%.4f", r.price);
            }
            ImGui::EndTable();
        }
    }

    void tab_results() {

        std::vector<double> eq;
        double init_cash = 0.0;
        double sharpe = 0.0, sortino = 0.0, calmar = 0.0, max_dd = 0.0, vol = 0.0;
        double final_pnl = 0.0;
        double win_ratio = 0.0, profit_factor = 1.0;
        uint64_t fills_total = 0, orders_sent = 0, win_trades = 0, loss_trades = 0;
        uint64_t bot_fills = 0;
        double bot_vol = 0.0, maker_ratio = 0.0;
        uint64_t tick_num = 0, total_ticks = 0;
        std::string strat_name;
        {
            std::lock_guard lk(state_.mu);
            if (state_.equity.empty()) { ImGui::TextDisabled("Run a strategy to see results."); return; }
            eq          = state_.equity;
            init_cash   = state_.initial_cash;
            sharpe      = state_.sharpe;   sortino    = state_.sortino;
            calmar      = state_.calmar;   max_dd     = state_.max_dd;
            vol         = state_.vol;      final_pnl  = state_.final_pnl;
            win_ratio   = state_.win_ratio; profit_factor = state_.profit_factor;
            fills_total = state_.fills_total; orders_sent = state_.orders_sent;
            win_trades  = state_.win_trades; loss_trades = state_.loss_trades;
            bot_fills   = state_.bot_fills_total; bot_vol = state_.bot_volume;
            maker_ratio = state_.maker_ratio;
            tick_num    = state_.tick_num; total_ticks = state_.total_ticks;
            strat_name  = state_.strategy_name;
        }

        if (ImGui::Button(ICON_FA_FILE_EXPORT "  Export Report")) export_report();
        ImGui::SameLine();
        ImGui::TextDisabled("JSON + CSV + standalone HTML  ->  reports/");
        ImGui::Separator();

        int n = (int)eq.size();
        double pnl     = eq.back() - init_cash;
        double pnl_pct = init_cash > 0 ? pnl / init_cash * 100.0 : 0.0;

        std::vector<double> xs(n), rets(n > 1 ? n-1 : 0);
        for (int i = 0; i < n; ++i) xs[i] = (double)i;
        for (int i = 1; i < n; ++i)
            rets[i-1] = eq[i-1] > 0 ? (eq[i] - eq[i-1]) / eq[i-1] : 0.0;

        std::vector<double> dd_series(n, 0.0);
        double peak = eq[0];
        for (int i = 0; i < n; ++i) {
            if (eq[i] > peak) peak = eq[i];
            dd_series[i] = peak > 0 ? (eq[i] - peak) / peak * 100.0 : 0.0;
        }

        double ret_mean = 0.0;
        for (double r : rets) ret_mean += r;
        if (!rets.empty()) ret_mean /= (double)rets.size();

        float avail_h = ImGui::GetContentRegionAvail().y;
        float row1_h  = avail_h * 0.28f;
        float row2_h  = avail_h * 0.26f;
        float row3_h  = avail_h * 0.40f;

        ImVec4 curve_col = pnl >= 0 ? ImVec4(0.30f, 0.70f, 1.0f, 1.0f)
                                     : ImVec4(0.95f, 0.40f, 0.40f, 1.0f);

        if (ImPlot::BeginPlot("Equity Curve", ImVec2(-1, row1_h),
                              ImPlotFlags_Crosshairs | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("", "Portfolio Value",
                              ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.0f");
            ImPlot::SetNextLineStyle(curve_col, 1.5f);
            ImPlot::PlotLine("Strategy", xs.data(), eq.data(), n);
            ImPlot::EndPlot();
        }
        ImGui::TextDisabled("Equity Curve");

        if (ImPlot::BeginPlot("##Drawdown", ImVec2(-1, row2_h),
                              ImPlotFlags_Crosshairs | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("", "DD %",
                              ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "%.3f%%");
            static const double zero_line = 0.0;
            ImPlot::SetNextFillStyle(ImVec4(0.85f, 0.20f, 0.20f, 0.45f));
            ImPlot::PlotShaded("##DD", xs.data(), dd_series.data(), n, 0.0);
            ImPlot::SetNextLineStyle(ImVec4(0.55f, 0.10f, 0.10f, 1.0f), 1.2f);
            ImPlot::PlotLine("##DDLine", xs.data(), dd_series.data(), n);
            ImPlot::EndPlot();
        }
        ImGui::TextDisabled("Drawdown (%%)");

        ImGuiTableFlags tbl = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoHostExtendX;
        if (ImGui::BeginTable("##BottomRow", 2, tbl)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            if (!rets.empty() && ImPlot::BeginPlot("Returns Distribution (%%)",
                                                   ImVec2(-1, row3_h - 20.0f),
                                                   ImPlotFlags_Crosshairs)) {
                ImPlot::SetupAxes("Return %%", "Count",
                                  ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);

                ImPlot::SetNextFillStyle(ImVec4(0.20f, 0.70f, 0.30f, 0.75f));
                ImPlot::PlotHistogram("Returns", rets.data(), (int)rets.size(),
                                     ImPlotBin_Sqrt, 1.0, ImPlotRange(), ImPlotHistogramFlags_None);

                double meanv[2]  = {ret_mean, ret_mean};
                double meanvY[2] = {0.0, 1e9};
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 0.0f, 0.9f), 1.5f);
                ImPlot::PlotInfLines("mean", &ret_mean, 1);
                ImPlot::EndPlot();
            }

            ImGui::TableNextColumn();

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.70f, 1.0f), "%-20s  %-8s  %s",
                               "Metric", "", "Value");
            ImGui::Separator();

            ImGuiTableFlags mtbl_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders
                                       | ImGuiTableFlags_SizingStretchSame;
            if (ImGui::BeginTable("##MetricsTable", 2, mtbl_flags,
                                  ImVec2(-1, row3_h - 36.0f))) {
                ImGui::TableSetupColumn("Metric");
                ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();

                auto metric_row = [&](const char* name, const char* val,
                                      bool colored = false, bool good = true) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(name);
                    ImGui::TableNextColumn();
                    if (colored)
                        ImGui::TextColored(good ? ImVec4(0.25f,0.85f,0.50f,1) : ImVec4(0.95f,0.35f,0.35f,1), "%s", val);
                    else
                        ImGui::TextUnformatted(val);
                };

                char buf[64];
                snprintf(buf, sizeof buf, "%.2f%%", pnl_pct);
                metric_row("Total Return", buf, true, pnl >= 0);

                snprintf(buf, sizeof buf, "%.3f", sharpe);
                metric_row("Sharpe (ann)", buf, true, sharpe >= 0);

                snprintf(buf, sizeof buf, "%.3f", sortino);
                metric_row("Sortino", buf, true, sortino >= 0);

                snprintf(buf, sizeof buf, "%.3f", calmar);
                metric_row("Calmar", buf, true, calmar >= 0);

                snprintf(buf, sizeof buf, "-%.2f%%", max_dd * 100.0);
                metric_row("Max Drawdown", buf, true, false);

                snprintf(buf, sizeof buf, "%.2f%%", vol * 100.0);
                metric_row("Volatility", buf);

                snprintf(buf, sizeof buf, "%.3f", profit_factor);
                metric_row("Profit Factor", buf, true, profit_factor >= 1.0);

                snprintf(buf, sizeof buf, "%.1f%%", win_ratio * 100.0);
                metric_row("Win Rate", buf, true, win_ratio >= 0.5);

                snprintf(buf, sizeof buf, "%llu", (unsigned long long)fills_total);
                metric_row("Total Fills", buf);

                snprintf(buf, sizeof buf, "$%+.0f", final_pnl);
                metric_row("Final PnL", buf, true, final_pnl >= 0);

                ImGui::EndTable();
            }

            ImGui::EndTable();
        }
    }

    static std::string strategies_dir() {
        std::error_code ec; fs::create_directories("strategies", ec);
        return "strategies";
    }
    static const char* TEMPLATE() {
        return "// MyCustomStrategy.cpp\n"
               "#include <hft_simulator/strategies/custom_strat_api.hpp>\n"
               "#include <cstdio>\n\n"
               "extern \"C\" const char* strategy_name() {\n"
               "    return \"Custom SMA Strategy\";\n"
               "}\n\n"
               "extern \"C\" void on_init(int ticks, double initial_cash) {\n"
               "    // optional init logic\n"
               "}\n\n"
               "extern \"C\" StrategyOrder on_tick(StrategyContext ctx) {\n"
               "    // Buy if mid goes up, Sell if mid goes down\n"
               "    static double last_mid = 0.0;\n"
               "    StrategyOrder order = { Action::None, 0.0, 0.0 };\n"
               "    if (last_mid > 0.0) {\n"
               "        if (ctx.mid > last_mid && ctx.position < 100.0) {\n"
               "            order = { Action::Buy, 10.0, ctx.ask };\n"
               "        } else if (ctx.mid < last_mid && ctx.position > -100.0) {\n"
               "            order = { Action::Sell, 10.0, ctx.bid };\n"
               "        }\n"
               "    }\n"
               "    last_mid = ctx.mid;\n"
               "    return order;\n"
               "}\n";
    }

    static const std::vector<std::pair<std::string,std::string>>& strategy_templates() {
        static const std::vector<std::pair<std::string,std::string>> T = {
            {"Blank (SMA cross)", TEMPLATE()},
            {"Market Making", R"CPP(// market_making.cpp — Avellaneda-Stoikov style quoting
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <cmath>

namespace {
    const double GAMMA = 0.5;    // inventory aversion
    const double SPREAD_K = 0.5; // base half-spread (price units)
    const double SIZE = 20.0;
    const double MAX_POS = 200.0;
}

extern "C" const char* strategy_name() { return "Custom Market Maker"; }

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };
    // Reservation price skews quotes against current inventory.
    double reservation = ctx.mid - GAMMA * ctx.position * ctx.volatility;
    double half = SPREAD_K + GAMMA * ctx.volatility;
    double bid = reservation - half;
    double ask = reservation + half;
    // Quote the side that reduces inventory risk first.
    if (ctx.position <= 0 && ctx.position > -MAX_POS) return { Action::Buy,  SIZE, bid };
    if (ctx.position >  0 && ctx.position <  MAX_POS) return { Action::Sell, SIZE, ask };
    return { Action::None, 0, 0 };
}
)CPP"},
            {"Trend Following (Momentum)", R"CPP(// momentum.cpp — trend following on N-tick return
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>

namespace {
    const int LOOKBACK = 30;
    const double THRESH = 0.0003; // 0.03%
    const double SIZE = 50.0, MAX_POS = 200.0;
    std::deque<double> prices;
}

extern "C" const char* strategy_name() { return "Custom Momentum"; }
extern "C" void on_init(int, double) { prices.clear(); }

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };
    prices.push_back(ctx.mid);
    if ((int)prices.size() > LOOKBACK + 1) prices.pop_front();
    if ((int)prices.size() < LOOKBACK) return { Action::None, 0, 0 };
    double ret = (ctx.mid - prices.front()) / prices.front();
    if (ret >  THRESH && ctx.position <  MAX_POS) return { Action::Buy,  SIZE, ctx.ask };
    if (ret < -THRESH && ctx.position > -MAX_POS) return { Action::Sell, SIZE, ctx.bid };
    return { Action::None, 0, 0 };
}
)CPP"},
            {"Mean Reversion (Bollinger)", R"CPP(// mean_reversion.cpp — fade moves outside Bollinger bands
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>
#include <cmath>

namespace {
    const int W = 50;
    const double MULT = 2.0, SIZE = 40.0, MAX_POS = 160.0;
    std::deque<double> prices;
}

extern "C" const char* strategy_name() { return "Custom Mean Reversion"; }
extern "C" void on_init(int, double) { prices.clear(); }

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };
    prices.push_back(ctx.mid);
    if ((int)prices.size() > W) prices.pop_front();
    if ((int)prices.size() < W/2) return { Action::None, 0, 0 };
    double mean = 0; for (double p : prices) mean += p; mean /= prices.size();
    double var = 0; for (double p : prices) var += (p-mean)*(p-mean); var /= prices.size();
    double sd = std::sqrt(var);
    if (ctx.mid > mean + MULT*sd && ctx.position > -MAX_POS) return { Action::Sell, SIZE, ctx.bid };
    if (ctx.mid < mean - MULT*sd && ctx.position <  MAX_POS) return { Action::Buy,  SIZE, ctx.ask };
    if (std::fabs(ctx.mid - mean) < sd*0.3 && ctx.position != 0)
        return { Action::Close, std::fabs(ctx.position), ctx.mid };
    return { Action::None, 0, 0 };
}
)CPP"},
            {"RSI Reversal", R"CPP(// rsi.cpp — buy oversold, sell overbought
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>

namespace {
    const int PERIOD = 14;
    const double OS = 30.0, OB = 70.0, SIZE = 30.0, MAX_POS = 120.0;
    std::deque<double> prices;
}

extern "C" const char* strategy_name() { return "Custom RSI"; }
extern "C" void on_init(int, double) { prices.clear(); }

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };
    prices.push_back(ctx.mid);
    if ((int)prices.size() > PERIOD + 5) prices.pop_front();
    if ((int)prices.size() < PERIOD) return { Action::None, 0, 0 };
    double g = 0, l = 0;
    for (size_t i = 1; i < prices.size(); ++i) {
        double d = prices[i] - prices[i-1];
        if (d > 0) g += d; else l -= d;
    }
    int n = (int)prices.size() - 1;
    double rsi = (l == 0.0) ? 100.0 : 100.0 - 100.0/(1.0 + (g/n)/(l/n));
    if (rsi < OS && ctx.position <  MAX_POS) return { Action::Buy,  SIZE, ctx.ask };
    if (rsi > OB && ctx.position > -MAX_POS) return { Action::Sell, SIZE, ctx.bid };
    return { Action::None, 0, 0 };
}
)CPP"},
            {"Breakout (Donchian)", R"CPP(// breakout.cpp — Donchian channel breakout with stop-loss
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>
#include <algorithm>

namespace {
    const int CHANNEL = 20;
    const double STOP = 0.005, SIZE = 25.0, MAX_POS = 100.0;
    std::deque<double> prices;
    double entry = 0.0; int dir = 0;
}

extern "C" const char* strategy_name() { return "Custom Breakout"; }
extern "C" void on_init(int, double) { prices.clear(); entry = 0; dir = 0; }

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };
    prices.push_back(ctx.mid);
    if ((int)prices.size() > CHANNEL) prices.pop_front();
    if ((int)prices.size() < CHANNEL) return { Action::None, 0, 0 };
    double hi = *std::max_element(prices.begin(), prices.end());
    double lo = *std::min_element(prices.begin(), prices.end());
    if (dir == 1 && entry > 0 && ctx.mid < entry*(1-STOP)) { dir=0; entry=0; return { Action::Close, ctx.position, ctx.bid }; }
    if (dir == -1 && entry > 0 && ctx.mid > entry*(1+STOP)) { dir=0; entry=0; return { Action::Close, -ctx.position, ctx.ask }; }
    if (ctx.ask >= hi && ctx.position <  MAX_POS && dir != 1)  { dir=1;  entry=ctx.ask; return { Action::Buy,  SIZE, ctx.ask }; }
    if (ctx.bid <= lo && ctx.position > -MAX_POS && dir != -1) { dir=-1; entry=ctx.bid; return { Action::Sell, SIZE, ctx.bid }; }
    return { Action::None, 0, 0 };
}
)CPP"},
            {"Pairs / Spread (single-symbol)", R"CPP(// pairs_spread.cpp — mean-reversion of price vs its own rolling reference.
// NOTE: the custom-strategy ABI exposes ONE symbol per tick (StrategyContext),
// so a true two-leg pairs trade isn't expressible here. This skeleton trades
// the z-score of price vs a rolling mean (a single-leg proxy for the spread).
// For a real 2-leg pair, use the built-in "Statistical Arbitrage" strategy.
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>
#include <cmath>

namespace {
    const int W = 60;
    const double ENTRY_Z = 2.0, EXIT_Z = 0.3, SIZE = 30.0;
    std::deque<double> prices;
}

extern "C" const char* strategy_name() { return "Custom Pairs (proxy)"; }
extern "C" void on_init(int, double) { prices.clear(); }

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };
    prices.push_back(ctx.mid);
    if ((int)prices.size() > W) prices.pop_front();
    if ((int)prices.size() < W) return { Action::None, 0, 0 };
    double mean = 0; for (double p : prices) mean += p; mean /= W;
    double var = 0; for (double p : prices) var += (p-mean)*(p-mean); var /= W;
    double sd = std::sqrt(var);
    double z = sd > 0 ? (ctx.mid - mean) / sd : 0.0;
    if (std::fabs(z) < EXIT_Z && ctx.position != 0)
        return { Action::Close, std::fabs(ctx.position), ctx.mid };
    if (z >  ENTRY_Z && ctx.position >= 0) return { Action::Sell, SIZE, ctx.bid };
    if (z < -ENTRY_Z && ctx.position <= 0) return { Action::Buy,  SIZE, ctx.ask };
    return { Action::None, 0, 0 };
}
)CPP"},
            {"Order Types Demo (post-only/stop/iceberg)", R"CPP(// order_types.cpp - demonstrates the advanced order types.
// Set order.ord_type (OrderKind) + stop_price / display_qty on the returned order.
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>
#include <cmath>

namespace {
    std::deque<double> prices;
    double entry = 0.0;
}

extern "C" const char* strategy_name() { return "Order Types Demo"; }
extern "C" void on_init(int, double) { prices.clear(); entry = 0; }

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0) return { Action::None, 0, 0 };
    prices.push_back(ctx.mid);
    if ((int)prices.size() > 30) prices.pop_front();
    if ((int)prices.size() < 30) return { Action::None, 0, 0 };

    // Flat -> enter long working a large order quietly via an ICEBERG
    // (show only 20 of 100) as a POST-ONLY maker bid (earns rebate, never crosses).
    if (ctx.position == 0) {
        entry = ctx.mid;
        StrategyOrder o = { Action::Buy, 100.0, ctx.bid };
        o.ord_type = OK_Iceberg;       // hidden size
        o.display_qty = 20.0;          // only 20 visible at a time
        return o;
    }

    // While long, protect with a STOP that becomes a market order on trigger.
    if (ctx.position > 0) {
        StrategyOrder o = { Action::Sell, ctx.position, 0.0 };
        o.ord_type = OK_Stop;
        o.stop_price = entry * 0.99;   // 1% stop-loss
        return o;
    }
    return { Action::None, 0, 0 };
}
)CPP"},
        };
        return T;
    }

    void ed_new_from_template(int idx) {
        const auto& T = strategy_templates();
        if (idx < 0 || idx >= (int)T.size()) return;
        static int n = 1;
        ed_cur_ = "strategies/strategy_" + std::to_string(n++) + ".cpp";
        while (fs::exists(ed_cur_))
            ed_cur_ = "strategies/strategy_" + std::to_string(n++) + ".cpp";
        ed_te_.SetText(T[idx].second);
        ed_save(); ed_refresh();
        ed_console_ += "% created " + ed_cur_ + " from template: " + T[idx].first + "\n";
    }

    void ed_refresh() {
        ed_files_.clear();
        std::error_code ec;
        if (fs::exists("strategies")) {
            for (auto& e : fs::directory_iterator("strategies", ec)) {
                if (e.path().extension() == ".cpp") {
                    ed_files_.push_back({e.path().string(), true});
                }
            }
        }
        std::sort(ed_files_.begin(), ed_files_.end());
    }
    void ed_load(const std::string& path) {
        std::ifstream f(path);
        std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ed_te_.SetText(txt);
        ed_cur_ = path;
    }
    void ed_save() {
        if (ed_cur_.empty()) return;
        std::ofstream f(ed_cur_); f << ed_te_.GetText();
        ed_console_ += "saved " + ed_cur_ + "\n";
    }
    void ed_new() {
        static int n = 1;
        ed_cur_ = "strategies/strategy_" + std::to_string(n++) + ".cpp";
        while (fs::exists(ed_cur_)) {
            ed_cur_ = "strategies/strategy_" + std::to_string(n++) + ".cpp";
        }
        ed_te_.SetText(TEMPLATE());
        ed_save(); ed_refresh();
    }
    void ed_delete(const std::string& path) {
        std::error_code ec; fs::remove(path, ec);
        if (ed_cur_==path) { ed_cur_.clear(); ed_te_.SetText(""); }
        ed_console_ += "deleted " + path + "\n";
        ed_refresh();
    }
    void tab_editor() {
        if (!ed_init_) {
            ed_te_.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
            ed_te_.SetPalette(TextEditor::GetDarkPalette());
            ed_te_.SetShowWhitespaces(false);
            ed_refresh(); ed_init_ = true;
        }

        ImGui::BeginChild("ed_files", ImVec2(240, 0), ImGuiChildFlags_Border);
        if (ImGui::Button(ICON_FA_PLUS "  New file", ImVec2(-1, 0))) ed_new();

        {
            const auto& T = strategy_templates();
            std::vector<const char*> names;
            for (const auto& t : T) names.push_back(t.first.c_str());
            ImGui::TextDisabled("Template");
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##tmpl", &ed_template_idx_, names.data(), (int)names.size());
            if (ImGui::Button(ICON_FA_FILE_CIRCLE_PLUS "  New from Template", ImVec2(-1, 0)))
                ed_new_from_template(ed_template_idx_);
        }
        ImGui::Separator();
        for (auto& [path, del] : ed_files_) {
            std::string name = fs::path(path).filename().string();
            if (del) name = ICON_FA_PEN " " + name; else name = ICON_FA_TABLE_LIST " " + name;
            ImGui::PushID(path.c_str());
            if (ImGui::Selectable(name.c_str(), ed_cur_==path, 0, ImVec2(del?190:0, 0)))
                ed_load(path);
            if (del) {
                ImGui::SameLine(196);
                if (ImGui::SmallButton(ICON_FA_TRASH)) { ImGui::PopID(); ed_delete(path); break; }
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("ed_main", ImVec2(0, 0));
        ImGui::TextDisabled("%s", ed_cur_.empty() ? "(no file)" : ed_cur_.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 220);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save")) ed_save();
        ImGui::SameLine();
        if (ed_proc_.running()) ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_PLAY " Compile") && !ed_cur_.empty()) {
            ed_save(); ed_console_.clear();
            fs::path cpp_path(ed_cur_);
            fs::path dylib_path = cpp_path;
            dylib_path.replace_extension(".dylib");

            std::string cmd = "clang++ -std=c++17 -shared -fPIC -undefined dynamic_lookup -Iinclude -o \""
                + dylib_path.string() + "\" \"" + cpp_path.string() + "\" 2>&1";

            ed_console_ += "% compiling " + cpp_path.filename().string() + " ...\n";
            ed_console_ += "% " + cmd + "\n";
            ed_proc_.start(cmd);
        }
        if (ed_proc_.running()) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STOP " Stop")) ed_proc_.stop();

        float ch = ImGui::GetContentRegionAvail().y;
        auto cpos = ed_te_.GetCursorPosition();
        ImGui::TextDisabled("Ln %d, Col %d  |  %d lines  |  %s", cpos.mLine+1, cpos.mColumn+1,
                            ed_te_.GetTotalLines(), ed_te_.CanUndo() ? "*" : " ");
        ed_te_.Render("##code", ImVec2(-1, ch*0.62f), true);
        ImGui::TextDisabled("Console");
        ImVec4 termbg = dark_ ? ImVec4(0.05f,0.05f,0.06f,1) : ImVec4(0.10f,0.11f,0.13f,1);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, termbg);
        if (font_mono_) ImGui::PushFont(font_mono_);
        ImGui::BeginChild("ed_console", ImVec2(-1, -1), ImGuiChildFlags_Border);

        float footer = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("ed_scrollback", ImVec2(0, -footer), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        std::istringstream ss(ed_console_); std::string line;
        while (std::getline(ss, line)) {
            ImVec4 col(0.82f,0.84f,0.88f,1);
            if (line.size()>2 && line[0]=='%' && line[1]==' ') col = ImVec4(0.55f,0.80f,0.55f,1);
            else if (line.find("Error")!=std::string::npos || line.find("error")!=std::string::npos ||
                     line.find("Traceback")!=std::string::npos) col = ImVec4(0.95f,0.45f,0.45f,1);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()-4) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        std::string p = term_prompt();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f,0.70f,0.95f,1));
        ImGui::TextUnformatted(p.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);
        ImGui::SetNextItemWidth(-1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0,0,0,0));
        ImGuiInputTextFlags fl = ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_CallbackHistory;
        if (ed_term_focus_) { ImGui::SetKeyboardFocusHere(); ed_term_focus_ = false; }
        bool entered = ImGui::InputText("##ed_cmdline", &ed_term_cmd_, fl, ed_term_hist_cb, this);
        ImGui::PopStyleColor();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive())
            ed_term_focus_ = true;
        if (entered) {
            if (!ed_term_cmd_.empty()) {
                ed_console_ += p + " " + ed_term_cmd_ + "\n";
                ed_term_proc_.start("PYTHONPATH=python " + ed_term_cmd_ + " 2>&1");
                ed_term_hist_.push_back(ed_term_cmd_);
                ed_term_hist_pos_ = -1; ed_term_cmd_.clear();
            }
            ed_term_focus_ = true;
        }

        ImGui::EndChild();
        if (font_mono_) ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::EndChild();
    }

    std::string term_prompt() {
        const char* u = std::getenv("USER");
        std::string user = u ? u : "user";
        std::string dir;
        std::error_code ec; auto cwd = fs::current_path(ec);
        dir = ec ? "~" : cwd.filename().string();
        return user + "@quantsim " + dir + " %";
    }

    static int term_hist_cb(ImGuiInputTextCallbackData* d) {
        App* self = (App*)d->UserData;
        if (self->term_hist_.empty()) return 0;
        int prev = self->term_hist_pos_;
        if (d->EventKey == ImGuiKey_UpArrow) {
            if (self->term_hist_pos_ < 0) self->term_hist_pos_ = (int)self->term_hist_.size()-1;
            else if (self->term_hist_pos_ > 0) self->term_hist_pos_--;
        } else if (d->EventKey == ImGuiKey_DownArrow) {
            if (self->term_hist_pos_ >= 0) self->term_hist_pos_++;
            if (self->term_hist_pos_ >= (int)self->term_hist_.size()) self->term_hist_pos_ = -1;
        }
        if (prev != self->term_hist_pos_) {
            const std::string& s = self->term_hist_pos_ >= 0 ? self->term_hist_[self->term_hist_pos_] : self->term_empty_;
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, s.c_str());
        }
        return 0;
    }

    static int ed_term_hist_cb(ImGuiInputTextCallbackData* d) {
        App* self = (App*)d->UserData;
        if (self->ed_term_hist_.empty()) return 0;
        int prev = self->ed_term_hist_pos_;
        if (d->EventKey == ImGuiKey_UpArrow) {
            if (self->ed_term_hist_pos_ < 0) self->ed_term_hist_pos_ = (int)self->ed_term_hist_.size()-1;
            else if (self->ed_term_hist_pos_ > 0) self->ed_term_hist_pos_--;
        } else if (d->EventKey == ImGuiKey_DownArrow) {
            if (self->ed_term_hist_pos_ >= 0) self->ed_term_hist_pos_++;
            if (self->ed_term_hist_pos_ >= (int)self->ed_term_hist_.size()) self->ed_term_hist_pos_ = -1;
        }
        if (prev != self->ed_term_hist_pos_) {
            const std::string& s = self->ed_term_hist_pos_ >= 0 ? self->ed_term_hist_[self->ed_term_hist_pos_] : self->term_empty_;
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, s.c_str());
        }
        return 0;
    }

    void tab_terminal() {

        if (ImGui::SmallButton(ICON_FA_TRASH " clear")) term_out_.clear();
        ImGui::SameLine();
        if (term_proc_.running()) { ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.20f,0.80f,0.45f,1), ICON_FA_CIRCLE " running"); }

        ImVec4 termbg = dark_ ? ImVec4(0.05f,0.05f,0.06f,1) : ImVec4(0.10f,0.11f,0.13f,1);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, termbg);
        if (font_mono_) ImGui::PushFont(font_mono_);
        ImGui::BeginChild("term", ImVec2(0,0), ImGuiChildFlags_Border);

        float footer = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("scrollback", ImVec2(0, -footer), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        std::istringstream ss(term_out_); std::string line;
        while (std::getline(ss, line)) {
            ImVec4 col(0.82f,0.84f,0.88f,1);
            if (line.size()>2 && line[0]=='%' && line[1]==' ') col = ImVec4(0.55f,0.80f,0.55f,1);
            else if (line.find("Error")!=std::string::npos || line.find("error")!=std::string::npos ||
                     line.find("Traceback")!=std::string::npos) col = ImVec4(0.95f,0.45f,0.45f,1);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()-4) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        std::string p = term_prompt();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f,0.70f,0.95f,1));
        ImGui::TextUnformatted(p.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);
        ImGui::SetNextItemWidth(-1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0,0,0,0));
        ImGuiInputTextFlags fl = ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_CallbackHistory;
        if (term_focus_) { ImGui::SetKeyboardFocusHere(); term_focus_ = false; }
        bool entered = ImGui::InputText("##cmdline", &term_cmd_, fl, term_hist_cb, this);
        ImGui::PopStyleColor();

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive())
            term_focus_ = true;
        if (entered) {
            if (!term_cmd_.empty()) {
                term_out_ += p + " " + term_cmd_ + "\n";
                term_proc_.start("PYTHONPATH=python " + term_cmd_ + " 2>&1");
                term_hist_.push_back(term_cmd_);
                term_hist_pos_ = -1; term_cmd_.clear();
            }
            term_focus_ = true;
        }

        ImGui::EndChild();
        if (font_mono_) ImGui::PopFont();
        ImGui::PopStyleColor();
    }

    void on_run(const char* strat, bool crypto) {
        state_.save_to_prev();
        log_text_.clear();
        if (crypto) {
            {
                std::lock_guard lk(state_.mu);
                state_.live_mode = true; state_.initial_cash = cash_;
                state_.equity.clear(); state_.fill_log.clear();
                state_.live_bids.clear(); state_.live_asks.clear();
                state_.fills_total = state_.orders_sent = state_.orders_blocked = 0;
                state_.pos_net = state_.pos_avg = state_.pos_realized = state_.pos_unreal = 0;
                state_.risk_state = "OK"; state_.risk_detail.clear();
                state_.running = true; state_.done = false; state_.tick_num = 0;
                state_.strategy_name = strat;
            }
            live_.start(symbol_, strat, cash_, dylib_path_);
        } else {
            {
                std::lock_guard lk(state_.mu);
                state_.live_mode = false;
                state_.pf_n_assets     = n_assets_;
                state_.pf_sigma_common = sigma_common_;
                state_.pf_sigma_idio   = sigma_idio_;
            }
            bool csv_replay = (source_idx_ == 1);

            if (csv_replay && std::string(strat) == "portfolio") {
                bool empty; { std::lock_guard lk(state_.mu);
                    empty = state_.csv_paths_multi.find_first_not_of(" \t\r\n") == std::string::npos; }
                if (empty) toast("No CSV files listed — running synthetic factor model instead.",
                                 ImVec4(0.95f,0.65f,0.20f,1), 5.0);
            }
            int save_slot = save_as_compare_ ? save_slot_idx_ : -1;
            runner_.start(strat, ticks_, sigma_, seed_, cash_, enable_bots_, [this, save_slot]{
                if (save_slot >= 0) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Saved as Compare Slot %d", save_slot + 1);
                    toast(msg, ImVec4(0.90f,0.62f,0.15f,1), 4.0);
                    save_as_compare_ = false;
                }
            }, csv_replay, save_slot);
        }
    }

public:
    SimState   state_;
    SimRunner  runner_;
    LiveRunner live_;
    ImFont*    font_big_  = nullptr;
    ImFont*    font_mono_ = nullptr;
    bool       dark_      = true;

    std::vector<std::string> dylibs_;
    bool   ed_was_compiling_ = false;

    void refresh_dylibs() {
        dylibs_.clear();
        std::error_code ec;
        if (fs::exists("strategies")) {
            for (auto& e : fs::directory_iterator("strategies", ec)) {
                if (e.path().extension() == ".dylib") {
                    dylibs_.push_back(e.path().filename().string());
                }
            }
        }
        std::sort(dylibs_.begin(), dylibs_.end());
    }

    int    strat_idx_  = 0;
    int    source_idx_ = 0;
    char   symbol_[32] = "BTCUSDT";
    char   dylib_path_[512] = {};
    int    ticks_      = 5000;
    double sigma_      = 0.0008;
    int    seed_       = 42;
    double cash_       = 500000.0;

    int    n_assets_       = 4;
    double sigma_common_   = 0.0006;
    double sigma_idio_     = 0.0004;
    bool   enable_bots_      = true;
    bool   show_bot_pnl_chart_ = true;
    bool   show_heatmap_       = true;
    bool   save_as_compare_    = false;
    int    save_slot_idx_      = 0;
    struct QueuedRun {
        std::string strategy_id;
        std::string dylib_path;
    };
    bool   batch_select_[7]    = { true, true, true, true, true, true, true };
    std::unordered_map<std::string, bool> batch_custom_select_;
    std::vector<QueuedRun> run_queue_;
    int    run_queue_idx_      = -1;
    std::string log_text_;

    struct SweepAxis { bool on=false; bool inited=false; float lo=0, hi=1; int steps=6; };
    int    opt_strat_idx_ = 0;
    int    opt_objective_ = 0;
    std::unordered_map<std::string, std::unordered_map<std::string, SweepAxis>> opt_axes_;
    std::string sweep_strat_;
    std::vector<std::string> sweep_keys_;
    std::vector<double> sweep_av_, sweep_bv_;
    std::vector<ParamMap> sweep_queue_;
    std::vector<float> sweep_results_;
    int    sweep_na_ = 0, sweep_nb_ = 1;
    int    sweep_idx_ = -1;
    int    sweep_pending_ = -1;
    int    sweep_best_ = -1;
    bool   sweep_objhib_ = true;

    int    robust_n_ = 12;
    int    robust_idx_ = -1, robust_pending_ = -1;
    std::string robust_strat_;
    ParamMap robust_params_;
    std::vector<float> robust_sharpe_, robust_pnl_;

    std::string ed_cur_, ed_console_;
    std::vector<std::pair<std::string,bool>> ed_files_;
    ProcRunner  ed_proc_;
    TextEditor  ed_te_;
    bool        ed_init_ = false;
    int         ed_template_idx_ = 0;
    std::string ed_term_cmd_;
    ProcRunner  ed_term_proc_;
    std::vector<std::string> ed_term_hist_;
    int         ed_term_hist_pos_ = -1;
    bool        ed_term_focus_ = true;

    std::string term_cmd_, term_out_;
    ProcRunner  term_proc_;
    std::vector<std::string> term_hist_;
    int         term_hist_pos_ = -1;
    bool        term_focus_ = true;
    std::string term_empty_;

    struct Toast { std::string msg; double expire; ImVec4 col; };
    std::vector<Toast> toasts_;
    bool prev_done_ = true;
    std::string prev_risk_ = "OK";

    void toast(const std::string& m, ImVec4 col, double secs = 4.0) {
        toasts_.push_back({ m, ImGui::GetTime() + secs, col });
        if (toasts_.size() > 5) toasts_.erase(toasts_.begin());
    }

    void render_toasts() {
        double now = ImGui::GetTime();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float y = vp->WorkPos.y + vp->WorkSize.y - 20;
        for (int i = (int)toasts_.size()-1; i >= 0; --i) {
            if (now > toasts_[i].expire) { toasts_.erase(toasts_.begin()+i); continue; }
        }
        for (int i = (int)toasts_.size()-1; i >= 0; --i) {
            auto& t = toasts_[i];
            float alpha = (float)std::clamp((t.expire - now)/0.6, 0.0, 1.0);
            ImGui::SetNextWindowBgAlpha(0.92f * alpha);
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20, y),
                                    ImGuiCond_Always, ImVec2(1, 1));
            char id[32]; snprintf(id, sizeof id, "##toast%d", i);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(t.col.x,t.col.y,t.col.z,alpha));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
            ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
                         ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing|
                         ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoSavedSettings);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(t.col.x,t.col.y,t.col.z,alpha));
            ImGui::TextUnformatted(t.msg.c_str());
            ImGui::PopStyleColor();
            y = ImGui::GetWindowPos().y - 8;
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
    }

    void export_report() {
        ReportData d;
        {
            std::lock_guard lk(state_.mu);
            if (state_.equity.empty()) {
                toast("No run to export — run a strategy first.", ImVec4(0.95f,0.30f,0.36f,1));
                return;
            }
            d.strategy        = state_.strategy_name.empty() ? "strategy" : state_.strategy_name;
            d.initial_cash    = state_.initial_cash;
            d.equity          = state_.equity;
            d.final_pnl       = state_.equity.back() - state_.initial_cash;
            d.total_return_pct= state_.initial_cash > 0 ? d.final_pnl/state_.initial_cash*100.0 : 0;
            d.sharpe=state_.sharpe; d.sortino=state_.sortino; d.calmar=state_.calmar;
            d.max_dd=state_.max_dd; d.vol=state_.vol;
            d.win_ratio=state_.win_ratio; d.profit_factor=state_.profit_factor;
            d.fills_total=state_.fills_total; d.win_trades=state_.win_trades; d.loss_trades=state_.loss_trades;
            for (auto& fr : state_.fill_log) d.fills.push_back({fr.sym, fr.side, fr.qty, fr.price});
        }
        d.timestamp = report_timestamp();
        std::string safe;
        for (char ch : d.strategy) safe += (std::isalnum((unsigned char)ch) ? ch : '_');
        std::error_code ec; fs::create_directories("reports", ec);
        std::string base = "reports/" + safe + "_" + d.timestamp;
        bool ok = export_json(base+".json", d) && export_csv(base+".csv", d) && export_html(base+".html", d);
        if (ok) {
            toast(ICON_FA_FILE_EXPORT "  Report -> " + base + ".html", ImVec4(0.20f,0.80f,0.45f,1), 6.0);
            std::string cmd = "open '" + base + ".html' >/dev/null 2>&1 &";
            std::system(cmd.c_str());
        } else {
            toast("Export failed (could not write reports/)", ImVec4(0.95f,0.30f,0.36f,1));
        }
    }

    void check_events() {
        bool done; std::string risk; double pnl, sharpe; std::string strat;
        {
            std::lock_guard lk(state_.mu);
            done = state_.done; risk = state_.risk_state; strat = state_.strategy_name;
            pnl = state_.equity.empty()?0:state_.equity.back()-state_.initial_cash;
            sharpe = state_.sharpe;
        }
        if (done && !prev_done_) {
            char b[128];
            snprintf(b, sizeof b, "%s complete  ·  PnL $%+.2f  ·  Sharpe %.2f",
                     strat.c_str(), pnl, sharpe);
            toast(b, pnl>=0 ? ImVec4(0.20f,0.80f,0.45f,1) : ImVec4(0.95f,0.30f,0.36f,1), 6);
        }
        prev_done_ = done;
        if (risk != prev_risk_) {
            if (risk == "ARMED")   toast(ICON_FA_TRIANGLE_EXCLAMATION " Kill switch ARMED", ImVec4(0.95f,0.65f,0.20f,1), 6);
            if (risk == "BLOCKED") toast(ICON_FA_BAN " Orders BLOCKED", ImVec4(0.95f,0.30f,0.36f,1), 6);
            prev_risk_ = risk;
        }
    }

    void pump() {
        log_text_   += live_.drain_log();
        ed_console_ += ed_proc_.drain();

        bool is_compiling = ed_proc_.running();
        if (ed_was_compiling_ && !is_compiling) {
            refresh_dylibs();
            ed_console_ += "% compilation finished.\n";
        }
        ed_was_compiling_ = is_compiling;

        if (run_queue_idx_ >= 0) {
            bool sim_running = state_.running;
            if (!sim_running) {
                if (run_queue_idx_ < (int)run_queue_.size()) {
                    auto& qr = run_queue_[run_queue_idx_];
                    {
                        std::lock_guard lk(state_.mu);
                        state_.live_mode = false;
                        state_.custom_dylib_path = qr.dylib_path;
                    }
                    bool csv_replay = (source_idx_ == 1);
                    int target_slot = run_queue_idx_;
                    runner_.start(qr.strategy_id, ticks_, sigma_, seed_, cash_, enable_bots_, [this, target_slot]{

                    }, csv_replay, target_slot);

                    char msg[256];
                    if (!qr.dylib_path.empty()) {
                        snprintf(msg, sizeof(msg), "[Batch %d/%d] Running C++ %s...", 
                                 run_queue_idx_ + 1, (int)run_queue_.size(), fs::path(qr.dylib_path).filename().string().c_str());
                    } else {
                        snprintf(msg, sizeof(msg), "[Batch %d/%d] Running %s...", 
                                 run_queue_idx_ + 1, (int)run_queue_.size(), qr.strategy_id.c_str());
                    }
                    toast(msg, ImVec4(0.10f,0.75f,0.75f,1.0f), 3.0);

                    run_queue_idx_++;
                } else {
                    run_queue_idx_ = -1;
                    toast("Batch comparison complete!", ImVec4(0.20f,0.80f,0.45f,1.0f), 5.0);
                }
            }
        }

        if (sweep_idx_ >= 0 && !state_.running) {
            if (sweep_pending_ >= 0) {
                double obj;
                {
                    std::lock_guard lk(state_.mu);
                    obj = opt_objective_==0 ? state_.sharpe
                        : opt_objective_==1 ? state_.final_pnl
                                            : state_.max_dd * 100.0;
                }
                if (sweep_pending_ < (int)sweep_results_.size())
                    sweep_results_[sweep_pending_] = (float)obj;
                sweep_pending_ = -1;
            }
            if (sweep_idx_ < (int)sweep_queue_.size()) {
                {
                    std::lock_guard lk(state_.mu);
                    state_.live_mode = false;
                    state_.params[sweep_strat_] = sweep_queue_[sweep_idx_];
                }
                runner_.start(sweep_strat_, ticks_, sigma_, seed_, cash_, enable_bots_,
                              []{}, source_idx_ == 1, -1);
                sweep_pending_ = sweep_idx_;
                ++sweep_idx_;
            } else {
                sweep_idx_ = -1;
                int bi = -1; double bv = sweep_objhib_ ? -1e300 : 1e300;
                for (int i = 0; i < (int)sweep_results_.size(); ++i) {
                    double v = sweep_results_[i];
                    if (sweep_objhib_ ? v > bv : v < bv) { bv = v; bi = i; }
                }
                sweep_best_ = bi;
                toast(ICON_FA_SLIDERS "  Sweep complete", ImVec4(0.20f,0.80f,0.45f,1), 5.0);
            }
        }

        if (robust_idx_ >= 0 && !state_.running) {
            if (robust_pending_ >= 0) {
                double sh, pn;
                { std::lock_guard lk(state_.mu); sh = state_.sharpe; pn = state_.final_pnl; }
                if (robust_pending_ < (int)robust_sharpe_.size()) {
                    robust_sharpe_[robust_pending_] = (float)sh;
                    robust_pnl_[robust_pending_]    = (float)pn;
                }
                robust_pending_ = -1;
            }
            if (robust_idx_ < robust_n_) {
                {
                    std::lock_guard lk(state_.mu);
                    state_.live_mode = false;
                    state_.params[robust_strat_] = robust_params_;
                }
                runner_.start(robust_strat_, ticks_, sigma_, seed_ + 1000 + robust_idx_,
                              cash_, enable_bots_, []{}, source_idx_ == 1, -1);
                robust_pending_ = robust_idx_;
                ++robust_idx_;
            } else {
                robust_idx_ = -1;
                toast(ICON_FA_CHECK "  Robustness validation complete", ImVec4(0.20f,0.80f,0.45f,1), 5.0);
            }
        }

        ed_console_ += ed_term_proc_.drain();
        term_out_   += term_proc_.drain();
        check_events();
    }
};

int run_imgui_app() {
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* win = glfwCreateWindow(1480, 900, "QuantSim — HFT Simulation Platform", nullptr, nullptr);
    if (!win) { fprintf(stderr, "window failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    apply_theme(true);

    float xs=1, ys=1; glfwGetWindowContentScale(win, &xs, &ys);
    float scale = xs > 0 ? xs : 1.0f;
    App app;
    const char* ui_fonts[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    const char* mono_fonts[] = {
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Supplemental/Menlo.ttc",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Supplemental/Courier New.ttf",
    };
    auto pick = [](const char* const* arr, int n){ for (int i=0;i<n;++i) if (FILE* fp=fopen(arr[i],"rb")){fclose(fp);return arr[i];} return (const char*)nullptr; };
    const char* uif  = pick(ui_fonts,   3);
    const char* monf = pick(mono_fonts, 4);

    static const ImWchar icon_range[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    const char* fa_path = nullptr;
    const char* candidates[] = {
        "third_party/fa-solid-900.ttf",
        "../third_party/fa-solid-900.ttf",
        "../../third_party/fa-solid-900.ttf",
        "/Users/ace/Documents/collage/HFT/third_party/fa-solid-900.ttf"
    };
    for (const char* c : candidates) {
        if (FILE* fp = fopen(c, "rb")) {
            fclose(fp);
            fa_path = c;
            break;
        }
    }
    auto merge_icons = [&](float sz){
        if (!fa_path) return;
        ImFontConfig cfg; cfg.MergeMode = true; cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = sz; cfg.GlyphOffset.y = 1.0f*scale;
        io.Fonts->AddFontFromFileTTF(fa_path, sz, &cfg, icon_range);
    };

    if (uif) io.Fonts->AddFontFromFileTTF(uif, 16.0f*scale); else io.Fonts->AddFontDefault();
    merge_icons(14.0f*scale);

    if (uif) { app.font_big_ = io.Fonts->AddFontFromFileTTF(uif, 24.0f*scale); merge_icons(20.0f*scale); }

    if (monf) { app.font_mono_ = io.Fonts->AddFontFromFileTTF(monf, 14.5f*scale); merge_icons(13.0f*scale); }
    io.FontGlobalScale = 1.0f / scale;
    ImGui::GetStyle().ScaleAllSizes(1.0f);

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 410");

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.pump();
        app.frame();

        ImGui::Render();
        int dw, dh; glfwGetFramebufferSize(win, &dw, &dh);
        glViewport(0, 0, dw, dh);
        if (app.dark_) glClearColor(0.04f, 0.04f, 0.05f, 1.0f);
        else           glClearColor(0.92f, 0.93f, 0.95f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    app.runner_.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}

}

int main() { return hft::run_imgui_app(); }

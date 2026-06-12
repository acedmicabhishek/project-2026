#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cmath>
#include <cstdint>

namespace hft {

struct ReportData {
    std::string strategy;
    double initial_cash{0};
    double final_pnl{0};
    double total_return_pct{0};
    double sharpe{0}, sortino{0}, calmar{0}, max_dd{0}, vol{0};
    double win_ratio{0}, profit_factor{0};
    uint64_t fills_total{0}, win_trades{0}, loss_trades{0};
    std::vector<double> equity;
    struct Fill { std::string sym, side; double qty, price; };
    std::vector<Fill> fills;
    std::string timestamp;
};

inline std::string report_timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

inline std::vector<double> drawdown_series(const std::vector<double>& eq) {
    std::vector<double> dd(eq.size(), 0.0);
    if (eq.empty()) return dd;
    double peak = eq[0];
    for (size_t i = 0; i < eq.size(); ++i) {
        if (eq[i] > peak) peak = eq[i];
        dd[i] = peak > 0 ? (eq[i] - peak) / peak * 100.0 : 0.0;
    }
    return dd;
}

inline bool export_json(const std::string& path, const ReportData& d) {
    std::ofstream f(path);
    if (!f) return false;
    f << std::setprecision(10);
    f << "{\n";
    f << "  \"strategy\": \"" << d.strategy << "\",\n";
    f << "  \"timestamp\": \"" << d.timestamp << "\",\n";
    f << "  \"initial_cash\": " << d.initial_cash << ",\n";
    f << "  \"final_pnl\": " << d.final_pnl << ",\n";
    f << "  \"total_return_pct\": " << d.total_return_pct << ",\n";
    f << "  \"sharpe\": " << d.sharpe << ",\n";
    f << "  \"sortino\": " << d.sortino << ",\n";
    f << "  \"calmar\": " << d.calmar << ",\n";
    f << "  \"max_drawdown\": " << d.max_dd << ",\n";
    f << "  \"volatility\": " << d.vol << ",\n";
    f << "  \"win_ratio\": " << d.win_ratio << ",\n";
    f << "  \"profit_factor\": " << d.profit_factor << ",\n";
    f << "  \"fills_total\": " << d.fills_total << ",\n";
    f << "  \"win_trades\": " << d.win_trades << ",\n";
    f << "  \"loss_trades\": " << d.loss_trades << ",\n";
    f << "  \"equity\": [";
    for (size_t i = 0; i < d.equity.size(); ++i) { if (i) f << ","; f << d.equity[i]; }
    f << "],\n";
    f << "  \"fills\": [\n";
    for (size_t i = 0; i < d.fills.size(); ++i) {
        const auto& fl = d.fills[i];
        f << "    {\"symbol\":\"" << fl.sym << "\",\"side\":\"" << fl.side
          << "\",\"qty\":" << fl.qty << ",\"price\":" << fl.price << "}";
        f << (i + 1 < d.fills.size() ? ",\n" : "\n");
    }
    f << "  ]\n}\n";
    return true;
}

inline bool export_csv(const std::string& path, const ReportData& d) {
    std::ofstream f(path);
    if (!f) return false;
    f << std::setprecision(10);
    f << "# QuantSim Report," << d.strategy << "," << d.timestamp << "\n";
    f << "metric,value\n";
    f << "initial_cash," << d.initial_cash << "\n";
    f << "final_pnl," << d.final_pnl << "\n";
    f << "total_return_pct," << d.total_return_pct << "\n";
    f << "sharpe," << d.sharpe << "\n";
    f << "sortino," << d.sortino << "\n";
    f << "calmar," << d.calmar << "\n";
    f << "max_drawdown," << d.max_dd << "\n";
    f << "volatility," << d.vol << "\n";
    f << "win_ratio," << d.win_ratio << "\n";
    f << "profit_factor," << d.profit_factor << "\n";
    f << "fills_total," << d.fills_total << "\n";
    f << "win_trades," << d.win_trades << "\n";
    f << "loss_trades," << d.loss_trades << "\n";
    auto dd = drawdown_series(d.equity);
    f << "\ntick,equity,drawdown_pct\n";
    for (size_t i = 0; i < d.equity.size(); ++i)
        f << i << "," << d.equity[i] << "," << dd[i] << "\n";
    f << "\nfill_idx,symbol,side,qty,price\n";
    for (size_t i = 0; i < d.fills.size(); ++i)
        f << i << "," << d.fills[i].sym << "," << d.fills[i].side << ","
          << d.fills[i].qty << "," << d.fills[i].price << "\n";
    return true;
}

inline bool export_html(const std::string& path, const ReportData& d) {
    std::ofstream f(path);
    if (!f) return false;
    f << std::setprecision(8);

    auto js_array = [&](const std::vector<double>& v) {
        std::ostringstream s; s << "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) s << ","; s << v[i]; }
        s << "]"; return s.str();
    };
    std::vector<double> pnl(d.equity.size());
    for (size_t i = 0; i < d.equity.size(); ++i) pnl[i] = d.equity[i] - d.initial_cash;
    auto dd = drawdown_series(d.equity);

    auto metric = [&](const char* name, double val, const char* fmt) {
        char buf[64]; snprintf(buf, sizeof buf, fmt, val);
        f << "<div class='card'><div class='k'>" << name << "</div><div class='v'>"
          << buf << "</div></div>\n";
    };

    f << "<!doctype html><html><head><meta charset='utf-8'>\n";
    f << "<title>QuantSim Tearsheet — " << d.strategy << "</title>\n";
    f << "<style>\n"
         "body{background:#0d0d12;color:#e6e8f0;font-family:-apple-system,Segoe UI,Roboto,sans-serif;margin:0;padding:28px}\n"
         "h1{font-weight:600;margin:0 0 2px;color:#9aa0ff}h1 .s{color:#6b7280;font-size:15px;font-weight:400}\n"
         ".sub{color:#6b7280;margin:0 0 22px;font-size:13px}\n"
         ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:24px}\n"
         ".card{background:#16161d;border:1px solid #ffffff14;border-radius:10px;padding:14px 16px}\n"
         ".card .k{color:#8a90a2;font-size:12px;text-transform:uppercase;letter-spacing:.04em}\n"
         ".card .v{font-size:22px;font-weight:600;margin-top:4px}\n"
         ".pos{color:#33cc73}.neg{color:#f24d5c}\n"
         ".chart{background:#16161d;border:1px solid #ffffff14;border-radius:10px;padding:16px;margin-bottom:20px}\n"
         ".chart h3{margin:0 0 10px;font-size:14px;color:#aab}\n"
         "table{width:100%;border-collapse:collapse;font-size:13px}\n"
         "th,td{text-align:left;padding:6px 10px;border-bottom:1px solid #ffffff10}\n"
         "th{color:#8a90a2;font-weight:500}canvas{width:100%;display:block}\n"
         "</style></head><body>\n";

    f << "<h1>QuantSim Tearsheet <span class='s'>· " << d.strategy << "</span></h1>\n";
    f << "<p class='sub'>Generated " << d.timestamp << "</p>\n";

    f << "<div class='grid'>\n";
    f << "<div class='card'><div class='k'>Final PnL</div><div class='v "
      << (d.final_pnl >= 0 ? "pos" : "neg") << "'>";
    { char b[64]; snprintf(b, sizeof b, "$%+.2f", d.final_pnl); f << b; }
    f << "</div></div>\n";
    metric("Total Return", d.total_return_pct, "%.2f%%");
    metric("Sharpe", d.sharpe, "%.3f");
    metric("Sortino", d.sortino, "%.3f");
    metric("Calmar", d.calmar, "%.3f");
    metric("Max Drawdown", d.max_dd * 100.0, "%.2f%%");
    metric("Volatility", d.vol * 100.0, "%.2f%%");
    metric("Profit Factor", d.profit_factor, "%.3f");
    metric("Win Rate", d.win_ratio * 100.0, "%.1f%%");
    metric("Total Fills", (double)d.fills_total, "%.0f");
    metric("Win / Loss", (double)d.win_trades, "%.0f");
    f << "</div>\n";

    f << "<div class='chart'><h3>Equity (PnL) Curve</h3><canvas id='eq' height='220'></canvas></div>\n";
    f << "<div class='chart'><h3>Drawdown (%)</h3><canvas id='dd' height='180'></canvas></div>\n";

    f << "<div class='chart'><h3>Fills</h3><table><tr><th>#</th><th>Symbol</th><th>Side</th><th>Qty</th><th>Price</th></tr>\n";
    size_t maxr = d.fills.size() < 500 ? d.fills.size() : 500;
    for (size_t i = 0; i < maxr; ++i) {
        const auto& fl = d.fills[i];
        f << "<tr><td>" << i << "</td><td>" << fl.sym << "</td><td class='"
          << (fl.side == "BUY" ? "pos" : "neg") << "'>" << fl.side << "</td><td>"
          << fl.qty << "</td><td>" << fl.price << "</td></tr>\n";
    }
    f << "</table></div>\n";

    f << "<script>\n";
    f << "const PNL=" << js_array(pnl) << ";\n";
    f << "const DD=" << js_array(dd) << ";\n";
    f << "function draw(id,data,color,fill,zero){\n"
         " const c=document.getElementById(id);const dpr=window.devicePixelRatio||1;\n"
         " const w=c.clientWidth,h=c.height;c.width=w*dpr;c.height=h*dpr;const x=c.getContext('2d');x.scale(dpr,dpr);\n"
         " if(!data.length)return;let mn=Math.min(...data),mx=Math.max(...data);if(zero){mn=Math.min(mn,0);mx=Math.max(mx,0);}\n"
         " if(mn===mx){mx+=1;mn-=1;}const pad=8;const sx=v=>pad+(w-2*pad)*v/(data.length-1||1);\n"
         " const sy=v=>pad+(h-2*pad)*(1-(v-mn)/(mx-mn));\n"
         " x.strokeStyle='#ffffff14';x.beginPath();const zy=sy(0);x.moveTo(pad,zy);x.lineTo(w-pad,zy);x.stroke();\n"
         " x.beginPath();x.moveTo(sx(0),sy(data[0]));for(let i=1;i<data.length;i++)x.lineTo(sx(i),sy(data[i]));\n"
         " x.strokeStyle=color;x.lineWidth=1.6;x.stroke();\n"
         " if(fill){x.lineTo(sx(data.length-1),zy);x.lineTo(sx(0),zy);x.closePath();x.fillStyle=fill;x.fill();}\n"
         "}\n";
    f << "function redraw(){const up=PNL.length&&PNL[PNL.length-1]>=0;\n"
         " draw('eq',PNL,up?'#33cc73':'#f24d5c',up?'#33cc7322':'#f24d5c22',true);\n"
         " draw('dd',DD,'#e0563f','#e0563f33',true);}\n";
    f << "window.addEventListener('resize',redraw);redraw();\n";
    f << "</script>\n</body></html>\n";
    return true;
}

}

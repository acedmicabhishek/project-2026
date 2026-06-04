"""
Generates QuantSim Project Synopsis PDF following CIT CSE formatting guidelines:
  Font: Times New Roman (Times-Roman in PDF)
  Size: 12pt
  Line Spacing: 1.5
  Paper: A4
  Margins: Left 3.5 cm, others 2.5 cm
"""

from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle,
    HRFlowable, PageBreak, KeepTogether
)
from reportlab.platypus.flowables import Flowable

class _FillRemainder(Flowable):
    """Expands to fill whatever vertical space remains in the current frame."""
    def wrap(self, aW, aH):
        self._h = aH
        return (aW, aH)
    def draw(self):
        pass
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import cm, mm
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT, TA_JUSTIFY
from reportlab.platypus import ListFlowable, ListItem

OUTPUT = "QuantSim_Project_Synopsis.pdf"

# ── Page setup ────────────────────────────────────────────────────────────────
LEFT   = 3.5 * cm
RIGHT  = 2.5 * cm
TOP    = 2.5 * cm
BOTTOM = 2.5 * cm

doc = SimpleDocTemplate(
    OUTPUT,
    pagesize=A4,
    leftMargin=LEFT, rightMargin=RIGHT,
    topMargin=TOP,   bottomMargin=BOTTOM,
    title="QuantSim Project Synopsis",
    author="Abhishek Anand, Shakir Ahmad, Amik Affan",
)

# ── Styles ────────────────────────────────────────────────────────────────────
BASE_FONT  = "Times-Roman"
BOLD_FONT  = "Times-Bold"
ITALIC_FONT= "Times-Italic"
BOLD_ITALIC= "Times-BoldItalic"
FS         = 12          # base font size
LEAD       = FS * 1.5    # 1.5 line spacing

styles = getSampleStyleSheet()

def style(name, **kwargs):
    defaults = dict(fontName=BASE_FONT, fontSize=FS, leading=LEAD,
                    alignment=TA_JUSTIFY, spaceAfter=6)
    defaults.update(kwargs)
    return ParagraphStyle(name, **defaults)

S_BODY   = style("Body")
S_BODY_C = style("BodyCenter", alignment=TA_CENTER)
S_BODY_L = style("BodyLeft",   alignment=TA_LEFT)

S_H1 = style("H1", fontName=BOLD_FONT, fontSize=14, leading=21,
             alignment=TA_CENTER, spaceAfter=10, spaceBefore=10)
S_H2 = style("H2", fontName=BOLD_FONT, fontSize=12, leading=18,
             alignment=TA_LEFT, spaceAfter=4, spaceBefore=10)
S_H3 = style("H3", fontName=BOLD_ITALIC, fontSize=12, leading=18,
             alignment=TA_LEFT, spaceAfter=2, spaceBefore=6)

S_TITLE_INST = style("TitleInst", fontName=BOLD_FONT, fontSize=14,
                     alignment=TA_CENTER, leading=21, spaceAfter=4)
S_TITLE_DEPT = style("TitleDept", fontName=BOLD_FONT, fontSize=13,
                     alignment=TA_CENTER, leading=19.5, spaceAfter=4)
S_TITLE_DOC  = style("TitleDoc",  fontName=BOLD_FONT, fontSize=14,
                     alignment=TA_CENTER, leading=21, spaceAfter=16)

S_TABLE_HDR = style("TblHdr", fontName=BOLD_FONT, fontSize=11,
                    alignment=TA_CENTER, leading=16.5)
S_TABLE_CELL= style("TblCell", fontName=BASE_FONT, fontSize=11,
                    alignment=TA_LEFT, leading=16.5)
S_SMALL     = style("Small", fontSize=11, leading=16.5)
S_MONO      = style("Mono", fontName="Courier", fontSize=10, leading=14,
                    alignment=TA_LEFT, spaceAfter=4)

def H(text, level=2):
    s = S_H2 if level == 2 else S_H3
    return Paragraph(text, s)

def P(text, s=None):
    return Paragraph(text, s or S_BODY)

def SP(n=6):
    return Spacer(1, n)

def HR():
    return HRFlowable(width="100%", thickness=0.5, color=colors.black, spaceAfter=6, spaceBefore=6)

def bullet_list(items, style=S_BODY_L):
    elems = []
    for item in items:
        elems.append(Paragraph(f"• &nbsp; {item}", style))
    return elems

# ── Content ───────────────────────────────────────────────────────────────────
story = []

# ════════════════════════════════════════════════════════════════
# COVER PAGE
# ════════════════════════════════════════════════════════════════
story += [
    SP(30),
    P("CAMBRIDGE INSTITUTE OF TECHNOLOGY", S_TITLE_INST),
    P("Tatisilwai, Ranchi – 835103", S_BODY_C),
    SP(4),
    P("Department of Computer Science &amp; Engineering", S_TITLE_DEPT),
    SP(20),
    HR(),
    SP(8),
    P("PROJECT SYNOPSIS", S_TITLE_DOC),
    HR(),
    SP(20),
    P("QuantSim: A High-Frequency Trading Simulation Platform", S_H1),
    SP(30),
]

# Student table
student_data = [
    [P("S.No.", S_TABLE_HDR), P("Student Name",  S_TABLE_HDR),
     P("Roll Number", S_TABLE_HDR),          P("Branch",      S_TABLE_HDR)],
    [P("1.", S_TABLE_CELL), P("Abhishek Anand", S_TABLE_CELL),
     P("360/CSE/22", S_TABLE_CELL),  P("CSE", S_TABLE_CELL)],
    [P("2.", S_TABLE_CELL), P("Shakir Ahmad",   S_TABLE_CELL),
     P("323/CSE/22", S_TABLE_CELL),  P("CSE", S_TABLE_CELL)],
    [P("3.", S_TABLE_CELL), P("Amik Affan",     S_TABLE_CELL),
     P("371/CSE/22", S_TABLE_CELL),  P("CSE", S_TABLE_CELL)],
]
t = Table(student_data, colWidths=[2*cm, 6*cm, 4*cm, 3*cm])
t.setStyle(TableStyle([
    ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#d9d9d9')),
    ('GRID',       (0,0), (-1,-1), 0.5, colors.black),
    ('VALIGN',     (0,0), (-1,-1), 'MIDDLE'),
    ('ROWBACKGROUNDS', (0,1), (-1,-1), [colors.white, colors.HexColor('#f5f5f5')]),
    ('TOPPADDING',  (0,0), (-1,-1), 5),
    ('BOTTOMPADDING',(0,0),(-1,-1), 5),
]))
story.append(t)
story.append(SP(20))

# Guide / Year
meta_data = [
    [P("<b>Project Guide:</b>", S_BODY_L),   P("Prof. Deepak K. Verma", S_BODY_L)],
    [P("<b>Academic Year:</b>", S_BODY_L),   P("2025–2026", S_BODY_L)],
    [P("<b>Programme:</b>",     S_BODY_L),   P("B.Tech – Computer Science &amp; Engineering", S_BODY_L)],
]
tm = Table(meta_data, colWidths=[5*cm, 10.5*cm])
tm.setStyle(TableStyle([
    ('VALIGN', (0,0),(-1,-1),'TOP'),
    ('TOPPADDING',(0,0),(-1,-1),4),
    ('BOTTOMPADDING',(0,0),(-1,-1),4),
]))
story.append(tm)
story.append(PageBreak())

# ════════════════════════════════════════════════════════════════
# 1. INTRODUCTION
# ════════════════════════════════════════════════════════════════
story += [
    H("1. Introduction"),
    H("1.1 Background", level=3),
    P("""High-frequency trading (HFT) represents a domain where microsecond-level
decision-making determines profitability. Modern financial exchanges process millions of
orders per second through electronic matching engines that implement price-time priority
algorithms, a technology stack combining low-latency C++, binary network protocols,
and advanced mathematical models. The professionals who design and maintain these
systems. Quantitative Developers (QD) occupy a critical role at the intersection of
software engineering and quantitative finance."""),
    SP(6),
    P("""Electronic trading now accounts for over 70% of daily equity volume in developed
markets. Every order submitted to an exchange passes through a Limit Order Book (LOB)
a data structure maintaining ranked queues of buy and sell orders, before being
matched against a counterparty. The engineering challenges involved in building, testing,
and optimizing such systems require specialized knowledge that bridges computer science
and financial mathematics."""),
    SP(6),

    H("1.2 Motivation", level=3),
    P("""Despite the industry's scale and technical depth, academic curricula rarely expose
Computer Science students to the real engineering realities of algorithmic trading. Existing
simulation tools either focus on simplified portfolio-level backtesting (lacking realistic
market mechanics) or are proprietary systems inaccessible for research and education.
Students entering quantitative developer roles consistently report a significant gap between
academic preparation and industry expectations."""),
    SP(6),
    P("""Furthermore, no open-source platform currently combines a production-grade C++
matching engine with Python bindings, realistic exchange protocols (Nasdaq ITCH 5.0),
and educational content targeting quantitative developer skill development."""),
    SP(6),

    H("1.3 Overview of Proposed Solution", level=3),
    P("""QuantSim is a full-stack High-Frequency Trading simulation platform comprising
a production-grade C++23 matching engine with Python bindings via pybind11. The
platform implements a real Limit Order Book with price-time priority matching, Nasdaq
ITCH 5.0 binary feed handler, event-driven strategy framework, and comprehensive
analytics pipeline. Three canonical strategies are provided as reference implementations:
Avellaneda-Stoikov market making, statistical arbitrage using Ornstein-Uhlenbeck process
modeling, and TWAP execution with Implementation Shortfall analysis."""),
    SP(6),
]

# ════════════════════════════════════════════════════════════════
# 2. PROBLEM STATEMENT
# ════════════════════════════════════════════════════════════════
story += [
    H("2. Problem Statement"),
    P("""Quantitative Developer roles at trading firms require a unique combination of
low-latency C++ systems engineering, market microstructure understanding, and
mathematical modeling capability. Academic Computer Science programs address none of
these in sufficient depth. The specific problem addressed by this project is the absence of
an open, educational, production-realistic simulation platform for training and research in
algorithmic trading system design."""),
    SP(6),

    H("2.1 Existing System Limitations", level=3),
    *bullet_list([
        "<b>Backtrader / Zipline (Python):</b> Vectorized architecture; no real LOB; no latency modeling; no order type differentiation",
        "<b>Academic simulators:</b> Simplified price models; no market microstructure; no exchange protocol handling",
        "<b>Proprietary systems:</b> Inaccessible for academic use; non-educational",
        "<b>No unified platform:</b> No existing open-source tool combines C++ performance, Python strategy API, and pedagogical documentation",
    ]),
    SP(6),

    H("2.2 Need for Proposed System", level=3),
    *bullet_list([
        "Bridge gap between academic CS education and Quantitative Developer industry requirements",
        "Provide hands-on experience with production-grade LOB matching (price-time priority, FIFO)",
        "Simulate realistic fill mechanics across Market, Limit, IOC, and FOK order types",
        "Enable realistic backtesting with latency modeling and walk-forward validation",
        "Teach production risk controls: kill switches, circuit breakers, pre-trade checks",
    ]),
    SP(6),
]

# ════════════════════════════════════════════════════════════════
# 3. OBJECTIVES
# ════════════════════════════════════════════════════════════════
story += [
    H("3. Objectives"),
    H("3.1 Primary Objective", level=3),
    P("""Design, implement, and validate a full-platform high-frequency trading simulator
that provides Quantitative Developer-level educational depth, combining a production-grade
C++ matching engine with an accessible Python strategy framework and analytics pipeline."""),
    SP(6),

    H("3.2 Secondary Objectives", level=3),
    *bullet_list([
        "Implement a real Limit Order Book with O(log n) price-time priority matching supporting all standard order types",
        "Build a Nasdaq ITCH 5.0 binary protocol parser for real historical market data replay",
        "Develop a Python strategy ABC enabling strategy prototyping in under 50 lines of code",
        "Implement production risk controls: four-level kill switch hierarchy, token bucket rate limiter, stale price guard",
        "Provide three complete strategy implementations: Avellaneda-Stoikov market making, OU-based statistical arbitrage, and TWAP execution",
        "Build an analytics pipeline computing Sharpe, Sortino, Calmar ratios, maximum drawdown, and fill quality metrics",
        "Validate correctness with 21 unit tests covering LOB operations and matching engine behavior",
    ]),
    SP(6),
]

# ════════════════════════════════════════════════════════════════
# 4. LITERATURE REVIEW
# ════════════════════════════════════════════════════════════════
story += [
    H("4. Literature Review"),
    P("""This section reviews the key theoretical foundations and existing techniques
relevant to high-frequency trading simulation, market microstructure, and execution
algorithm design."""),
    SP(6),

    H("4.1 Market Microstructure", level=3),
    P("""Madhavan (2000) provides a comprehensive survey of market microstructure theory,
explaining how prices are formed through order flow and how the Limit Order Book
aggregates liquidity. His work establishes the theoretical basis for adverse selection — the
risk that a liquidity provider fills against an informed trader — which directly motivates the
inventory risk component of our market making model. Harris (2003) extends this to
practical exchange mechanics, covering price-time priority, order book depth, and the
mechanics of dark pools and smart order routing."""),
    SP(6),

    H("4.2 Market Making Models", level=3),
    P("""Avellaneda and Stoikov (2008) derived closed-form solutions for the optimal
bid-ask spread in a limit order book setting. Their model defines reservation price
r = mid − q·γ·σ²·T and optimal spread δ* = γ·σ²·T + (2/γ)·ln(1 + γ/κ), where γ is
risk aversion, σ volatility, T time horizon, and κ order arrival rate. This model is the
foundation of modern quantitative market making and is directly implemented in QuantSim's
market making strategy module. Ho and Stoll (1981) earlier established inventory-based
models showing how dealers widen spreads when inventory becomes directional."""),
    SP(6),

    H("4.3 Execution Algorithms", level=3),
    P("""Almgren and Chriss (2000) solved the optimal portfolio liquidation problem,
deriving the Implementation Shortfall (IS) trading schedule that minimizes the combination
of market impact cost (η·Σvk²) and timing risk (λ·σ²·Σqk²·Δt). Their optimal trajectory
q(t) = Q·sinh(κ(T−t))/sinh(κT) is implemented in QuantSim's execution algorithms module.
Bertsimas and Lo (1998) studied dynamic programming approaches to optimal liquidation
under linear price impact, providing alternative formulations that inform the POV
(Percentage of Volume) algorithm."""),
    SP(6),

    H("4.4 Statistical Arbitrage", level=3),
    P("""Gatev, Goetzmann, and Rouwenhorst (2006) documented pairs trading strategies
exploiting mean reversion in co-integrated asset pairs. The Ornstein-Uhlenbeck (OU)
process dX = θ(μ−X)dt + σdW provides a continuous-time model for mean-reverting
spreads. Engle and Granger (1987) developed co-integration theory, providing the
statistical framework for identifying long-term equilibrium relationships between price
series. These techniques underpin QuantSim's statistical arbitrage module."""),
    SP(6),

    H("4.5 Machine Learning for Finance", level=3),
    P("""Lopez de Prado (2018) in 'Advances in Financial Machine Learning' introduced
purged K-fold cross-validation for financial time series to address look-ahead bias, and
triple-barrier labeling for generating classification targets from continuous price series.
These methods are implemented in QuantSim's backtesting validation framework."""),
    SP(6),

    H("4.6 Research Gap", level=3),
    P("""Existing literature addresses individual components (matching engine design,
execution optimization, strategy modeling) but no open-source platform synthesizes these
into a unified, pedagogically-organized simulation environment with production-grade C++
performance and accessible Python interface. This gap represents the primary motivation
for QuantSim."""),
    SP(6),
]

# ════════════════════════════════════════════════════════════════
# 5. PROPOSED METHODOLOGY / SYSTEM DESIGN
# ════════════════════════════════════════════════════════════════
story += [
    H("5. Proposed Methodology / System Design"),
    H("5.1 Overall System Architecture", level=3),
    P("The system follows a layered architecture with C++ performance-critical components at the core and Python at the strategy layer:"),
    SP(4),
    P("""<font name="Courier" size="10">
Market Data Feed (ITCH 5.0 / CSV / Synthetic GBM)<br/>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;↓<br/>
&nbsp;&nbsp;&nbsp;&nbsp;Feed Handler (C++) → TickEvent stream<br/>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;↓<br/>
&nbsp;Limit Order Book (C++) ← Add/Cancel/Modify<br/>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;↓<br/>
&nbsp;Matching Engine (C++) → Fill Events → on_fill()<br/>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;↓<br/>
&nbsp;Pre-Trade Risk (C++) → Kill Switch (4 levels)<br/>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;↓<br/>
&nbsp;pybind11 Python Bindings (quantsim_core)<br/>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;↓<br/>
&nbsp;Python Strategy API → BacktestEngine<br/>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;↓<br/>
&nbsp;Analytics + Terminal Dashboard (rich)
</font>""", S_BODY_L),
    SP(6),

    H("5.2 Module-wise Description", level=3),
    SP(4),

    H("Module 1: Limit Order Book (C++)", level=3),
    P("""The LOB uses two sorted maps: <i>std::map&lt;double, PriceLevel, std::greater&lt;&gt;&gt;</i>
for bids (descending price) and <i>std::map&lt;double, PriceLevel&gt;</i> for asks (ascending
price). Each PriceLevel contains a doubly-linked list (<i>std::list&lt;Order*&gt;</i>) implementing
FIFO queue discipline within a price level. An unordered hash map provides O(1) order
lookup by ID for cancellations. Operations: add O(log n), cancel O(1), best bid/ask O(1)."""),
    SP(4),

    H("Module 2: Matching Engine (C++)", level=3),
    P("""Routes submitted orders to the appropriate order book and performs matching.
Supports four order types: <b>Market</b> (sweep opposite side until filled or exhausted),
<b>Limit</b> (match if price crosses, otherwise queue; loses time priority if cancel/replace),
<b>IOC</b> (Immediate-or-Cancel: match available quantity, cancel remainder),
<b>FOK</b> (Fill-or-Kill: reject entirely if full quantity unavailable, without touching book).
Emits Fill events via std::function callbacks: on_fill, on_add, on_cancel, on_book_update."""),
    SP(4),

    H("Module 3: Risk Engine (C++)", level=3),
    P("""Implements a four-level kill switch hierarchy: (1) Strategy-level software kill
(&lt;1ms), (2) Portfolio-level risk engine kill (&lt;5ms), (3) Gateway-level connection drop
(&lt;10ms), (4) Exchange-side Mass Cancel on connection drop (1–50ms). Pre-trade checks:
order quantity limit (10,000 shares), position notional limit ($1M), price sanity check
(50 bps from mid), token bucket rate limiter (500 orders/sec). Stale price guard
auto-arms kill switch on feed gap exceeding 5ms."""),
    SP(4),

    H("Module 4: Feed Handler (C++)", level=3),
    P("""Three data sources: (a) <b>ITCH 5.0 binary parser</b> — packed C structs with
big-endian timestamp decoding, supporting Add Order (A), Delete Order (D), Cancel (X),
and Trade (P) message types; (b) <b>CSV tick replay</b> — configurable speed multiplier
(1×, 10×, max-speed with no sleep); (c) <b>Synthetic generator</b> — GBM
(dS = μS dt + σS dW, Euler-Maruyama) and OU process (dX = θ(μ−X)dt + σdW)."""),
    SP(4),

    H("Module 5: Python Strategy Framework", level=3),
    P("""Abstract base class Strategy with event callbacks: <i>on_book_update(BookUpdate)</i>,
<i>on_fill(FillEvent)</i>, <i>on_timer(ts_ns)</i>, <i>on_start()</i>, <i>on_stop()</i>. Gateway API:
<i>send_order(symbol, side, qty, price, ord_type)</i> returning order_id,
<i>cancel_order(order_id)</i>, <i>cancel_all(symbol)</i>. Open order tracking via dict."""),
    SP(4),

    H("Module 6: Backtesting Engine (Python)", level=3),
    P("""Event-driven loop: for each tick, updates internal LOB state, fires
on_book_update, processes pending orders (with configurable latency_ns delay), marks
equity to market. Supports Walk-Forward testing (rolling and anchored windows) and
Purged K-Fold cross-validation (removes train samples with label overlap in test window
plus embargo period)."""),
    SP(4),

    H("Module 7: Analytics and Dashboard", level=3),
    P("""Analytics module computes: Sharpe ratio (annualized), Sortino ratio (downside
deviation), Calmar ratio (return/max_drawdown), maximum drawdown, profit factor, and
fill quality report. Dashboard uses the <i>rich</i> library to render real-time terminal panels
showing order book depth, recent fills, position, PnL, and kill switch status."""),
    SP(6),

    PageBreak(),
    H("5.3 Key Algorithms", level=3),
    SP(4),
]

algo_data = [
    [P("Algorithm", S_TABLE_HDR), P("Description", S_TABLE_HDR)],
    [P("Price-Time Priority", S_TABLE_CELL),
     P("Sort by price (best first), FIFO within each price level. O(log n) insert, O(1) best-level access.", S_TABLE_CELL)],
    [P("Avellaneda-Stoikov MM", S_TABLE_CELL),
     P("r = mid − q·γ·σ²·T; δ* = γ·σ²·T + (2/γ)·ln(1+γ/κ). Reprice on mid move > threshold.", S_TABLE_CELL)],
    [P("Almgren-Chriss IS", S_TABLE_CELL),
     P("q(t) = Q·sinh(κ(T−t))/sinh(κT); κ = √(λσ²/η). Minimizes market impact + timing risk.", S_TABLE_CELL)],
    [P("OU Mean Reversion", S_TABLE_CELL),
     P("dX = θ(μ−X)dt + σdW. Z-score entry/exit signals on rolling spread.", S_TABLE_CELL)],
    [P("Purged K-Fold CV", S_TABLE_CELL),
     P("Remove train samples whose label evaluation time overlaps test window. Add embargo gap.", S_TABLE_CELL)],
]
ta = Table(algo_data, colWidths=[4.5*cm, 11*cm])
ta.setStyle(TableStyle([
    ('BACKGROUND', (0,0),(-1,0), colors.HexColor('#d9d9d9')),
    ('GRID', (0,0),(-1,-1), 0.5, colors.black),
    ('VALIGN', (0,0),(-1,-1), 'TOP'),
    ('ROWBACKGROUNDS', (0,1),(-1,-1), [colors.white, colors.HexColor('#f5f5f5')]),
    ('TOPPADDING', (0,0),(-1,-1), 4),
    ('BOTTOMPADDING',(0,0),(-1,-1), 4),
]))
story += [ta, SP(6)]

# ════════════════════════════════════════════════════════════════
# 6. TOOLS & TECHNOLOGIES
# ════════════════════════════════════════════════════════════════
story += [
    H("6. Tools and Technologies"),
    H("6.1 Programming Languages", level=3),
    *bullet_list([
        "<b>C++23:</b> Core matching engine, order book, risk controls, feed handler — chosen for deterministic latency and zero-overhead abstractions",
        "<b>Python 3.11+:</b> Strategy framework, backtester, analytics, dashboard — chosen for rapid prototyping and scientific library ecosystem",
    ]),
    SP(4),

    H("6.2 Libraries and Frameworks", level=3),
    *bullet_list([
        "<b>pybind11 2.11:</b> Seamless C++/Python bindings with zero overhead for hot-path calls",
        "<b>NumPy 1.24+:</b> Numerical computation for analytics (Sharpe, drawdown, portfolio metrics)",
        "<b>Matplotlib 3.7+:</b> Equity curve and order book depth visualization",
        "<b>Rich 13.0+:</b> Terminal dashboard with live panels, tables, and color-coded metrics",
        "<b>STL (C++):</b> std::map, std::list, std::unordered_map, std::atomic, std::function",
    ]),
    SP(4),

    H("6.3 Build Tools and Environment", level=3),
    *bullet_list([
        "<b>CMake 3.23+:</b> Cross-platform build system with optional targets (Python bindings, GUI, tests)",
        "<b>scikit-build-core:</b> Python package build backend integrating CMake with pip",
        "<b>Git:</b> Version control",
        "<b>VS Code / CLion:</b> IDE with C++23 language server support",
    ]),
    SP(4),

    H("6.4 Hardware", level=3),
    *bullet_list([
        "Standard x86-64 development workstation (Intel/AMD processor)",
        "macOS 14+ or Linux (Ubuntu 22.04+) operating system",
        "8 GB+ RAM recommended for large tick data replay",
    ]),
    SP(6),
]

# ════════════════════════════════════════════════════════════════
# 7. EXPECTED OUTCOMES
# ════════════════════════════════════════════════════════════════
story += [
    H("7. Expected Outcomes"),
    H("7.1 Anticipated Results", level=3),
    *bullet_list([
        "Functional C++ matching engine processing >500,000 orders/second on commodity hardware",
        "Python strategy API enabling complete strategy implementation in under 50 lines",
        "Realistic backtesting across three canonical strategies with quantified performance metrics",
        "ITCH 5.0 feed handler enabling replay of real Nasdaq historical order book data",
        "21+ unit tests verifying LOB correctness across all order types and edge cases",
    ]),
    SP(4),

    H("7.2 Performance Improvements vs. Existing Tools", level=3),
]

perf_data = [
    [P("Metric", S_TABLE_HDR), P("Existing Tools", S_TABLE_HDR), P("QuantSim", S_TABLE_HDR)],
    [P("Order matching latency", S_TABLE_CELL), P("~1 ms (Python)", S_TABLE_CELL), P("< 1 µs (C++)", S_TABLE_CELL)],
    [P("LOB realism", S_TABLE_CELL), P("Simulated (fixed spread)", S_TABLE_CELL), P("Real price-time priority FIFO", S_TABLE_CELL)],
    [P("Order types", S_TABLE_CELL), P("Market, Limit only", S_TABLE_CELL), P("Market, Limit, IOC, FOK", S_TABLE_CELL)],
    [P("Exchange protocols", S_TABLE_CELL), P("Not supported", S_TABLE_CELL), P("ITCH 5.0, FIX 4.2 (documented)", S_TABLE_CELL)],
    [P("Risk controls", S_TABLE_CELL), P("None", S_TABLE_CELL), P("Kill switch, rate limiter, pre-trade checks", S_TABLE_CELL)],
]
tp = Table(perf_data, colWidths=[4.5*cm, 5.5*cm, 5.5*cm])
tp.setStyle(TableStyle([
    ('BACKGROUND', (0,0),(-1,0), colors.HexColor('#d9d9d9')),
    ('GRID', (0,0),(-1,-1), 0.5, colors.black),
    ('VALIGN', (0,0),(-1,-1), 'MIDDLE'),
    ('ROWBACKGROUNDS', (0,1),(-1,-1), [colors.white, colors.HexColor('#f5f5f5')]),
    ('TOPPADDING', (0,0),(-1,-1), 4),
    ('BOTTOMPADDING',(0,0),(-1,-1), 4),
]))
story += [tp, SP(4),

    H("7.3 Practical Applications", level=3),
    *bullet_list([
        "Final year B.Tech / M.Tech project: complete, self-contained system with measurable outcomes",
        "Interview preparation for Quantitative Developer roles at trading firms",
        "Research platform for testing novel execution algorithms and market making models",
        "Educational tool for market microstructure courses in CSE/Finance programs",
    ]),
    SP(6),
]

# ════════════════════════════════════════════════════════════════
# 8. APPLICATIONS / SCOPE
# ════════════════════════════════════════════════════════════════
story += [
    H("8. Applications and Scope"),
    H("8.1 Real-World Use Cases", level=3),
    *bullet_list([
        "<b>Quantitative Developer Training:</b> Hands-on platform for developing LOB design, latency optimization, and protocol handling skills directly applicable to industry roles",
        "<b>Academic Research:</b> Backtesting environment for novel execution algorithms, market making models, and market microstructure research with real Nasdaq ITCH data",
        "<b>Interview Preparation:</b> Simulates technical questions asked at HFT firms (Citadel, Jane Street, Two Sigma) on LOB design and low-latency C++ patterns",
        "<b>Fintech Prototyping:</b> Rapid strategy prototyping before production deployment using Python API over C++ performance core",
        "<b>Educational Curriculum:</b> Complete teaching system for market microstructure, algorithmic trading, and low-latency systems courses",
    ]),
    SP(4),

    H("8.2 Future Enhancement Possibilities", level=3),
    *bullet_list([
        "<b>GPU Acceleration:</b> CUDA kernels for parallel order matching in ultra-high-throughput simulation scenarios",
        "<b>Live Paper Trading:</b> WebSocket connectivity to real exchange REST/WebSocket APIs for paper trading on real market data",
        "<b>Multi-Asset Portfolio:</b> Correlation-aware risk management across multiple correlated instrument books",
        "<b>Reinforcement Learning:</b> OpenAI Gym-compatible environment for training RL-based execution and market making agents",
        "<b>FIX Engine:</b> Full FIX 4.2/4.4 session layer implementation for broker connectivity",
        "<b>FPGA Integration:</b> Hardware matching engine prototype using Xilinx Alveo or Exablaze ExaNIC for sub-microsecond latency research",
    ]),
    SP(6),
]

# ════════════════════════════════════════════════════════════════
# 9. CONCLUSION
# ════════════════════════════════════════════════════════════════
story += [
    H("9. Conclusion"),
    P("""QuantSim addresses a well-defined gap in quantitative finance education by
providing an open, production-realistic simulation platform for algorithmic trading
system design. The platform combines a C++23 matching engine, Nasdaq ITCH 5.0
feed handler, production risk controls, and an accessible Python strategy framework
with three reference implementations: Avellaneda-Stoikov market making,
OU-based statistical arbitrage, and TWAP execution with Implementation Shortfall
analysis. With 21/21 unit tests passing and a modular architecture supporting
future extension to live paper trading and reinforcement learning, QuantSim
constitutes a practically valuable contribution to open-source quantitative finance
education."""),
    SP(6),
]

# ════════════════════════════════════════════════════════════════
# 10. REFERENCES
# ════════════════════════════════════════════════════════════════
story += [
    H("10. References"),
]
refs = [
    "[1] M. Avellaneda and S. Stoikov, \"High-frequency trading in a limit order book,\" <i>Quantitative Finance</i>, vol. 8, no. 3, pp. 217–224, Apr. 2008.",
    "[2] R. Almgren and N. Chriss, \"Optimal execution of portfolio transactions,\" <i>Journal of Risk</i>, vol. 3, no. 2, pp. 5–39, 2000.",
]
for ref in refs:
    story.append(P(ref, S_BODY_L))
    story.append(SP(3))

story.append(KeepTogether([
    Spacer(1, 10*cm),
    HRFlowable(width=8*cm, thickness=0.5, color=colors.black,
               spaceAfter=4, spaceBefore=0, hAlign='LEFT'),
    P("(Signature)", S_BODY_L),
    P("<b>Prof. Deepak K. Verma</b>", S_BODY_L),
    P("Head of the Department", S_BODY_L),
    P("Department of Computer Science &amp; Engineering", S_BODY_L),
    P("Cambridge Institute of Technology, Tatisilwai, Ranchi – 835103", S_BODY_L),
]))

# ── Build PDF ─────────────────────────────────────────────────────────────────
doc.build(story)
print(f"Generated: {OUTPUT}")

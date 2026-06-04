"""
Tests for the live trading layer: position accounting, paper-gateway fills
(taker book-walk + maker queue model), and pre-trade risk.

Run:  PYTHONPATH=python python3 -m pytest tests/test_live.py -q
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "python"))

from quantsim.live.engine  import Position
from quantsim.live.gateway import PaperGateway
from quantsim.live.risk    import RiskManager, RiskLimits
from quantsim.live.feed    import LiveBook, BookLevel, Trade


# ── Position ──────────────────────────────────────────────────────────────────

def test_position_avg_and_realized():
    p = Position()
    p.apply("B", 1, 100, 0)
    p.apply("B", 1, 102, 0)
    assert abs(p.avg_px - 101) < 1e-9 and abs(p.net - 2) < 1e-9
    p.apply("S", 1, 110, 0)
    assert abs(p.realized - 9) < 1e-9 and abs(p.net - 1) < 1e-9


def test_position_flip():
    p = Position()
    p.apply("B", 1, 100, 0); p.apply("B", 1, 102, 0); p.apply("S", 1, 110, 0)
    p.apply("S", 2, 90, 0)                      # close 1 then open short 1
    assert abs(p.realized - (-2)) < 1e-9        # 9 + (90-101)
    assert abs(p.net + 1) < 1e-9 and abs(p.avg_px - 90) < 1e-9
    assert abs(p.unrealized(80) - 10) < 1e-9    # short profits as price falls


def test_position_fees_reduce_realized():
    p = Position()
    p.apply("B", 1, 100, 0.5)
    assert abs(p.realized + 0.5) < 1e-9 and abs(p.fees - 0.5) < 1e-9


# ── Gateway ───────────────────────────────────────────────────────────────────

def _book(bids, asks):
    return LiveBook("X", 0, [BookLevel(p, q) for p, q in bids],
                    [BookLevel(p, q) for p, q in asks])


def test_gateway_taker_walks_book():
    gw = PaperGateway(taker_fee_bps=0)
    fills = []; gw.on_fill = fills.append
    gw.on_book(_book([(99, 5)], [(100, 1), (101, 2)]))
    gw.submit(1, "X", "B", 2, 0, "MARKET")
    assert [f.price for f in fills] == [100, 101]
    assert all(not f.is_maker for f in fills)


def test_gateway_maker_queue_model():
    gw = PaperGateway(maker_fee_bps=0)
    fills = []; gw.on_fill = fills.append
    gw.on_book(_book([(99, 5), (98, 2)], [(100, 1)]))
    gw.submit(10, "X", "B", 2, 99, "LIMIT", post_only=True)   # 5 resting ahead
    gw.on_trade(Trade("X", 0, 99, 3, is_buyer_maker=True))    # consumes 3
    assert fills == []
    gw.on_trade(Trade("X", 0, 99, 4, is_buyer_maker=True))    # drains 2, 2 overflow
    assert len(fills) == 1 and abs(fills[0].qty - 2) < 1e-9 and fills[0].is_maker


def test_gateway_book_cross_fills_maker():
    gw = PaperGateway(maker_fee_bps=0)
    fills = []; gw.on_fill = fills.append
    gw.on_book(_book([(99, 5)], [(100, 1)]))
    gw.submit(1, "X", "B", 1, 99, "LIMIT", post_only=True)
    gw.on_book(_book([(98, 5)], [(99, 1)]))                   # ask drops to our bid
    assert len(fills) == 1 and fills[0].price == 99 and fills[0].is_maker


def test_gateway_post_only_rejects_cross():
    gw = PaperGateway()
    fills = []; gw.on_fill = fills.append
    gw.on_book(_book([(99, 5)], [(100, 1)]))
    gw.submit(1, "X", "B", 1, 101, "LIMIT", post_only=True)   # would take -> reject
    assert fills == [] and gw.open_orders == 0


# ── Risk ──────────────────────────────────────────────────────────────────────

def test_risk_position_limit():
    rm = RiskManager(RiskLimits(max_position=0.01, max_order_qty=0.05, max_notional=1e9))
    assert rm.check(True, 0.005, 100, 100, 0.0, "LIMIT")[0]
    assert not rm.check(True, 0.02, 100, 100, 0.0, "LIMIT")[0]


def test_risk_fat_finger():
    rm = RiskManager(RiskLimits(max_position=1, max_order_qty=1,
                                max_notional=1e9, fat_finger_bps=200))
    assert not rm.check(True, 0.001, 200, 100, 0.0, "LIMIT")[0]


def test_risk_kill_switch_on_loss():
    rm = RiskManager(RiskLimits(max_daily_loss=5000))
    rm.on_pnl(-6000)
    assert rm.kill_armed
    assert not rm.check(True, 0.001, 100, 100, 0.0, "LIMIT")[0]

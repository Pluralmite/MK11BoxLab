#include "ui.h"
#include "movelist.h"
#include "config.h"
#include "game.h"
#include "boxes.h"
#include "memory.h"
#include "log.h"
#include "imgui.h"
#include "mk11.h"
#include "trace.h"
#include "buffer.h"
#include "screenshot.h"
#define MKBL_VERSION "1.0"
#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cfloat>

UiState g_ui;
volatile bool g_reloadRequested = false;

static Camera g_cam;

// ------------------------------------------------------------------ tracer preset
static int g_traceN = 0;
void UiStartTrace() {
    if (trace::Active()) return;
    mk::Snapshot sn = mk::GetSnapshot();
    uintptr_t p1 = sn.p[0].pci, p2 = sn.p[1].pci;
    if (!p1 || !p2) { Log("trace: no PlayerCollisionInfo yet"); return; }
    uintptr_t a[4] = {p2 + 0x130, p2 + 0x124, p2 + 0x250, p1 + 0x344};
    const char* n[4] = {"P2 hurt[6] radius (Spine2)", "P2 hurt[6] X", "P2 hurt[15] radius (Head)", "P1 hurt[23] X (RightHand)"};
    trace::Start(a, n, 5, std::to_string(++g_traceN));
}

// ------------------------------------------------------------------ hotkeys
static bool KeyPressed(int vk) {
    static bool prev[256] = {};
    if (vk <= 0 || vk > 255) return false;
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fired = down && !prev[vk];
    prev[vk] = down;
    return fired;
}

void UiHotkeys() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool focused = pid == GetCurrentProcessId();
    // always poll so edge state stays correct
    bool menu = KeyPressed(g_cfg.menuKey), boxes = KeyPressed(g_cfg.boxesKey), meter = KeyPressed(g_cfg.meterKey),
         pause = KeyPressed(g_cfg.pauseKey), step = KeyPressed(g_cfg.stepKey), shotKey = KeyPressed(VK_F8), rec = KeyPressed(VK_F9), trc = KeyPressed(VK_F10), mtr = KeyPressed(VK_F11);
    if (!focused) return;
    if (menu) g_ui.gameMenu = !g_ui.gameMenu;
    if (shotKey) shot::Request();
    if (boxes) g_ui.showBoxes = !g_ui.showBoxes;
    (void)meter;   // MK11 Box Lab: hit / hurtboxes only, no frame meter
    if (mk::Ready()) {
        if (pause) mk::SetFrozen(!mk::Frozen());
        if (step) mk::StepOnce();
        if (rec && !mk::Recording()) mk::StartRecording(g_ui.recTicks, g_ui.recLabel);
        if (trc) UiStartTrace();
        if (mtr) mk::ArmMeleeTrace();
    } else {
        if (pause) GameTogglePause();
        if (step) GameStep();
    }
}

// ------------------------------------------------------------------ colors
static ImU32 StateColor(FState s) {
    switch (s) {
    case FState::Idle: return IM_COL32(38, 50, 64, 255);
    case FState::Startup: return IM_COL32(72, 206, 140, 255);
    case FState::Active: return IM_COL32(238, 70, 84, 255);
    case FState::Recovery: return IM_COL32(64, 150, 240, 255);
    case FState::Hitstun: return IM_COL32(244, 200, 72, 255);   // hitstun / other busy
    case FState::Blockstun: return IM_COL32(232, 140, 52, 255);
    default: return IM_COL32(24, 32, 42, 200);
    }
}

// ------------------------------------------------------------------ frame data HUD (game-style)
ImFont* g_hudFont = nullptr;   // serif caps font loaded in dllmain (falls back to default)
ImFont* g_uiFont = nullptr;
ImFont* g_menuFont = nullptr;
ImFont* g_valueFont = nullptr;

// Text with extra letter spacing, like the game's caps labels
static void SpacedText(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* t, float spacing) {
    for (const char* c = t; *c; ++c) {
        char ch[2] = {*c, 0};
        dl->AddText(f, size, pos, col, ch);
        pos.x += f->CalcTextSizeA(size, FLT_MAX, 0.f, ch).x + spacing;
    }
}
static float SpacedWidth(ImFont* f, float size, const char* t, float spacing) {
    float w = 0;
    for (const char* c = t; *c; ++c) { char ch[2] = {*c, 0}; w += f->CalcTextSizeA(size, FLT_MAX, 0.f, ch).x + spacing; }
    return w > 0 ? w - spacing : 0;
}

// One 3-row table: label cell (black) | value cell (dark red)
static void DrawStatTable(ImDrawList* dl, ImVec2 o, float k, const char* const labels[3], const std::string values[3], const bool live[3]) {
    ImFont* f = g_hudFont ? g_hudFont : ImGui::GetFont();
    const float lw = 136 * k, vw = 66 * k, rh = 36.5f * k;
    const float fs = 19.f * k, vs = 20.f * k, sp = 1.6f * k;
    const ImU32 white = IM_COL32(235, 235, 235, 255), labelBg = IM_COL32(8, 8, 10, 190), valBg = IM_COL32(110, 22, 20, 215),
                valText = IM_COL32(236, 178, 160, 255), liveText = IM_COL32(255, 230, 200, 255);
    ImVec2 end(o.x + lw + vw, o.y + rh * 3);
    dl->AddRectFilled(o, ImVec2(o.x + lw, end.y), labelBg);
    dl->AddRectFilled(ImVec2(o.x + lw, o.y), end, valBg);
    for (int r = 0; r < 3; ++r) {
        float y = o.y + r * rh;
        float tw = SpacedWidth(f, fs, labels[r], sp);
        SpacedText(dl, f, fs, ImVec2(o.x + lw - 14 * k - tw, y + (rh - fs) * 0.5f), white, labels[r], sp);
        ImVec2 vsz = f->CalcTextSizeA(vs, FLT_MAX, 0.f, values[r].c_str());
        dl->AddText(f, vs, ImVec2(o.x + lw + 14 * k, y + (rh - vsz.y) * 0.5f), live[r] ? liveText : valText, values[r].c_str());
        if (r > 0) dl->AddLine(ImVec2(o.x, y), ImVec2(end.x, y), IM_COL32(235, 235, 235, 200), 1.2f * k);
    }
    dl->AddLine(ImVec2(o.x + lw, o.y), ImVec2(o.x + lw, end.y), IM_COL32(235, 235, 235, 200), 1.2f * k);
    dl->AddRect(o, end, white, 0.f, 0, 2.2f * k);
}

static std::string Num(int v) { return std::to_string(v); }
static std::string Adv(int v) { char b[16]; snprintf(b, sizeof(b), "%+d", v); return b; }

// stats to show: the move in progress once it has hit its first active frame, else the last finished move
static MoveStats Shown(const PlayerMeter& pm) {
    if (!(pm.inMove && pm.seenActive)) return pm.last;
    MoveStats s = pm.cur;
    s.valid = true;
    s.startup = pm.startupFrames;
    s.active = pm.lastActive - pm.firstActive + 1;
    s.recovery = pm.moveFrames - pm.lastActive;
    s.total = pm.moveFrames;
    s.offMismatch = false;
    return s;
}

static void DrawPlayerPanel(ImDrawList* dl, int p, ImVec2 o, float k) {
    const PlayerMeter& m = g_game.meter[p];
    const MoveStats& ms = m.last;
    static const char* const l1[3] = {"STARTUP", "ACTIVE", "RECOVERY"};
    static const char* const l2[3] = {"HIT", "BLOCK", "CANCEL"};
    std::string v1[3], v2[3];
    bool live1[3] = {false, false, false}, live2[3] = {false, false, false};
    if (m.inMove) {   // live while the move runs
        if (m.seenActive) { v1[0] = Num(m.startupFrames - 1); v1[1] = Num(m.activeFrames); v1[2] = Num(m.recoveryFrames); live1[1] = live1[2] = true; }
        else { v1[0] = Num(m.moveFrames); v1[1] = "-"; v1[2] = "-"; live1[0] = true; }
    } else if (ms.valid) {
        v1[0] = Num(ms.startup - 1); v1[1] = Num(ms.active); v1[2] = Num(ms.recovery + 1);
    } else { v1[0] = v1[1] = v1[2] = "-"; }
    v2[0] = m.hitValid ? Adv(m.hitAdv) : "-";
    v2[1] = m.blockValid ? Adv(m.blockAdv) : "-";
    v2[2] = m.cancelValid ? Adv(m.cancelAdv) : (m.cancelUnknown ? "?" : "-");
    if (m.liveCell == 0 || m.liveCell == 1) { v2[m.liveCell] = Adv(m.liveAdv); live2[m.liveCell] = true; }
    DrawStatTable(dl, o, k, l1, v1, live1);
    DrawStatTable(dl, ImVec2(o.x + (136 + 66 + 22) * k, o.y), k, l2, v2, live2);
}

static std::string HitText(const PlayerMeter& m) {
    if (m.liveCell == 0) return Adv(m.liveAdv) + "...";
    if (!m.hitValid) return "-";
    std::string t = Adv(m.hitAdv);
    if (m.hitKD) t += m.kdDelay > 0 ? " (knockdown, opponent delayed +" + std::to_string(m.kdDelay) + ")" : " (knockdown)";
    return t;
}


// ------------------------------------------------------------------ game-styled frame data panel
// Mirrors the in-game MOVE DATA / FRAME DATA panels: steel-blue header bar with spaced caps title,
// dark navy body, banded rows of small caps labels over large white values.
static void DrawFrameMeterGame(float sw, float sh) {
    const float k = g_ui.hudScale;
    const int cells = 80;
    const float pad = 14 * k, cw = 9 * k, ch = 15 * k, cg = 1 * k, labelW = 30 * k;
    const float W = labelW + cells * (cw + cg) + pad * 2;
    const float headerH = 30 * k, rowLab = 22 * k, rowVal = 38 * k;
    ImFont* uf = g_uiFont ? g_uiFont : ImGui::GetFont();
    ImFont* vf = g_valueFont ? g_valueFont : uf;
    const float ls = 15 * k, vs = 28 * k, ts = 17 * k;

    const ImU32 cBody = IM_COL32(22, 30, 40, 232), cHeader = IM_COL32(62, 96, 121, 245), cBorder = IM_COL32(70, 102, 128, 255);
    const ImU32 cBandA = IM_COL32(30, 41, 54, 235), cBandB = IM_COL32(26, 36, 48, 235), cTitle = IM_COL32(226, 236, 244, 255);
    const ImU32 cLabel = IM_COL32(150, 180, 206, 255), cValue = IM_COL32(242, 246, 250, 255), cCyan = IM_COL32(72, 222, 248, 255);

    const PlayerMeter& pm = g_game.meter[0];
    const MoveStats ms = Shown(pm);
    bool haveList = ms.valid && ms.off;
    bool haveGaps = !pm.gaps.empty();
    float H = headerH + pad + 2 * (ch + 6 * k) + pad * 0.5f + (rowLab + rowVal) * 2 + (haveList ? 28 * k : 0) + (haveGaps ? 28 * k : 0) + pad * 0.6f;

    ImGui::SetNextWindowPos(ImVec2((sw - W) * 0.5f, sh * 0.70f), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    if (!g_ui.menuOpen) fl |= ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##framemeter_game", nullptr, fl);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(W, H));

    // body + header
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), cBody);
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + headerH), cHeader);
    const char* title = "FRAME DATA";
    float tw = SpacedWidth(uf, ts, title, 4.5f * k);
    SpacedText(dl, uf, ts, ImVec2(o.x + (W - tw) * 0.5f, o.y + (headerH - ts) * 0.5f), cTitle, title, 4.5f * k);
    if (g_game.paused || mk::Frozen()) SpacedText(dl, uf, ls, ImVec2(o.x + pad, o.y + (headerH - ls) * 0.5f), cCyan, "FROZEN", 2.f * k);

    // frame bars
    float y = o.y + headerH + pad;
    for (int p = 0; p < 2; ++p) {
        const PlayerMeter& m = g_game.meter[p];
        ImU32 pc = p == 0 ? IM_COL32(245, 202, 82, 255) : IM_COL32(90, 204, 246, 255);
        dl->AddText(uf, ls, ImVec2(o.x + pad, y + (ch - ls) * 0.5f), pc, p == 0 ? "P1" : "P2");
        size_t n = m.history.size(), start = n > (size_t)cells ? n - cells : 0;
        float x0 = o.x + pad + labelW;
        for (int i = 0; i < cells; ++i) {
            size_t idx = start + i;
            FState s = idx < n ? m.history[idx] : FState::None;
            ImVec2 a(x0 + i * (cw + cg), y), b(a.x + cw, a.y + ch);
            dl->AddRectFilled(a, b, StateColor(s));
            if (i % 10 == 9) dl->AddLine(ImVec2(b.x + cg * 0.5f, y - 2 * k), ImVec2(b.x + cg * 0.5f, b.y + 2 * k), IM_COL32(110, 140, 166, 120), 1.f);
        }
        y += ch + 6 * k;
    }
    y += pad * 0.5f;

    // stat grid: 2 rows x 3 columns, like the move list's FRAME DATA panel
    auto colOf = [](FState f) { return StateColor(f); };
    char v[6][48];
    ImU32 vc[6] = {cValue, cValue, cValue, cValue, cValue, cValue};
    const char* lab[6] = {"START-UP", "ACTIVE", "RECOVER", "HIT ADV", "CANCEL", "TOTAL"};
    ImU32 lc[6] = {colOf(FState::Startup), colOf(FState::Active), colOf(FState::Recovery), colOf(FState::Hitstun), cLabel, cLabel};
    auto advCol = [&](int a) { return a > 0 ? IM_COL32(110, 236, 140, 255) : a < 0 ? IM_COL32(255, 112, 112, 255) : cValue; };
    if (pm.inMove && !pm.seenActive) {
        snprintf(v[0], 48, "%d", pm.moveFrames); snprintf(v[1], 48, "-"); snprintf(v[2], 48, "-"); snprintf(v[5], 48, "%d", pm.moveFrames);
        vc[0] = cCyan;
    } else if (ms.valid) {
        snprintf(v[0], 48, "%d", ms.startup - 1); snprintf(v[1], 48, "%d", ms.active); snprintf(v[2], 48, "%d", ms.recovery + 1);
        snprintf(v[5], 48, "%d", ms.total);
    } else {
        for (int i : {0, 1, 2, 5}) snprintf(v[i], 48, "-");
    }
    bool showBlock = pm.liveCell == 1 || (pm.liveCell != 0 && pm.blockValid);
    bool showHit = pm.liveCell == 0 || (pm.liveCell != 1 && pm.hitValid);
    if (showBlock && !showHit) { lab[3] = "BLOCK ADV"; lc[3] = colOf(FState::Blockstun); }
    if (pm.liveCell == 0 || pm.liveCell == 1) { snprintf(v[3], 48, "%+d", pm.liveAdv); vc[3] = cCyan; }
    else if (showHit) { snprintf(v[3], 48, "%+d", pm.hitAdv); vc[3] = advCol(pm.hitAdv); }
    else if (showBlock) { snprintf(v[3], 48, "%+d", pm.blockAdv); vc[3] = advCol(pm.blockAdv); }
    else snprintf(v[3], 48, "-");
    if (ms.valid && ms.off) snprintf(v[4], 48, ms.offCancel ? "%+d" : "N/A", ms.offCancel);
    else snprintf(v[4], 48, "-");
    const char* sub3 = nullptr;
    if (showHit && pm.liveCell != 0 && pm.hitKD) sub3 = pm.kdDelay > 0 ? "KNOCKDOWN  (DELAYED)" : "KNOCKDOWN";

    float colW = (W - pad * 2) / 3.f;
    for (int r = 0; r < 2; ++r) {
        float ry = y + r * (rowLab + rowVal);
        dl->AddRectFilled(ImVec2(o.x + pad, ry), ImVec2(o.x + W - pad, ry + rowLab), cBandA);
        dl->AddRectFilled(ImVec2(o.x + pad, ry + rowLab), ImVec2(o.x + W - pad, ry + rowLab + rowVal), cBandB);
        for (int c = 0; c < 3; ++c) {
            int i = r * 3 + c;
            float cx = o.x + pad + colW * c + colW * 0.5f;
            float lw = SpacedWidth(uf, ls, lab[i], 2.f * k);
            SpacedText(dl, uf, ls, ImVec2(cx - lw * 0.5f, ry + (rowLab - ls) * 0.5f), lc[i], lab[i], 2.f * k);
            ImVec2 vsz = vf->CalcTextSizeA(vs, FLT_MAX, 0.f, v[i]);
            dl->AddText(vf, vs, ImVec2(cx - vsz.x * 0.5f, ry + rowLab + (rowVal - vsz.y) * 0.5f), vc[i], v[i]);
            if (i == 3 && sub3) {
                float sw2 = SpacedWidth(uf, 12 * k, sub3, 1.5f * k);
                SpacedText(dl, uf, 12 * k, ImVec2(cx + vsz.x * 0.5f + 8 * k, ry + rowLab + (rowVal - 12 * k) * 0.5f), lc[3], sub3, 1.5f * k);
                (void)sw2;
            }
        }
    }
    y += (rowLab + rowVal) * 2 + 6 * k;

    // move list line
    if (haveList) {
        char t[200];
        snprintf(t, sizeof(t), "%s  (%s)   %d / %d / %d   BLOCK %+d   HIT %+d", movelist::Notation(ms.offName).c_str(), movelist::TypeName(ms.offType), ms.offStartup,
                 ms.offActive, ms.offRecovery, ms.offBlock, ms.offHit);
        SpacedText(dl, uf, 12 * k, ImVec2(o.x + pad, y + 6 * k), cLabel, "MOVE LIST", 2.f * k);
        dl->AddText(uf, ls, ImVec2(o.x + pad + 96 * k, y + 4 * k), cValue, t);
        if (ms.offMismatch) {
            const char* md = "MEASURED DIFFERS";
            float mw = SpacedWidth(uf, 12 * k, md, 1.5f * k);
            SpacedText(dl, uf, 12 * k, ImVec2(o.x + W - pad - mw, y + 6 * k), IM_COL32(255, 160, 70, 255), md, 1.5f * k);
        }
        y += 28 * k;
    }
    // gaps
    if (haveGaps) {
        SpacedText(dl, uf, 12 * k, ImVec2(o.x + pad, y + 6 * k), cLabel, pm.gapsBlocked ? "GAPS ON BLOCK" : "GAPS ON HIT", 2.f * k);
        float gx = o.x + pad + 130 * k;
        for (int gp : pm.gaps) {
            char t[40];
            snprintf(t, sizeof(t), "%dF%s", gp, gp >= g_cfg.gapArmor ? "  ARMOR" : gp >= g_cfg.gapBackdash ? "  BACKDASH" : "");
            ImU32 gc = gp >= g_cfg.gapArmor ? IM_COL32(255, 96, 96, 255) : gp >= g_cfg.gapBackdash ? IM_COL32(250, 190, 70, 255) : cLabel;
            float w = SpacedWidth(uf, 13 * k, t, 1.5f * k) + 16 * k;
            dl->AddRect(ImVec2(gx, y + 2 * k), ImVec2(gx + w, y + 22 * k), gc, 2.f, 0, 1.2f);
            SpacedText(dl, uf, 13 * k, ImVec2(gx + 8 * k, y + 5 * k), gc, t, 1.5f * k);
            gx += w + 8 * k;
        }
        y += 28 * k;
    }
    // frame + accent lines
    dl->AddRect(o, ImVec2(o.x + W, o.y + H), cBorder, 0.f, 0, 1.f);
    dl->AddLine(ImVec2(o.x, o.y + headerH), ImVec2(o.x + W, o.y + headerH), IM_COL32(110, 150, 180, 255), 1.f);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void DrawFrameMeter(float sw, float sh) {
    const int cells = 80;
    const float cw = 9.f, chh = 16.f, gap = 1.f;
    float width = cells * (cw + gap) + 16;
    ImGui::SetNextWindowPos(ImVec2((sw - width) * 0.5f, sh * 0.80f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                          ImGuiWindowFlags_NoNav;
    if (!g_ui.menuOpen) fl |= ImGuiWindowFlags_NoInputs;
    ImGui::Begin("##framemeter", nullptr, fl);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int p = 0; p < 2; ++p) {
        const PlayerMeter& m = g_game.meter[p];
        ImGui::TextColored(p == 0 ? ImVec4(1, .86f, .25f, 1) : ImVec4(.25f, .86f, 1, 1), "P%d", p + 1);
        ImGui::SameLine(34);
        ImVec2 o = ImGui::GetCursorScreenPos();
        size_t n = m.history.size();
        size_t start = n > (size_t)cells ? n - cells : 0;
        for (int i = 0; i < cells; ++i) {
            size_t idx = start + i;
            FState s = idx < n ? m.history[idx] : FState::None;
            ImVec2 a(o.x + i * (cw + gap), o.y), b(a.x + cw, a.y + chh);
            dl->AddRectFilled(a, b, StateColor(s));
        }
        ImGui::Dummy(ImVec2(cells * (cw + gap), chh));
    }

    // move line (live while the move runs)
    const PlayerMeter& pm = g_game.meter[0];
    const MoveStats ms = Shown(pm);
    char buf[256];
    // During strings / combos keep showing the last move that finished (or was chained into the next),
    // with the move currently in progress as a short tail.
    char cur[96] = "";
    if (pm.inMove) {
        if (pm.seenActive) snprintf(cur, sizeof(cur), "   [now %d/%d/%d...]", pm.startupFrames - 1, pm.activeFrames, pm.recoveryFrames + 1);
        else snprintf(cur, sizeof(cur), "   [now startup %d...]", pm.moveFrames);
    }
    // move line: labels coloured like the bars (Startup / Active / Recovery)
    auto colOf = [](FState f) { return ImGui::ColorConvertU32ToFloat4(StateColor(f)); };
    ImGui::TextUnformatted("P1 move ");
    ImGui::SameLine(0, 4);
    if (ms.valid) {
        ImGui::TextColored(colOf(FState::Startup), "Startup %d", ms.startup - 1); ImGui::SameLine(0, 14);
        ImGui::TextColored(colOf(FState::Active), "Active %d", ms.active); ImGui::SameLine(0, 14);
        ImGui::TextColored(colOf(FState::Recovery), "Recovery %d", ms.recovery + 1); ImGui::SameLine(0, 14);
        ImGui::Text("Total %d%s%s%s", ms.total, ms.chained ? " (chained)" : "", ms.fromCache ? "  (whiff/block data)" : "", cur);
    } else if (pm.noHitbox && ms.total > 0) {
        ImGui::Text("Total %d   (no hitbox found - throw, stance or movement?)%s", ms.total, cur);
    } else {
        ImGui::Text("-%s", cur);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("   P1 %s | P2 %s%s", FStateName(g_game.p[0].fstate), FStateName(g_game.p[1].fstate),
                        (g_game.paused || mk::Frozen()) ? "   [FROZEN]" : "");
    // official move-list values for the identified move
    if (ms.valid && ms.off) {
        ImGui::TextDisabled("Move list: %s (%s)  %d / %d / %d   block %+d   hit %+d   cancel %s", movelist::Notation(ms.offName).c_str(), movelist::TypeName(ms.offType),
                            ms.offStartup, ms.offActive, ms.offRecovery, ms.offBlock, ms.offHit,
                            ms.offCancel ? std::to_string(ms.offCancel).c_str() : "N/A");
        if (ms.offMismatch) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, .55f, .2f, 1), "  measured differs"); }
    } else if (movelist::Scanning()) {
        ImGui::TextDisabled("Move list: loading...");
    }

    // advantage line: Hit / Block / Cancel
    auto advCol = [](int a) { return a > 0 ? ImVec4(.4f, 1, .5f, 1) : a < 0 ? ImVec4(1, .45f, .45f, 1) : ImVec4(1, 1, 1, 1); };
    // show either hit or block advantage - whichever the latest (or live) interaction was
    bool showBlock = pm.liveCell == 1 || (pm.liveCell != 0 && pm.blockValid);
    bool showHit = pm.liveCell == 0 || (pm.liveCell != 1 && pm.hitValid);
    if (showHit) {
        ImGui::TextColored(colOf(FState::Hitstun), "Hit advantage");
        ImGui::SameLine();
        ImGui::TextColored(advCol(pm.liveCell == 0 ? pm.liveAdv : pm.hitAdv), "%s", HitText(pm).c_str());
        ImGui::SameLine(0, 24);
    } else if (showBlock) {
        ImGui::TextColored(colOf(FState::Blockstun), "Block advantage");
        ImGui::SameLine();
        if (pm.liveCell == 1) ImGui::TextColored(advCol(pm.liveAdv), "%s...", Adv(pm.liveAdv).c_str());
        else ImGui::TextColored(advCol(pm.blockAdv), "%s", Adv(pm.blockAdv).c_str());
        ImGui::SameLine(0, 24);
    } else {
        ImGui::TextDisabled("Advantage -");
        ImGui::SameLine(0, 24);
    }
    ImGui::TextUnformatted("Cancel");
    ImGui::SameLine();
    if (ms.valid && ms.off) ImGui::Text("%s", ms.offCancel ? Adv(ms.offCancel).c_str() : "N/A");
    else if (pm.cancelValid) ImGui::TextColored(advCol(pm.cancelAdv), "%s", Adv(pm.cancelAdv).c_str());
    else ImGui::TextDisabled("-");
    // gaps in the last string / multi-hit special: free frames the defender had between hits
    if (!pm.gaps.empty()) {
        ImGui::TextUnformatted(pm.gapsBlocked ? "Gaps on block:" : "Gaps on hit:");
        for (int gp : pm.gaps) {
            ImGui::SameLine();
            ImVec4 c = gp >= g_cfg.gapArmor ? ImVec4(1, .35f, .35f, 1) : gp >= g_cfg.gapBackdash ? ImVec4(1, .75f, .25f, 1) : ImVec4(.8f, .8f, .8f, 1);
            ImGui::TextColored(c, "%df%s", gp, gp >= g_cfg.gapArmor ? " (armor)" : gp >= g_cfg.gapBackdash ? " (backdash)" : "");
        }
    } else if (pm.blockValid || pm.hitValid) {
        ImGui::TextDisabled("No gaps in the last string");
    }
    ImGui::End();
}

// ------------------------------------------------------------------ struct explorer
struct Watch { std::string label; uintptr_t addr; int type; }; // 0 int32, 1 float, 2 u8, 3 ptr
static std::vector<Watch> g_watches;

static char g_exAddrText[256] = "p1";
static int g_exSize = 0x400;
static int g_exView = 0; // 0 int32, 1 float, 2 hex
static std::vector<uint8_t> g_exCur, g_exPrev, g_snapA, g_snapB;
static std::vector<uint32_t> g_exLastChange; // frame index of last change per dword
static uintptr_t g_exAddr = 0;
static std::string g_exErr;
static bool g_onlyChanged = false, g_onlyDiff = false;

static uintptr_t EvalText(const char* text, std::string* err) {
    auto e = mem::Expr::Parse(text, err);
    if (!e) return 0;
    mem::EvalCtx c;
    if (g_cfg.p1) c.p1 = g_cfg.p1->Eval(c);
    if (g_cfg.p2) c.p2 = g_cfg.p2->Eval(c);
    c.self = c.p1;
    uintptr_t a = e->Eval(c);
    if (!a && err) *err = "expression evaluated to 0 (unresolved pointer?)";
    return a;
}

static void ExplorerRead() {
    g_exAddr = EvalText(g_exAddrText, &g_exErr);
    if (!g_exAddr) return;
    g_exErr.clear();
    size_t n = (size_t)g_exSize;
    std::vector<uint8_t> buf(n);
    if (!mem::ReadRaw(g_exAddr, buf.data(), n)) {
        // read what we can, dword by dword
        for (size_t i = 0; i + 4 <= n; i += 4) if (!mem::ReadRaw(g_exAddr + i, &buf[i], 4)) memset(&buf[i], 0xCD, 4);
    }
    if (g_exCur.size() != n) { g_exCur = buf; g_exPrev = buf; g_exLastChange.assign(n / 4, 0); return; }
    g_exPrev = g_exCur;
    g_exCur = buf;
    for (size_t i = 0; i + 4 <= n; i += 4)
        if (memcmp(&g_exCur[i], &g_exPrev[i], 4) != 0) g_exLastChange[i / 4] = (uint32_t)g_game.logicalFrame;
}

static void DrawExplorer() {
    ImGui::SetNextWindowSize(ImVec2(560, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Struct Explorer", &g_ui.showExplorer)) { ImGui::End(); return; }
    ImGui::TextWrapped("Live view of memory at an address expression. Use p1 / p2 for the player objects, "
                       "e.g. p1, [p1+40], rva:1234. Green rows changed in the last 30 frames.");
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("Address", g_exAddrText, sizeof(g_exAddrText));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("Size", &g_exSize, 0x100, 0x400, ImGuiInputTextFlags_CharsHexadecimal);
    if (g_exSize < 0x10) g_exSize = 0x10;
    if (g_exSize > 0x4000) g_exSize = 0x4000;
    ImGui::RadioButton("int32", &g_exView, 0); ImGui::SameLine();
    ImGui::RadioButton("float", &g_exView, 1); ImGui::SameLine();
    ImGui::RadioButton("hex", &g_exView, 2); ImGui::SameLine();
    ImGui::Checkbox("Only changing", &g_onlyChanged);

    if (ImGui::Button("Snapshot A")) g_snapA = g_exCur;
    ImGui::SameLine();
    if (ImGui::Button("Snapshot B")) g_snapB = g_exCur;
    ImGui::SameLine();
    ImGui::Checkbox("Only A != B", &g_onlyDiff);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snapshot A while idle, do a move and freeze (F5) on an active frame, Snapshot B.\n"
                          "Offsets that differ are candidates for state / active-flag fields.");

    ExplorerRead();
    if (!g_exErr.empty()) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", g_exErr.c_str());
    else ImGui::Text("Base: %llX", (unsigned long long)g_exAddr);

    bool diffMode = g_onlyDiff && g_snapA.size() == g_exCur.size() && g_snapB.size() == g_exCur.size();
    if (ImGui::BeginTable("ex", diffMode ? 5 : 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Value");
        if (diffMode) ImGui::TableSetupColumn("A -> B");
        ImGui::TableSetupColumn("Ptr?", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();
        for (size_t off = 0; off + 4 <= g_exCur.size(); off += 4) {
            bool recent = g_game.logicalFrame - g_exLastChange[off / 4] < 30 && g_exLastChange[off / 4] != 0;
            if (g_onlyChanged && !recent) continue;
            if (diffMode && memcmp(&g_snapA[off], &g_snapB[off], 4) == 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("+%03zX", off);
            ImGui::TableNextColumn();
            int32_t iv; float fv; memcpy(&iv, &g_exCur[off], 4); memcpy(&fv, &g_exCur[off], 4);
            ImVec4 col = recent ? ImVec4(.4f, 1, .5f, 1) : ImVec4(.85f, .85f, .85f, 1);
            if (g_exView == 0) ImGui::TextColored(col, "%d", iv);
            else if (g_exView == 1) ImGui::TextColored(col, "%.4f", fv);
            else ImGui::TextColored(col, "%08X", (uint32_t)iv);
            if (diffMode) {
                ImGui::TableNextColumn();
                int32_t a, b; memcpy(&a, &g_snapA[off], 4); memcpy(&b, &g_snapB[off], 4);
                if (g_exView == 1) { float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); ImGui::Text("%.3f -> %.3f", fa, fb); }
                else ImGui::Text("%d -> %d", a, b);
            }
            ImGui::TableNextColumn();
            ImGui::PushID((int)off);
            if (off % 8 == 0 && off + 8 <= g_exCur.size()) {
                uint64_t pv; memcpy(&pv, &g_exCur[off], 8);
                if (mem::LooksLikePointer(pv) && ImGui::SmallButton("go")) {
                    std::string t = std::string("[") + g_exAddrText + "+" ;
                    char h[32]; snprintf(h, sizeof(h), "%zX]", off);
                    t += h;
                    strncpy(g_exAddrText, t.c_str(), sizeof(g_exAddrText) - 1);
                    g_exCur.clear();
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("watch")) {
                char lbl[300]; snprintf(lbl, sizeof(lbl), "%s+%zX", g_exAddrText, off);
                g_watches.push_back({lbl, g_exAddr + off, g_exView == 1 ? 1 : 0});
                g_ui.showWatches = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void DrawWatches() {
    ImGui::SetNextWindowSize(ImVec2(460, 260), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Watches", &g_ui.showWatches)) { ImGui::End(); return; }
    ImGui::TextDisabled("Pinned addresses. Copy the label into MK11BoxLab.ini (replace the p1 prefix with self for [Fields]).");
    int remove = -1;
    for (size_t i = 0; i < g_watches.size(); ++i) {
        Watch& w = g_watches[i];
        ImGui::PushID((int)i);
        const char* types[] = {"int32", "float", "u8", "ptr"};
        ImGui::SetNextItemWidth(70);
        ImGui::Combo("##t", &w.type, types, 4);
        ImGui::SameLine();
        switch (w.type) {
        case 0: ImGui::Text("%-12d", mem::ReadOr<int32_t>(w.addr, 0)); break;
        case 1: ImGui::Text("%-12.4f", mem::ReadOr<float>(w.addr, 0)); break;
        case 2: ImGui::Text("%-12u", (unsigned)mem::ReadOr<uint8_t>(w.addr, 0)); break;
        case 3: ImGui::Text("%llX", (unsigned long long)mem::ReadOr<uint64_t>(w.addr, 0)); break;
        }
        ImGui::SameLine(220);
        ImGui::TextUnformatted(w.label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("copy")) ImGui::SetClipboardText(w.label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = (int)i;
        ImGui::PopID();
    }
    if (remove >= 0) g_watches.erase(g_watches.begin() + remove);
    ImGui::End();
}

static void DrawCollList() {
    ImGui::SetNextWindowSize(ImVec2(620, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Collision objects", &g_ui.showCollList)) { ImGui::End(); return; }
    mk::Snapshot sn = mk::GetSnapshot();
    for (const auto& t : sn.live)
        ImGui::Text("%-24s %llX  born tick %llu  caller %llX  args %llX %llX", mk::KindName(t.kind), (unsigned long long)t.ptr,
                    (unsigned long long)t.createdTick, (unsigned long long)t.callerRva, (unsigned long long)t.arg1,
                    (unsigned long long)t.arg2);
    ImGui::Separator();
    for (const auto& sh : sn.shapes)
        ImGui::Text("%-22s type %d state %d owner P%d  r %.1f w %.1f l %.1f  pos %.0f %.0f %.0f %s", mk::KindName(sh.kind), sh.type, sh.state,
                    sh.owner + 1, sh.radius, sh.width, sh.length, sh.pos.x, sh.pos.y, sh.pos.z, sh.hasMat ? "mat" : "");
    ImGui::Separator();
    auto ev = mk::RecentEvents();
    for (int i = (int)ev.size() - 1; i >= 0 && i >= (int)ev.size() - 40; --i) ImGui::TextUnformatted(ev[i].c_str());
    ImGui::End();
}

// ------------------------------------------------------------------ main menu
static void DrawMenu() {
    // after every (re)load the window starts at the top left, expanded and sized to its contents
    static bool placed = false;
    if (!placed) {
        placed = true;
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
    }
    ImGui::Begin("MK11 Box Lab", &g_ui.menuOpen);
    ImGui::TextDisabled("Build %s %s", __DATE__, __TIME__);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload mod")) g_reloadRequested = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unloads the mod and loads the newest MK11BoxLab_core.dll (also Ctrl+F5).\nNo game restart needed.");
    ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "Offline / Practice mode only. Do not use online.");
    if (g_game.locked) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", g_game.lockReason.c_str());
    if (g_cfg.demo) ImGui::TextColored(ImVec4(.5f, .8f, 1, 1), "DEMO MODE: meter shows a scripted pattern (Demo=1 in ini).");


    if (ImGui::CollapsingHeader("Mortal Kombat 11 link", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", mk::Status().c_str());
        if (mk::Ready()) {
            mk::Snapshot sn = mk::GetSnapshot();
            ImGui::Text("Tick %llu   %s", (unsigned long long)sn.tick, mk::Frozen() ? "[FROZEN]" : "");
            for (int i = 0; i < 2; ++i) {
                const auto& p = sn.p[i];
                if (p.valid)
                    ImGui::Text("P%d state %04X move %d attack %d  hurt %d strike %d  pos %.0f %.0f %.0f", i + 1, p.stateFlags, p.moveId,
                                p.meleeState, (int)p.hurt.size(), (int)p.strike.size(), p.pos.x, p.pos.y, p.pos.z);
                else
                    ImGui::TextDisabled("P%d not in a fight", i + 1);
            }
            if (sn.camValid)
                ImGui::Text("Camera %.0f %.0f %.0f  rot %d %d %d  fov %.1f", sn.camPos.x, sn.camPos.y, sn.camPos.z, sn.camRot.pitch,
                            sn.camRot.yaw, sn.camRot.roll, sn.camFov);
            else
                ImGui::TextDisabled("Camera not captured yet");
            ImGui::Checkbox("Hurtboxes", &g_ui.showHurtPts); ImGui::SameLine();
            ImGui::Checkbox("Hitboxes", &g_ui.showStrike); ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::SliderInt("Hitbox linger", &g_ui.strikeLinger, 0, 30, "%d f");
            ImGui::Checkbox("Projectiles / shaped attacks", &g_ui.showShapes); ImGui::SameLine();
            ImGui::Checkbox("incl. inactive (faint)", &g_ui.shapesInactive);
            ImGui::Text("Shaped collision objects: %d", (int)sn.shapes.size());
            ImGui::Checkbox("Part ids", &g_ui.hurtLabels); ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("r=0 size", &g_ui.zeroRadius, 0.25f, 0.f, 30.f, "%.1f");
            ImGui::Checkbox("Show bones", &g_ui.showBones); ImGui::SameLine();
            ImGui::Checkbox("Bone names", &g_ui.boneLabels); ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("Joint radius", &g_ui.boneRadius, 0.25f, 0.f, 40.f, "%.1f");
            if (ImGui::Button(mk::Frozen() ? "Unfreeze (F5)" : "Freeze (F5)")) mk::SetFrozen(!mk::Frozen());
            ImGui::SameLine();
            if (ImGui::Button("Step (F6)")) mk::StepOnce();
            ImGui::Separator();
            ImGui::SetNextItemWidth(120);
            ImGui::InputText("Label", g_ui.recLabel, sizeof(g_ui.recLabel));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("Ticks", &g_ui.recTicks, 60, 120);
            if (g_ui.recTicks < 10) g_ui.recTicks = 10;
            if (g_ui.recTicks > 900) g_ui.recTicks = 900;
            if (mk::Recording()) ImGui::Text("Recording... %d%%", mk::RecordProgress());
            else if (ImGui::Button("Record (F9)")) mk::StartRecording(g_ui.recTicks, g_ui.recLabel);
            std::string last = mk::LastRecordingPath();
            if (!last.empty()) ImGui::TextWrapped("Last: %s", last.c_str());
            if (trace::Active()) ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "Tracing hit-test reads... attack now!");
            else if (ImGui::Button("Trace hit test (F10)")) UiStartTrace();
            ImGui::SameLine();
            if (mk::MeleeTraceArmed()) ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "Armed: attack now");
            else if (ImGui::Button("Trace next attack (F11)")) mk::ArmMeleeTrace();
            if (!trace::LastFile().empty()) ImGui::TextWrapped("Trace: %s", trace::LastFile().c_str());
            ImGui::Text("Live collision objects: %d", (int)sn.live.size());
            ImGui::SameLine();
            ImGui::Checkbox("List", &g_ui.showCollList);
        }
    }
    if (ImGui::CollapsingHeader("Input buffer")) {
        ImGui::TextDisabled("%s", ibuf::Status().c_str());
        int slot = ibuf::GameBufferSlot();
        if (slot >= 0) ImGui::TextColored(ImVec4(.4f, 1, .5f, 1), "Game buffered-move slot: move #%d", slot);
        else ImGui::TextDisabled("Game buffered-move slot: empty");
        ImGui::Checkbox("Extended buffer (offline only)", &ibuf::g_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::SliderInt("window", &ibuf::g_window, 1, 15, "%d frames");
        ImGui::TextDisabled("Pad 1 (XInput). Presses made while busy are re-pressed on your first free frame.");
        ImGui::Checkbox("Probe log", &ibuf::g_probe);
        if (ImGui::BeginTable("probe", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Pressed early");
            ImGui::TableSetupColumn("Came out");
            ImGui::TableSetupColumn("Game buffered");
            ImGui::TableSetupColumn("Our buffer");
            ImGui::TableSetupColumn("Buttons");
            ImGui::TableHeadersRow();
            auto rows = ibuf::Rows();
            for (size_t i = 0; i < rows.size() && i < 12; ++i) {
                const auto& r = rows[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d f", r.framesEarly);
                ImGui::TableNextColumn(); ImGui::TextColored(r.cameOut ? ImVec4(.4f, 1, .5f, 1) : ImVec4(1, .45f, .45f, 1), r.cameOut ? "yes" : "no");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.gameBuffered ? "yes" : "-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.injected ? "re-pressed" : "-");
                ImGui::TableNextColumn();
                char b[64] = "";
                if (r.buttons & 0x4000) strcat(b, "X ");
                if (r.buttons & 0x8000) strcat(b, "Y ");
                if (r.buttons & 0x2000) strcat(b, "B ");
                if (r.buttons & 0x1000) strcat(b, "A ");
                if (r.buttons & 0x0100) strcat(b, "LB ");
                if (r.buttons & 0x0200) strcat(b, "RB ");
                ImGui::TextUnformatted(b);
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Display")) {
        ImGui::Checkbox("Frame meter (F4)", &g_ui.showMeter);
        ImGui::SameLine();
        ImGui::Checkbox("Classic look", &g_ui.classicMeter);
        ImGui::SliderFloat("Panel size", &g_ui.hudScale, 0.7f, 1.6f, "%.2f");
        ImGui::Checkbox("Hit / hurt shapes (F3)", &g_ui.showBoxes);
        ImGui::Indent();
        ImGui::Checkbox("Hurt", &g_ui.showHurt); ImGui::SameLine();
        ImGui::Checkbox("Hit", &g_ui.showHit); ImGui::SameLine();
        ImGui::Checkbox("Throw", &g_ui.showThrow); ImGui::SameLine();
        ImGui::Checkbox("Push", &g_ui.showPush); ImGui::SameLine();
        ImGui::Checkbox("Other", &g_ui.showOther);
        ImGui::SliderFloat("Hitbox opacity", &g_ui.hitAlpha, 0.1f, 4.f, "%.2f");
        ImGui::SliderFloat("Hurtbox opacity", &g_ui.hurtAlpha, 0.1f, 4.f, "%.2f");
        ImGui::Checkbox("Filled", &g_ui.fill); ImGui::SameLine();
        ImGui::Checkbox("Player position markers", &g_ui.showMarkers);
        ImGui::Unindent();
        if (ImGui::Button(g_game.paused ? "Resume (F5)" : "Pause (F5)")) GameTogglePause();
        ImGui::SameLine();
        if (ImGui::Button("Step 1 frame (F6)")) GameStep();
        if (!g_cfg.gameSpeed) { ImGui::SameLine(); ImGui::TextDisabled("(meter-only: set GameSpeed)"); }
    }

    if (ImGui::CollapsingHeader("Live values")) {
        for (int i = 0; i < 2; ++i) {
            const PlayerSample& p = g_game.p[i];
            ImGui::Text("P%d obj %llX %s", i + 1, (unsigned long long)p.object, p.valid ? "" : "(unresolved)");
            if (!p.valid) continue;
            ImGui::Text("  pos %.1f %.1f %.1f  state %d  moveFrame %d", p.pos[0], p.pos[1], p.pos[2], p.state, p.moveFrame);
            ImGui::Text("  actionable %d  active %d  hitstun %d  blockstun %d  -> %s", p.actionable, p.attackActive,
                        p.hitstun, p.blockstun, FStateName(p.fstate));
        }
        ImGui::Text("Logical frame %llu   Camera %s", (unsigned long long)g_game.logicalFrame, g_cam.valid ? "OK" : "not set");
    }

    if (ImGui::CollapsingHeader("Reverse-engineering tools")) {
        ImGui::Checkbox("Struct Explorer", &g_ui.showExplorer); ImGui::SameLine();
        ImGui::Checkbox("Watches", &g_ui.showWatches);
        static char sig[256] = "";
        static char res[128] = "";
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("##sig", sig, sizeof(sig));
        ImGui::SameLine();
        if (ImGui::Button("Find pattern")) {
            uintptr_t a = mem::FindPattern(sig);
            if (a) snprintf(res, sizeof(res), "match at rva:%llX", (unsigned long long)(a - mem::ModuleBase()));
            else snprintf(res, sizeof(res), "no match");
        }
        ImGui::TextUnformatted(res);
        ImGui::Text("Module base %llX  size %llX", (unsigned long long)mem::ModuleBase(), (unsigned long long)mem::ModuleSize());
    }

    if (ImGui::CollapsingHeader("Config")) {
        ImGui::TextWrapped("%s", g_cfg.path.c_str());
        ImGui::Text("%d shape sources configured", (int)g_cfg.boxes.size());
        for (auto& e : g_cfg.errors) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "%s", e.c_str());
        if (ImGui::Button("Reload MK11BoxLab.ini")) {
            std::string p = g_cfg.path;
            g_cfg.Load(p);
            g_game = GameState();
        }
    }
    ImGui::End();
}

// ------------------------------------------------------------------ BOX LAB menu (F2)
// Laid out like the game's practice pause menu, mirrored on the right: white caps items on a thin arc of
// bullets, a pointer line from the screen edge to the selected one. Pad / keyboard belong to it while open.
namespace dib { void SetBlock(bool on); }
bool UiCapturesInput() { return g_ui.gameMenu; }

static bool g_gmFresh = true;
struct Repeat { bool prev = false; ULONGLONG next = 0; };
static bool Fire(bool down, Repeat& r) {
    ULONGLONG now = GetTickCount64();
    bool f = false;
    if (down && !r.prev) { f = true; r.next = now + 320; }
    else if (down && now >= r.next) { f = true; r.next = now + 85; }
    r.prev = down;
    return f;
}

enum class GmKind { Toggle, Choice, Slider, Action };
enum GmId { GmFrameData, GmPanelStyle, GmPanelSize, GmHitboxes, GmHurtboxes, GmHitAlpha, GmHurtAlpha, GmLinger, GmFill, GmBuffer, GmBufWindow, GmScreenshot, GmAdvanced, GmReload, GmClose };
struct GmItem { GmId id; const char* label; GmKind kind; };
static const GmItem kGm[] = {
    {GmHitboxes, "HITBOXES", GmKind::Toggle},      {GmHurtboxes, "HURTBOXES", GmKind::Toggle},    {GmHitAlpha, "HITBOX OPACITY", GmKind::Slider},
    {GmHurtAlpha, "HURTBOX OPACITY", GmKind::Slider},
    {GmLinger, "HITBOX LINGER", GmKind::Slider},
    {GmFill, "FILLED SHAPES", GmKind::Toggle},
    {GmScreenshot, "TAKE SCREENSHOT", GmKind::Action}, {GmAdvanced, "ADVANCED SETTINGS", GmKind::Toggle}, {GmReload, "RELOAD MOD", GmKind::Action}, {GmClose, "CLOSE", GmKind::Action},
};
static const int kGmN = (int)(sizeof(kGm) / sizeof(kGm[0]));

// sliders: range and step
struct GmRange { float lo, hi, step; };
static GmRange GmRangeOf(GmId id) {
    switch (id) {
    case GmPanelSize: return {0.7f, 1.6f, 0.05f};
    case GmHitAlpha: case GmHurtAlpha: return {0.1f, 4.f, 0.05f};   // above 100% = stronger than the default look
    case GmLinger: return {0.f, 30.f, 1.f};
    case GmBufWindow: return {1.f, 15.f, 1.f};
    default: return {0.f, 1.f, 1.f};
    }
}
static float GmGet(GmId id) {
    switch (id) {
    case GmPanelSize: return g_ui.hudScale;
    case GmHitAlpha: return g_ui.hitAlpha;
    case GmHurtAlpha: return g_ui.hurtAlpha;
    case GmLinger: return (float)g_ui.strikeLinger;
    case GmBufWindow: return (float)ibuf::g_window;
    default: return 0.f;
    }
}
static void GmSet(GmId id, float v) {
    GmRange r = GmRangeOf(id);
    v = r.lo + std::round((v - r.lo) / r.step) * r.step;
    v = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
    switch (id) {
    case GmPanelSize: g_ui.hudScale = v; break;
    case GmHitAlpha: g_ui.hitAlpha = v; break;
    case GmHurtAlpha: g_ui.hurtAlpha = v; break;
    case GmLinger: g_ui.strikeLinger = (int)std::lround(v); break;
    case GmBufWindow: ibuf::g_window = (int)std::lround(v); break;
    default: break;
    }
}

static std::string GmValue(GmId id) {
    char b[32];
    switch (id) {
    case GmFrameData: return g_ui.showMeter ? "ON" : "OFF";
    case GmPanelStyle: return g_ui.classicMeter ? "CLASSIC" : "GAME";
    case GmPanelSize: snprintf(b, sizeof(b), "%.2f", g_ui.hudScale); return b;
    case GmHitAlpha: snprintf(b, sizeof(b), "%d%%", (int)std::lround(g_ui.hitAlpha * 100)); return b;
    case GmHurtAlpha: snprintf(b, sizeof(b), "%d%%", (int)std::lround(g_ui.hurtAlpha * 100)); return b;
    case GmHitboxes: return (g_ui.showBoxes && g_ui.showStrike) ? "ON" : "OFF";
    case GmHurtboxes: return (g_ui.showBoxes && g_ui.showHurtPts) ? "ON" : "OFF";
    case GmLinger: snprintf(b, sizeof(b), "%d F", g_ui.strikeLinger); return b;
    case GmFill: return g_ui.fill ? "ON" : "OFF";
    case GmBuffer: return ibuf::g_enabled ? "ON" : "OFF";
    case GmBufWindow: snprintf(b, sizeof(b), "%d F", ibuf::g_window); return b;
    case GmAdvanced: return g_ui.menuOpen ? "OPEN" : "";
    default: return "";
    }
}
// dir: -1 / +1 = left / right, 0 = select (A / Enter / click)
static void GmChange(int i, int dir) {
    GmId id = kGm[i].id;
    if (kGm[i].kind == GmKind::Slider) {
        GmRange r = GmRangeOf(id);
        float v = GmGet(id) + (dir < 0 ? -r.step : r.step);
        if (dir == 0 && v > r.hi + r.step * 0.01f) v = r.lo;   // A on a slider steps up and wraps
        GmSet(id, v);
        return;
    }
    switch (id) {
    case GmFrameData: g_ui.showMeter = !g_ui.showMeter; break;
    case GmPanelStyle: g_ui.classicMeter = !g_ui.classicMeter; break;
    case GmHitboxes: {
        bool on = !(g_ui.showBoxes && g_ui.showStrike);
        g_ui.showStrike = g_ui.showShapes = on;
        if (on) g_ui.showBoxes = true;
        break;
    }
    case GmHurtboxes: {
        bool on = !(g_ui.showBoxes && g_ui.showHurtPts);
        g_ui.showHurtPts = on;
        if (on) g_ui.showBoxes = true;
        break;
    }
    case GmFill: g_ui.fill = !g_ui.fill; break;
    case GmBuffer: ibuf::g_enabled = !ibuf::g_enabled; break;
    case GmAdvanced: g_ui.menuOpen = !g_ui.menuOpen; break;
    case GmReload: if (dir == 0) g_reloadRequested = true; break;
    case GmScreenshot: if (dir == 0) shot::Request(); break;
    case GmClose: if (dir == 0) g_ui.gameMenu = false; break;
    default: break;
    }
}

// ---- Mortal Kombat 11 menu drawing helpers (colours sampled from the game's pause menu)
static ImU32 MkCol(unsigned rrggbbaa, float a) {
    unsigned al = (unsigned)((rrggbbaa & 0xFF) * a);
    return IM_COL32((rrggbbaa >> 24) & 0xFF, (rrggbbaa >> 16) & 0xFF, (rrggbbaa >> 8) & 0xFF, al > 255 ? 255 : al);
}
static ImU32 MkGoldText(int a) { return IM_COL32(150, 126, 84, a); }
static ImU32 MkGoldDim() { return IM_COL32(138, 111, 67, 255); }
static void MkCentered(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* t) {
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, t);
    dl->AddText(f, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, t);
}
// blood-red title banner with gold rules, chevron ornaments and the small notch under the centre
static void MkBanner(ImDrawList* dl, ImFont* f, float k, ImVec2 a, ImVec2 b, const char* title, float size, bool notch) {
    float my = (a.y + b.y) * 0.5f, w = b.x - a.x;
    ImU32 deep = IM_COL32(40, 2, 2, 255), mid = IM_COL32(58, 2, 2, 255);
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + w * 0.5f, b.y), deep, mid, mid, deep);
    dl->AddRectFilledMultiColor(ImVec2(a.x + w * 0.5f, a.y), b, mid, deep, deep, mid);
    ImU32 gold = IM_COL32(124, 94, 54, 255);
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, a.y), gold, 2.f * k);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), gold, 2.f * k);
    dl->AddLine(ImVec2(a.x, a.y + 7 * k), ImVec2(b.x, a.y + 7 * k), IM_COL32(90, 12, 12, 255), 1.f * k);
    dl->AddLine(ImVec2(a.x, b.y - 7 * k), ImVec2(b.x, b.y - 7 * k), IM_COL32(90, 12, 12, 255), 1.f * k);
    // chevrons: >>-- on the left, --<< on the right
    ImU32 orn = IM_COL32(120, 16, 16, 255);
    for (int side = 0; side < 2; ++side) {
        float dir = side == 0 ? 1.f : -1.f;
        float x0 = side == 0 ? a.x + 28 * k : b.x - 28 * k;
        dl->AddLine(ImVec2(x0, my), ImVec2(x0 + dir * 110 * k, my), orn, 2.f * k);
        for (int c = 0; c < 2; ++c) {
            float cx = x0 + dir * (78 * k + c * 16 * k), h = 11 * k;
            dl->AddLine(ImVec2(cx, my - h), ImVec2(cx + dir * h, my), orn, 2.5f * k);
            dl->AddLine(ImVec2(cx + dir * h, my), ImVec2(cx, my + h), orn, 2.5f * k);
        }
    }
    if (notch) {
        float cx = (a.x + b.x) * 0.5f, n = 9 * k;
        dl->AddTriangleFilled(ImVec2(cx - n, b.y), ImVec2(cx + n, b.y), ImVec2(cx, b.y + n), IM_COL32(40, 2, 2, 255));
        dl->AddLine(ImVec2(cx - n, b.y), ImVec2(cx, b.y + n), gold, 2.f * k);
        dl->AddLine(ImVec2(cx, b.y + n), ImVec2(cx + n, b.y), gold, 2.f * k);
    }
    MkCentered(dl, f, size, ImVec2((a.x + b.x) * 0.5f, my), IM_COL32(234, 226, 224, 255), title);
}
// section header inside a reference card (dark teal fade, like "SPECIAL MOVES")
static void MkSection(ImDrawList* dl, ImFont* f, float k, ImVec2 a, ImVec2 b, const char* t) {
    ImU32 l = IM_COL32(8, 28, 32, 255), r = IM_COL32(2, 10, 12, 255);
    dl->AddRectFilledMultiColor(a, b, l, r, r, l);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), IM_COL32(30, 70, 80, 255), 1.5f * k);
    ImVec2 ts = f->CalcTextSizeA(28 * k, FLT_MAX, 0, t);
    dl->AddText(f, 28 * k, ImVec2(a.x + 14 * k, (a.y + b.y) * 0.5f - ts.y * 0.5f), MkGoldText(255), t);
}
// one line per menu item for the reference card
static const char* GmDesc(GmId id) {
    switch (id) {
    case GmHitboxes: return "Attack areas: strike spheres, weapon shapes and projectiles, drawn exactly where the game's hit test checks them on the frame they are live.";
    case GmHurtboxes: return "The body spheres each fighter can be hit on.";
    case GmHitAlpha: return "How strong hitboxes are drawn. The mark is the default (100%).";
    case GmHurtAlpha: return "How strong hurtboxes are drawn. The mark is the default (100%).";
    case GmLinger: return "Keeps a hitbox on screen for a few frames after it was tested, so 1-frame actives are easy to see.";
    case GmFill: return "Filled shapes, or outlines only.";
    case GmScreenshot: return "Saves a screenshot with the overlay to MK11BoxLab_shots next to the mod.";
    case GmAdvanced: return "Opens the full settings window (colours, labels, debug tools).";
    case GmReload: return "Reloads the mod's settings and code without restarting the game.";
    case GmClose: return "Closes this menu (F2 opens it again).";
    default: return "";
    }
}

static void PadBox(ImDrawList* dl, ImFont* f, float k, ImVec2 p, const char* t, ImU32 col) {
    float fs = 22 * k;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, t);
    float w = ts.x + 12 * k > 30 * k ? ts.x + 12 * k : 30 * k, h = 30 * k;
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), col, 3 * k, 0, 1.6f * k);
    dl->AddText(f, fs, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), col, t);
}

static void DrawGameMenu(float sw, float sh) {
    // ---- input (pad 1 as the game would have seen it, plus keyboard when the game has focus)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool focused = pid == GetCurrentProcessId();
    uint16_t pb = 0; int16_t lx = 0, ly = 0;
    ibuf::RealPad(pb, lx, ly);
    auto key = [&](int vk) { return focused && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool up = (pb & 0x0001) || ly > 16000 || key(VK_UP), dn = (pb & 0x0002) || ly < -16000 || key(VK_DOWN);
    bool lf = (pb & 0x0004) || lx < -16000 || key(VK_LEFT), rt = (pb & 0x0008) || lx > 16000 || key(VK_RIGHT);
    bool ok = (pb & 0x1000) || key(VK_RETURN) || key(VK_SPACE), back = (pb & 0x2000) || key(VK_BACK) || key(VK_ESCAPE);
    static Repeat rUp, rDn, rLf, rRt, rOk, rBack;
    bool& fresh = g_gmFresh;    // ignore buttons already held when the menu opened
    if (fresh) { rUp.prev = up; rDn.prev = dn; rLf.prev = lf; rRt.prev = rt; rOk.prev = ok; rBack.prev = back; fresh = false; }
    int& sel = g_ui.gmSel;
    if (Fire(up, rUp)) sel = (sel + kGmN - 1) % kGmN;
    if (Fire(dn, rDn)) sel = (sel + 1) % kGmN;
    bool okF = Fire(ok, rOk), lfF = Fire(lf, rLf), rtF = Fire(rt, rRt), backF = rBack.prev != back && back;
    rBack.prev = back;
    if (lfF && kGm[sel].kind != GmKind::Action) GmChange(sel, -1);
    if (rtF && kGm[sel].kind != GmKind::Action) GmChange(sel, +1);
    if (okF) GmChange(sel, 0);
    if (backF) g_ui.gameMenu = false;
    if (!g_ui.gameMenu) return;

    // ---- layout: Mortal Kombat 11's pause menu (1440p reference): left panel in a thin gold frame, blood-red
    // title banner, gold caps items, the selected row on a brushed-gold bar; a reference card beside it
    float k = sh / 1440.f;
    ImFont* mf = g_menuFont ? g_menuFont : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float px1 = sw - 46 * k, px0 = px1 - 920 * k, py0 = 118 * k;   // right half of the screen
    const float rowH = 92 * k, fs = 38 * k, vfs = 32 * k;
    const float bannerY0 = 150 * k, bannerY1 = 250 * k;
    const float itemsY0 = 330 * k;
    const float py1 = itemsY0 + kGmN * rowH + 60 * k;

    // panel + frame
    dl->AddRectFilled(ImVec2(px0, py0), ImVec2(px1, py1), IM_COL32(0, 0, 0, 250));
    dl->AddRect(ImVec2(px0, py0), ImVec2(px1, py1), MkGoldDim(), 0, 0, 2.f * k);
    dl->AddRect(ImVec2(px0 - 6 * k, py0 - 6 * k), ImVec2(px1 + 6 * k, py1 + 6 * k), IM_COL32(110, 91, 55, 110), 0, 0, 1.f * k);
    MkBanner(dl, mf, k, ImVec2(px0 + 2 * k, bannerY0), ImVec2(px1 - 2 * k, bannerY1), "BOX LAB", 44 * k, true);
    {   // subtitle under the banner
        const char* sub = "HITBOX VIEWER  -  PRACTICE ONLY  -  v" MKBL_VERSION;
        ImVec2 ss = mf->CalcTextSizeA(22 * k, FLT_MAX, 0, sub);
        dl->AddText(mf, 22 * k, ImVec2((px0 + px1) * 0.5f - ss.x * 0.5f, bannerY1 + 26 * k), MkGoldText(170), sub);
    }

    // mouse: an invisible window over the panel so clicks don't reach the game
    ImGui::SetNextWindowPos(ImVec2(px0, itemsY0));
    ImGui::SetNextWindowSize(ImVec2(px1 - px0, rowH * kGmN));
    ImGui::Begin("##boxlabmenu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
    for (int i = 0; i < kGmN; ++i) {
        float y0 = itemsY0 + i * rowH, y1 = y0 + rowH, cy = (y0 + y1) * 0.5f;
        bool s = i == sel;
        if (s) {
            // brushed gold bar: darker at the ends, lighter toward the left third, bright gold edges
            float xa = px0 + 2 * k, xm = px0 + (px1 - px0) * 0.42f, xb = px1 - 2 * k;
            ImU32 dark = IM_COL32(120, 99, 68, 255), light = IM_COL32(200, 176, 130, 255), dark2 = IM_COL32(118, 98, 66, 255);
            dl->AddRectFilledMultiColor(ImVec2(xa, y0 + 4 * k), ImVec2(xm, y1 - 4 * k), dark, light, light, dark);
            dl->AddRectFilledMultiColor(ImVec2(xm, y0 + 4 * k), ImVec2(xb, y1 - 4 * k), light, dark2, dark2, light);
            dl->AddLine(ImVec2(xa, y0 + 4 * k), ImVec2(xb, y0 + 4 * k), IM_COL32(236, 206, 130, 255), 2.f * k);
            dl->AddLine(ImVec2(xa, y1 - 4 * k), ImVec2(xb, y1 - 4 * k), IM_COL32(236, 206, 130, 255), 2.f * k);
        } else if (i + 1 < kGmN && i + 1 != sel) {
            dl->AddLine(ImVec2(px0 + 2 * k, y1), ImVec2(px1 - 2 * k, y1), IM_COL32(54, 54, 54, 255), 1.5f * k);   // row divider
        }
        const char* lab = kGm[i].label;
        ImVec2 ls = mf->CalcTextSizeA(fs, FLT_MAX, 0, lab);
        float tx = px0 + 62 * k;
        dl->AddText(mf, fs, ImVec2(tx, cy - ls.y * 0.5f), s ? IM_COL32(250, 246, 238, 255) : MkGoldText(255), lab);

        // value, right-aligned in the row
        std::string v = GmValue(kGm[i].id);
        bool slider = kGm[i].kind == GmKind::Slider;
        float right = px1 - 54 * k;
        ImVec2 barA(0, 0), barB(0, 0);
        float vxLeft = right;
        if (!v.empty()) {
            ImU32 vc = s ? IM_COL32(255, 252, 244, 255) : (v == "OFF" ? IM_COL32(120, 104, 78, 255) : IM_COL32(214, 186, 128, 255));
            float vw = slider ? mf->CalcTextSizeA(vfs, FLT_MAX, 0, "100%").x : mf->CalcTextSizeA(vfs, FLT_MAX, 0, "OPEN").x;
            ImVec2 vs = mf->CalcTextSizeA(vfs, FLT_MAX, 0, v.c_str());
            float vx = right - vw;
            dl->AddText(mf, vfs, ImVec2(vx + (vw - vs.x) * 0.5f, cy - vs.y * 0.5f), vc, v.c_str());
            vxLeft = vx;
            if (slider) {
                GmRange r = GmRangeOf(kGm[i].id);
                float frac = (GmGet(kGm[i].id) - r.lo) / (r.hi - r.lo);
                float bw = 210 * k, bh = 8 * k;
                barB = ImVec2(vx - 40 * k, cy + bh * 0.5f);
                barA = ImVec2(barB.x - bw, cy - bh * 0.5f);
                dl->AddRectFilled(barA, barB, s ? IM_COL32(60, 46, 28, 255) : IM_COL32(28, 24, 18, 255));
                dl->AddRectFilledMultiColor(barA, ImVec2(barA.x + bw * frac, barB.y), IM_COL32(110, 22, 22, 255), s ? IM_COL32(200, 40, 36, 255) : IM_COL32(150, 30, 28, 255),
                                            s ? IM_COL32(200, 40, 36, 255) : IM_COL32(150, 30, 28, 255), IM_COL32(110, 22, 22, 255));
                dl->AddRect(ImVec2(barA.x - 1 * k, barA.y - 1 * k), ImVec2(barB.x + 1 * k, barB.y + 1 * k), s ? IM_COL32(250, 236, 200, 220) : MkGoldDim(), 0, 0, 1.f * k);
                if (kGm[i].id == GmHitAlpha || kGm[i].id == GmHurtAlpha) {   // notch at the default (100%)
                    float x = barA.x + bw * (1.f - r.lo) / (r.hi - r.lo);
                    dl->AddLine(ImVec2(x, barA.y - 5 * k), ImVec2(x, barB.y + 5 * k), s ? IM_COL32(255, 250, 240, 230) : MkGoldText(160), 1.5f * k);
                }
                // diamond knob
                ImVec2 kc(barA.x + bw * frac, cy);
                float kr = (s ? 10 : 7) * k;
                dl->AddQuadFilled(ImVec2(kc.x, kc.y - kr), ImVec2(kc.x + kr, kc.y), ImVec2(kc.x, kc.y + kr), ImVec2(kc.x - kr, kc.y),
                                  s ? IM_COL32(255, 250, 240, 255) : IM_COL32(214, 186, 128, 255));
                vxLeft = barA.x;
            }
            if (s && kGm[i].kind != GmKind::Action) {   // arrows around the value
                float ah = 9 * k, ax = vxLeft - 26 * k, bx = right + 16 * k;
                ImU32 ac = IM_COL32(255, 252, 244, 255);
                dl->AddTriangleFilled(ImVec2(ax, cy), ImVec2(ax + ah, cy - ah), ImVec2(ax + ah, cy + ah), ac);
                dl->AddTriangleFilled(ImVec2(bx + ah, cy), ImVec2(bx, cy - ah), ImVec2(bx, cy + ah), ac);
            }
        }
        ImGui::PushID(i);
        if (slider) {
            // the bar can be clicked / dragged; the rest of the row behaves like any item
            float pad = 12 * k;
            ImGui::SetCursorScreenPos(ImVec2(barA.x - pad, y0));
            ImGui::InvisibleButton("bar", ImVec2(barB.x - barA.x + pad * 2, rowH));
            if (ImGui::IsItemActive()) {
                sel = i;
                GmRange r = GmRangeOf(kGm[i].id);
                float f = (ImGui::GetIO().MousePos.x - barA.x) / (barB.x - barA.x);
                f = f < 0 ? 0 : f > 1 ? 1 : f;
                GmSet(kGm[i].id, r.lo + f * (r.hi - r.lo));
            }
            if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) sel = i;
            ImGui::SetCursorScreenPos(ImVec2(px0, y0));
            if (ImGui::InvisibleButton("it", ImVec2(barA.x - pad - px0, rowH))) { sel = i; GmChange(i, 0); }
            if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) sel = i;
            ImGui::SetCursorScreenPos(ImVec2(barB.x + pad, y0));
            if (ImGui::InvisibleButton("it2", ImVec2(px1 - (barB.x + pad), rowH))) { sel = i; GmChange(i, +1); }
        } else {
            ImGui::SetCursorScreenPos(ImVec2(px0, y0));
            if (ImGui::InvisibleButton("it", ImVec2(px1 - px0, rowH))) { sel = i; GmChange(i, 0); }
        }
        if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) sel = i;
        ImGui::PopID();
    }
    ImGui::End();

    // ---- reference card beside the panel (the game's move-list card): legend + what the selected item does
    {
        float cx1 = px0 - 60 * k, cx0 = cx1 - 620 * k, cy0 = bannerY0 + 20 * k;   // left of the panel
        float hdr = 58 * k;
        const char* desc = GmDesc(kGm[sel].id);
        // body height from the wrapped description
        float wrapW = cx1 - cx0 - 56 * k;
        ImVec2 ds = mf->CalcTextSizeA(26 * k, FLT_MAX, wrapW, desc);
        float legendH = 3 * 60 * k;
        float cy1 = cy0 + hdr + 70 * k + legendH + 70 * k + ds.y + 40 * k;
        dl->AddRectFilled(ImVec2(cx0, cy0), ImVec2(cx1, cy1), IM_COL32(0, 0, 0, 228));
        dl->AddRect(ImVec2(cx0, cy0), ImVec2(cx1, cy1), IM_COL32(150, 150, 150, 200), 0, 0, 1.5f * k);
        dl->AddRectFilled(ImVec2(cx0 + 1.5f * k, cy0 + 1.5f * k), ImVec2(cx1 - 1.5f * k, cy0 + hdr), IM_COL32(16, 44, 60, 255));
        dl->AddLine(ImVec2(cx0, cy0 + hdr), ImVec2(cx1, cy0 + hdr), IM_COL32(120, 150, 170, 200), 1.5f * k);
        MkCentered(dl, mf, 32 * k, ImVec2((cx0 + cx1) * 0.5f, cy0 + hdr * 0.5f), IM_COL32(236, 230, 218, 255), "QUICK REFERENCE");
        float y = cy0 + hdr + 14 * k;
        MkSection(dl, mf, k, ImVec2(cx0 + 14 * k, y), ImVec2(cx1 - 14 * k, y + 48 * k), "COLOURS");
        y += 62 * k;
        struct Leg { ImU32 col; const char* name; const char* what; } legs[] = {
            {g_cfg.colHit, "HITBOX", "attack, as the game tests it"},
            {0xFF9020D0u, "PROJECTILE", "projectile / thrown shape"},
            {g_cfg.colHurt, "HURTBOX", "where the fighter can be hit"},
        };
        for (auto& l : legs) {
            ImVec2 a(cx0 + 22 * k, y), b(cx1 - 22 * k, y + 50 * k);
            dl->AddRect(a, b, IM_COL32(60, 60, 60, 255), 0, 0, 1.5f * k);
            ImVec2 c(a.x + 32 * k, (a.y + b.y) * 0.5f);
            dl->AddCircleFilled(c, 13 * k, MkCol(l.col, 0.6f), 24);
            dl->AddCircle(c, 13 * k, MkCol(l.col | 0xFF, 1.f), 24, 2.f * k);
            ImVec2 ns = mf->CalcTextSizeA(26 * k, FLT_MAX, 0, l.name);
            dl->AddText(mf, 26 * k, ImVec2(a.x + 62 * k, c.y - ns.y * 0.5f), IM_COL32(222, 214, 200, 255), l.name);
            ImVec2 ws = mf->CalcTextSizeA(21 * k, FLT_MAX, 0, l.what);
            dl->AddText(mf, 21 * k, ImVec2(b.x - 16 * k - ws.x, c.y - ws.y * 0.5f), MkGoldText(200), l.what);
            y += 60 * k;
        }
        y += 10 * k;
        MkSection(dl, mf, k, ImVec2(cx0 + 14 * k, y), ImVec2(cx1 - 14 * k, y + 48 * k), kGm[sel].label);
        y += 66 * k;
        dl->AddText(mf, 26 * k, ImVec2(cx0 + 28 * k, y), IM_COL32(222, 214, 200, 255), desc, nullptr, wrapW);
    }

    // ---- footer: the game's centred status line + button prompts
    {
        const char* st = "PRACTICE MODE ONLY  -  HITBOXES ARE READ FROM THE GAME'S OWN HIT TEST";
        MkCentered(dl, mf, 26 * k, ImVec2(sw * 0.5f, sh - 50 * k), MkGoldText(230), st);
        float y = py1 + 30 * k, x = px0 + 8 * k;
        struct Hint { char btn; ImU32 col; const char* key; const char* text; } hints[] = {
            {'A', IM_COL32(98, 160, 52, 255), "ENTER", "SELECT"}, {'B', IM_COL32(200, 40, 40, 255), "ESC", "BACK"},
            {0, 0, "F2", "CLOSE"}, {0, 0, "F8", "SCREENSHOT"}};
        for (auto& h : hints) {
            if (h.btn) {
                char t[2] = {h.btn, 0};
                ImVec2 c(x + 17 * k, y + 17 * k);
                dl->AddCircleFilled(c, 17 * k, IM_COL32(16, 14, 12, 255), 24);
                dl->AddCircle(c, 17 * k, h.col, 24, 2.5f * k);
                MkCentered(dl, mf, 24 * k, c, h.col, t);
                x += 42 * k;
            } else {
                ImVec2 ks = mf->CalcTextSizeA(22 * k, FLT_MAX, 0, h.key);
                float w = ks.x + 16 * k;
                dl->AddRect(ImVec2(x, y + 2 * k), ImVec2(x + w, y + 32 * k), MkGoldDim(), 0, 0, 1.5f * k);
                dl->AddText(mf, 22 * k, ImVec2(x + 8 * k, y + 17 * k - ks.y * 0.5f), MkGoldText(255), h.key);
                x += w + 10 * k;
            }
            ImVec2 ts = mf->CalcTextSizeA(26 * k, FLT_MAX, 0, h.text);
            dl->AddText(mf, 26 * k, ImVec2(x, y + 17 * k - ts.y * 0.5f), IM_COL32(232, 222, 202, 255), h.text);
            x += ts.x + 34 * k;
        }
    }
}

// a short notice in the game's banner style (under the health bars)
static void MkToast(float sw, float sh, const char* t) {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ImFont* f = g_menuFont ? g_menuFont : ImGui::GetFont();
    float k = sh / 1440.f, fs = 30 * k;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, t);
    float w = ts.x + 300 * k;
    float cx = g_ui.gameMenu ? (sw - 966 * k) * 0.5f : sw * 0.5f;   // beside the Box Lab panel when it is open
    ImVec2 a(cx - w * 0.5f, sh - 250 * k), b(cx + w * 0.5f, sh - 250 * k + 64 * k);   // above the game's status line
    MkBanner(fg, f, k, a, b, t, fs, false);
}

void UiDraw(float sw, float sh) {
    trace::Poll();
    if (mk::Ready()) mk::PollStep();
    mem::EvalCtx ctx;
    if (!g_cfg.demo) {
        if (g_cfg.p1) ctx.p1 = g_cfg.p1->Eval(ctx);
        if (g_cfg.p2) ctx.p2 = g_cfg.p2->Eval(ctx);
    }
    mk::Snapshot snap;
    bool haveGame = mk::Ready();
    if (haveGame) snap = mk::GetSnapshot();
    if (!(haveGame && CameraFromGame(g_cam, snap, sw, sh))) CameraUpdate(g_cam, ctx, sw, sh);

    bool hideForPause = haveGame && mk::GamePaused();   // pause menu open: keep the menu readable
    // in a fight = both fighters exist and the match clock is running (or deliberately stopped: pause menu /
    // freeze). Main menu, character select, loading and intros show nothing.
    bool inFight = false;
    {
        static uint64_t lastFrame = 0;
        static ULONGLONG lastAdvance = 0;
        ULONGLONG now = GetTickCount64();
        // practice mode only: anywhere else (versus, online, story, towers ...) the overlay stays off
        bool practice = haveGame && (snap.gameMode == "practice_mode" || g_cfg.devUnlock);
        {
            static std::string lastMode = "?";
            if (haveGame && snap.gameMode != lastMode) { lastMode = snap.gameMode; Log("ui: game mode '%s'", lastMode.c_str()); }
        }
        ibuf::SetAllowed(practice);
        g_game.locked = haveGame && !practice && !g_cfg.demo;
        if (g_game.locked) g_game.lockReason = "Overlay off: practice mode only (" + (snap.gameMode.empty() ? std::string("menus") : snap.gameMode) + ")";
        bool both = practice && snap.p[0].valid && snap.p[1].valid;
        if (both && snap.frame != lastFrame) { lastFrame = snap.frame; lastAdvance = now; }
        if (!both) lastAdvance = 0;
        inFight = both && (hideForPause || mk::Frozen() || (lastAdvance && now - lastAdvance < 1500));
        static int logged = -1;
        if ((int)inFight != logged) {
            logged = inFight;
            Log("ui: in fight %d (P1 %d P2 %d, paused %d, frame %llu)", (int)inFight, haveGame && snap.p[0].valid, haveGame && snap.p[1].valid,
                (int)hideForPause, (unsigned long long)snap.frame);
        }
    }
    {
        // diagnostics: log pause-flag candidates and camera changes
        static int lastP[3] = {-1, -1, -1};
        static uintptr_t lastCam = 0;
        uintptr_t gi = haveGame ? mk::GameInfo() : 0;
        if (gi) {
            int v[3] = {mem::ReadOr<int>(gi + 0x10, 0), mem::ReadOr<int>(gi + 0x14, 0), mem::ReadOr<int>(gi + 0x148, 0)};
            if (v[0] != lastP[0] || v[1] != lastP[1] || v[2] != lastP[2]) {
                Log("pauseflags GI+10 %d GI+14 %X GI+148 %d (P1 %X P2 %X)", v[0], v[1], v[2], snap.p[0].stateFlags, snap.p[1].stateFlags);
                for (int i = 0; i < 3; ++i) lastP[i] = v[i];
            }
        }
        if (haveGame && snap.camPtr != lastCam) { Log("camera object %llX -> %llX", (unsigned long long)lastCam, (unsigned long long)snap.camPtr); lastCam = snap.camPtr; }
    }
    if (!g_game.locked && inFight && hideForPause && g_ui.gameMenu && g_ui.showMeter) {
        if (g_ui.classicMeter) DrawFrameMeter(sw, sh); else DrawFrameMeterGame(sw, sh);
    }
    if (!g_game.locked && !hideForPause && (inFight || g_cfg.demo)) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        if (g_ui.showBoxes)
            BoxesDraw(bg, g_cam, ctx, g_ui.showHurt, g_ui.showHit, g_ui.showThrow, g_ui.showPush, g_ui.showOther, g_ui.fill);
        if (g_ui.showMarkers) DrawPlayerMarkers(bg, g_cam);
        if (haveGame && g_ui.showBones) DrawBones(bg, g_cam, snap, g_ui.boneRadius, g_ui.boneLabels);
        const int hurtV0 = bg->VtxBuffer.Size;   // vertex ranges: hurtboxes, then hitboxes (strike + shaped / projectiles)
        if (haveGame && g_ui.showHurtPts && g_ui.showBoxes) DrawHurt(bg, g_cam, snap, g_ui.zeroRadius, g_ui.fill, g_ui.hurtLabels);
        const int hitV0 = bg->VtxBuffer.Size;
        if (haveGame && g_ui.showStrike && g_ui.showBoxes) DrawStrike(bg, g_cam, snap, g_ui.strikeLinger, g_ui.fill);
        if (haveGame && g_ui.showShapes && g_ui.showBoxes) DrawShapes(bg, g_cam, snap, g_ui.shapesInactive, g_ui.fill, g_ui.strikeLinger);
        const int hitV1 = bg->VtxBuffer.Size;
        auto scaleAlpha = [&](int v0, int v1, float a) {
            if (a > 0.999f && a < 1.001f) return;
            if (a < 0.05f) a = 0.05f;
            for (int i = v0; i < v1; ++i) {
                ImU32& c = bg->VtxBuffer[i].col;
                float fa = ((c >> IM_COL32_A_SHIFT) & 0xFF) * a + 0.5f;
                ImU32 al = (ImU32)(fa > 255.f ? 255.f : fa);   // above 100%: fills get denser; outlines are already opaque
                c = (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
            }
        };
        scaleAlpha(hurtV0, hitV0, g_ui.hurtAlpha);
        scaleAlpha(hitV0, hitV1, g_ui.hitAlpha);
        if (g_cfg.demo && g_ui.showBoxes) {
            // demo shapes so rendering can be verified with no offsets
            float cx = sw * 0.5f, cy = sh * 0.5f;
            bool active = g_game.p[0].fstate == FState::Active;
            bg->AddCircle(ImVec2(cx - 140, cy), 60, IM_COL32(60, 140, 255, 255), 32, 2.f);
            bg->AddCircleFilled(ImVec2(cx - 140, cy), 60, IM_COL32(60, 140, 255, 70), 32);
            bg->AddCircle(ImVec2(cx + 140, cy), 60, IM_COL32(60, 140, 255, 255), 32, 2.f);
            bg->AddCircleFilled(ImVec2(cx + 140, cy), 60, IM_COL32(60, 140, 255, 70), 32);
            if (active) {
                bg->AddCircleFilled(ImVec2(cx - 30, cy - 10), 30, IM_COL32(255, 48, 48, 110), 24);
                bg->AddCircle(ImVec2(cx - 30, cy - 10), 30, IM_COL32(255, 48, 48, 255), 24, 2.f);
            }
        }
        if (g_ui.showMeter) { if (g_ui.classicMeter) DrawFrameMeter(sw, sh); else DrawFrameMeterGame(sw, sh); }
    }
    {
        // which menus are open survives a hot reload (kept in the process environment)
        static bool restored = false;
        static int saved = -1;
        if (!restored) {
            restored = true;
            char b[8] = "";
            if (GetEnvironmentVariableA("MKBL_MENUS", b, sizeof(b)) && b[0]) {
                int v = atoi(b);
                g_ui.menuOpen = (v & 1) != 0;
                g_ui.gameMenu = (v & 2) != 0;
                saved = v;
            }
        }
        int cur = (g_ui.menuOpen ? 1 : 0) | (g_ui.gameMenu ? 2 : 0);
        if (cur != saved) { saved = cur; char b[8]; snprintf(b, sizeof(b), "%d", cur); SetEnvironmentVariableA("MKBL_MENUS", b); }
    }
    {
        static bool wasOpen = false;
        if (g_ui.gameMenu && !wasOpen) g_gmFresh = true;
        wasOpen = g_ui.gameMenu;
    }
    ibuf::SetCapture(g_ui.gameMenu);
    dib::SetBlock(g_ui.gameMenu);
    bool hideMenus = shot::HidingMenus();   // a screenshot is being taken: only the overlay itself
    if (g_ui.gameMenu && !hideMenus) DrawGameMenu(sw, sh);
    if (g_ui.menuOpen && !hideMenus) DrawMenu();
    {
        // once per game session: tell new users where the menu is
        static ULONGLONG hintUntil = 0;
        static bool hinted = false;
        if (!hinted && inFight) {
            hinted = true;
            char b[4] = "";
            if (!GetEnvironmentVariableA("MKBL_HINTED", b, sizeof(b))) { hintUntil = GetTickCount64() + 7000; SetEnvironmentVariableA("MKBL_HINTED", "1"); }
        }
        if (!hideMenus && !g_ui.gameMenu && GetTickCount64() < hintUntil && shot::Status().empty()) {
            MkToast(sw, sh, "MK11 BOX LAB  -  PRESS F2 FOR THE MENU");
        }
    }
    if (!hideMenus) {
        std::string st = shot::Status();
        if (!st.empty()) {
            MkToast(sw, sh, st.c_str());
        }
    }
    if (g_ui.showExplorer && g_ui.menuOpen && !hideMenus) DrawExplorer();
    if (g_ui.showWatches && g_ui.menuOpen && !hideMenus) DrawWatches();
    if (g_ui.showCollList && g_ui.menuOpen && !hideMenus) DrawCollList();
}

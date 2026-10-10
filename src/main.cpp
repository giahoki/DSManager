// DSM — Dark Souls mod manager. GLFW + OpenGL, hand-drawn UI.
#include "gl.h"
#include "modmanager.h"
#include "patch.h"
#include "remote.h"
#include "render.h"
#include "version.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <dwmapi.h>
#include <shobjidl.h>

#include "stb_image_write.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <memory>
#include <unordered_map>

// The live palette: blended every frame towards the theme of the current screen.
namespace theme {
Col parch, parchDim, ash, gold, goldDim, ember, emberHot, blood;
}  // namespace theme
using namespace theme;

static const char* kQuotes[] = {
    "Пламя угасает, но пепел ещё помнит тепло.",
    "Каждый мод — уголёк, брошенный во тьму.",
    "Здесь некуда спешить. Только костёр и тишина.",
    "Даже угасший мир можно переписать заново.",
    "Отдохни у огня. Дорога ещё долгая.",
};

struct LogLine {
    std::string time, text;
    LogLevel level;
    float at = 0;  // app time, for the fading message line
};

static fs::path pickFolder(HWND owner, const wchar_t* title)
{
    fs::path out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

struct App {
    GLFWwindow* win = nullptr;
    Renderer R;
    std::vector<std::unique_ptr<ModManager>> managers;  // one per game in games()
    int cur = -1;                                       // -1: the game selection screen
    std::vector<Image> logos;                           // per game, empty if it has none
    ModManager& mm() { return *managers[cur < 0 ? 0 : cur]; }
    fs::path root;
    float s = 1;  // UI scale

    // input
    float mx = -1, my = -1, wheel = 0;
    bool down = false, pressed = false, prevDown = false;

    // animation
    float t = 0, dt = 1.f / 60.f;
    std::unordered_map<std::string, float> anim;

    // state
    int selected = 0;
    float scroll = 0, scrollTarget = 0;
    std::mutex logMx;
    std::deque<LogLine> log;
    std::thread worker;
    std::atomic<bool> busy{false};
    std::atomic<float> progress{0};
    std::atomic<int> rescan{-1};  // game whose mods folder the worker changed: rescanned on the main thread
    bool dragging = false;
    bool back = false;
    Theme live = selectionTheme();  // what is on screen now, eased towards the target theme
    Rect titleHit;                    // game title + medallion: click to go back

    // selection <-> game, like resting at a bonfire: the screen goes dark around the sigil,
    // the fire sinks to an ember, then flares up again and the other screen appears in its light
    bool resting = false, restEnter = false;
    int restGame = -1;
    float restStart = 0, selStart = 0;
    float emberX = 0, emberY = 0;  // where the sigil starts (the card), it drifts to the centre

    float restDuration() const { return restEnter ? 2.0f : 2.0f; }

    void startRest(int game, bool enter, float fromX, float fromY)
    {
        if (resting) return;
        resting = true;
        restEnter = enter;
        restGame = game;
        restStart = t;
        emberX = fromX;
        emberY = fromY;
        if (enter) open(game);
        else selStart = t + 0.55f * restDuration() - 0.4f;  // cards rise as the light comes back
    }

    // window open / close / minimize animation
    bool shot = false;            // headless screenshot: no window opacity
    float openStart = 0, openDur = 1.2f;
    bool closing = false, allowClose = false, minimizing = false, iconified = false;
    float closeStart = 0, minStart = 0;

    void startClose()
    {
        if (closing) return;
        closing = true;
        closeStart = t;
    }
    void startMinimize()
    {
        if (closing || minimizing) return;
        minimizing = true;
        minStart = t;
    }
    void restored()
    {
        iconified = false;
        minimizing = false;
        openStart = t;
        openDur = 0.7f;
    }
    POINT dragCursor{};
    int dragWinX = 0, dragWinY = 0;

    float S(float v) const { return v * s; }

    float ease(const std::string& id, float target, float speed = 12.f)
    {
        float& a = anim.try_emplace(id, target).first->second;
        a += (target - a) * (1.f - std::exp(-dt * speed));
        return a;
    }
    bool hover(const Rect& r) const { return !dragging && r.contains(mx, my); }
    bool click(const Rect& r) const { return pressed && hover(r); }

    void addLog(LogLevel lvl, const std::string& text)
    {
        std::lock_guard lock(logMx);
        log.push_back({clockTime(), text, lvl, t});
        while (log.size() > 60) log.pop_front();
    }

    void runTask(bool restore)
    {
        if (busy) return;
        if (worker.joinable()) worker.join();
        busy = true;
        progress = 0;
        addLog(LogLevel::Info, restore ? "Возвращаю оригинальные файлы…" : "Применяю моды…");
        worker = std::thread([this, restore] {
            auto L = [this](LogLevel l, const std::string& m) { addLog(l, m); };
            if (restore) mm().restoreAll(L, &progress);
            else mm().apply(L, &progress);
            busy = false;
        });
    }

    // Local mods are rescanned at once; GitHub is checked in the background for new mods and newer versions.
    void runSync()
    {
        if (busy) return;
        if (worker.joinable()) worker.join();
        mm().scan();
        busy = true;
        progress = 0;
        addLog(LogLevel::Info, "Проверяю моды на GitHub…");
        worker = std::thread([this, game = cur, m = &mm()] {
            auto L = [this](LogLevel l, const std::string& msg) { addLog(l, msg); };
            syncMods(*m->game, m->modsDir, L, &progress);
            rescan = game;
            busy = false;
        });
    }

    // ------------------------------------------------------------ decorations

    void divider(float cx, float y, float halfW, float alpha)
    {
        R.gradH({cx - halfW, y, halfW - S(10), S(1)}, gold.withA(0), gold.withA(0.55f * alpha));
        R.gradH({cx + S(10), y, halfW - S(10), S(1)}, gold.withA(0.55f * alpha), gold.withA(0));
        R.diamond(cx, y + S(0.5f), S(3.5f), gold.withA(0.85f * alpha));
    }

    void panel(const Rect& r, const char* title)
    {
        R.shadow(r, S(4), S(26), rgb(0, 0, 0, 0.60f));
        R.gradV(r, rgb(19, 16, 16, 0.64f), rgb(8, 7, 8, 0.80f), S(3));
        R.frame(r, gold.withA(0.26f), S(1), S(3));
        R.frame(r.inset(S(5)), gold.withA(0.07f), S(1), S(2));

        // corner ornaments
        float L = S(46);
        struct C { float x, y; int sx, sy; } cs[] = {
            {r.x, r.y, 1, 1}, {r.r(), r.y, -1, 1}, {r.x, r.b(), 1, -1}, {r.r(), r.b(), -1, -1}};
        for (auto& c : cs) {
            float lx = c.sx > 0 ? c.x + S(7) : c.x - S(7) - L;
            R.gradH({lx, c.y - S(0.5f), L, S(1)}, gold.withA(c.sx > 0 ? 0.8f : 0.f), gold.withA(c.sx > 0 ? 0.f : 0.8f));
            float ly = c.sy > 0 ? c.y + S(7) : c.y - S(7) - L;
            R.gradV({c.x - S(0.5f), ly, S(1), L}, gold.withA(c.sy > 0 ? 0.8f : 0.f), gold.withA(c.sy > 0 ? 0.f : 0.8f));
            R.diamond(c.x, c.y, S(4), gold.withA(0.9f));
            R.diamond(c.x, c.y, S(1.6f), rgb(20, 16, 14));
        }

        if (title) {
            float tw = R.measure(R.heading, title, S(4));
            float cx = r.x + r.w * 0.5f, ty = r.y + S(16);
            R.text(R.heading, cx - tw * 0.5f, ty, title, gold, S(4));
            float ly = ty + R.heading.ascent * 0.62f;
            float side = std::min(S(150), r.w * 0.5f - tw * 0.5f - S(30));
            R.gradH({cx - tw * 0.5f - S(16) - side, ly, side, S(1)}, gold.withA(0), gold.withA(0.45f));
            R.gradH({cx + tw * 0.5f + S(16), ly, side, S(1)}, gold.withA(0.45f), gold.withA(0));
            R.diamond(cx - tw * 0.5f - S(11), ly + S(0.5f), S(2.5f), gold.withA(0.8f));
            R.diamond(cx + tw * 0.5f + S(11), ly + S(0.5f), S(2.5f), gold.withA(0.8f));
        }
    }

    // kind: 0 normal, 1 primary (ember), 2 quiet
    bool button(const std::string& id, const Rect& r, const char* label, int kind = 0, bool enabled = true)
    {
        bool hv = enabled && hover(r);
        float h = ease(id, hv ? 1.f : 0.f, 14);
        float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        float dim = enabled ? 1.f : 0.35f;

        if (kind == 1 && enabled) {
            float pulse = mm().dirty ? 0.5f + 0.5f * std::sin(t * 2.4f) : 0.f;
            R.glow(cx, cy, r.w * 0.75f, ember.withA(0.06f + 0.10f * h + 0.08f * pulse));
        }
        R.shadow(r, S(2), S(10), rgb(0, 0, 0, 0.5f));
        R.gradV(r, rgb(30, 25, 23, 0.92f), rgb(13, 11, 11, 0.96f), S(2));
        if (h > 0.01f) R.gradV(r, ember.withA(0.10f * h), ember.withA(0.0f), S(2));
        Col border = kind == 1 ? lerp(gold.withA(0.65f), emberHot.withA(0.95f), h) : lerp(goldDim.withA(0.55f), gold.withA(0.9f), h);
        R.frame(r, border.mul(dim), S(1), S(2));
        R.gradH({r.x + S(10), r.y + S(1), r.w * 0.5f - S(10), S(1)}, rgb(255, 230, 200, 0), rgb(255, 230, 200, 0.10f * dim));
        R.gradH({cx, r.y + S(1), r.w * 0.5f - S(10), S(1)}, rgb(255, 230, 200, 0.10f * dim), rgb(255, 230, 200, 0));
        if (h > 0.01f) {
            R.diamond(r.x, cy, S(3.5f) * h, emberHot.withA(h));
            R.diamond(r.r(), cy, S(3.5f) * h, emberHot.withA(h));
        }

        const Font& f = R.button;
        float tw = R.measure(f, label, S(2.5f));
        Col tc = kind == 1 ? lerp(gold, emberHot, h) : lerp(parchDim, parch, h);
        R.text(f, cx - tw * 0.5f, cy - (f.ascent + f.descent) * 0.5f + S(1), label, tc.mul(dim), S(2.5f));
        return enabled && click(r);
    }

    bool linkButton(const std::string& id, float x, float y, const char* label, bool enabled = true)
    {
        float w = R.measure(R.italic, label);
        Rect r{x, y, w, R.italic.ascent + R.italic.descent};
        float h = ease(id, enabled && hover(r) ? 1.f : 0.f, 14);
        R.text(R.italic, x, y, label, lerp(parchDim, gold, h).mul(enabled ? 1.f : 0.4f));
        R.gradH({x, r.b() - S(1), w * h, S(1)}, gold.withA(0.6f * h), gold.withA(0));
        return enabled && click(r);
    }

    // A bonfire: lit when the mod is on.
    bool bonfire(const std::string& id, float cx, float cy, bool on, bool enabled)
    {
        Rect hit{cx - S(18), cy - S(18), S(36), S(36)};
        float a = ease(id, on ? 1.f : 0.f, 9);
        float h = ease(id + "h", enabled && hover(hit) ? 1.f : 0.f, 14);
        float seed = float(std::hash<std::string>{}(id) % 1000);
        float flick = 0.80f + 0.12f * std::sin(t * 9.1f + seed) + 0.08f * std::sin(t * 15.3f + seed * 2.f);

        R.glow(cx, cy, S(30), ember.withA(0.32f * a * flick));
        float rr = S(10);
        R.rect({cx - rr, cy - rr, rr * 2, rr * 2}, rgb(12, 10, 10, 0.95f), rr);
        R.frame({cx - rr, cy - rr, rr * 2, rr * 2}, lerp(lerp(goldDim, gold, h), emberHot, a).withA(0.85f), S(1.2f), rr);
        float ir = S(5.5f) * (0.35f + 0.65f * a);
        R.rect({cx - ir, cy - ir, ir * 2, ir * 2}, lerp(ash.withA(0.35f), ember, a).withA(0.35f + 0.65f * a), ir);
        R.glow(cx, cy, S(8), emberHot.withA(0.9f * a * flick));
        return enabled && click(hit);
    }

    // ------------------------------------------------------------ layout

    void header(float W)
    {
        float x = S(44), y = S(24);
        const std::string& gname = mm().game->name;
        float tw = R.measure(R.title, gname, S(5));
        const Image& logo = logos[cur];

        // the medallion and the title lead back to the world selection
        titleHit = {x - S(6), y - S(8), (logo ? S(70) : 0.f) + tw + S(12), S(66)};
        bool canBack = !busy && !resting;
        float th = ease("titleHover", canBack && hover(titleHit) ? 1.f : 0.f, 12);
        if (canBack && click(titleHit)) back = true;

        if (logo) {  // small medallion left of the title
            medallion(logo, x + S(26), y + S(28), S(28), ember, th, true);
            x += S(70);
        }
        R.glow(x + tw * 0.5f, y + S(30), tw * 0.9f, ember.withA(0.05f + 0.07f * th));
        R.text(R.title, x, y, gname, lerp(parch, emberHot, th * 0.35f), S(5));

        float lx = x + tw + S(22);
        float cardX = W - S(44) - S(96) - S(380);
        if (lx + S(20) + R.measure(R.heading, "менеджер модов", S(4)) < cardX - S(24)) {
            R.gradV({lx, y + S(10), S(1), S(36)}, gold.withA(0), gold.withA(0.6f));
            R.text(R.heading, lx + S(20), y + S(22), "менеджер модов", gold, S(4));
        }

        // slowly cycling line under the title
        const int n = (int)std::size(kQuotes);
        float period = 9.f;
        int qi = int(t / period) % n;
        float ph = std::fmod(t, period);
        float qa = std::min(1.f, std::min(ph / 1.2f, (period - ph) / 1.2f));
        // on hover the quote gives way to where the click leads
        R.text(R.italic, x + S(2), y + S(60), kQuotes[qi], parchDim.withA(0.85f * qa * (1.f - th)));
        if (th > 0.01f) {
            R.text(R.italic, x + S(2) + S(10) * (1.f - th), y + S(60), "‹ вернуться к выбору мира", gold.withA(th));
            R.gradH({x + S(2), y + S(84), S(200) * th, S(1)}, gold.withA(0.6f * th), gold.withA(0));
        }

        // game card
        Rect card{W - S(44) - S(96) - S(380), S(26), S(380), S(64)};
        bool found = !mm().gamePath.empty();
        float h = ease("card", hover(card) && !busy ? 1.f : 0.f, 12);
        R.shadow(card, S(3), S(16), rgb(0, 0, 0, 0.5f));
        R.gradV(card, rgb(22, 18, 17, 0.85f), rgb(10, 9, 9, 0.9f), S(3));
        R.frame(card, lerp(goldDim.withA(0.4f), gold.withA(0.8f), h), S(1), S(3));
        float dcx = card.x + S(24), dcy = card.y + card.h * 0.5f;
        Col dc = found ? ember : blood;
        R.glow(dcx, dcy, S(18), dc.withA(0.35f + 0.1f * std::sin(t * 3.f)));
        R.diamond(dcx, dcy, S(5), dc);
        R.text(R.heading, card.x + S(46), card.y + S(11), "папка игры", gold, S(3));
        std::string sub = found ? pathUtf8(mm().gamePath) : "игра не найдена — нажми, чтобы указать папку";
        R.text(R.caption, card.x + S(46), card.y + S(36), R.ellipsize(R.caption, sub, card.w - S(60)),
               found ? parchDim.withA(0.8f) : blood);
        if (click(card) && !busy) {
            std::wstring pickTitle = L"Папка с " + mm().game->exe;
            fs::path p = pickFolder(glfwGetWin32Window(win), pickTitle.c_str());
            if (!p.empty()) {
                if (isGameFolder(*mm().game, p)) {
                    mm().setGamePath(p);
                    addLog(LogLevel::Good, "Папка игры: " + pathUtf8(p));
                } else {
                    addLog(LogLevel::Error, "В этой папке нет " + toUtf8(mm().game->exe));
                }
            }
        }

        chrome(W, card);
    }

    // close / minimize and dragging the borderless window by its top strip
    void chrome(float W, const Rect& keepOut)
    {
        Rect closeR{W - S(56), S(12), S(40), S(30)}, minR{W - S(100), S(12), S(40), S(30)};
        float hc = ease("close", hover(closeR) ? 1.f : 0.f, 16), hm = ease("min", hover(minR) ? 1.f : 0.f, 16);
        R.rect(closeR, blood.withA(0.55f * hc), S(3));
        R.rect(minR, rgb(255, 240, 220, 0.06f * hm), S(3));
        {
            Col cc = lerp(parchDim, parch, hc), mc = lerp(parchDim, parch, hm);
            float ccx = closeR.x + closeR.w * 0.5f, ccy = closeR.y + closeR.h * 0.5f;
            for (int k = -5; k <= 5; ++k) {  // an x out of small diamonds: crisp at any scale
                R.diamond(ccx + S(float(k)), ccy + S(float(k)), S(1.1f), cc);
                R.diamond(ccx + S(float(k)), ccy - S(float(k)), S(1.1f), cc);
            }
            R.rect({minR.x + minR.w * 0.5f - S(6), minR.y + minR.h * 0.5f, S(12), S(1.3f)}, mc);
        }
        if (click(closeR)) startClose();
        if (click(minR)) startMinimize();

        // drag the borderless window by the header
        Rect drag{0, 0, W, S(100)};
        if (pressed && drag.contains(mx, my) && !closeR.contains(mx, my) && !minR.contains(mx, my) &&
            !keepOut.contains(mx, my) && !(cur >= 0 && titleHit.contains(mx, my))) {
            dragging = true;
            GetCursorPos(&dragCursor);
            glfwGetWindowPos(win, &dragWinX, &dragWinY);
        }
        if (dragging) {
            if (!down) dragging = false;
            else {
                POINT p;
                GetCursorPos(&p);
                glfwSetWindowPos(win, dragWinX + (p.x - dragCursor.x), dragWinY + (p.y - dragCursor.y));
            }
        }
    }

    void modList(const Rect& pr)
    {
        panel(pr, "моды");
        Rect area{pr.x + S(14), pr.y + S(52), pr.w - S(28), pr.h - S(66)};
        bool interactive = !busy;

        if (mm().mods.empty()) {
            float cy = area.y + area.h * 0.38f;
            const char* l1 = "Здесь пока пусто.";
            R.text(R.big, pr.x + (pr.w - R.measure(R.big, l1)) * 0.5f, cy - S(40), l1, parch.withA(0.85f));
            std::string l2 = "Положи мод в  mods\\" + mm().game->name + "\\<имя мода>\\files";
            R.text(R.caption, pr.x + (pr.w - R.measure(R.caption, l2)) * 0.5f, cy + S(10), l2, parchDim);
            Rect b{pr.x + pr.w * 0.5f - S(110), cy + S(48), S(220), S(44)};
            if (button("emptyOpen", b, "открыть папку", 0, interactive)) shellOpen(mm().modsDir.wstring());
            return;
        }

        const float rowH = S(80);
        float content = rowH * mm().mods.size();
        float maxScroll = std::max(0.f, content - area.h);
        if (hover(area)) scrollTarget -= wheel * S(60);
        scrollTarget = std::clamp(scrollTarget, 0.f, maxScroll);
        scroll += (scrollTarget - scroll) * (1.f - std::exp(-dt * 14.f));

        R.setClip(&area);
        int moveIdx = -1, moveDelta = 0;
        for (int i = 0; i < (int)mm().mods.size(); ++i) {
            const ModInfo& m = mm().mods[i];
            float rowY = ease("rowpos" + m.folder, i * rowH, 16);  // glides to its new place when reordered
            Rect row{area.x, area.y + rowY - scroll, area.w, rowH};
            if (row.b() < area.y || row.y > area.b()) continue;
            std::string id = "row" + m.folder;
            bool on = mm().enabled.count(m.folder) > 0;
            bool hv = interactive && hover(row) && area.contains(mx, my);
            float h = ease(id, hv ? 1.f : 0.f, 14);
            float sel = ease(id + "sel", i == selected ? 1.f : 0.f, 10);

            if (sel > 0.01f) {
                R.gradH(row, ember.withA(0.11f * sel), ember.withA(0));
                R.rect({row.x, row.y + S(10), S(2), row.h - S(20)}, ember.withA(0.9f * sel));
                R.glow(row.x, row.y + row.h * 0.5f, S(26), ember.withA(0.25f * sel));
            }
            if (h > 0.01f) R.rect(row, rgb(255, 238, 215, 0.025f * h));
            if (i + 1 < (int)mm().mods.size()) {
                float ly = row.b() - S(0.5f), half = row.w * 0.5f;
                R.gradH({row.x, ly, half, S(1)}, gold.withA(0), gold.withA(0.14f));
                R.gradH({row.x + half, ly, half, S(1)}, gold.withA(0.14f), gold.withA(0));
            }

            float cy = row.y + row.h * 0.5f;
            bool toggled = bonfire("fire" + m.folder, row.x + S(32), cy, on, interactive && area.contains(mx, my));

            // order arrows (on hover)
            float arrowsW = 0;
            if (h > 0.02f && mm().mods.size() > 1) {
                float k = h * h * (3.f - 2.f * h);
                arrowsW = S(34) * k;
                float ax = row.r() - S(22) + S(10) * (1.f - k);
                Rect up{ax - S(12), cy - S(22), S(24), S(20)}, dn{ax - S(12), cy + S(2), S(24), S(20)};
                bool canUp = i > 0, canDn = i + 1 < (int)mm().mods.size();
                float hu = ease(id + "u", hover(up) && canUp ? 1.f : 0.f), hd = ease(id + "d", hover(dn) && canDn ? 1.f : 0.f);
                R.triangle(ax, cy - S(11), S(6), lerp(goldDim, emberHot, hu).withA(h * (canUp ? 1.f : 0.25f)), true);
                R.triangle(ax, cy + S(11), S(6), lerp(goldDim, emberHot, hd).withA(h * (canDn ? 1.f : 0.25f)), false);
                if (canUp && click(up)) { moveIdx = i; moveDelta = -1; }
                else if (canDn && click(dn)) { moveIdx = i; moveDelta = 1; }
            }

            // status on the right
            int dep = mm().deployedCount(m.folder);
            bool upd = on && dep > 0 && mm().hasUpdate(m.folder);
            std::string status = upd ? "есть обновление" : on ? (dep == 0 ? "ждёт применения" : "зажжён") : (dep > 0 ? "ещё в игре" : "угас");
            Col sc = upd ? emberHot : on ? (dep == 0 ? gold : ember) : (dep > 0 ? gold : ash);
            float sw = R.measure(R.italic, status);
            float sx = row.r() - S(20) - arrowsW - sw;
            R.text(R.italic, sx, cy - S(14), status, sc);

            // only between mods that are both lit: that's what will actually clash in the game
            const char* mark = nullptr;
            if (on)
                for (auto& c : mm().compatibility(m.folder)) {
                    if (!mm().enabled.count(c.other)) continue;
                    if (c.declared) { mark = "несовместим"; break; }
                    mark = "конфликт";
                }
            if (mark) {
                float cw = R.measure(R.caption, mark);
                R.diamond(sx - cw - S(16), cy - S(1), S(3), blood);
                R.text(R.caption, sx - cw - S(8), cy - S(10), mark, blood);
            }

            float nx = row.x + S(62), maxW = sx - nx - S(100);
            R.text(R.body, nx, cy - S(25), R.ellipsize(R.body, m.name, maxW), lerp(parchDim, parch, std::max(on ? 1.f : 0.f, h * 0.6f)));
            std::string meta;
            if (!m.version.empty()) meta += "v" + m.version;
            if (!m.author.empty()) meta += (meta.empty() ? "" : "  ·  ") + m.author;
            if (meta.empty()) meta = "без описания версии";
            R.text(R.caption, nx, cy + S(5), R.ellipsize(R.caption, meta, maxW), parchDim.withA(0.75f));

            if (toggled) {
                if (!on)
                    for (auto& c : mm().compatibility(m.folder))
                        if (c.declared && mm().enabled.count(c.other))
                            addLog(LogLevel::Warn, "«" + m.name + "» не работает вместе с «" + mm().displayName(c.other) + "».");
                mm().setEnabled(m.folder, !on);
                selected = i;
            } else if (hv && pressed && moveIdx < 0) {
                selected = i;
            }
        }
        R.setClip(nullptr);

        if (content + S(90) < area.h) {
            const char* hint = "Новые моды кладутся в папку mods, затем «обновить список».";
            float hw = R.measure(R.italic, hint);
            float hy = area.b() - S(34);
            divider(area.x + area.w * 0.5f, hy - S(16), S(90), 0.45f);
            R.text(R.italic, area.x + (area.w - hw) * 0.5f, hy, hint, parchDim.withA(0.45f));
        }

        if (moveIdx >= 0) {
            mm().move(moveIdx, moveDelta);
            if (selected == moveIdx) selected += moveDelta;
            else if (selected == moveIdx + moveDelta) selected -= moveDelta;
        }

        // scrollbar
        if (maxScroll > 0) {
            float th = area.h * area.h / content, ty = area.y + (area.h - th) * (scroll / maxScroll);
            R.rect({pr.r() - S(9), ty, S(2), th}, gold.withA(0.35f), S(1));
        }
    }

    void details(const Rect& pr)
    {
        panel(pr, "описание");
        if (mm().mods.empty()) {
            const GameDef& g = *mm().game;
            float x = pr.x + S(30), w = pr.w - S(60), y = pr.y + S(56);
            R.text(R.big, x, y, g.name, parch);
            y += R.big.ascent + R.big.descent + S(10);
            divider(x + w * 0.5f, y, w * 0.5f, 1.f);
            y += S(20);
            R.text(R.heading, x, y, g.realm, gold.withA(0.8f), S(2));
            y += S(30);
            R.text(R.italic, x, y, g.tagline, parchDim);
            y += S(44);
            std::string where = "Моды для этой игры: mods\\" + g.name;
            for (auto& l : R.wrap(R.body, where, w)) { R.text(R.body, x, y, l, parch.withA(0.8f)); y += S(26); }
            y += S(10);
            if (!g.warning.empty())
                for (auto& l : R.wrap(R.caption, g.warning, w - S(16))) {
                    R.diamond(x + S(3), y + S(10), S(2.5f), blood);
                    R.text(R.caption, x + S(14), y, l, blood);
                    y += S(22);
                }
            return;
        }
        selected = std::clamp(selected, 0, (int)mm().mods.size() - 1);
        const ModInfo& m = mm().mods[selected];
        float x = pr.x + S(30), w = pr.w - S(60), y = pr.y + S(56);

        float fade = ease("detailFade" + m.folder, 1.f, 8);
        anim["detailFade" + m.folder] = fade;
        R.setAlpha(std::min(1.f, fade + 0.15f));

        R.text(R.big, x, y, R.ellipsize(R.big, m.name, w), parch);
        y += R.big.ascent + R.big.descent + S(10);
        divider(x + w * 0.5f, y, w * 0.5f, 1.f);
        y += S(18);

        auto rowKV = [&](const char* k, const std::string& v, Col vc) {
            R.text(R.caption, x, y + S(2), k, parchDim.withA(0.8f), S(1));
            R.text(R.body, x + S(130), y - S(2), R.ellipsize(R.body, v, w - S(130)), vc);
            y += S(32);
        };
        bool on = mm().enabled.count(m.folder) > 0;
        int dep = mm().deployedCount(m.folder);
        if (!m.version.empty()) rowKV("Версия", m.version, parch);
        if (!m.author.empty()) rowKV("Автор", m.author, parch);
        std::string inst = mm().installedVersion(m.folder);
        if (dep && mm().hasUpdate(m.folder))
            rowKV("Состояние", "в игре v" + (inst.empty() ? std::string("?") : inst) + ", нажми «обновить»", emberHot);
        else
            rowKV("Состояние", dep ? "установлен" : (on ? "отмечен, нажми «применить»" : "не установлен"), dep ? ember : on ? gold : ash);
        y += S(8);

        // compatibility with the other mods: red when both are lit, dimmer when it only would matter if lit
        auto note = [&](const std::string& text, Col c) {
            bool first = true;
            for (auto& l : R.wrap(R.caption, text, w - S(16))) {
                if (first) R.diamond(x + S(3), y + S(10), S(2.5f), c);
                first = false;
                R.text(R.caption, x + S(14), y, l, c);
                y += S(22);
            }
            y += S(6);
        };
        auto issues = mm().compatibility(m.folder);
        if (issues.empty()) note("Совместим со всеми модами в списке.", gold.withA(0.75f));
        for (auto& c : issues) {
            std::string other = "«" + mm().displayName(c.other) + "»";
            Col col = on && mm().enabled.count(c.other) ? blood : emberHot.withA(0.8f);
            if (c.declared) note("Не работает вместе с " + other + ". Включай что-то одно.", col);
            else note("Меняет те же файлы, что и " + other + ". Если включить оба, сработает тот, что ниже в списке.", col);
        }
        y += S(4);

        // the mod's own hotkeys in the game, as keycaps
        if (!m.keys.empty()) {
            R.text(R.caption, x, y + S(2), "Клавиши", parchDim.withA(0.8f), S(1));
            float capsW = 0;  // actions line up after the widest key
            for (auto& [key, action] : m.keys) capsW = std::max(capsW, R.measure(R.caption, key) + S(18));
            for (auto& [key, action] : m.keys) {
                float kw = R.measure(R.caption, key) + S(18);
                Rect cap{x + S(130), y - S(2), kw, S(26)};
                R.rect(cap, rgb(12, 10, 10, 0.9f), S(3));
                R.frame(cap, gold.withA(0.55f), S(1), S(3));
                R.text(R.caption, cap.x + S(9), cap.y + S(3), key, gold);
                float ax = x + S(130) + capsW + S(12);
                R.text(R.body, ax, y - S(3), R.ellipsize(R.body, action, x + w - ax), parch.withA(0.85f));
                y += S(34);
            }
            y += S(6);
        }

        Rect openR{x, pr.b() - S(66), S(220), S(42)};
        std::string desc = m.description.empty() ? "Описания нет. Его можно добавить в mod.json." : m.description;
        for (auto& l : R.wrap(R.body, desc, w)) {
            if (y + S(29) > openR.y - S(10)) break;
            R.text(R.body, x, y, l, parch.withA(0.82f));
            y += S(29);
        }
        R.setAlpha(1.f);

        if (button("openMod", openR, "папка мода", 0, true)) shellOpen(m.root.wstring());
    }

    void bottom(float W, float H)
    {
        float lw = (W - S(84)) * 0.57f;
        float bx = S(28) + lw + S(28), bw = W - S(28) - bx;
        float by = H - S(28) - S(50);
        bool ready = !busy && !mm().gamePath.empty();
        float gap = S(14);
        float w1 = (bw - gap * 2) * 0.36f, w2 = (bw - gap * 2) * 0.40f, w3 = bw - gap * 2 - w1 - w2;
        // The main button follows the state: pending changes are applied; otherwise it acts on the
        // selected mod: updates it, removes it from the game, or installs it.
        enum { Apply, Update, Remove, Install } action = Apply;
        const ModInfo* sel = nullptr;
        if (!mm().mods.empty()) {
            selected = std::clamp(selected, 0, (int)mm().mods.size() - 1);
            sel = &mm().mods[selected];
        }
        if (!mm().dirty && sel)
            action = mm().hasUpdate(sel->folder) ? Update : mm().deployedCount(sel->folder) ? Remove : Install;
        const char* label = action == Update ? "обновить" : action == Remove ? "удалить" : action == Install ? "установить" : "применить";
        if (button("apply", {bx, by, w1, S(50)}, label, 1, ready && sel)) {
            if (action == Remove) mm().setEnabled(sel->folder, false);
            else if (action == Install) mm().setEnabled(sel->folder, true);
            runTask(false);
        }
        if (button("restore", {bx + w1 + gap, by, w2, S(50)}, "вернуть оригинал", 0, ready && mm().deployedTotal() > 0))
            runTask(true);
        if (button("play", {bx + w1 + w2 + gap * 2, by, w3, S(50)}, "играть", 0, ready && !mm().dirty))
            shellOpen(L"steam://rungameid/" + fromUtf8(mm().game->steamAppId));

        // left of the buttons: the latest message for a few seconds, otherwise the state
        float sx = S(44), ly = by + S(50) * 0.5f - (R.italic.ascent + R.italic.descent) * 0.5f;
        float linksX = S(28) + lw;
        LogLine last;
        {
            std::lock_guard lock(logMx);
            if (!log.empty()) last = log.back();
        }
        float age = t - last.at;
        bool hasMsg = !last.text.empty() && age < 6.f;
        float msgA = hasMsg ? std::min(1.f, age * 4.f) * std::clamp((6.f - age) / 0.5f, 0.f, 1.f) : 0.f;
        if (msgA > 0.f) {
            Col c = last.level == LogLevel::Good ? gold : last.level == LogLevel::Warn ? emberHot
                  : last.level == LogLevel::Error ? blood : parchDim;
            float maxW = linksX - sx - S(310);
            R.diamond(sx - S(12), ly + S(13), S(3), c.withA(msgA));
            R.text(R.italic, sx, ly, R.ellipsize(R.italic, last.text, maxW), c.withA(msgA));
        }
        float stA = hasMsg ? 0.f : last.text.empty() ? 1.f : std::clamp((age - 6.f) / 0.5f, 0.f, 1.f);
        if (stA > 0.01f) {
            if (busy) {
                R.text(R.italic, sx, ly, "работаю…", emberHot.withA(stA * (0.7f + 0.3f * std::sin(t * 5.f))));
            } else if (!mm().dirty && std::any_of(mm().mods.begin(), mm().mods.end(),
                                                    [&](const ModInfo& m) { return mm().hasUpdate(m.folder); })) {
                R.text(R.italic, sx, ly, "есть новая версия мода", ember.withA(stA * (0.65f + 0.35f * std::sin(t * 2.4f))));
            } else if (mm().dirty) {
                R.text(R.italic, sx, ly, "есть неприменённые изменения", ember.withA(stA * (0.65f + 0.35f * std::sin(t * 2.4f))));
            } else {
                R.text(R.italic, sx, ly, mm().deployedTotal() ? "всё применено" : "игра в оригинале",
                       parchDim.withA(0.7f * stA));
            }
        }
        float lx = linksX;
        lx -= R.measure(R.italic, "обновить список");
        if (linkButton("refresh", lx, ly, "обновить список", !busy)) runSync();
        lx -= S(26) + R.measure(R.italic, "папка модов");
        if (linkButton("modsDir", lx, ly, "папка модов")) shellOpen(mm().modsDir.wstring());

        if (busy || progress > 0.f) {
            float p = busy ? progress.load() : ease("progressOut", 0.f, 3);
            R.gradH({0, H - S(3), W * p, S(3)}, ember.withA(0.2f), emberHot);
            R.glow(W * p, H - S(2), S(22), emberHot.withA(0.6f));
            if (!busy && p < 0.01f) progress = 0;
        }
    }

    // A sigil per game: a ring, a blade and the game's own light.
    void emblem(float cx, float cy, Col c, float h, float seed)
    {
        float flick = 0.85f + 0.10f * std::sin(t * 7.3f + seed) + 0.05f * std::sin(t * 13.1f + seed * 1.7f);
        R.glow(cx, cy, S(120) + S(20) * h, c.withA((0.16f + 0.10f * h) * flick));
        R.glow(cx, cy + S(18), S(42), c.withA(0.30f * flick));
        float rr = S(46);
        R.frame({cx - rr, cy - rr, rr * 2, rr * 2}, lerp(goldDim, gold, h).withA(0.55f), S(1), rr);
        float r2 = S(38);
        R.frame({cx - r2, cy - r2, r2 * 2, r2 * 2}, gold.withA(0.12f), S(1), r2);
        R.gradV({cx - S(0.75f), cy - S(66), S(1.5f), S(64)}, gold.withA(0), gold.withA(0.85f));
        R.gradV({cx - S(0.75f), cy - S(2), S(1.5f), S(48)}, gold.withA(0.85f), gold.withA(0.1f));
        R.rect({cx - S(12), cy - S(4), S(24), S(1.5f)}, gold.withA(0.8f));
        R.diamond(cx, cy + S(18), S(7), c.withA(0.9f));
        R.glow(cx, cy + S(18), S(10), lerp(c, emberHot, 0.5f).withA(0.9f * flick));
        for (int k = 0; k < 4; ++k) {
            float a = k * 1.5707963f + t * 0.05f;
            R.diamond(cx + std::cos(a) * rr, cy + std::sin(a) * rr, S(2.5f), gold.withA(0.7f));
        }
    }

    // The game's logo set into a round medallion: soft edge into the dark, gold ring, orbiting diamonds.
    void medallion(const Image& logo, float cx, float cy, float radius, Col accent, float h, bool compact)
    {
        R.glow(cx, cy, radius * 2.f + S(20) * h, accent.withA(0.12f + 0.08f * h));
        if (!compact) {
            R.gradV({cx - S(0.75f), cy - radius - S(30), S(1.5f), S(26)}, gold.withA(0), gold.withA(0.8f));
            R.diamond(cx, cy + radius + S(10), S(3), accent.withA(0.9f));
        }
        float d = radius * 2.f;
        R.image(logo, {cx - radius, cy - radius, d, d}, 1.f, radius, radius * 0.22f, 0.06f);
        float rr = radius + (compact ? S(3) : S(6));
        R.frame({cx - rr, cy - rr, rr * 2, rr * 2}, lerp(goldDim, gold, h).withA(compact ? 0.5f : 0.6f), S(1), rr);
        if (compact) return;
        float r2 = rr + S(6);
        R.frame({cx - r2, cy - r2, r2 * 2, r2 * 2}, gold.withA(0.10f), S(1), r2);
        for (int k = 0; k < 4; ++k) {
            float a = k * 1.5707963f + 0.785398f + t * 0.05f;
            R.diamond(cx + std::cos(a) * rr, cy + std::sin(a) * rr, S(2.5f), gold.withA(0.75f));
        }
    }

    void selection(float W, float H, bool interactive, int hideSigil = -1)
    {
        const char* title = "Выбор мира";
        float tw = R.measure(R.title, title, S(5));
        R.glow(W * 0.5f, S(70), tw, ember.withA(0.05f));
        float hw = R.measure(R.heading, "менеджер модов", S(5));
        R.text(R.heading, W * 0.5f - hw * 0.5f, S(26), "менеджер модов", gold, S(5));
        R.text(R.title, W * 0.5f - tw * 0.5f, S(48), title, parch, S(5));
        divider(W * 0.5f, S(112), S(220), 0.9f);

        const int n = (int)std::size(kQuotes);
        float period = 9.f, ph = std::fmod(t, period);
        float qa = std::min(1.f, std::min(ph / 1.2f, (period - ph) / 1.2f));
        const char* q = kQuotes[int(t / period) % n];
        R.text(R.italic, W * 0.5f - R.measure(R.italic, q) * 0.5f, S(126), q, parchDim.withA(0.8f * qa));

        const auto& list = games();
        float cw = S(352), ch = S(520), gap = S(34);
        float total = cw * list.size() + gap * (list.size() - 1);
        float x0 = (W - total) * 0.5f, y0 = S(186);
        Rect area{x0, y0 - S(10), total, ch + S(20)};

        for (int i = 0; i < (int)list.size(); ++i) {
            const GameDef& g = list[i];
            ModManager& m = *managers[i];
            std::string id = "game" + g.id;
            Rect base{x0 + i * (cw + gap), y0, cw, ch};
            float h = ease(id, interactive && hover(base) ? 1.f : 0.f, 10);
            float appear = std::clamp((t - selStart - 0.4f - i * 0.18f) / 0.9f, 0.f, 1.f);
            appear = appear * appear * (3 - 2 * appear);
            Rect r{base.x, base.y - S(8) * h + S(24) * (1 - appear), base.w, base.h};
            R.setAlpha(appear);

            panel(r, nullptr);
            if (h > 0.01f) R.frame(r, lerp(gold, g.theme.ember, 0.4f).withA(0.55f * h), S(1), S(3));
            float cx = r.x + r.w * 0.5f;
            if (i == hideSigil) {
                // drawn by restBurn / restAsh
            } else if (const Image& logo = logos[i]) {
                medallion(logo, cx, r.y + S(118), S(66) + S(3) * h, g.theme.ember, h, false);
            } else {
                emblem(cx, r.y + S(118), g.theme.ember, h, float(i) * 3.1f);
            }

            float y = r.y + S(214);
            float nw = R.measure(R.big, g.name);
            R.text(R.big, cx - nw * 0.5f, y, g.name, lerp(parch, rgb(255, 240, 215), h));
            y += S(46);
            float rw = R.measure(R.heading, g.realm, S(2));
            R.text(R.heading, cx - rw * 0.5f, y, g.realm, gold.withA(0.8f), S(2));
            y += S(30);
            float lw2 = R.measure(R.italic, g.tagline);
            R.text(R.italic, cx - lw2 * 0.5f, y, g.tagline, parchDim);
            y += S(42);
            divider(cx, y, r.w * 0.36f, 0.7f);
            y += S(22);

            bool found = !m.gamePath.empty();
            std::string st = found ? "игра найдена" : "игра не найдена";
            float sw = R.measure(R.caption, st);
            R.diamond(cx - sw * 0.5f - S(12), y + S(10), S(3.5f), found ? g.theme.ember : ash);
            R.text(R.caption, cx - sw * 0.5f, y, st, found ? parch.withA(0.85f) : ash);
            y += S(26);
            std::string mods = "модов: " + std::to_string(m.mods.size()) + "   ·   включено: " + std::to_string(m.enabled.size());
            float mw = R.measure(R.caption, mods);
            R.text(R.caption, cx - mw * 0.5f, y, mods, parchDim.withA(0.75f));
            y += S(34);

            if (!g.warning.empty())
                for (auto& l : R.wrap(R.caption, g.warning, r.w - S(56))) {
                    float ww = R.measure(R.caption, l);
                    R.text(R.caption, cx - ww * 0.5f, y, l, blood.withA(0.85f));
                    y += S(21);
                }

            const char* enter = "войти";
            float ew = R.measure(R.button, enter, S(4));
            float ey = r.b() - S(46);
            R.text(R.button, cx - ew * 0.5f, ey, enter, lerp(goldDim, emberHot, h).withA(0.5f + 0.5f * h), S(4));
            R.diamond(cx - ew * 0.5f - S(14), ey + S(11), S(2.5f) + S(1.5f) * h, gold.withA(0.4f + 0.6f * h));
            R.diamond(cx + ew * 0.5f + S(14), ey + S(11), S(2.5f) + S(1.5f) * h, gold.withA(0.4f + 0.6f * h));
            R.setAlpha(1.f);

            if (interactive && click(base) && appear > 0.5f) startRest(i, true, cx, r.y + S(118));
        }

        const char* foot = "Моды каждой игры лежат в своей папке:  mods\\<игра>";
        R.text(R.caption, W * 0.5f - R.measure(R.caption, foot) * 0.5f, H - S(46), foot, parchDim.withA(0.5f));
        const char* ver = "v" DSM_VERSION;
        R.text(R.caption, W - S(44) - R.measure(R.caption, ver, S(1)), H - S(46), ver, parchDim.withA(0.35f), S(1));
        chrome(W, area);
    }

    void gameScreen(float W, float H)
    {
        header(W);
        float top = S(112), bot = H - S(106);
        float lw = (W - S(84)) * 0.57f;
        modList({S(28), top, lw, bot - top});
        details({S(28) + lw + S(28), top, W - S(84) - lw, bot - top});
        bottom(W, H);
    }

    static float hash1(float x)  // 0..1, stable per x
    {
        float v = std::sin(x * 12.9898f) * 43758.5453f;
        return v - std::floor(v);
    }

    static float smooth01(float x)
    {
        x = std::clamp(x, 0.f, 1.f);
        return x * x * (3.f - 2.f * x);
    }

    // How dark the world is during the rest: 0 normal, 1 nothing but the ember.
    float restDarkness(float rp) const
    {
        if (restEnter) return rp < kBurnAt ? smooth01(rp / 0.25f) : 0.f;  // then the sheet is the dark
        return smooth01((rp - 0.1f) / 0.4f) * (1.f - smooth01((rp - 0.55f) / 0.4f));
    }

    // Where a game's sigil sits on the selection screen (same layout as selection()).
    void cardSigil(int i, float W, float& x, float& y) const
    {
        float cw = S(352), gap = S(34), n = (float)games().size();
        float x0 = (W - (cw * n + gap * (n - 1))) * 0.5f;
        x = x0 + i * (cw + gap) + cw * 0.5f;
        y = S(186) + S(118);
    }

    // Where the light comes to rest: the medallion by the game title, or back in the game's card.
    void restHome(float W, float& x, float& y) const
    {
        if (!restEnter) return cardSigil(restGame, W, x, y);
        x = S(44) + (logos[restGame] ? S(26) : S(8));
        y = S(24) + S(28);
    }

    // Entering a game: the world goes dark around the sigil, it sinks to an ember, and the ember sets
    // the dark alight like a sheet of paper: it burns away from the middle and the game is behind it.
    static constexpr float kBurnAt = 0.27f;
    // starts where the first holes open right away (the front sits at 0.24 behind the noise), fast first, easing out
    float burnAmount(float rp) const
    {
        float x = std::clamp((rp - kBurnAt) / (1.f - kBurnAt), 0.f, 1.f);
        return 0.24f + 0.76f * x * (1.6f - 0.6f * x);
    }

    void restBurn(float W, float H, float rp)
    {
        const GameDef& g = games()[restGame];
        float move = outCubic(rp / 0.3f);
        float cx = emberX + (W * 0.5f - emberX) * move, cy = emberY + (H * 0.45f - emberY) * move;

        float sigilA = 1.f - smooth01((rp - 0.12f) / 0.2f);
        if (sigilA > 0.01f) {
            R.setAlpha(sigilA);
            if (const Image& logo = logos[restGame]) medallion(logo, cx, cy, S(66), g.theme.ember, 1.f, false);
            else emblem(cx, cy, g.theme.ember, 1.f, float(restGame) * 3.1f);
            R.setAlpha(1.f);
        }

        if (rp >= kBurnAt && rp < 1.f) R.burnSheet(W * 0.5f, H * 0.45f, burnAmount(rp), 0.85f, g.burn);

        // the ember sinks, flares up, and the sheet catches in the same instant; it goes out as the hole opens
        float flick = 0.8f + 0.12f * std::sin(t * 11.3f) + 0.08f * std::sin(t * 17.9f);
        float sink = smooth01((rp - 0.1f) / 0.13f);
        float gather = smooth01((rp - (kBurnAt - 0.04f)) / 0.04f) * (1.f - smooth01((rp - kBurnAt) / 0.1f));
        float radius = S(60) + (S(12) - S(60)) * sink + S(40) * gather;
        float a = ((0.30f + 0.3f * sink) * flick + 0.6f * gather) * (1.f - smooth01((rp - kBurnAt) / 0.1f));
        if (a > 0.01f) {
            R.glow(cx, cy, radius * 2.4f, g.burn.rim.withA(0.45f * a));
            R.glow(cx, cy, radius * 0.7f + S(6), g.burn.rimHot.withA(std::min(1.f, a * 1.5f)));
            float core = S(2.5f) + S(2) * flick + S(2) * gather;
            R.rect({cx - core, cy - core, core * 2, core * 2}, rgb(255, 240, 210, std::min(1.f, a * 2.f)), core);
        }

        // the last of the light settles in the title medallion
        float homeX, homeY;
        restHome(W, homeX, homeY);
        float pulse = smooth01((rp - 0.82f) / 0.08f) * (1.f - smooth01((rp - 0.9f) / 0.1f));
        if (pulse > 0.f) {
            R.glow(homeX, homeY, S(40) + S(60) * pulse, ember.withA(0.35f * pulse));
            R.glow(homeX, homeY, S(16), emberHot.withA(0.6f * pulse));
        }
    }

    // Leaving a game: its screen crumbles to ash from the edges in, what is left of it gathers
    // into one ember in the middle, and the ember drifts back into the game's card.
    static constexpr float kAshEnd = 0.55f;

    void restAsh(float W, float H, float rp)
    {
        const float D = restDuration();
        float cx = W * 0.5f, cy = H * 0.45f;
        float maxD = std::hypot(std::max(cx, W - cx), std::max(cy, H - cy));

        // flakes lift off where the edge passes (same front as the shader's, without its noise)
        for (int k = 0; k < 130; ++k) {
            float seed = k * 5.31f;
            float x = hash1(seed) * W, y = hash1(seed + 1.f) * H;
            float d = std::hypot(x - cx, y - cy) / maxD;
            float at = kAshEnd * (((1.f - d) * 0.8f + 0.35f) / 1.5f + (hash1(seed + 2.f) - 0.5f) * 0.12f);
            float age = (rp - at) * D, life = 0.9f + 0.6f * hash1(seed + 3.f);
            if (age <= 0.f || age > life) continue;
            float f = std::min(1.f, age * 8.f) * (1.f - age / life);
            float fx = x + std::sin(age * 2.3f + k) * S(10) * age + (hash1(seed + 4.f) - 0.5f) * S(40) * age;
            float fy = y - (S(30) + S(60) * hash1(seed + 5.f)) * age;
            float sz = S(1) + S(1.5f) * hash1(seed + 6.f);
            R.rect({fx - sz, fy - sz, sz * 2, sz * 2}, rgb(168, 158, 146, 0.75f * f), S(0.5f));
        }

        // the ember: forms as the last of the screen goes, then drifts home
        float homeX, homeY;
        restHome(W, homeX, homeY);
        float form = smooth01((rp - 0.3f) / 0.18f);
        float travel = outCubic((rp - 0.55f) / 0.25f);
        float landed = smooth01((rp - 0.8f) / 0.04f);
        float fx = cx + (homeX - cx) * travel, fy = cy + (homeY - cy) * travel;
        float flick = 0.8f + 0.12f * std::sin(t * 11.3f) + 0.08f * std::sin(t * 17.9f);
        float a = form * flick * (1.f - landed);
        float radius = S(16) * (1.f - 0.3f * travel);
        if (travel > 0.f && travel < 1.f)
            for (int k = 1; k <= 7; ++k) {
                float back = std::max(0.f, travel - k * 0.035f);
                R.glow(cx + (homeX - cx) * back, cy + (homeY - cy) * back, radius * (1.1f - k * 0.1f),
                       ember.withA(0.18f * a * (1.f - k / 8.f)));
            }
        if (a > 0.01f) {
            R.glow(fx, fy, radius * 2.4f, ember.withA(0.4f * a));
            R.glow(fx, fy, radius * 0.7f + S(6), emberHot.withA(std::min(1.f, a * 1.3f)));
            float core = S(2.5f) + S(1.5f) * flick;
            R.rect({fx - core, fy - core, core * 2, core * 2}, rgb(255, 240, 210, std::min(1.f, a * 2.f)), core);
        }

        // it settles: one soft pulse in the card
        float pulse = smooth01((rp - 0.78f) / 0.06f) * (1.f - smooth01((rp - 0.84f) / 0.1f));
        if (pulse > 0.f) {
            R.glow(homeX, homeY, S(40) + S(60) * pulse, ember.withA(0.35f * pulse));
            R.glow(homeX, homeY, S(16), emberHot.withA(0.6f * pulse));
        }
    }

    void open(int i)
    {
        cur = i;
        selected = 0;
        scroll = scrollTarget = 0;
        mm().scan();
        addLog(LogLevel::Info, mm().game->name + ": модов " + std::to_string(mm().mods.size()));
        if (mm().gamePath.empty()) addLog(LogLevel::Warn, "Игра не найдена, укажи папку в карточке сверху.");
        if (!mm().game->warning.empty()) addLog(LogLevel::Warn, mm().game->warning);
    }

    static float outCubic(float x)
    {
        x = std::clamp(x, 0.f, 1.f);
        return 1.f - (1.f - x) * (1.f - x) * (1.f - x);
    }

    void frame(float W, float H)
    {
        if (int g = rescan.exchange(-1); g >= 0) managers[g]->scan();
        // open: the bonfire is lit, the panels rise out of the dark
        float op = std::clamp((t - openStart) / openDur, 0.f, 1.f), oe = outCubic(op);
        // close: everything sinks, the light pulls into one last ember
        float cp = closing ? std::clamp((t - closeStart) / 0.85f, 0.f, 1.f) : 0.f;
        float ce = cp * cp * (3.f - 2.f * cp);
        float mp = minimizing ? std::clamp((t - minStart) / 0.26f, 0.f, 1.f) : 0.f;

        bool showGameTheme = cur >= 0 && !(resting && !restEnter);
        const Theme& target = showGameTheme ? mm().game->theme : selectionTheme();
        live = mix(live, target, 1.f - std::exp(-dt * 2.5f));
        parch = live.parch; parchDim = live.parchDim; ash = live.ash; gold = live.gold; goldDim = live.goldDim;
        ember = live.ember; emberHot = live.emberHot; blood = live.blood;

        float rp = resting ? std::clamp((t - restStart) / restDuration(), 0.f, 1.f) : 0.f;
        float dark = resting ? restDarkness(rp) : 0.f;
        R.background(oe * (1.f - ce * 0.85f) * (1.f - mp * 0.6f) * (1.f - 0.85f * dark), live.bg);

        if (op < 1.f) {
            float rise = std::min(1.f, op * 5.f) * (1.f - op);
            R.glow(W * 0.5f, H * 0.96f, S(160) + S(900) * oe, ember.withA(0.30f * rise));
            R.glow(W * 0.5f, H * 0.96f, S(70) + S(220) * oe, emberHot.withA(0.35f * rise));
        }

        float content = outCubic((op - 0.15f) / 0.85f) * (1.f - std::clamp(ce * 1.5f, 0.f, 1.f)) * (1.f - mp);
        float baseScale = (0.955f + 0.045f * oe) * (1.f - 0.035f * ce) * (1.f - 0.02f * mp);
        float baseDy = S(22) * (1.f - oe) + S(16) * ce + S(10) * mp;
        R.setTransform(baseScale, baseDy);
        R.setMasterAlpha(content);

        if (resting) {
            // the screen we leave fades into the dark, the one we go to comes up in the new light
            float outA = restEnter ? 1.f - smooth01(rp / 0.25f) : rp < kAshEnd ? 1.f : 0.f;
            float inA = restEnter ? (rp >= kBurnAt ? 1.f : 0.f) : smooth01((rp - 0.55f) / 0.45f);
            float smx = mx, smy = my;
            bool sp = pressed;
            mx = my = -1;
            pressed = false;
            if (outA > 0.01f) {
                R.setMasterAlpha(content * outA);
                if (restEnter) selection(W, H, false, restGame);
                else {
                    R.setDissolve(Renderer::Dissolve::Ash, std::min(1.f, rp / kAshEnd), W * 0.5f, H * 0.45f);
                    gameScreen(W, H);
                    R.setDissolve(Renderer::Dissolve::None);
                }
            }
            if (inA > 0.01f) {
                R.setMasterAlpha(content * inA);
                if (restEnter) {
                    R.setDissolve(Renderer::Dissolve::Reveal, burnAmount(rp), W * 0.5f, H * 0.45f, games()[restGame].burn.grain);
                    gameScreen(W, H);
                    R.setDissolve(Renderer::Dissolve::None);
                } else selection(W, H, false, rp < 0.9f ? restGame : -1);
            }
            R.setMasterAlpha(content);
            if (restEnter) restBurn(W, H, rp);
            else restAsh(W, H, rp);
            mx = smx; my = smy; pressed = sp;
            if (rp >= 1.f) {
                resting = false;
                if (!restEnter) cur = -1;
            }
        } else if (cur < 0) {
            selection(W, H, true);
        } else {
            gameScreen(W, H);
            if (back) {
                back = false;
                startRest(cur, false, W * 0.5f, H * 0.45f);
            }
        }

        R.setMasterAlpha(1.f);
        R.setTransform(1.f, 0.f);

        if (closing) {
            float shrink = 1.f - ce;
            float last = 1.f - std::clamp((cp - 0.7f) / 0.3f, 0.f, 1.f);
            R.rect({0, 0, W, H}, rgb(0, 0, 0, 0.8f * ce));
            R.glow(W * 0.5f, H * 0.5f, S(24) + S(760) * shrink, ember.withA(0.32f * std::sin(3.14159f * cp)));
            R.glow(W * 0.5f, H * 0.5f, S(5) + S(26) * shrink, emberHot.withA(0.95f * last * std::min(1.f, cp * 4.f)));
        }
        R.end();

        if (!shot) {
            float o = std::clamp(op / 0.3f, 0.f, 1.f) * (1.f - std::clamp((cp - 0.6f) / 0.4f, 0.f, 1.f)) * (1.f - mp);
            glfwSetWindowOpacity(win, o);
        }
        if (closing && cp >= 1.f) {
            allowClose = true;
            glfwSetWindowShouldClose(win, GLFW_TRUE);
        }
        if (minimizing && mp >= 1.f && !iconified) {
            iconified = true;
            glfwIconifyWindow(win);
        }
    }
};

static App* gApp = nullptr;

static bool runCli(App& app, int argc, char** argv)
{
    std::string cmd = argc > 2 ? argv[2] : "status";
    printf("[%s]\n", app.mm().game->name.c_str());
    auto L = [](LogLevel, const std::string& m) { printf("%s\n", m.c_str()); };
    if (cmd == "status") {
        printf("game: %s\n", pathUtf8(app.mm().gamePath).c_str());
        for (auto& m : app.mm().mods)
            printf("%s [%s] files=%zu deployed=%d\n", m.folder.c_str(), app.mm().enabled.count(m.folder) ? "on" : "off",
                   m.files.size(), app.mm().deployedCount(m.folder));
        printf("dirty=%d deployed=%zu\n", app.mm().dirty, app.mm().deployedTotal());
        return true;
    }
    if ((cmd == "enable" || cmd == "disable") && argc > 3) {
        app.mm().setEnabled(argv[3], cmd == "enable");
        printf("%s %s\n", cmd.c_str(), argv[3]);
        return true;
    }
    if (cmd == "apply") return app.mm().apply(L, nullptr);
    if (cmd == "restore") return app.mm().restoreAll(L, nullptr);
    if (cmd == "sync") return syncMods(*app.mm().game, app.mm().modsDir, L, nullptr);
    printf("usage: DSM --cli status|apply|restore|sync|enable <mod>|disable <mod>\n");
    return false;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string(argv[1]) == "--version") {
        printf("DSM %s\n", DSM_VERSION);
        return 0;
    }
    // DSM.exe --make-patch <original> <modified> <out.dsmpatch>: share your changes without the game's file
    if (argc > 4 && std::string(argv[1]) == "--make-patch") {
        std::string json, err;
        if (!makePatch(readBytes(pathFromUtf8(argv[2])), readBytes(pathFromUtf8(argv[3])), json, err)) {
            printf("error: %s\n", err.c_str());
            return 1;
        }
        writeTextFile(pathFromUtf8(argv[4]), json);
        printf("patch: %s (%zu bytes)\n", argv[4], json.size());
        return 0;
    }
    bool cli = argc > 1 && std::string(argv[1]) == "--cli";

    // Two managers touching the game folder at once would corrupt the backups.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DSM_DarkSoulsModManager");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (cli) {
            printf("DSM уже запущен, закрой его и повтори.\n");
            return 4;
        }
        if (HWND other = FindWindowW(nullptr, L"DSM — Dark Souls")) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    App app;
    gApp = &app;
    app.root = exeDir();  // portable: data\ and mods\ sit next to DSM.exe, nothing goes elsewhere
    for (auto& g : games()) {
        app.managers.push_back(std::make_unique<ModManager>());
        app.managers.back()->init(g, app.root);
    }
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--game")
            for (int k = 0; k < (int)games().size(); ++k)
                if (games()[k].id == argv[i + 1]) app.cur = k;

    if (cli) {
        if (app.cur < 0) app.cur = 0;
        return runCli(app, argc, argv) ? 0 : 1;
    }

    std::string shotPath;
    float shotTime = 3.f;
    float closeAt = -1;  // debug: start the closing animation at this time
    float pickAt = -1;   // debug: enter a world (--pick-game, the first by default) at this time
    int pickGame = 0;
    float backAt = -1;   // debug: go back to the world selection at this time
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--screenshot") shotPath = argv[i + 1];
        if (std::string(argv[i]) == "--shot-time") shotTime = std::stof(argv[i + 1]);
        if (std::string(argv[i]) == "--select") app.selected = std::stoi(argv[i + 1]);
        if (std::string(argv[i]) == "--close-at") closeAt = std::stof(argv[i + 1]);
        if (std::string(argv[i]) == "--pick-at") pickAt = std::stof(argv[i + 1]);
        if (std::string(argv[i]) == "--pick-game") pickGame = std::stoi(argv[i + 1]);
        if (std::string(argv[i]) == "--back-at") backAt = std::stof(argv[i + 1]);
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    app.win = glfwCreateWindow(1280, 800, "DSM — Dark Souls", nullptr, nullptr);
    if (!app.win) return 1;
    glfwMakeContextCurrent(app.win);
    glfwSwapInterval(1);
    if (!glLoad([](const char* n) { return (void*)glfwGetProcAddress(n); })) return 2;

    HWND hwnd = glfwGetWin32Window(app.win);
    DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    float sx = 1, sy = 1;
    glfwGetWindowContentScale(app.win, &sx, &sy);
    app.s = sx;
    if (!app.R.init(app.s)) {
        MessageBoxW(hwnd, L"Не удалось загрузить встроенные шрифты", L"DSM", MB_ICONERROR);
        return 3;
    }

    for (auto& g : games())
        app.logos.push_back(g.logo.empty() ? Image{} : app.R.loadImage(resourceBytes(fromUtf8(g.logo).c_str())));

    glfwSetScrollCallback(app.win, [](GLFWwindow*, double, double y) { gApp->wheel += (float)y; });
    // Alt+F4 and the taskbar close go through the same fading-out as the close button
    glfwSetWindowCloseCallback(app.win, [](GLFWwindow* w) {
        if (gApp->allowClose) return;
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        gApp->startClose();
    });
    glfwSetWindowIconifyCallback(app.win, [](GLFWwindow*, int iconified) {
        if (!iconified) gApp->restored();
    });
    app.shot = !shotPath.empty();

    // center on the primary monitor
    if (shotPath.empty()) {
        int ww, wh;
        glfwGetWindowSize(app.win, &ww, &wh);
        const GLFWvidmode* vm = glfwGetVideoMode(glfwGetPrimaryMonitor());
        glfwSetWindowPos(app.win, (vm->width - ww) / 2, (vm->height - wh) / 2);
        glfwSetWindowOpacity(app.win, 0.f);
        glfwShowWindow(app.win);
    }

    if (app.cur >= 0) app.open(app.cur);

    double last = glfwGetTime();
    while (!glfwWindowShouldClose(app.win)) {
        glfwPollEvents();
        double now = glfwGetTime();
        app.dt = shotPath.empty() ? (float)std::min(0.1, now - last) : 1.f / 60.f;
        last = now;
        app.t += app.dt;

        double cx, cy;
        glfwGetCursorPos(app.win, &cx, &cy);
        int fbw, fbh, ww, wh;
        glfwGetFramebufferSize(app.win, &fbw, &fbh);
        glfwGetWindowSize(app.win, &ww, &wh);
        float k = ww ? float(fbw) / float(ww) : 1.f;
        app.mx = shotPath.empty() ? float(cx) * k : -1.f;
        app.my = shotPath.empty() ? float(cy) * k : -1.f;
        app.down = glfwGetMouseButton(app.win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        app.pressed = app.down && !app.prevDown;
        app.prevDown = app.down;
        if (!glfwGetWindowAttrib(app.win, GLFW_FOCUSED) && shotPath.empty()) app.mx = app.my = -1;

        if (closeAt >= 0 && app.t >= closeAt) app.startClose();
        if (pickAt >= 0 && app.t >= pickAt && app.cur < 0 && !app.resting) {
            app.startRest(pickGame, true, fbw * (0.2f + 0.3f * pickGame), fbh * 0.4f);
            pickAt = -1;
        }
        if (backAt >= 0 && app.t >= backAt && app.cur >= 0 && !app.resting) {
            app.back = true;
            backAt = -1;
        }
        app.R.begin(fbw, fbh, app.t);
        app.frame((float)fbw, (float)fbh);
        app.wheel = 0;

        if (!shotPath.empty() && app.t >= shotTime) {
            std::vector<unsigned char> px((size_t)fbw * fbh * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // the window is opaque; blending leaves alpha < 1
            stbi_flip_vertically_on_write(1);
            stbi_write_png(shotPath.c_str(), fbw, fbh, 4, px.data(), fbw * 4);
            break;
        }
        glfwSwapBuffers(app.win);
    }

    if (app.worker.joinable()) app.worker.join();
    glfwTerminate();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}

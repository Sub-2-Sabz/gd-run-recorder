// Run Recorder - records every attempt, throws away deaths, keeps successful runs.
// Android only (uses MediaCodec). See Recorder.cpp for the encoder.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "Recorder.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>

using namespace geode::prelude;

#ifdef __ANDROID__

namespace {
    bool s_unsupported = false; // set if the encoder fails once, so we don't retry every attempt

    std::string safeName(std::string s) {
        for (auto& c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        }
        return s;
    }

    int snap16(float v) {
        return std::max(16, static_cast<int>(std::lround(v / 16.f)) * 16);
    }
}

// delete any half-written recording left over from a crash
$execute {
    std::error_code ec;
    std::filesystem::remove(Mod::get()->getSaveDir() / "current.mp4", ec);
}

class $modify(RunRecLayer, PlayLayer) {
    struct Fields {
        bool recording = false;
        bool qualified = false;   // run reached the save %, being finished off
        float tail = 0.f;         // seconds left to keep recording after qualifying
        float startPct = 0.f;     // where this attempt began (start pos / checkpoint)
        float threshold = 100.f;  // % that counts as a successful run
        float sinceCapture = 0.f;
        int fps = 30;
        int srcW = 0;
        int srcH = 0;
        CCRenderTexture* rt = nullptr;
        std::string dest;

        ~Fields() {
            if (recording) rec::finish(qualified, dest);
            if (rt) rt->release();
        }
    };

    bool wantRecord() {
        auto mod = Mod::get();
        if (s_unsupported) return false;
        if (!mod->getSettingValue<bool>("auto-record")) return false;
        if (m_level->isPlatformer()) return false; // % doesn't make sense there
        if (m_isPracticeMode && !mod->getSettingValue<bool>("record-practice")) return false;
        return true;
    }

    void startRun() {
        auto f = m_fields.self();
        if (!this->wantRecord()) return;
        auto mod = Mod::get();

        if (!f->rt) {
            auto ws = CCDirector::get()->getWinSize();
            auto rt = CCRenderTexture::create(static_cast<int>(ws.width), static_cast<int>(ws.height));
            if (!rt) return;
            rt->retain();
            f->rt = rt;
            auto px = rt->getSprite()->getTexture()->getContentSizeInPixels();
            f->srcW = static_cast<int>(px.width);
            f->srcH = static_cast<int>(px.height);
        }

        f->fps = static_cast<int>(mod->getSettingValue<int64_t>("fps"));
        int outH = snap16(static_cast<float>(mod->getSettingValue<int64_t>("video-height")));
        int outW = snap16(outH * static_cast<float>(f->srcW) / static_cast<float>(f->srcH));
        int kbps = static_cast<int>(mod->getSettingValue<int64_t>("bitrate-kbps"));

        auto tmp = (mod->getSaveDir() / "current.mp4").string();
        if (!rec::begin(tmp, f->srcW, f->srcH, outW, outH, f->fps, kbps)) {
            log::warn("Run Recorder: could not start encoder: {}", rec::lastError());
            s_unsupported = true;
            Notification::create("Run Recorder: " + rec::lastError(), NotificationIcon::Error, 4.f)->show();
            return;
        }

        f->recording = true;
        f->qualified = false;
        f->tail = 0.f;
        f->sinceCapture = 1.f; // grab the first frame right away
        f->startPct = this->getCurrentPercent();

        // Start-pos / checkpoint attempts use their own "save at %"
        bool fromMiddle = f->startPct > 0.5f;
        float thr = static_cast<float>(
            mod->getSettingValue<int64_t>(fromMiddle ? "startpos-save-percent" : "save-percent"));
        if (thr <= f->startPct) thr = 100.f; // save % must be ahead of where you start
        f->threshold = thr;
    }

    void qualify() {
        auto f = m_fields.self();
        f->qualified = true;
        f->tail = 1.5f; // keep recording a moment so the ending is in the video

        std::error_code ec;
        auto dir = Mod::get()->getSaveDir() / "runs";
        std::filesystem::create_directories(dir, ec);

        char ts[32];
        std::time_t now = std::time(nullptr);
        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", std::localtime(&now));

        int endPct = std::min(100, static_cast<int>(this->getCurrentPercent()));
        auto name = fmt::format(
            "{}_{}to{}_{}.mp4",
            safeName(std::string(m_level->m_levelName)),
            static_cast<int>(f->startPct), endPct, ts
        );
        f->dest = (dir / name).string();
    }

    // save if the run qualified, otherwise throw it away
    void stopRun() {
        auto f = m_fields.self();
        if (!f->recording) return;
        f->recording = false;

        if (f->qualified) {
            bool ok = rec::finish(true, f->dest);
            auto name = std::filesystem::path(f->dest).filename().string();
            if (ok) {
                Notification::create("Saved " + name, NotificationIcon::Success, 2.5f)->show();
            } else {
                log::warn("Run Recorder: save failed: {}", rec::lastError());
                Notification::create("Run save failed: " + rec::lastError(), NotificationIcon::Error, 3.f)->show();
            }
        } else {
            rec::finish(false, "");
        }
        f->qualified = false;
    }

    void captureFrame(float dt) {
        auto f = m_fields.self();
        float interval = 1.f / static_cast<float>(f->fps);
        f->sinceCapture += dt;
        if (f->sinceCapture + 0.0005f < interval) return;
        f->sinceCapture = std::min(f->sinceCapture - interval, interval);

        if (!f->rt || !rec::wantsFrame()) return; // encoder is behind, skip this frame
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene) return;

        std::vector<uint8_t> buf(static_cast<size_t>(f->srcW) * f->srcH * 4);
        f->rt->beginWithClear(0.f, 0.f, 0.f, 1.f);
        scene->visit();
        glReadPixels(0, 0, f->srcW, f->srcH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        f->rt->end();
        rec::submit(std::move(buf));
    }

    // ---- hooks ----

    void resetLevel() {
        this->stopRun();               // previous attempt: save if it qualified, else delete
        PlayLayer::resetLevel();
        this->startRun();              // new attempt
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();
        if (!f->recording) return;

        this->captureFrame(dt);

        if (!f->qualified && this->getCurrentPercent() >= f->threshold) {
            this->qualify();
        }
        if (f->qualified) {
            f->tail -= dt;
            if (f->tail <= 0.f) this->stopRun();
        }
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        auto f = m_fields.self();
        if (f->recording && !f->qualified) this->qualify();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        PlayLayer::destroyPlayer(player, obj);
        auto f = m_fields.self();
        if (!f->recording || f->qualified) return;
        bool dead = (m_player1 && m_player1->m_isDead) || (m_player2 && m_player2->m_isDead);
        if (dead) this->stopRun(); // failed run -> deleted immediately
    }

    void onQuit() {
        this->stopRun();
        PlayLayer::onQuit();
    }
};

#endif // __ANDROID__

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>

// Menüs, die wir überspringen
#include <Geode/modify/SecretLayer.hpp>
#include <Geode/modify/SecretLayer2.hpp>
#include <Geode/modify/SecretLayer3.hpp>
#include <Geode/modify/SecretLayer4.hpp>
#include <Geode/modify/SecretRewardsLayer.hpp>
#include <Geode/modify/GauntletSelectLayer.hpp>

// Gameplay-Erkennung
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>

// Sonstige Layer
#include <Geode/modify/LevelSelectLayer.hpp>
#include <Geode/modify/GJGroundLayer.hpp>
#include <Geode/modify/MenuGameLayer.hpp>
#include <Geode/modify/CCScale9Sprite.hpp>
#include <Geode/modify/CCTextInputNode.hpp>

// Partikel-Override
#include <Geode/modify/CCParticleSystemQuad.hpp>

#include <Geode/utils/cocos.hpp>
#include <algorithm>
#include <cstring>

USING_NS_CC;
using namespace geode::prelude;

// ===== Auto-Switch Flag =====
static bool gInLevelOrEditor = false;
static bool suppressionOn() {
    bool userWants = false;
    if (auto m = Mod::get()) userWants = m->getSettingValue<bool>("suppress_level_particles");
    return gInLevelOrEditor && userWants;
}

// ===== Persistenter Menü-Emitter =====
static CCParticleSystemQuad* gEmitter = nullptr; // einmalig erstellt & retained

// -------- helpers --------
template <typename T>
T* getChildOfType(CCNode* parent, int index = 0) {
    if (!parent) return nullptr;
    int count = 0;
    if (auto* children = parent->getChildren()) {
        for (unsigned int i = 0; i < children->count(); ++i) {
            CCNode* child = static_cast<CCNode*>(children->objectAtIndex(i));
            if (auto casted = dynamic_cast<T*>(child)) {
                if (count == index) return casted;
                ++count;
            }
        }
    }
    return nullptr;
}

static bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// ---------------- BG: Wallpaper + Tree ----------------
CCMenu* st2BG() {
    auto win = CCDirector::sharedDirector()->getWinSize();

    auto menu = CCMenu::create();
    menu->setContentSize(win);
    menu->setPosition(0, 0);
    menu->setAnchorPoint({0, 0});
    menu->setID("st2-background");

    // background
    auto bg = CCSprite::create("background.png"_spr);
    if (!bg) {
        log::error("background.png not found");
        return menu;
    }
    bg->setID("background");
    bg->setPosition(win / 2);
    float bgScale = std::max(
        win.height / bg->getContentSize().height,
        win.width  / bg->getContentSize().width
    );
    bg->setScale(bgScale);

    auto bgSeq = CCArray::create();
    bgSeq->addObject(CCScaleTo::create(20.f, bgScale * 1.1f));
    bgSeq->addObject(CCScaleTo::create(20.f, bgScale));
    bg->runAction(CCRepeatForever::create(CCSequence::create(bgSeq)));
    menu->addChild(bg, 0);

    // tree (optional)
    if (auto tree = CCSprite::create("tree.png"_spr)) {
        tree->setID("tree");
        tree->setPosition(win / 2);
        float tScale = std::max(
            win.height / tree->getContentSize().height,
            win.width  / tree->getContentSize().width
        );
        tree->setScale(tScale);

        auto tSeq = CCArray::create();
        tSeq->addObject(CCScaleTo::create(20.f, tScale * 1.2f));
        tSeq->addObject(CCScaleTo::create(20.f, tScale));
        tree->runAction(CCRepeatForever::create(CCSequence::create(tSeq)));
        menu->addChild(tree, 1);
    }

    // *** WICHTIG: Partikel NICHT hier erstellen/anhängen ***
    // -> gEmitter wird getrennt erstellt & bei Szenewechsel repar­ented

    return menu;
}

// ------- Utilities -------
static void hideGradientsInLayer(CCLayer* layer) {
    if (!layer) return;
    auto* cacheDict = CCTextureCache::sharedTextureCache()->m_pTextures;
    if (!cacheDict) return;

    if (auto* children = layer->getChildren()) {
        for (unsigned int i = 0; i < children->count(); ++i) {
            if (auto sprite = dynamic_cast<CCSprite*>(children->objectAtIndex(i))) {
                if (auto tp = dynamic_cast<CCTextureProtocol*>(sprite)) {
                    if (auto tex = tp->getTexture()) {
                        for (auto kv : CCDictionaryExt<std::string, CCTexture2D*>(cacheDict)) {
                            const std::string& key = kv.first;
                            CCTexture2D* obj = kv.second;
                            if (obj == tex &&
                                (ends_with(key, "GJ_gradientBG.png") ||
                                 ends_with(key, "GJ_gradientBG-hd.png"))) {
                                sprite->setVisible(false);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

void ensureBG(CCNode* host, int zBack = -999) {
    if (!host) return;
    if (!host->getChildByID("st2-background")) {
        host->addChild(st2BG(), zBack);
    }
}

// Erstellt gEmitter EINMAL bei Bedarf (ohne reset hinterher)
static void ensureEmitterCreated() {
    if (gEmitter || !Mod::get()->getSettingValue<bool>("particles")) return;

    std::string plist = "particleEffect.plist"_spr;
    std::string dir;
    if (!plist.empty()) {
        size_t p = plist.find_last_of("/\\");
        if (p != std::string::npos) dir = plist.substr(0, p);
    }

    auto dict = CCDictionary::createWithContentsOfFileThreadSafe(plist.c_str());
    if (!dict) {
        log::error("Particles: plist not found: {}", plist);
        return;
    }

    auto emitter = CCParticleSystemQuad::create();
    if (!(emitter && emitter->initWithDictionary(dict, dir.c_str()))) {
        log::error("Particles: initWithDictionary failed (dir={})", dir);
        return;
    }

    // Fallback-Textur
    if (!emitter->getTexture()) {
        if (auto spr = CCSprite::create("sun.png"_spr))
            emitter->setTexture(spr->getTexture());
    }

    // Normale, dezente Defaults (nur falls im PLIST „leer“)
    emitter->setDuration(-1);
    emitter->setEmitterMode(kCCParticleModeGravity);
    emitter->setOpacityModifyRGB(true); // PMA

    auto win = CCDirector::sharedDirector()->getWinSize();
    emitter->setPosition(ccp(win.width * 0.5f, 0.f));
    emitter->setPosVar  (ccp(win.width * 0.5f, 0.f));
    emitter->setGravity(ccp(0.f, 160.f));
    emitter->setAngle(90.f);
    emitter->setAngleVar(5.f);
    emitter->setPositionType(kCCPositionTypeGrouped); // Bewegungen mit Parent (ok fürs Menü) :contentReference[oaicite:3]{index=3}

    int   total = emitter->getTotalParticles() > 0 ? emitter->getTotalParticles() : 150;
    float life  = emitter->getLife()            > 0 ? emitter->getLife()            : 2.f;
    emitter->setTotalParticles(total);
    emitter->setLife(life);
    if (emitter->getEmissionRate() <= 0.f)
        emitter->setEmissionRate(static_cast<float>(total) / life);

    if (emitter->getStartSize() <= 0.f) emitter->setStartSize(8.f);
    if (emitter->getEndSize()   <  0.f) emitter->setEndSize(4.f);

    // PMA-freundliches Alpha
    emitter->setBlendFunc({ GL_ONE, GL_ONE_MINUS_SRC_ALPHA });

    emitter->setID("emitter");
    emitter->setVisible(true);
    emitter->setAutoRemoveOnFinish(false);

    // *** Nur beim ALLERERSTEN Mal starten ***
    emitter->resetSystem(); // Achtung: reset killt Partikel – nur hier, nie beim Reparent! :contentReference[oaicite:4]{index=4}

    // persistent machen
    emitter->retain();   // wir managen das selbst (reparenten ohne cleanup) :contentReference[oaicite:5]{index=5}
    gEmitter = emitter;

    log::info("[Sunix Wallpaper]: Particles CREATED: tex={}, total={}, life={}, rate={}",
        static_cast<const void*>(gEmitter->getTexture()),
        gEmitter->getTotalParticles(),
        gEmitter->getLife(),
        gEmitter->getEmissionRate()
    );
}

// Emitter an das aktuelle Menü-BG-Menü hängen (ohne Reset)
static void attachEmitterToMenuBG(CCLayer* layer) {
    if (!layer || !gEmitter) return;
    auto menu = dynamic_cast<CCNode*>(layer->getChildByID("st2-background"));
    if (!menu) return;

    if (gEmitter->getParent() == menu) return;

    // vom alten Parent lösen, Zustand behalten
    if (gEmitter->getParent()) {
        gEmitter->removeFromParentAndCleanup(false); // keep state/schedule :contentReference[oaicite:6]{index=6}
    }
    menu->addChild(gEmitter, 2); // über BG/Tree, unter UI
}

void replaceBG(CCLayer* layer) {
    if (!layer) return;

    hideGradientsInLayer(layer);

    if (auto mgl = getChildOfType<MenuGameLayer>(layer, 0)) {
        mgl->setVisible(false);
    }

    ensureBG(layer, -999);

    if (dynamic_cast<LevelSelectLayer*>(layer)) {
        if (auto ground = getChildOfType<GJGroundLayer>(layer, 0))
            ground->setVisible(false);
    }

    if (auto* children = layer->getChildren()) {
        for (unsigned int i = 0; i < children->count(); ++i) {
            CCNode* child = static_cast<CCNode*>(children->objectAtIndex(i));
            if (auto nine = dynamic_cast<CCScale9Sprite*>(child)) {
                nine->setOpacity(100);
                nine->setColor(ccc3(0, 0, 0));
                child->setScale(child->getScale() * 0.5f);
                child->setContentSize(child->getContentSize() / 0.5f);
            } else if (auto ti = dynamic_cast<CCTextInputNode*>(child)) {
                ti->setLabelPlaceholderColor(ccc3(255, 255, 255));
            }
        }
    }

    // Emitter bei Menü-Szenen anhängen (ohne Neustart)
    attachEmitterToMenuBG(layer);
}

// -------- hooks --------
class $modify(MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (auto mgl = getChildOfType<MenuGameLayer>(this, 0)) mgl->setVisible(false);
        ensureBG(this, -999);

        // Beim ersten Einstieg ins Hauptmenü ggf. Emitter erstellen & anhängen
        ensureEmitterCreated();
        attachEmitterToMenuBG(this);
        return true;
    }
};

class $modify(CCDirector) {
    void willSwitchToScene(CCScene* scene) {
        CCDirector::willSwitchToScene(scene);

        // Gameplay-Erkennung robust: PlayLayer/Editor direkt suchen
        bool isPlay  = getChildOfType<PlayLayer>(scene, 0)       != nullptr;
        bool isEdit  = getChildOfType<LevelEditorLayer>(scene, 0) != nullptr;
        gInLevelOrEditor = (isPlay || isEdit);

        log::info("[Sunix Wallpaper] gameplay particles {}",
                  gInLevelOrEditor ? "SUPPRESSED" : "ENABLED");

        // In Menüs: BG aufbauen und (falls vorhanden) Emitter rüberhängen
        if (auto layer = getChildOfType<CCLayer>(scene, 0)) {
            if (!dynamic_cast<SecretLayer*>(layer)
             && !dynamic_cast<SecretLayer2*>(layer)
             && !dynamic_cast<SecretLayer3*>(layer)
             && !dynamic_cast<SecretLayer4*>(layer)
             && !dynamic_cast<SecretRewardsLayer*>(layer)
             && !dynamic_cast<GauntletSelectLayer*>(layer)) {
                if (!gInLevelOrEditor) {
                    replaceBG(layer);
                } else {
                    // Beim Eintritt ins Level den Emitter ggf. vom alten Menü lösen,
                    // damit er nicht zerstört wird, wenn die Menüszenen freigegeben werden
                    if (gEmitter && gEmitter->getParent()) {
                        gEmitter->removeFromParentAndCleanup(false);
                    }
                }
            }
        }
    }
};

class $modify(LevelInfoLayer) {
    void onPlay(CCObject* sender) {
        LevelInfoLayer::onPlay(sender);

        CCNode* playMenu   = this->getChildByID("play-menu");
        if (!playMenu) return;
        CCNode* playButton = playMenu->getChildByID("play-button");
        if (!playButton) return;
        CCNode* sprite     = playButton->getChildByTag(1);
        if (!sprite) return;

        if (auto* children = sprite->getChildren()) {
            for (unsigned int i = 0; i < children->count(); ++i) {
                CCNode* c = static_cast<CCNode*>(children->objectAtIndex(i));
                if (!dynamic_cast<CCProgressTimer*>(c)) {
                    if (c->getZOrder() == -4) {
                        c->setVisible(false);
                    } else if (auto rgba = dynamic_cast<CCNodeRGBA*>(c)) {
                        rgba->setOpacity(100);
                        rgba->setColor(ccc3(0, 0, 0));
                        rgba->setCascadeOpacityEnabled(false);
                    }
                }
            }
        }
    }
};

// == Globale Partikel-Unterdrückung im Gameplay ==
class $modify(CCParticleSystemQuad) {
    void draw() {
        if (suppressionOn()) {
            // Nichts zeichnen im Level/Editor (unsere Menü-Partikel hängen dort nicht)
            return;
        }
        CCParticleSystemQuad::draw();
    }
};

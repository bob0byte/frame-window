#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/GameObject.hpp>

using namespace geode::prelude;

// settings, saved between sessions; x/y are fractions of the screen (0..1)
struct CounterSettings {
	static bool enabled() { return Mod::get()->getSavedValue<bool>("enabled", true); }
	static bool clickLabels() { return Mod::get()->getSavedValue<bool>("click-labels", true); }
	static bool ding() { return Mod::get()->getSavedValue<bool>("ding", true); }
	static float x() { return Mod::get()->getSavedValue<float>("x", 0.01f); }
	static float y() { return Mod::get()->getSavedValue<float>("y", 0.98f); }
	static float scale() { return Mod::get()->getSavedValue<float>("scale", 0.5f); }
	static float opacity() { return Mod::get()->getSavedValue<float>("opacity", 1.f); }
};

// window-size buckets, top row of the panel first
struct Bucket {
	const char* name;
	int min, max;
	ccColor3B color;
};
static constexpr int BUCKET_COUNT = 7;
static Bucket const BUCKETS[BUCKET_COUNT] = {
	{"9-12:", 9, 12, {110, 110, 255}},
	{"7-8:", 7, 8, {90, 200, 255}},
	{"5-6:", 5, 6, {100, 230, 100}},
	{"4:", 4, 4, {255, 255, 255}},
	{"3:", 3, 3, {255, 220, 0}},
	{"2:", 2, 2, {255, 150, 30}},
	{"1:", 1, 1, {255, 60, 60}},
};

// windows wider than 12 frames are easy, so they're not counted
static int bucketFor(int frames) {
	for (int i = 0; i < BUCKET_COUNT; i++) {
		if (frames >= BUCKETS[i].min && frames <= BUCKETS[i].max) return i;
	}
	return -1;
}

static ccColor3B colorFor(int frames) {
	auto b = bucketFor(frames);
	return b < 0 ? ccColor3B{200, 200, 200} : BUCKETS[b].color;
}

// ding for every counted window; the tighter the window, the lower the pitch
// (1 frame plays at half speed, 12 frames at normal speed)
static void playDing(int frames) {
	if (!CounterSettings::ding() || bucketFor(frames) < 0) return;
	float speed = 0.5f + 0.5f * (frames - 1) / 11.f;
	FMODAudioEngine::get()->playEffect("ding.wav"_spr, speed, 0.f, 1.f);
}

static void applyPanelSettings(CCNode* panel) {
	auto win = CCDirector::get()->getWinSize();
	panel->setVisible(CounterSettings::enabled());
	panel->setPosition({win.width * CounterSettings::x(), win.height * CounterSettings::y()});
	panel->setScale(CounterSettings::scale());
	auto opacity = static_cast<GLubyte>(CounterSettings::opacity() * 255);
	for (auto child : CCArrayExt<CCLabelBMFont*>(panel->getChildren())) {
		child->setOpacity(opacity);
	}
}

// ---------------------------------------------------------------------------
// Trajectory simulation
//
// Every physics tick we save player 1's state and whether jump is held. When
// the player presses jump on tick F, we wait HORIZON ticks, then replay that
// stretch with an invisible fake player, moving the press 1, 2, 3... ticks
// earlier and later. The window is the number of press ticks that survive.
// ---------------------------------------------------------------------------

static constexpr int MAX_SHIFT = 12;  // furthest shift tried on each side
static constexpr int HORIZON = 60;    // ticks after the press that must be survived
static constexpr int BUFFER = 128;    // must be > HORIZON + MAX_SHIFT + 1

struct Snapshot {
	int tick = -1;
	Ref<PlayerCheckpoint> checkpoint;
	CCPoint position;
	double yVelocity = 0;
	bool onGround = false;
	float rotation = 0.f;
	bool held = false;  // jump held during this tick
};

struct RingUse {
	RingObject* ring;
	int tick;
	bool activated, disabled, disabled2;  // flags before the real player used it
};

struct Sim {
	PlayerObject* fake = nullptr;
	bool simulating = false;
	bool fakeDied = false;
	int tick = 0;
	float dt = 0.25f;
	bool held = false;
	int realDeathTick = -1;
	std::array<Snapshot, BUFFER> snaps;
	std::vector<int> pending;
	std::deque<RingUse> ringUses;

	static Sim& get() {
		static Sim instance;
		return instance;
	}

	void reset() {
		tick = 0;
		held = false;
		realDeathTick = -1;
		pending.clear();
		ringUses.clear();
		for (auto& s : snaps) s.tick = -1;
	}

	Snapshot* snap(int t) {
		if (t < 0) return nullptr;
		auto& s = snaps[t % BUFFER];
		return s.tick == t ? &s : nullptr;
	}

	void record(PlayerObject* player) {
		auto& s = snaps[tick % BUFFER];
		if (!s.checkpoint) s.checkpoint = PlayerCheckpoint::create();
		player->saveToCheckpoint(s.checkpoint);
		s.position = player->getPosition();
		s.yVelocity = player->m_yVelocity;
		s.onGround = player->m_isOnGround;
		s.rotation = player->getRotation();
		s.held = held;
		s.tick = tick;
	}

	// input for tick t when the press at `press` is shifted by `shift` ticks
	bool heldAt(int t, int press, int shift) {
		auto s = snap(t);
		bool h = s && s->held;
		if (shift < 0 && t >= press + shift && t < press) return true;
		if (shift > 0) {
			if (t == press + shift) return true;
			if (t >= press && t < press + shift) return false;
		}
		return h;
	}

	static void clearLogs(PlayerObject* p) {
		p->m_collisionLogTop->removeAllObjects();
		p->m_collisionLogBottom->removeAllObjects();
		p->m_collisionLogLeft->removeAllObjects();
		p->m_collisionLogRight->removeAllObjects();
	}

	// true if the fake player survives to press + HORIZON
	bool survives(GJBaseGameLayer* layer, int press, int shift) {
		int start = press + std::min(shift, 0);
		auto s = snap(start);
		if (!s || !fake) return false;

		// rings the real player used since `start` must look unused again
		std::vector<RingUse> restore;
		for (auto it = ringUses.rbegin(); it != ringUses.rend(); ++it) {
			if (it->tick < start) continue;
			auto r = it->ring;
			restore.push_back({r, 0, r->m_isActivated, r->m_isDisabled, r->m_isDisabled2});
			r->m_isActivated = it->activated;
			r->m_isDisabled = it->disabled;
			r->m_isDisabled2 = it->disabled2;
		}

		fake->loadFromCheckpoint(s->checkpoint);
		fake->setPosition(s->position);
		fake->m_yVelocity = s->yVelocity;
		fake->m_isOnGround = s->onGround;
		fake->setRotation(s->rotation);
		fake->m_isDead = false;
		if (fake->m_touchingRings) fake->m_touchingRings->removeAllObjects();
		fake->releaseButton(PlayerButton::Jump);

		simulating = true;
		fakeDied = false;
		bool prev = false;
		for (int t = start; t <= press + HORIZON && !fakeDied; t++) {
			bool h = this->heldAt(t, press, shift);
			if (h != prev) {
				h ? fake->pushButton(PlayerButton::Jump) : fake->releaseButton(PlayerButton::Jump);
				prev = h;
			}
			clearLogs(fake);
			layer->checkCollisions(fake, dt, false);
			if (fakeDied) break;
			fake->update(dt);
			fake->updateRotation(dt);
			fake->updatePlayerScale();
		}
		simulating = false;

		for (auto& r : restore) {
			r.ring->m_isActivated = r.activated;
			r.ring->m_isDisabled = r.disabled;
			r.ring->m_isDisabled2 = r.disabled2;
		}
		return !fakeDied;
	}

	// frames in a row the press could have happened on, or -1 if unknown
	int measure(GJBaseGameLayer* layer, int press) {
		if (realDeathTick >= press && realDeathTick <= press + HORIZON) return -1;
		// if the unshifted replay dies, the simulation doesn't match this spot
		if (!this->survives(layer, press, 0)) return -1;

		int window = 1;
		for (int k = 1; k <= MAX_SHIFT && this->survives(layer, press, -k); k++) window++;
		for (int k = 1; k <= MAX_SHIFT && this->survives(layer, press, k); k++) window++;
		return window;
	}
};

// ---------------------------------------------------------------------------
// Settings popup
// ---------------------------------------------------------------------------

class FrameCounterPopup : public Popup {
protected:
	struct Row {
		const char* key;
		Slider* slider;
		CCLabelBMFont* value;
		float min, max;
	};
	std::vector<Row> m_rows;

	bool init() {
		if (!Popup::init(340.f, 310.f)) return false;
		this->setTitle("Frame Window Counter");

		this->addToggle("Show counter", "enabled", CounterSettings::enabled(), -48.f);
		this->addToggle("Click numbers", "click-labels", CounterSettings::clickLabels(), -76.f);
		this->addToggle("Ding sound", "ding", CounterSettings::ding(), -104.f);

		this->addRow("Size", "scale", 0.15f, 1.5f, -140.f);
		this->addRow("Move X", "x", 0.f, 1.f, -175.f);
		this->addRow("Move Y", "y", 0.f, 1.f, -210.f);
		this->addRow("Opacity", "opacity", 0.1f, 1.f, -245.f);

		auto resetBtn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Reset", "goldFont.fnt", "GJ_button_04.png", 0.7f),
			this, menu_selector(FrameCounterPopup::onReset)
		);
		m_buttonMenu->addChildAtPosition(resetBtn, Anchor::Bottom, {0.f, 22.f});
		return true;
	}

	void addToggle(const char* name, const char* key, bool on, float y) {
		auto toggle = CCMenuItemToggler::createWithStandardSprites(
			this, menu_selector(FrameCounterPopup::onToggle), 0.65f
		);
		toggle->toggle(on);
		toggle->setUserObject(CCString::create(key));
		auto label = CCLabelBMFont::create(name, "bigFont.fnt");
		label->setScale(0.45f);
		label->setAnchorPoint({0.f, 0.5f});
		m_buttonMenu->addChildAtPosition(toggle, Anchor::Top, {-110.f, y});
		m_mainLayer->addChildAtPosition(label, Anchor::Top, {-88.f, y});
	}

	void addRow(const char* name, const char* key, float min, float max, float y) {
		auto title = CCLabelBMFont::create(name, "goldFont.fnt");
		title->setScale(0.55f);
		title->setAnchorPoint({0.f, 0.5f});
		m_mainLayer->addChildAtPosition(title, Anchor::Top, {-140.f, y});

		auto slider = Slider::create(this, menu_selector(FrameCounterPopup::onSlider), 0.65f);
		slider->setTag(static_cast<int>(m_rows.size()));
		m_mainLayer->addChildAtPosition(slider, Anchor::Top, {25.f, y});

		auto value = CCLabelBMFont::create("", "bigFont.fnt");
		value->setScale(0.35f);
		m_mainLayer->addChildAtPosition(value, Anchor::Top, {125.f, y});

		m_rows.push_back({key, slider, value, min, max});
		this->syncRow(m_rows.back());
	}

	float savedValue(Row const& row) {
		std::string_view key = row.key;
		if (key == "x") return CounterSettings::x();
		if (key == "y") return CounterSettings::y();
		if (key == "scale") return CounterSettings::scale();
		return CounterSettings::opacity();
	}

	void syncRow(Row& row) {
		float v = savedValue(row);
		row.slider->setValue((v - row.min) / (row.max - row.min));
		row.value->setString(fmt::format("{:.2f}", v).c_str());
	}

	void onSlider(CCObject* sender) {
		// the thumb is the sender; its parent chain leads back to the Slider
		auto node = static_cast<CCNode*>(sender);
		while (node && !typeinfo_cast<Slider*>(node)) node = node->getParent();
		if (!node) return;
		auto& row = m_rows.at(node->getTag());
		float v = row.min + row.slider->getValue() * (row.max - row.min);
		Mod::get()->setSavedValue<float>(row.key, v);
		row.value->setString(fmt::format("{:.2f}", v).c_str());
		this->refreshPanel();
	}

	void onToggle(CCObject* sender) {
		auto toggle = static_cast<CCMenuItemToggler*>(sender);
		auto key = static_cast<CCString*>(toggle->getUserObject())->getCString();
		// toggler reports its state before flipping
		Mod::get()->setSavedValue<bool>(key, !toggle->isToggled());
		this->refreshPanel();
	}

	void onReset(CCObject*) {
		Mod::get()->setSavedValue<float>("x", 0.01f);
		Mod::get()->setSavedValue<float>("y", 0.98f);
		Mod::get()->setSavedValue<float>("scale", 0.5f);
		Mod::get()->setSavedValue<float>("opacity", 1.f);
		for (auto& row : m_rows) this->syncRow(row);
		this->refreshPanel();
	}

	void refreshPanel();

public:
	static FrameCounterPopup* create() {
		auto ret = new FrameCounterPopup();
		if (ret->init()) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};

class $modify(FrameCounterPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto spr = CircleButtonSprite::createWithSpriteFrameName(
			"GJ_timeIcon_001.png", 1.f, CircleBaseColor::Green, CircleBaseSize::Small
		);
		auto btn = CCMenuItemSpriteExtra::create(
			spr, this, menu_selector(FrameCounterPause::onFrameCounterSettings)
		);
		btn->setID("frame-counter-settings"_spr);

		if (auto menu = this->getChildByID("right-button-menu")) {
			menu->addChild(btn);
			menu->updateLayout();
		}
		else {
			auto fallback = CCMenu::create();
			fallback->addChild(btn);
			auto win = CCDirector::get()->getWinSize();
			fallback->setPosition({win.width - 30.f, 30.f});
			this->addChild(fallback);
		}
	}

	void onFrameCounterSettings(CCObject*) {
		FrameCounterPopup::create()->show();
	}
};

// ---------------------------------------------------------------------------
// Play layer: panel, labels and per-tick recording
// ---------------------------------------------------------------------------

class $modify(FrameCounterPL, PlayLayer) {
	struct Fields {
		CCNode* panel = nullptr;
		std::array<CCLabelBMFont*, BUCKET_COUNT> countLabels{};
		std::array<int, BUCKET_COUNT> counts{};
		std::vector<CCLabelBMFont*> clickLabels;
	};

	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		Sim::get().reset();
		Sim::get().fake = nullptr;
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		auto panel = CCNode::create();
		panel->setID("frame-counter-panel"_spr);
		panel->setZOrder(1000);
		for (int i = 0; i < BUCKET_COUNT; i++) {
			auto name = CCLabelBMFont::create(BUCKETS[i].name, "bigFont.fnt");
			name->setAnchorPoint({0.f, 1.f});
			name->setColor(BUCKETS[i].color);
			name->setPosition({0.f, -i * 36.f});
			panel->addChild(name);

			auto count = CCLabelBMFont::create("0", "bigFont.fnt");
			count->setAnchorPoint({0.f, 1.f});
			count->setColor(BUCKETS[i].color);
			count->setPosition({115.f, -i * 36.f});
			panel->addChild(count);
			m_fields->countLabels[i] = count;
		}
		applyPanelSettings(panel);
		this->addChild(panel);
		m_fields->panel = panel;
		return true;
	}

	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();
		auto fake = PlayerObject::create(1, 1, this, this, true);
		fake->setPosition({0.f, 105.f});
		fake->setVisible(false);
		m_objectLayer->addChild(fake);
		Sim::get().fake = fake;
	}

	void onQuit() {
		Sim::get().fake = nullptr;
		Sim::get().reset();
		PlayLayer::onQuit();
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		Sim::get().reset();
		auto f = m_fields.self();
		f->counts.fill(0);
		for (auto label : f->countLabels) {
			if (label) label->setString("0");
		}
		for (auto label : f->clickLabels) label->removeFromParent();
		f->clickLabels.clear();
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		auto& sim = Sim::get();
		if (sim.simulating || player == sim.fake) {
			sim.fakeDied = true;
			return;
		}
		if (player == m_player1 && sim.realDeathTick < 0) sim.realDeathTick = sim.tick;
		PlayLayer::destroyPlayer(player, object);
	}

	void playEndAnimationToPos(CCPoint pos) {
		if (!Sim::get().simulating) PlayLayer::playEndAnimationToPos(pos);
	}

	void recordWindow(int press, int frames) {
		auto f = m_fields.self();
		auto b = bucketFor(frames);
		if (b >= 0) {
			f->counts[b]++;
			f->countLabels[b]->setString(std::to_string(f->counts[b]).c_str());
		}
		playDing(frames);

		auto s = Sim::get().snap(press);
		if (!s || !m_objectLayer) return;
		auto label = CCLabelBMFont::create(std::to_string(frames).c_str(), "bigFont.fnt");
		label->setScale(0.45f);
		label->setColor(colorFor(frames));
		label->setPosition(s->position + CCPoint{0.f, 22.f});
		label->setVisible(CounterSettings::clickLabels());
		m_objectLayer->addChild(label, 9999);
		f->clickLabels.push_back(label);
	}
};

class $modify(FrameCounterGL, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		auto& sim = Sim::get();
		if (typeinfo_cast<PlayLayer*>(this) && button == 1 && isPlayer1 && !sim.simulating) {
			if (down && !sim.held) sim.pending.push_back(sim.tick);
			sim.held = down;
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		auto pl = typeinfo_cast<PlayLayer*>(this);
		auto& sim = Sim::get();
		if (!pl || sim.simulating || m_isPlatformer || !sim.fake) {
			GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
			return;
		}

		sim.record(m_player1);
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		sim.tick++;

		while (!sim.ringUses.empty() && sim.ringUses.front().tick < sim.tick - BUFFER) {
			sim.ringUses.pop_front();
		}

		// presses whose horizon has fully played out can be measured now
		for (auto it = sim.pending.begin(); it != sim.pending.end();) {
			if (sim.tick <= *it + HORIZON) {
				++it;
				continue;
			}
			int window = sim.measure(this, *it);
			if (window > 0) static_cast<FrameCounterPL*>(pl)->recordWindow(*it, window);
			it = sim.pending.erase(it);
		}
	}

	// keep the fake player from touching anything but solids, hazards, orbs,
	// pads and portals, so the replay doesn't fire triggers or collect coins
	void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
		if (!Sim::get().simulating) {
			GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
			return;
		}
		std::vector<GameObject*> disabled;
		for (int i = 0; i < count && i < static_cast<int>(objects->size()); i++) {
			auto obj = objects->at(i);
			if (!obj || obj->m_isDisabled || obj->m_isDisabled2) continue;
			auto type = obj->m_objectType;
			bool keep = type == GameObjectType::Solid || type == GameObjectType::Hazard
				|| type == GameObjectType::AnimatedHazard || type == GameObjectType::Slope
				|| typeinfo_cast<RingObject*>(obj) || obj->m_objectID == 35 || obj->m_objectID == 67
				|| obj->m_objectID == 140 || obj->m_objectID == 1332 || obj->m_objectID == 3005
				|| type == GameObjectType::InverseGravityPortal || type == GameObjectType::NormalGravityPortal
				|| type == GameObjectType::MiniSizePortal || type == GameObjectType::RegularSizePortal;
			if (keep) continue;
			obj->m_isDisabled = true;
			obj->m_isDisabled2 = true;
			disabled.push_back(obj);
		}
		GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
		for (auto obj : disabled) {
			obj->m_isDisabled = false;
			obj->m_isDisabled2 = false;
		}
	}

	void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
		if (!Sim::get().simulating) GJBaseGameLayer::playerTouchedTrigger(player, object);
	}

	void gameEventTriggered(GJGameEvent event, int material, int playerID) {
		if (!Sim::get().simulating) GJBaseGameLayer::gameEventTriggered(event, material, playerID);
	}

	void activateSFXTrigger(SFXTriggerGameObject* object) {
		if (!Sim::get().simulating) GJBaseGameLayer::activateSFXTrigger(object);
	}

	void activateSongEditTrigger(SongTriggerGameObject* object) {
		if (!Sim::get().simulating) GJBaseGameLayer::activateSongEditTrigger(object);
	}
};

class $modify(FrameCounterPO, PlayerObject) {
	void update(float dt) {
		PlayerObject::update(dt);
		auto& sim = Sim::get();
		if (!sim.simulating && this != sim.fake) sim.dt = dt;
	}

	void ringJump(RingObject* ring, bool skipCheck) {
		auto& sim = Sim::get();
		if (sim.simulating) {
			// let the fake bounce off the orb without using it up
			bool a = ring->m_isActivated, d = ring->m_isDisabled, d2 = ring->m_isDisabled2;
			PlayerObject::ringJump(ring, skipCheck);
			ring->m_isActivated = a;
			ring->m_isDisabled = d;
			ring->m_isDisabled2 = d2;
			return;
		}
		sim.ringUses.push_back({ring, sim.tick, ring->m_isActivated, ring->m_isDisabled, ring->m_isDisabled2});
		PlayerObject::ringJump(ring, skipCheck);
	}

	void incrementJumps() {
		if (!Sim::get().simulating) PlayerObject::incrementJumps();
	}

	void playSpiderDashEffect(CCPoint from, CCPoint to) {
		if (!Sim::get().simulating) PlayerObject::playSpiderDashEffect(from, to);
	}
};

class $modify(HardStreak) {
	void addPoint(CCPoint point) {
		if (!Sim::get().simulating) HardStreak::addPoint(point);
	}
};

class $modify(GameObject) {
	void playShineEffect() {
		if (!Sim::get().simulating) GameObject::playShineEffect();
	}
};

void FrameCounterPopup::refreshPanel() {
	if (auto pl = PlayLayer::get()) {
		if (auto panel = pl->getChildByID("frame-counter-panel"_spr)) {
			applyPanelSettings(panel);
		}
		for (auto label : static_cast<FrameCounterPL*>(pl)->m_fields->clickLabels) {
			label->setVisible(CounterSettings::clickLabels());
		}
	}
}

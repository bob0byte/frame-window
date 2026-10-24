#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/GameObject.hpp>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static constexpr int FPS_OPTIONS[] = {240, 165, 144, 120, 75, 60, 30};

struct CounterSettings {
	static bool get(char const* key, bool def) { return Mod::get()->getSavedValue<bool>(key, def); }
	static float num(char const* key, float def) { return Mod::get()->getSavedValue<float>(key, def); }

	static bool enabled() { return get("enabled", true); }
	static bool markers() { return get("click-labels", true); }
	static bool ding() { return get("ding", true); }
	static bool debug() { return get("debug", false); }
	static float x() { return num("x", 0.01f); }
	static float y() { return num("y", 0.98f); }
	static float scale() { return num("scale", 0.5f); }
	static float opacity() { return num("opacity", 1.f); }
	static float markerSize() { return num("marker-size", 1.f); }
	static float volume() { return num("volume", 0.8f); }
	static int fps() { return Mod::get()->getSavedValue<int>("fps", 240); }
};

static float defaultFor(std::string_view key) {
	if (key == "x") return 0.01f;
	if (key == "y") return 0.98f;
	if (key == "scale") return 0.5f;
	if (key == "opacity") return 1.f;
	if (key == "marker-size") return 1.f;
	return 0.8f;  // volume
}

// ---------------------------------------------------------------------------
// Buckets: hardest at the top. A window is measured in physics ticks (240/s)
// and shown in the chosen FPS, so at 60 FPS one tick is 0.25 frames.
// ---------------------------------------------------------------------------

struct Bucket {
	const char* name;
	float above, upTo;  // window is in (above, upTo]
	ccColor3B color;
};
static constexpr int BUCKET_COUNT = 7;
static Bucket const BUCKETS[BUCKET_COUNT] = {
	{"1:", 0.f, 1.f, {255, 60, 60}},
	{"2:", 1.f, 2.f, {255, 150, 30}},
	{"3:", 2.f, 3.f, {255, 220, 0}},
	{"4:", 3.f, 4.f, {255, 255, 255}},
	{"5-6:", 4.f, 6.f, {100, 230, 100}},
	{"7-8:", 6.f, 8.f, {90, 200, 255}},
	{"9-12:", 8.f, 12.f, {110, 110, 255}},
};

static float toFrames(int ticks) {
	return ticks * CounterSettings::fps() / 240.f;
}

static int bucketFor(float frames) {
	for (int i = 0; i < BUCKET_COUNT; i++) {
		if (frames > BUCKETS[i].above && frames <= BUCKETS[i].upTo) return i;
	}
	return -1;
}

static ccColor3B colorFor(float frames) {
	auto b = bucketFor(frames);
	return b < 0 ? ccColor3B{200, 200, 200} : BUCKETS[b].color;
}

static std::string formatFrames(float frames) {
	auto s = fmt::format("{:.2f}", frames);
	while (s.back() == '0') s.pop_back();
	if (s.back() == '.') s.pop_back();
	return s;
}

// ---------------------------------------------------------------------------
// Ding: a sound file named ding.ogg / ding.mp3 / ding.wav in the mod's config
// folder replaces the built-in one. Tighter windows play at a lower pitch.
// ---------------------------------------------------------------------------

static std::string dingPath() {
	for (auto name : {"ding.ogg", "ding.mp3", "ding.wav"}) {
		auto path = Mod::get()->getConfigDir() / name;
		std::error_code ec;
		if (std::filesystem::exists(path, ec)) return utils::string::pathToString(path);
	}
	return "ding.wav"_spr;
}

static void playDing(float frames) {
	if (!CounterSettings::ding() || bucketFor(frames) < 0) return;
	// 1 frame plays at half speed, 12 frames at normal speed
	float speed = 0.5f + 0.5f * (std::clamp(frames, 1.f, 12.f) - 1.f) / 11.f;
	FMODAudioEngine::get()->playEffect(dingPath(), speed, 0.f, CounterSettings::volume());
}

// ---------------------------------------------------------------------------
// Trajectory simulation
//
// Every physics tick we save player 1's state and whether jump is held. For
// each press and release on tick F we wait HORIZON ticks, then replay that
// stretch with an invisible fake player, moving that one input 1, 2, 3...
// ticks earlier and later (never past the inputs around it). The window is
// how many ticks in a row the input could have landed on and survived.
// ---------------------------------------------------------------------------

static constexpr int MAX_SHIFT = 48;  // furthest shift tried each side (12 frames at 60 FPS)
static constexpr int HORIZON = 60;    // ticks after the input that must be survived
static constexpr int BUFFER = 256;    // must be > HORIZON + MAX_SHIFT + 2

struct Snapshot {
	int tick = -1;
	Ref<PlayerCheckpoint> checkpoint;
	CCPoint position;
	double yVelocity = 0;
	bool onGround = false;
	float rotation = 0.f;
	bool held = false;  // jump held during this tick (after inputs were processed)
};

struct InputEvent {
	int tick;
	bool down;
};

struct RingUse {
	RingObject* ring;
	int tick;
	bool activated, disabled, disabled2;  // flags before the real player used it
};

struct Stats {
	int inputs = 0, measured = 0, mismatched = 0, skipped = 0;
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
	std::vector<InputEvent> events;
	std::vector<InputEvent> pending;
	std::deque<RingUse> ringUses;
	Stats stats;

	static Sim& get() {
		static Sim instance;
		return instance;
	}

	void reset() {
		tick = 0;
		held = false;
		realDeathTick = -1;
		events.clear();
		pending.clear();
		ringUses.clear();
		stats = {};
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

	void input(bool down) {
		if (down == held) return;
		held = down;
		events.push_back({tick, down});
		pending.push_back({tick, down});
		stats.inputs++;
	}

	// input for tick t when `ev` is moved by `shift` ticks
	bool heldAt(int t, InputEvent ev, int shift) {
		auto s = snap(t);
		bool h = s ? s->held : false;
		if (shift < 0 && t >= ev.tick + shift && t < ev.tick) return ev.down;
		if (shift > 0 && t >= ev.tick && t < ev.tick + shift) return !ev.down;
		return h;
	}

	static void clearLogs(PlayerObject* p) {
		p->m_collisionLogTop->removeAllObjects();
		p->m_collisionLogBottom->removeAllObjects();
		p->m_collisionLogLeft->removeAllObjects();
		p->m_collisionLogRight->removeAllObjects();
	}

	// replays from the earliest affected tick to ev.tick + HORIZON; returns
	// false if the fake dies, and puts its final position in `end`
	bool survives(GJBaseGameLayer* layer, InputEvent ev, int shift, CCPoint* end = nullptr) {
		int start = ev.tick + std::min(shift, 0);
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
		auto before = snap(start - 1);
		bool prev = before && before->held;
		if (prev) fake->pushButton(PlayerButton::Jump);
		for (int t = start; t <= ev.tick + HORIZON && !fakeDied; t++) {
			bool h = this->heldAt(t, ev, shift);
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
		if (end) *end = fake->getPosition();

		for (auto& r : restore) {
			r.ring->m_isActivated = r.activated;
			r.ring->m_isDisabled = r.disabled;
			r.ring->m_isDisabled2 = r.disabled2;
		}
		return !fakeDied;
	}

	// window in ticks, or -1 if it can't be measured reliably
	int measure(GJBaseGameLayer* layer, InputEvent ev) {
		if (realDeathTick >= ev.tick && realDeathTick <= ev.tick + HORIZON + 1) {
			stats.skipped++;
			return -1;
		}

		// the unshifted replay has to land where the real player did,
		// otherwise the simulation doesn't match this spot
		CCPoint end;
		auto real = snap(ev.tick + HORIZON + 1);
		if (!this->survives(layer, ev, 0, &end) || !real || end.getDistance(real->position) > 2.f) {
			stats.mismatched++;
			return -1;
		}

		// an input can't be moved past the inputs before and after it
		int prevTick = INT_MIN, nextTick = INT_MAX;
		for (auto& e : events) {
			if (e.tick < ev.tick) prevTick = std::max(prevTick, e.tick);
			if (e.tick > ev.tick) nextTick = std::min(nextTick, e.tick);
		}

		int window = 1;
		for (int k = 1; k <= MAX_SHIFT && ev.tick - k > prevTick && this->survives(layer, ev, -k); k++) window++;
		for (int k = 1; k <= MAX_SHIFT && ev.tick + k < nextTick && this->survives(layer, ev, k); k++) window++;
		stats.measured++;
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
	CCLabelBMFont* m_fpsLabel = nullptr;

	bool init() {
		if (!Popup::init(380.f, 380.f)) return false;
		this->setTitle("Frame Window Counter");

		this->addToggle("Show counter", "enabled", CounterSettings::enabled(), -165.f, -48.f);
		this->addToggle("Click markers", "click-labels", CounterSettings::markers(), 20.f, -48.f);
		this->addToggle("Ding sound", "ding", CounterSettings::ding(), -165.f, -76.f);
		this->addToggle("Debug info", "debug", CounterSettings::debug(), 20.f, -76.f);

		this->addRow("Size", "scale", 0.15f, 1.5f, -110.f);
		this->addRow("Move X", "x", 0.f, 1.f, -140.f);
		this->addRow("Move Y", "y", 0.f, 1.f, -170.f);
		this->addRow("Opacity", "opacity", 0.1f, 1.f, -200.f);
		this->addRow("Marker size", "marker-size", 0.3f, 2.5f, -230.f);
		this->addRow("Ding volume", "volume", 0.f, 2.f, -260.f);

		auto fpsTitle = CCLabelBMFont::create("Counter FPS", "goldFont.fnt");
		fpsTitle->setScale(0.55f);
		fpsTitle->setAnchorPoint({0.f, 0.5f});
		m_mainLayer->addChildAtPosition(fpsTitle, Anchor::Top, {-170.f, -292.f});
		m_fpsLabel = CCLabelBMFont::create("", "bigFont.fnt");
		m_fpsLabel->setScale(0.45f);
		m_mainLayer->addChildAtPosition(m_fpsLabel, Anchor::Top, {40.f, -292.f});
		for (int dir : {-1, 1}) {
			auto spr = CCSprite::createWithSpriteFrameName("edit_leftBtn_001.png");
			if (dir > 0) spr->setFlipX(true);
			auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FrameCounterPopup::onFps));
			btn->setTag(dir);
			m_buttonMenu->addChildAtPosition(btn, Anchor::Top, {40.f + dir * 55.f, -292.f});
		}
		this->updateFpsLabel();

		auto resetBtn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Reset", "goldFont.fnt", "GJ_button_04.png", 0.7f),
			this, menu_selector(FrameCounterPopup::onReset)
		);
		m_buttonMenu->addChildAtPosition(resetBtn, Anchor::Bottom, {-70.f, 25.f});
		auto folderBtn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Sound folder", "goldFont.fnt", "GJ_button_01.png", 0.7f),
			this, menu_selector(FrameCounterPopup::onSoundFolder)
		);
		m_buttonMenu->addChildAtPosition(folderBtn, Anchor::Bottom, {60.f, 25.f});
		return true;
	}

	void addToggle(const char* name, const char* key, bool on, float x, float y) {
		auto toggle = CCMenuItemToggler::createWithStandardSprites(
			this, menu_selector(FrameCounterPopup::onToggle), 0.6f
		);
		toggle->toggle(on);
		toggle->setUserObject(CCString::create(key));
		auto label = CCLabelBMFont::create(name, "bigFont.fnt");
		label->setScale(0.4f);
		label->setAnchorPoint({0.f, 0.5f});
		m_buttonMenu->addChildAtPosition(toggle, Anchor::Top, {x, y});
		m_mainLayer->addChildAtPosition(label, Anchor::Top, {x + 20.f, y});
	}

	void addRow(const char* name, const char* key, float min, float max, float y) {
		auto title = CCLabelBMFont::create(name, "goldFont.fnt");
		title->setScale(0.5f);
		title->setAnchorPoint({0.f, 0.5f});
		m_mainLayer->addChildAtPosition(title, Anchor::Top, {-170.f, y});

		auto slider = Slider::create(this, menu_selector(FrameCounterPopup::onSlider), 0.6f);
		slider->setTag(static_cast<int>(m_rows.size()));
		m_mainLayer->addChildAtPosition(slider, Anchor::Top, {40.f, y});

		auto value = CCLabelBMFont::create("", "bigFont.fnt");
		value->setScale(0.35f);
		m_mainLayer->addChildAtPosition(value, Anchor::Top, {155.f, y});

		m_rows.push_back({key, slider, value, min, max});
		this->syncRow(m_rows.back());
	}

	void syncRow(Row& row) {
		float v = CounterSettings::num(row.key, defaultFor(row.key));
		row.slider->setValue((v - row.min) / (row.max - row.min));
		row.value->setString(fmt::format("{:.2f}", v).c_str());
	}

	void updateFpsLabel() {
		m_fpsLabel->setString(fmt::format("{} FPS", CounterSettings::fps()).c_str());
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

	void onFps(CCObject* sender) {
		int count = static_cast<int>(std::size(FPS_OPTIONS));
		int idx = 0;
		for (int i = 0; i < count; i++) {
			if (FPS_OPTIONS[i] == CounterSettings::fps()) idx = i;
		}
		idx = (idx + static_cast<CCNode*>(sender)->getTag() + count) % count;
		Mod::get()->setSavedValue<int>("fps", FPS_OPTIONS[idx]);
		this->updateFpsLabel();
		this->refreshPanel();
	}

	void onReset(CCObject*) {
		for (auto key : {"x", "y", "scale", "opacity", "marker-size", "volume"}) {
			Mod::get()->setSavedValue<float>(key, defaultFor(key));
		}
		for (auto& row : m_rows) this->syncRow(row);
		this->refreshPanel();
	}

	void onSoundFolder(CCObject*) {
		FLAlertLayer::create(
			"Custom ding",
			"Put a sound named <cy>ding.ogg</c>, <cy>ding.mp3</c> or <cy>ding.wav</c> in the folder that opens. It replaces the built-in ding.",
			"OK"
		)->show();
		utils::file::openFolder(Mod::get()->getConfigDir());
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
// Play layer: panel, markers and per-tick recording
// ---------------------------------------------------------------------------

// a press is a filled circle, a release a hollow ring; the number sits beside it
static CCNode* createMarker(float frames, bool press) {
	auto node = CCNode::create();
	auto color = colorFor(frames);
	auto c4 = ccc4FFromccc3B(color);

	std::array<CCPoint, 28> verts;
	for (size_t i = 0; i < verts.size(); i++) {
		float a = static_cast<float>(i) / verts.size() * 2.f * static_cast<float>(M_PI);
		verts[i] = {std::cos(a) * 9.f, std::sin(a) * 9.f};
	}
	auto draw = CCDrawNode::create();
	auto fill = press ? ccColor4F{c4.r, c4.g, c4.b, 0.9f} : ccColor4F{0.f, 0.f, 0.f, 0.f};
	auto border = press ? ccColor4F{1.f, 1.f, 1.f, 0.9f} : ccColor4F{c4.r, c4.g, c4.b, 1.f};
	draw->drawPolygon(verts.data(), static_cast<unsigned>(verts.size()), fill, 2.f, border);
	node->addChild(draw);

	auto label = CCLabelBMFont::create(formatFrames(frames).c_str(), "bigFont.fnt");
	label->setScale(0.45f);
	label->setColor(color);
	label->setAnchorPoint({0.f, 0.5f});
	label->setPosition({13.f, 0.f});
	node->addChild(label);
	return node;
}

class $modify(FrameCounterPL, PlayLayer) {
	struct Marker {
		CCPoint world;  // position in the object layer
		CCNode* node;
	};

	struct Fields {
		CCNode* panel = nullptr;
		CCNode* markerLayer = nullptr;
		CCLabelBMFont* debugLabel = nullptr;
		std::array<CCLabelBMFont*, BUCKET_COUNT> countLabels{};
		std::array<int, BUCKET_COUNT> counts{};
		std::vector<Marker> markers;
	};

	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		Sim::get().reset();
		Sim::get().fake = nullptr;
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		// the UI layer isn't flipped by mirror portals or moved by the camera
		CCNode* hud = m_uiLayer ? static_cast<CCNode*>(m_uiLayer) : this;

		auto markerLayer = CCNode::create();
		markerLayer->setID("frame-counter-markers"_spr);
		hud->addChild(markerLayer, 999);
		m_fields->markerLayer = markerLayer;

		auto panel = CCNode::create();
		panel->setID("frame-counter-panel"_spr);
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
		auto debug = CCLabelBMFont::create("", "chatFont.fnt");
		debug->setAnchorPoint({0.f, 1.f});
		debug->setPosition({0.f, -BUCKET_COUNT * 36.f - 6.f});
		panel->addChild(debug);
		m_fields->debugLabel = debug;
		hud->addChild(panel, 1000);
		m_fields->panel = panel;
		this->applySettings();
		return true;
	}

	void applySettings() {
		auto f = m_fields.self();
		if (!f->panel) return;
		auto win = CCDirector::get()->getWinSize();
		f->panel->setVisible(CounterSettings::enabled());
		f->panel->setPosition({win.width * CounterSettings::x(), win.height * CounterSettings::y()});
		f->panel->setScale(CounterSettings::scale());
		auto opacity = static_cast<GLubyte>(CounterSettings::opacity() * 255);
		for (auto label : CCArrayExt<CCLabelBMFont*>(f->panel->getChildren())) {
			label->setOpacity(opacity);
		}
		f->debugLabel->setVisible(CounterSettings::debug());
		f->markerLayer->setVisible(CounterSettings::markers());
		for (auto& m : f->markers) m.node->setScale(CounterSettings::markerSize());
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
		for (auto& m : f->markers) m.node->removeFromParent();
		f->markers.clear();
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

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		auto f = m_fields.self();
		if (!f->markerLayer) return;

		// keep markers on their world spot but drawn upright in the HUD
		auto win = CCDirector::get()->getWinSize();
		for (auto& m : f->markers) {
			auto screen = m_objectLayer->convertToWorldSpace(m.world);
			auto local = f->markerLayer->convertToNodeSpace(screen);
			m.node->setPosition(local);
			m.node->setVisible(screen.x > -60.f && screen.x < win.width + 60.f && screen.y > -60.f && screen.y < win.height + 60.f);
		}

		if (CounterSettings::debug()) {
			auto& s = Sim::get().stats;
			f->debugLabel->setString(fmt::format(
				"inputs {}  measured {}  mismatch {}  died {}",
				s.inputs, s.measured, s.mismatched, s.skipped
			).c_str());
		}
	}

	void recordWindow(InputEvent ev, int ticks) {
		auto f = m_fields.self();
		float frames = toFrames(ticks);
		auto b = bucketFor(frames);
		if (b >= 0) {
			f->counts[b]++;
			f->countLabels[b]->setString(std::to_string(f->counts[b]).c_str());
		}
		playDing(frames);

		auto s = Sim::get().snap(ev.tick);
		if (!s || !f->markerLayer) return;
		auto node = createMarker(frames, ev.down);
		node->setScale(CounterSettings::markerSize());
		f->markerLayer->addChild(node);
		f->markers.push_back({s->position, node});
	}
};

class $modify(FrameCounterGL, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		auto& sim = Sim::get();
		if (typeinfo_cast<PlayLayer*>(this) && button == 1 && isPlayer1 && !sim.simulating) {
			sim.input(down);
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
		// queued inputs are handled inside the tick, so the held state for
		// this tick is only known now
		if (auto s = sim.snap(sim.tick)) s->held = sim.held;
		sim.tick++;

		while (!sim.ringUses.empty() && sim.ringUses.front().tick < sim.tick - BUFFER) {
			sim.ringUses.pop_front();
		}
		std::erase_if(sim.events, [&](InputEvent const& e) { return e.tick < sim.tick - BUFFER; });

		// inputs whose horizon has fully played out can be measured now
		for (auto it = sim.pending.begin(); it != sim.pending.end();) {
			if (sim.tick <= it->tick + HORIZON + 1) {
				++it;
				continue;
			}
			auto ev = *it;
			it = sim.pending.erase(it);
			int window = sim.measure(this, ev);
			if (window > 0) static_cast<FrameCounterPL*>(pl)->recordWindow(ev, window);
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
		static_cast<FrameCounterPL*>(pl)->applySettings();
	}
}

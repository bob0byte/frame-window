#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;

// settings, saved between sessions; x/y are fractions of the screen (0..1)
struct CounterSettings {
	static bool enabled() { return Mod::get()->getSavedValue<bool>("enabled", true); }
	static bool orbLabels() { return Mod::get()->getSavedValue<bool>("orb-labels", true); }
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
		if (!Popup::init(340.f, 290.f)) return false;
		this->setTitle("Frame Window Counter");

		this->addToggle("Show counter", "enabled", CounterSettings::enabled(), -50.f);
		this->addToggle("Orb numbers", "orb-labels", CounterSettings::orbLabels(), -82.f);

		this->addRow("Size", "scale", 0.15f, 1.5f, -120.f);
		this->addRow("Move X", "x", 0.f, 1.f, -155.f);
		this->addRow("Move Y", "y", 0.f, 1.f, -190.f);
		this->addRow("Opacity", "opacity", 0.1f, 1.f, -225.f);

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

class $modify(FrameCounterPL, PlayLayer) {
	struct Fields {
		CCNode* panel = nullptr;
		std::array<CCLabelBMFont*, BUCKET_COUNT> countLabels{};
		std::array<int, BUCKET_COUNT> counts{};
		// frames the player has been touching each orb so far
		std::unordered_map<RingObject*, int> touching;
		// orbs whose window has already been measured
		std::unordered_set<RingObject*> measured;
		std::unordered_map<RingObject*, CCLabelBMFont*> orbLabels;
	};

	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
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

	void resetLevel() {
		PlayLayer::resetLevel();
		auto f = m_fields.self();
		f->counts.fill(0);
		f->touching.clear();
		f->measured.clear();
		for (auto label : f->countLabels) {
			if (label) label->setString("0");
		}
	}

	// called once per physics tick
	void trackOrbs() {
		auto f = m_fields.self();
		std::unordered_set<RingObject*> now;
		for (auto player : {m_player1, m_player2}) {
			if (!player || !player->m_touchingRings) continue;
			for (auto ring : CCArrayExt<RingObject*>(player->m_touchingRings)) {
				now.insert(ring);
			}
		}

		for (auto ring : now) {
			if (!f->measured.contains(ring)) f->touching[ring]++;
		}

		// an orb the player stopped touching has a finished window
		for (auto it = f->touching.begin(); it != f->touching.end();) {
			if (now.contains(it->first)) {
				++it;
				continue;
			}
			this->recordWindow(it->first, it->second);
			f->measured.insert(it->first);
			it = f->touching.erase(it);
		}
	}

	void recordWindow(RingObject* ring, int frames) {
		auto f = m_fields.self();
		auto b = bucketFor(frames);
		if (b >= 0) {
			f->counts[b]++;
			f->countLabels[b]->setString(std::to_string(f->counts[b]).c_str());
		}

		if (!CounterSettings::orbLabels() || !m_objectLayer) return;
		auto& label = f->orbLabels[ring];
		if (!label) {
			label = CCLabelBMFont::create("", "bigFont.fnt");
			label->setScale(0.45f);
			label->setAnchorPoint({1.f, 0.5f});
			label->setPosition(ring->getPosition() + CCPoint{-14.f, 0.f});
			m_objectLayer->addChild(label, 9999);
		}
		label->setString(std::to_string(frames).c_str());
		label->setColor(colorFor(frames));
	}
};

class $modify(FrameCounterGL, GJBaseGameLayer) {
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		if (auto pl = typeinfo_cast<PlayLayer*>(this)) {
			static_cast<FrameCounterPL*>(pl)->trackOrbs();
		}
	}
};

void FrameCounterPopup::refreshPanel() {
	if (auto pl = PlayLayer::get()) {
		if (auto panel = pl->getChildByID("frame-counter-panel"_spr)) {
			applyPanelSettings(panel);
		}
		for (auto& [ring, label] : static_cast<FrameCounterPL*>(pl)->m_fields->orbLabels) {
			label->setVisible(CounterSettings::orbLabels());
		}
	}
}

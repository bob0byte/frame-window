#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;

// settings, saved between sessions; x/y are fractions of the screen (0..1)
struct CounterSettings {
	static bool enabled() { return Mod::get()->getSavedValue<bool>("enabled", true); }
	static float x() { return Mod::get()->getSavedValue<float>("x", 0.01f); }
	static float y() { return Mod::get()->getSavedValue<float>("y", 0.98f); }
	static float scale() { return Mod::get()->getSavedValue<float>("scale", 0.4f); }
	static float opacity() { return Mod::get()->getSavedValue<float>("opacity", 0.7f); }
};

static void applyLabelSettings(CCLabelBMFont* label) {
	auto win = CCDirector::get()->getWinSize();
	label->setVisible(CounterSettings::enabled());
	label->setPosition({win.width * CounterSettings::x(), win.height * CounterSettings::y()});
	label->setScale(CounterSettings::scale());
	label->setOpacity(static_cast<GLubyte>(CounterSettings::opacity() * 255));
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
		if (!Popup::init(340.f, 270.f)) return false;
		this->setTitle("Frame Counter");

		auto toggle = CCMenuItemToggler::createWithStandardSprites(
			this, menu_selector(FrameCounterPopup::onToggle), 0.7f
		);
		toggle->toggle(CounterSettings::enabled());
		auto toggleLabel = CCLabelBMFont::create("Show counter", "bigFont.fnt");
		toggleLabel->setScale(0.45f);
		m_buttonMenu->addChildAtPosition(toggle, Anchor::Top, {-70.f, -50.f});
		m_mainLayer->addChildAtPosition(toggleLabel, Anchor::Top, {15.f, -50.f});

		this->addRow("Size", "scale", 0.15f, 1.5f, -85.f);
		this->addRow("Move X", "x", 0.f, 1.f, -125.f);
		this->addRow("Move Y", "y", 0.f, 1.f, -165.f);
		this->addRow("Opacity", "opacity", 0.1f, 1.f, -205.f);

		auto resetBtn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Reset", "goldFont.fnt", "GJ_button_04.png", 0.7f),
			this, menu_selector(FrameCounterPopup::onReset)
		);
		m_buttonMenu->addChildAtPosition(resetBtn, Anchor::Bottom, {0.f, 22.f});
		return true;
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
		this->refreshLabel();
	}

	void onToggle(CCObject* sender) {
		// toggler reports its state before flipping
		bool on = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
		Mod::get()->setSavedValue<bool>("enabled", on);
		this->refreshLabel();
	}

	void onReset(CCObject*) {
		Mod::get()->setSavedValue<float>("x", 0.01f);
		Mod::get()->setSavedValue<float>("y", 0.98f);
		Mod::get()->setSavedValue<float>("scale", 0.4f);
		Mod::get()->setSavedValue<float>("opacity", 0.7f);
		for (auto& row : m_rows) this->syncRow(row);
		this->refreshLabel();
	}

	void refreshLabel() {
		if (auto pl = PlayLayer::get()) {
			if (auto label = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID("frame-counter-label"_spr))) {
				applyLabelSettings(label);
			}
		}
	}

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
		CCLabelBMFont* label = nullptr;
		int levelFrames = 0;
	};

	// physics ticks since the attempt started (240 per second in 2.2)
	int currentFrame() {
		return m_gameState.m_currentProgress;
	}

	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		auto label = CCLabelBMFont::create("Frame: 0", "bigFont.fnt");
		label->setAnchorPoint({0.f, 1.f});
		label->setZOrder(1000);
		label->setID("frame-counter-label"_spr);
		applyLabelSettings(label);
		this->addChild(label);
		m_fields->label = label;
		return true;
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		if (!m_fields->label || !m_fields->label->isVisible()) return;

		auto frames = this->currentFrame();
		auto text = fmt::format("Frame: {} ({:.3f}s)", frames, frames / 240.0);
		if (m_fields->levelFrames > 0) {
			text += fmt::format("\nLevel: {} frames", m_fields->levelFrames);
		}
		m_fields->label->setString(text.c_str());
	}

	void levelComplete() {
		m_fields->levelFrames = this->currentFrame();
		log::info("Level completed in {} frames", m_fields->levelFrames);
		Notification::create(
			fmt::format("Level length: {} frames", m_fields->levelFrames),
			NotificationIcon::Success
		)->show();
		PlayLayer::levelComplete();
	}
};

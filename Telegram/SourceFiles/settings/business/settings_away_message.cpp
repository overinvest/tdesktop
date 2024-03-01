/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/business/settings_away_message.h"

#include "base/unixtime.h"
#include "core/application.h"
#include "data/business/data_business_info.h"
#include "data/business/data_shortcut_messages.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/business/settings_recipients_helper.h"
#include "settings/business/settings_shortcut_messages.h"
#include "ui/boxes/choose_date_time.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

class AwayMessage : public BusinessSection<AwayMessage> {
public:
	AwayMessage(
		QWidget *parent,
		not_null<Window::SessionController*> controller);
	~AwayMessage();

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent(not_null<Window::SessionController*> controller);
	void save();

	rpl::variable<Data::BusinessRecipients> _recipients;
	rpl::variable<Data::AwaySchedule> _schedule;
	rpl::variable<bool> _enabled;

};

[[nodiscard]] TimeId StartTimeMin() {
	// Telegram was launched in August 2013 :)
	return base::unixtime::serialize(QDateTime(QDate(2013, 8, 1)));
}

[[nodiscard]] TimeId EndTimeMin() {
	return StartTimeMin() + 3600;
}

[[nodiscard]] bool BadCustomInterval(const Data::WorkingInterval &interval) {
	return !interval
		|| (interval.start < StartTimeMin())
		|| (interval.end < EndTimeMin());
}

struct AwayScheduleSelectorDescriptor {
	not_null<Window::SessionController*> controller;
	not_null<rpl::variable<Data::AwaySchedule>*> data;
};
void AddAwayScheduleSelector(
		not_null<Ui::VerticalLayout*> container,
		AwayScheduleSelectorDescriptor &&descriptor) {
	using Type = Data::AwayScheduleType;
	using namespace rpl::mappers;

	const auto controller = descriptor.controller;
	const auto data = descriptor.data;

	Ui::AddSubsectionTitle(container, tr::lng_away_schedule());
	const auto group = std::make_shared<Ui::RadioenumGroup<Type>>(
		data->current().type);

	const auto add = [&](Type type, const QString &label) {
		container->add(
			object_ptr<Ui::Radioenum<Type>>(
				container,
				group,
				type,
				label),
			st::boxRowPadding + st::settingsAwaySchedulePadding);
	};
	add(Type::Always, tr::lng_away_schedule_always(tr::now));
	add(Type::OutsideWorkingHours, tr::lng_away_schedule_outside(tr::now));
	add(Type::Custom, tr::lng_away_schedule_custom(tr::now));

	const auto customWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto customInner = customWrap->entity();
	customWrap->toggleOn(group->value() | rpl::map(_1 == Type::Custom));

	group->changes() | rpl::start_with_next([=](Type value) {
		auto copy = data->current();
		copy.type = value;
		*data = copy;
	}, customWrap->lifetime());

	const auto chooseDate = [=](
			rpl::producer<QString> title,
			TimeId now,
			Fn<TimeId()> min,
			Fn<TimeId()> max,
			Fn<void(TimeId)> done) {
		using namespace Ui;
		const auto box = std::make_shared<QPointer<Ui::BoxContent>>();
		const auto save = [=](TimeId time) {
			done(time);
			if (const auto strong = box->data()) {
				strong->closeBox();
			}
		};
		*box = controller->show(Box(ChooseDateTimeBox, ChooseDateTimeBoxArgs{
			.title = std::move(title),
			.submit = tr::lng_settings_save(),
			.done = save,
			.min = min,
			.time = now,
			.max = max,
		}));
	};

	Ui::AddSkip(customInner);
	Ui::AddDivider(customInner);
	Ui::AddSkip(customInner);

	auto startLabel = data->value(
	) | rpl::map([=](const Data::AwaySchedule &value) {
		return langDateTime(
			base::unixtime::parse(value.customInterval.start));
	});
	AddButtonWithLabel(
		customInner,
		tr::lng_away_custom_start(),
		std::move(startLabel),
		st::settingsButtonNoIcon
	)->setClickedCallback([=] {
		chooseDate(
			tr::lng_away_custom_start(),
			data->current().customInterval.start,
			StartTimeMin,
			[=] { return data->current().customInterval.end - 1; },
			[=](TimeId time) {
				auto copy = data->current();
				copy.customInterval.start = time;
				*data = copy;
			});
	});

	auto endLabel = data->value(
	) | rpl::map([=](const Data::AwaySchedule &value) {
		return langDateTime(
			base::unixtime::parse(value.customInterval.end));
	});
	AddButtonWithLabel(
		customInner,
		tr::lng_away_custom_end(),
		std::move(endLabel),
		st::settingsButtonNoIcon
	)->setClickedCallback([=] {
		chooseDate(
			tr::lng_away_custom_end(),
			data->current().customInterval.end,
			[=] { return data->current().customInterval.start + 1; },
			nullptr,
			[=](TimeId time) {
				auto copy = data->current();
				copy.customInterval.end = time;
				*data = copy;
			});
	});
}

AwayMessage::AwayMessage(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: BusinessSection(parent, controller) {
	setupContent(controller);
}

AwayMessage::~AwayMessage() {
	if (!Core::Quitting()) {
		save();
	}
}

rpl::producer<QString> AwayMessage::title() {
	return tr::lng_away_title();
}

void AwayMessage::setupContent(
		not_null<Window::SessionController*> controller) {
	using namespace Data;
	using namespace rpl::mappers;

	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	const auto info = &controller->session().data().businessInfo();
	const auto current = info->awaySettings();
	const auto disabled = (current.schedule.type == AwayScheduleType::Never);

	_recipients = current.recipients;
	auto initialSchedule = disabled ? AwaySchedule{
		.type = AwayScheduleType::Always,
	} : current.schedule;
	if (BadCustomInterval(initialSchedule.customInterval)) {
		const auto now = base::unixtime::now();
		initialSchedule.customInterval = WorkingInterval{
			.start = now,
			.end = now + 24 * 60 * 60,
		};
	}
	_schedule = initialSchedule;

	AddDividerTextWithLottie(content, {
		.lottie = u"sleep"_q,
		.lottieSize = st::settingsCloudPasswordIconSize,
		.lottieMargins = st::peerAppearanceIconPadding,
		.showFinished = showFinishes(),
		.about = tr::lng_away_about(Ui::Text::WithEntities),
		.aboutMargins = st::peerAppearanceCoverLabelMargin,
	});

	Ui::AddSkip(content);
	const auto enabled = content->add(object_ptr<Ui::SettingsButton>(
		content,
		tr::lng_away_enable(),
		st::settingsButtonNoIcon
	))->toggleOn(rpl::single(!disabled));
	_enabled = enabled->toggledValue();

	const auto wrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	const auto inner = wrap->entity();

	Ui::AddSkip(inner);
	Ui::AddDivider(inner);

	const auto createWrap = inner->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			inner,
			object_ptr<Ui::VerticalLayout>(inner)));
	const auto createInner = createWrap->entity();
	Ui::AddSkip(createInner);
	const auto create = createInner->add(object_ptr<Ui::SettingsButton>(
		createInner,
		tr::lng_away_create(),
		st::settingsButtonLightNoIcon
	));
	create->setClickedCallback([=] {
		const auto owner = &controller->session().data();
		const auto id = owner->shortcutMessages().emplaceShortcut("away");
		showOther(ShortcutMessagesId(id));
	});
	Ui::AddSkip(createInner);
	Ui::AddDivider(createInner);

	createWrap->toggleOn(rpl::single(true));

	Ui::AddSkip(inner);
	AddAwayScheduleSelector(inner, {
		.controller = controller,
		.data = &_schedule,
	});
	Ui::AddSkip(inner);
	Ui::AddDivider(inner);

	AddBusinessRecipientsSelector(inner, {
		.controller = controller,
		.title = tr::lng_away_recipients(),
		.data = &_recipients,
	});

	Ui::AddSkip(inner, st::settingsChatbotsAccessSkip);

	wrap->toggleOn(enabled->toggledValue());
	wrap->finishAnimating();

	Ui::ResizeFitChild(this, content);
}

void AwayMessage::save() {
	controller()->session().data().businessInfo().saveAwaySettings(
		_enabled.current() ? Data::AwaySettings{
			.recipients = _recipients.current(),
			.schedule = _schedule.current(),
		} : Data::AwaySettings());
}

} // namespace

Type AwayMessageId() {
	return AwayMessage::Id();
}

} // namespace Settings
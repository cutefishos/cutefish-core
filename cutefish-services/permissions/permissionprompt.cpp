#include "permissionprompt.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "base/application_registry.h"
#include "base/desktop_entry.h"
#include "base/file.h"
#include "base/i18n.h"
#include "ui/application.h"
#include "ui/cell_layout.h"
#include "ui/check_box.h"
#include "ui/dialog_window.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/scroll_view.h"
#include "ui/theme.h"
#include "ui/window.h"

using namespace cutefish;
using namespace cutefish::ui;
namespace dbus = cutefish::base::dbus;
using cutefish::i18n::format;
using cutefish::i18n::tr;

static constexpr double kWidth = 440;
static constexpr double kMargin = 20;
// A drag this far from the press moves the window.
static constexpr double kDragDistance = 10;

static std::string stringOf(const dbus::VariantMap &map, const std::string &key)
{
    const auto it = map.find(key);
    return it == map.end() ? std::string() : it->second.value<std::string>();
}

static int64_t integerOf(const dbus::VariantMap &map, const std::string &key)
{
    const auto it = map.find(key);
    if (it == map.end())
        return 0;
    const dbus::Variant &value = it->second;
    if (const auto n = value.get<int32_t>())
        return *n;
    if (const auto n = value.get<uint32_t>())
        return *n;
    if (const auto n = value.get<int64_t>())
        return *n;
    if (const auto n = value.get<uint64_t>())
        return int64_t(*n);
    if (const auto n = value.get<double>())
        return int64_t(*n);
    return 0;
}

// The dictionaries of an `aa{sv}` option; nothing for any other shape.
static std::vector<dbus::VariantMap> listOf(const dbus::VariantMap &map, const std::string &key)
{
    const auto it = map.find(key);
    if (it == map.end())
        return {};
    return it->second.value<std::vector<dbus::VariantMap>>();
}

namespace {

// The row's plate: a rounded rect filling the view, unsnapped.
class RowBackground final : public View
{
public:
    std::optional<ColorRecipe> color;

protected:
    void paint(Painter &painter) override
    {
        if (color)
            painter.drawSmoothRectangle(localRect(), Theme::current().mediumRadius, color->resolve());
    }
};

// One display or window to share, 52 high.
class ChoiceRow final : public View
{
public:
    ChoiceRow(const std::string &icon, const std::string &label, const std::string &detail)
    {
        m_background = addChild<RowBackground>();
        m_row = addChild<RowLayout>();
        m_row->setSpacing(12);
        m_row->setPadding({12, 0, 12, 0});
        auto *iconView = m_row->addChild<ThemeIcon>(icon, 28);
        iconView->setLayoutHints({.preferredWidth = 28, .preferredHeight = 28});
        auto *texts = m_row->addChild<ColumnLayout>();
        texts->setSpacing(1);
        texts->setLayoutHints({.fillWidth = true});
        m_label = texts->addChild<Label>(label);
        m_label->setLayoutHints({.fillWidth = true});
        m_label->setElide(Label::Elide::Right);
        m_detail = texts->addChild<Label>(detail);
        m_detail->setLayoutHints({.fillWidth = true});
        m_detail->setElide(Label::Elide::Right);
        m_detail->setFontSize(11);
        m_detail->setOpacity(0.8);
        m_detail->setVisible(!detail.empty());
        update();
    }

    void setSelected(bool selected)
    {
        m_selected = selected;
        update();
    }

    bool acceptsPointer() const override { return true; }

    std::function<void()> onClicked;
    std::function<void()> onDoubleClicked;

protected:
    void layout() override
    {
        m_background->setGeometry(localRect());
        m_row->setGeometry(localRect());
    }

    void pointerEnter() override
    {
        m_hovered = true;
        update();
    }

    void pointerLeave() override
    {
        m_hovered = false;
        update();
    }

    void pointerPress(const PointerEvent &event) override
    {
        if (event.button != MouseButton::Left)
            return;
        m_pressed = true;
        if (event.clickCount == 2 && onDoubleClicked)
            m_doubleClicked = true;
    }

    void pointerRelease(const PointerEvent &event) override
    {
        if (!m_pressed)
            return;
        m_pressed = false;
        if (!localRect().contains(event.position)) {
            m_doubleClicked = false;
            return;
        }
        if (onClicked)
            onClicked();
        if (std::exchange(m_doubleClicked, false) && onDoubleClicked)
            onDoubleClicked();
    }

private:
    void update()
    {
        m_background->color = m_selected ? std::optional<ColorRecipe>(ColorId::Highlight)
                : m_hovered               ? std::optional<ColorRecipe>(withAlpha(ColorId::Text, 0.1))
                                          : std::nullopt;
        m_background->requestPaint();
        m_label->setColor(m_selected ? ColorId::HighlightedText : ColorId::Text);
        m_detail->setColor(m_selected ? ColorId::HighlightedText : ColorId::DisabledText);
    }

    RowBackground *m_background;
    RowLayout *m_row;
    Label *m_label;
    Label *m_detail;
    bool m_selected = false;
    bool m_hovered = false;
    bool m_pressed = false;
    bool m_doubleClicked = false;
};

// A section's heading: 24 high, the text 12 in and centred on the height.
class SectionHeading final : public View
{
public:
    explicit SectionHeading(const std::string &text)
        : m_label(addChild<Label>(text))
    {
        m_label->setFontSize(11);
        m_label->setColor(ColorId::DisabledText);
        m_label->setVerticallyCentered(true);
    }

    void setText(const std::string &text) { m_label->setText(text); }

protected:
    void layout() override
    {
        m_label->setGeometry({12, 0, std::max(0.0, geometry().width - 12), geometry().height});
    }

private:
    Label *m_label;
};

// The rows in sections: a section's heading sits above its
// first row, and rows keep 4 apart.
class ChoiceListContent final : public View
{
public:
    static constexpr double kRowHeight = 52;
    static constexpr double kSectionHeight = 24;
    static constexpr double kSpacing = 4;

    struct Entry {
        SectionHeading *section = nullptr;
        ChoiceRow *row = nullptr;
    };

    std::vector<Entry> entries;

    double contentHeight() const
    {
        double height = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i > 0)
                height += kSpacing;
            height += (entries[i].section ? kSectionHeight : 0) + kRowHeight;
        }
        return height;
    }

    SizeF sizeHint() const override { return {0, contentHeight()}; }
    double heightForWidth(double) const override { return contentHeight(); }

protected:
    void layout() override
    {
        const double width = std::max(0.0, geometry().width - 2 * kMargin);
        double y = 0;
        for (const Entry &entry : entries) {
            if (entry.section) {
                entry.section->setGeometry({kMargin, y, width, kSectionHeight});
                y += kSectionHeight;
            }
            entry.row->setGeometry({kMargin, y, width, kRowHeight});
            y += kRowHeight + kSpacing;
        }
    }
};

// The screen-recording kind. The request carries `monitors` and, when the
// application may share windows, `windows`; the answer names the chosen one
// under the matching key.
class ScreenRecordingContent final : public PermissionPrompt::Content
{
public:
    ScreenRecordingContent(const PermissionPrompt::Application &application, const dbus::VariantMap &request)
        : m_application(application)
    {
        for (const dbus::VariantMap &monitor : listOf(request, "monitors")) {
            Choice choice;
            choice.window = false;
            choice.name = stringOf(monitor, "name");
            const std::string description = stringOf(monitor, "description");
            choice.label = !description.empty() ? description : choice.name;
            if (!choice.name.empty())
                choice.detail = std::format("{}  ·  {} × {}", choice.name, integerOf(monitor, "width"),
                                            integerOf(monitor, "height"));
            choice.icon = "video-display";
            m_choices.push_back(choice);
        }
        for (const dbus::VariantMap &window : listOf(request, "windows")) {
            Choice choice;
            choice.window = true;
            choice.name = stringOf(window, "identifier");
            const std::string title = stringOf(window, "title");
            const std::string appId = stringOf(window, "app_id");
            choice.label = !title.empty() ? title : appId;
            choice.detail = appId;
            const std::string icon = PermissionPrompt::describeApplication(appId).icon;
            choice.icon = !icon.empty() ? icon : std::string("window");
            m_choices.push_back(choice);
        }
        m_hasMonitors = std::any_of(m_choices.begin(), m_choices.end(), [](const Choice &c) { return !c.window; });
        m_hasWindows = std::any_of(m_choices.begin(), m_choices.end(), [](const Choice &c) { return c.window; });

        m_scroll = addChild<ScrollView>();
        m_list = m_scroll->setContent<ChoiceListContent>();
        m_list->setLayoutHints({.fillWidth = true});
        for (size_t i = 0; i < m_choices.size(); ++i) {
            const Choice &choice = m_choices[i];
            ChoiceListContent::Entry entry;
            if (i == 0 || m_choices[i - 1].window != choice.window)
                entry.section = m_list->addChild<SectionHeading>(sectionTitle(choice.window));
            entry.row = m_list->addChild<ChoiceRow>(choice.icon, choice.label, choice.detail);
            entry.row->onClicked = [this, i] {
                select(int(i));
            };
            entry.row->onDoubleClicked = [this] {
                if (onAccepted)
                    onAccepted();
            };
            m_list->entries.push_back(entry);
        }
        select(0);
    }

    std::string title() const override
    {
        return format(tr("Share your screen with “{name}”"), {{"name", m_application.name}});
    }

    std::string description() const override
    {
        if (m_hasMonitors && m_hasWindows)
            return tr("Choose a display or a window. What is shown on it will be visible to the application until it stops sharing.");
        if (m_hasWindows)
            return tr("Choose a window. Its contents will be visible to the application until it stops sharing.");
        return tr("Choose a display. Everything shown on it, including other windows, will be visible to the application until it stops sharing.");
    }

    std::string accept() const override { return tr("Share"); }

    bool ready() const override { return m_current >= 0 && m_current < int(m_choices.size()); }

    dbus::VariantMap details() const override
    {
        const Choice &choice = m_choices[size_t(m_current)];
        return {{choice.window ? "windows" : "monitors", dbus::Variant(std::vector<std::string> {choice.name})}};
    }

    void retranslate() override
    {
        for (size_t i = 0; i < m_list->entries.size(); ++i) {
            if (m_list->entries[i].section)
                m_list->entries[i].section->setText(sectionTitle(m_choices[i].window));
        }
    }

    void select(int index)
    {
        m_current = index;
        for (size_t i = 0; i < m_list->entries.size(); ++i)
            m_list->entries[i].row->setSelected(int(i) == index);
    }

    SizeF sizeHint() const override { return {0, height()}; }
    double heightForWidth(double) const override { return height(); }

protected:
    void layout() override
    {
        // The scroll area spans the window, past the prompt's padding.
        m_scroll->setGeometry({-kMargin, 0, geometry().width + 2 * kMargin, geometry().height});
    }

private:
    struct Choice {
        bool window = false;
        std::string name;
        std::string label;
        std::string detail;
        std::string icon;
    };

    static std::string sectionTitle(bool window) { return window ? tr("Windows") : tr("Displays"); }

    double height() const
    {
        return std::min(m_list->contentHeight(), 5 * ChoiceListContent::kRowHeight + 4 * ChoiceListContent::kSpacing
                                                         + 2 * ChoiceListContent::kSectionHeight);
    }

    PermissionPrompt::Application m_application;
    std::vector<Choice> m_choices;
    bool m_hasMonitors = false;
    bool m_hasWindows = false;
    ScrollView *m_scroll;
    ChoiceListContent *m_list;
    int m_current = -1;
};

// A check box that tells how far its box sits inside it.
class InsetCheckBox final : public CheckBox
{
public:
    using CheckBox::CheckBox;
    using CheckBox::padding;
};

// The computer-control kind. The request carries `agent_name`, the agent's own
// name for itself; the compositor asks, so there is no app id to show. With
// `rememberable` the user may allow the agent's application from now on, and
// the answer says so in `remember`.
class ComputerControlContent final : public PermissionPrompt::Content
{
public:
    explicit ComputerControlContent(const dbus::VariantMap &request)
    {
        m_agentName = stringOf(request, "agent_name");

        const auto rememberable = request.find("rememberable");
        if (rememberable == request.end() || !rememberable->second.value<bool>()) {
            setVisible(false);
            return;
        }
        m_remember = addChild<InsetCheckBox>(tr("Always allow this app"));
    }

    std::string title() const override
    {
        if (m_agentName.empty())
            return tr("Let “An app” control this computer?");
        return format(tr("Let “{name}” control this computer?"), {{"name", m_agentName}});
    }

    std::string description() const override
    {
        return tr("It will see your screen and use the pointer and keyboard until you stop it with the Stop button in the status bar. Using the pointer or keyboard yourself pauses it, and it cannot answer system dialogs like this one.");
    }

    std::string accept() const override { return tr("Allow"); }
    bool ready() const override { return true; }

    dbus::VariantMap details() const override
    {
        if (!m_remember)
            return {};
        return {{"remember", dbus::Variant(m_remember->isChecked())}};
    }

    void retranslate() override
    {
        if (m_remember)
            m_remember->setText(tr("Always allow this app"));
    }

    SizeF sizeHint() const override { return m_remember ? m_remember->sizeHint() : SizeF(); }
    double heightForWidth(double) const override { return sizeHint().height; }

protected:
    // The box lines up with the header's text, past its icon.
    void layout() override
    {
        if (!m_remember)
            return;
        const double x = kTextInset - m_remember->padding().left;
        m_remember->setGeometry({x, 0, std::max(0.0, geometry().width - x), geometry().height});
    }

private:
    static constexpr double kTextInset = 48 + 14;

    std::string m_agentName;
    InsetCheckBox *m_remember = nullptr;
};

// The window's view: the layout, and any empty part of it drags the window,
// as there is no title bar.
class PromptLayout final : public ColumnLayout
{
public:
    bool acceptsPointer() const override { return true; }

protected:
    void pointerPress(const PointerEvent &event) override
    {
        m_pressed = event.button == MouseButton::Left;
        m_pressPosition = event.position;
    }

    void pointerMove(const PointerEvent &event) override
    {
        if (!m_pressed || !window())
            return;
        if (std::abs(event.position.x - m_pressPosition.x) > kDragDistance
            || std::abs(event.position.y - m_pressPosition.y) > kDragDistance) {
            m_pressed = false;
            window()->startMove();
        }
    }

    void pointerRelease(const PointerEvent &) override { m_pressed = false; }

private:
    bool m_pressed = false;
    PointF m_pressPosition;
};

struct Kind {
    const char *name;
    const char *icon;
};

const Kind kinds[] = {
        {"screen-recording", "video-display"},
        {"computer-control", "computer"},
};

const Kind *findKind(const std::string &name)
{
    for (const Kind &kind : kinds) {
        if (name == kind.name)
            return &kind;
    }
    return nullptr;
}

} // namespace

bool PermissionPrompt::supports(const std::string &kind)
{
    return findKind(kind) != nullptr;
}

// The application shown to the user comes from the caller's app id; an app
// without a desktop file is still named, just by its id.
PermissionPrompt::Application PermissionPrompt::describeApplication(const std::string &appId)
{
    Application result {appId, appId, {}};
    if (appId.empty())
        return result;
    for (const std::string &directory : base::ApplicationRegistry::desktopPaths()) {
        const std::string path = directory + '/' + appId + ".desktop";
        const auto contents = base::file::read(path);
        if (!contents)
            continue;
        const auto entry = base::DesktopEntry::parse(appId, path, *contents);
        if (!entry)
            return result;
        if (!entry->name.empty())
            result.name = entry->name;
        result.icon = entry->icon;
        return result;
    }
    return result;
}

PermissionPrompt::PermissionPrompt(const std::string &kind, const std::string &appId,
                                   const std::string &parentWindow, const dbus::VariantMap &options,
                                   dbus::MethodCall call)
    : m_call(std::move(call))
    , m_parentWindow(parentWindow)
    , m_application(describeApplication(appId))
{
    if (const Kind *entry = findKind(kind))
        m_kindIcon = entry->icon;
    if (kind == "screen-recording")
        m_ownedContent = std::make_unique<ScreenRecordingContent>(m_application, options);
    else
        m_ownedContent = std::make_unique<ComputerControlContent>(options);
    m_content = m_ownedContent.get();
    m_content->onAccepted = [this] {
        allow();
    };
}

PermissionPrompt::~PermissionPrompt()
{
    reply(2, {});
}

void PermissionPrompt::watchCaller(dbus::Connection *bus)
{
    if (!bus)
        return;
    m_callerWatch = bus->watchName(caller(), [this](const std::string &owner) {
        if (owner.empty())
            dismiss();
    });
}

void PermissionPrompt::show()
{
    m_window = std::make_unique<Window>();
    m_window->setTitle(m_content->title());
    m_window->setSkipTaskbar(true);
    // Text lays out unhinted at every scale, so lines wrap where they always did.
    m_window->setHintedTextLayout(false);
    m_window->setBackgroundColor(ColorId::SecondBackground);
    m_window->onCloseRequested = [this] {
        deny();
    };
    if (!m_parentWindow.empty())
        m_window->setTransientForForeign(m_parentWindow, true);

    auto layout = std::make_unique<PromptLayout>();
    layout->setPadding(Margins::all(kMargin));
    layout->setSpacing(16);

    auto *header = layout->addChild<RowLayout>();
    header->setSpacing(14);
    header->setLayoutHints({.fillWidth = true});
    auto *icon = header->addChild<ThemeIcon>(!m_application.icon.empty() ? m_application.icon : m_kindIcon, 48);
    icon->setLayoutHints({.preferredWidth = 48, .preferredHeight = 48, .verticalAlignment = Alignment::Start});
    auto *texts = header->addChild<ColumnLayout>();
    texts->setSpacing(4);
    texts->setLayoutHints({.fillWidth = true});
    m_title = texts->addChild<Label>(m_content->title());
    m_title->setLayoutHints({.fillWidth = true});
    m_title->setWrap(true);
    m_title->setFontSize(14);
    m_title->setWeight(600);
    m_description = texts->addChild<Label>(m_content->description());
    m_description->setLayoutHints({.fillWidth = true});
    m_description->setWrap(true);
    m_description->setFontSize(12);
    m_description->setColor(ColorId::DisabledText);

    m_ownedContent->setLayoutHints({.fillWidth = true});
    layout->addChild(std::move(m_ownedContent));

    auto *buttons = layout->addChild<RowLayout>();
    buttons->setSpacing(8);
    buttons->setLayoutHints({.horizontalAlignment = Alignment::End});
    m_cancel = buttons->addChild<DialogButton>(tr("Cancel"));
    m_cancel->onClicked = [this] {
        deny();
    };
    m_accept = buttons->addChild<DialogButton>(m_content->accept(), true);
    m_accept->setEnabled(m_content->ready());
    m_accept->onClicked = [this] {
        allow();
    };

    m_window->addShortcut(NoModifier, key::Escape, [this] {
        deny();
    });
    m_window->addShortcut(NoModifier, key::Return, [this] {
        allow();
    });

    m_window->setContentView(std::move(layout));
    m_window->resize({kWidth, 0});
    m_window->setFitsContentHeight(true);
    m_accept->setFocus();
    m_window->show();
    m_window->activate();

    m_languageChanged = cutefish::ui::Application::instance()->languageChanged.connect([this] {
        retranslate();
    });
}

void PermissionPrompt::retranslate()
{
    m_window->setTitle(m_content->title());
    m_title->setText(m_content->title());
    m_description->setText(m_content->description());
    m_cancel->setText(tr("Cancel"));
    m_accept->setText(m_content->accept());
    m_content->retranslate();
}

void PermissionPrompt::dismiss()
{
    reply(1, {});
}

void PermissionPrompt::allow()
{
    if (!m_content->ready())
        return;
    reply(0, m_content->details());
}

void PermissionPrompt::deny()
{
    reply(1, {});
}

void PermissionPrompt::reply(uint32_t response, const dbus::VariantMap &results)
{
    if (m_finished)
        return;
    m_finished = true;
    m_call.reply(response, results);
    if (m_window)
        m_window->hide();
    if (onFinished)
        onFinished();
}

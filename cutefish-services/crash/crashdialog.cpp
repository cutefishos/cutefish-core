// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "crashdialog.h"

#include <cmath>

#include "base/dbus.h"
#include "base/i18n.h"
#include "ui/application.h"
#include "ui/cell_layout.h"
#include "ui/dialog_window.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/scroll_view.h"
#include "ui/text_area.h"
#include "ui/theme.h"
#include "ui/window.h"

using namespace cutefish;
using namespace cutefish::ui;
using cutefish::i18n::format;
using cutefish::i18n::tr;

// Wide enough for the report, whose lines do not wrap.
static constexpr double kWidth = 520;
static constexpr double kMargin = 16;
// A drag this far from the press moves the window.
static constexpr double kDragDistance = 10;

// The window's view: the layout, and any empty part of it drags the window,
// as there is no title bar.
class CrashDialog::Content final : public ColumnLayout
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

// The rounded plate behind the report, unsnapped.
class ReportFrame final : public View
{
public:
    explicit ReportFrame(std::unique_ptr<View> content)
        : m_content(content.get())
    {
        addChild(std::move(content));
    }

protected:
    void layout() override
    {
        // The text sits 8 in from the frame's edges.
        const RectF bounds = localRect();
        m_content->setGeometry({8, 8, std::max(0.0, bounds.width - 16), std::max(0.0, bounds.height - 16)});
    }

    void paint(Painter &painter) override
    {
        const Theme &theme = Theme::current();
        painter.drawSmoothRectangle(localRect(), theme.smallRadius, theme.alternateBackground);
    }

private:
    View *m_content;
};

CrashDialog::CrashDialog(Kind kind, const std::string &id, std::function<std::string()> name,
                         const std::string &icon, const std::string &fixedFont)
    : m_kind(kind)
    , m_id(id)
    , m_nameLookup(std::move(name))
    , m_name(m_nameLookup())
    , m_icon(icon)
    , m_fixedFont(fixedFont)
{
}

CrashDialog::~CrashDialog() = default;

std::string CrashDialog::reason() const
{
    return m_hasDump ? m_dump.signalDescription() : std::string();
}

std::string CrashDialog::report() const
{
    return m_hasDump && !m_dump.threads.empty() ? m_dump.report(m_name) : std::string();
}

void CrashDialog::setDump(const Coredump &dump)
{
    m_dump = dump;
    m_hasDump = true;
    update();
}

void CrashDialog::show()
{
    m_window = std::make_unique<Window>();
    m_window->setSkipTaskbar(true);
    // Text lays out unhinted at every scale, so lines wrap where they always did.
    m_window->setHintedTextLayout(false);
    m_window->setBackgroundColor(ColorId::SecondBackground);
    m_window->onCloseRequested = [this] {
        close();
    };
    m_window->addShortcut(NoModifier, key::Escape, [this] {
        close();
    });
    m_window->addShortcut(NoModifier, key::Return, [this] {
        accept();
    });

    buildContent();
    m_window->resize({kWidth, 0});
    m_window->setFitsContentHeight(true);
    m_window->show();
    m_window->activate();

    m_languageChanged = ui::Application::instance()->languageChanged.connect([this] {
        retranslate();
    });
}

// The views are made again.
void CrashDialog::retranslate()
{
    if (m_finished)
        return;
    m_name = m_nameLookup();
    buildContent();
    m_window->setFitsContentHeight(true);
}

void CrashDialog::buildContent()
{
    const bool canReopen = !sessionProcess();
    const std::string title = format(tr("“{name}” quit unexpectedly"), {{"name", m_name}});
    m_window->setTitle(title);

    auto content = std::make_unique<Content>();
    content->setPadding(Margins::all(kMargin));
    content->setSpacing(12);

    auto *header = content->addChild<RowLayout>();
    header->setSpacing(14);
    header->setLayoutHints({.fillWidth = true});
    auto *icon = header->addChild<ThemeIcon>(m_icon, 48);
    icon->setLayoutHints({.preferredWidth = 48, .preferredHeight = 48, .verticalAlignment = Alignment::Start});
    auto *texts = header->addChild<ColumnLayout>();
    texts->setSpacing(4);
    // At the top whether the text is shorter than the icon or not.
    texts->setLayoutHints({.fillWidth = true, .verticalAlignment = Alignment::Start});
    auto *titleLabel = texts->addChild<Label>(title);
    titleLabel->setLayoutHints({.fillWidth = true});
    titleLabel->setWrap(true);
    titleLabel->setFontSize(14);
    titleLabel->setWeight(600);
    m_text = texts->addChild<Label>();
    m_text->setLayoutHints({.fillWidth = true});
    m_text->setWrap(true);
    m_text->setFontSize(12);
    m_text->setColor(ColorId::DisabledText);

    auto scroll = std::make_unique<ScrollView>();
    scroll->setScrollDirections(true, true);
    m_report = scroll->setContent<TextArea>();
    m_report->setReadOnly(true);
    m_report->setWrapMode(TextArea::WrapMode::NoWrap);
    m_report->setFrameVisible(false);
    m_report->setPadding({});
    m_report->setFont(m_fixedFont, 11, 400);
    m_report->setLayoutHints({.fillWidth = true});
    m_reportFrame = content->addChild<ReportFrame>(std::move(scroll));
    m_reportFrame->setLayoutHints({.fillWidth = true, .preferredHeight = 220});

    auto *buttons = content->addChild<RowLayout>();
    buttons->setSpacing(8);
    buttons->setLayoutHints({.fillWidth = true});
    auto *spacer = buttons->addChild<View>();
    spacer->setLayoutHints({.fillWidth = true});
    auto *closeButton = buttons->addChild<DialogButton>(tr("Close"));
    closeButton->setVisible(canReopen);
    closeButton->onClicked = [this] {
        close();
    };
    auto *acceptButton = buttons->addChild<DialogButton>(canReopen ? tr("Reopen") : tr("OK"), true);
    acceptButton->onClicked = [this] {
        accept();
    };

    m_window->setContentView(std::move(content));
    update();
    acceptButton->setFocus();
}

void CrashDialog::update()
{
    if (!m_window)
        return;

    std::string text = reason().empty() ? std::string() : format(tr("Reason: {reason}"), {{"reason", reason()}});
    if (sessionProcess()) {
        const std::string state = tr("It is restarted automatically.");
        text = text.empty() ? state : state + "\n" + text;
    }
    m_text->setText(text);
    m_text->setVisible(!text.empty());

    const std::string details = report();
    if (m_report->text() != details)
        m_report->setText(details);
    m_reportFrame->setVisible(!details.empty());
}

void CrashDialog::accept()
{
    if (sessionProcess())
        close();
    else
        reopen();
}

void CrashDialog::reopen()
{
    if (m_kind != Kind::Application || m_finished) {
        close();
        return;
    }
    if (m_reopening)
        return;
    m_reopening = true;
    // A single-instance application hands the token to its running instance,
    // which can then be activated on another desk. The connection keeps the
    // callback, and the dialog may be closed and gone before the token comes.
    const std::weak_ptr<bool> alive = m_alive;
    m_window->requestActivationToken([this, id = m_id, alive](const std::string &token) {
        if (base::dbus::Connection *bus = base::dbus::Connection::session()) {
            const base::dbus::Proxy runtime(bus, "org.cutefish.AppRuntime", "/AppRuntime",
                                            "org.cutefish.AppRuntime");
            runtime.send("launchApplication", id, std::vector<std::string>(), token);
        }
        if (alive.lock())
            close();
    });
}

void CrashDialog::close()
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_window)
        m_window->hide();
    if (onFinished)
        onFinished();
}

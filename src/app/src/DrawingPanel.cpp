// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/DrawingPanel.h>

#include <lcad/drawing/Sheet.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace lcad::app {

using document::Drawing;
using document::DrawingDimension;
using document::DrawingStandard;
using document::DrawingView;
using document::SheetSize;
using document::ViewKind;

namespace {

constexpr std::array<SheetSize, 9> sheetSizes{
    SheetSize::A4,     SheetSize::A3,      SheetSize::A2,    SheetSize::A1,   SheetSize::A0,
    SheetSize::Letter, SheetSize::Tabloid, SheetSize::AnsiC, SheetSize::AnsiD};

constexpr std::array<double, 11> scales{10.0, 5.0, 2.0, 1.0,  0.5,  0.2,
                                        0.1,  0.05, 0.02, 0.01, 0.005};

QString viewKindName(ViewKind kind) {
    switch (kind) {
    case ViewKind::Front:
        return DrawingPanel::tr("Front view");
    case ViewKind::Back:
        return DrawingPanel::tr("Back view");
    case ViewKind::Top:
        return DrawingPanel::tr("Top view");
    case ViewKind::Bottom:
        return DrawingPanel::tr("Bottom view");
    case ViewKind::Left:
        return DrawingPanel::tr("Left view");
    case ViewKind::Right:
        return DrawingPanel::tr("Right view");
    case ViewKind::Isometric:
        return DrawingPanel::tr("Isometric view");
    case ViewKind::Section:
        return DrawingPanel::tr("Section");
    case ViewKind::Detail:
        return DrawingPanel::tr("Detail");
    }
    return {};
}

} // namespace

DrawingPanel::DrawingPanel(QWidget* parent)
    : QWidget(parent), m_prompt(new QLabel(this)), m_drawingBox(new QComboBox(this)),
      m_name(new QLineEdit(this)), m_sizeBox(new QComboBox(this)),
      m_standardBox(new QComboBox(this)), m_title(new QLineEdit(this)),
      m_partNumber(new QLineEdit(this)), m_revision(new QLineEdit(this)),
      m_drawnBy(new QLineEdit(this)), m_date(new QLineEdit(this)), m_material(new QLineEdit(this)),
      m_company(new QLineEdit(this)), m_viewBox(new QGroupBox(tr("View"), this)),
      m_viewKind(new QLabel(this)), m_scale(new QComboBox(this)), m_hidden(new QCheckBox(this)),
      m_centerMarks(new QCheckBox(this)), m_flipped(new QCheckBox(this)),
      m_dimensionBox(new QGroupBox(tr("Dimension"), this)), m_suffix(new QLineEdit(this)),
      m_offset(new QDoubleSpinBox(this)), m_problems(new QLabel(this)) {
    setObjectName(QStringLiteral("drawingPanel"));
    auto* layout = new QVBoxLayout(this);

    m_prompt->setObjectName(QStringLiteral("sketchPrompt"));
    m_prompt->setWordWrap(true);
    m_prompt->hide();
    layout->addWidget(m_prompt);

    m_drawingBox->setObjectName(QStringLiteral("drawingBox"));
    m_drawingBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_drawingBox->setMinimumContentsLength(8);
    m_sizeBox->setObjectName(QStringLiteral("sheetSize"));
    m_sizeBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_sizeBox->setMinimumContentsLength(8);
    for (const SheetSize size : sheetSizes) {
        m_sizeBox->addItem(QString::fromLatin1(toString(size)), static_cast<int>(size));
    }
    m_standardBox->setObjectName(QStringLiteral("drawingStandard"));
    m_standardBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_standardBox->setMinimumContentsLength(8);
    m_standardBox->addItem(tr("ISO, first-angle"));
    m_standardBox->addItem(tr("ANSI, third-angle"));

    auto* form = new QFormLayout;
    form->addRow(tr("Drawing"), m_drawingBox);
    form->addRow(tr("Name"), m_name);
    form->addRow(tr("Sheet"), m_sizeBox);
    form->addRow(tr("Standard"), m_standardBox);
    layout->addLayout(form);

    auto* titleBox = new QGroupBox(tr("Title block"), this);
    auto* titleForm = new QFormLayout(titleBox);
    m_title->setObjectName(QStringLiteral("titleField"));
    m_partNumber->setObjectName(QStringLiteral("partNumberField"));
    titleForm->addRow(tr("Title"), m_title);
    titleForm->addRow(tr("Part no."), m_partNumber);
    titleForm->addRow(tr("Revision"), m_revision);
    titleForm->addRow(tr("Drawn by"), m_drawnBy);
    titleForm->addRow(tr("Date"), m_date);
    titleForm->addRow(tr("Material"), m_material);
    titleForm->addRow(tr("Company"), m_company);
    layout->addWidget(titleBox);

    auto* viewForm = new QFormLayout(m_viewBox);
    m_viewKind->setObjectName(QStringLiteral("secondary"));
    viewForm->addRow(m_viewKind);
    m_scale->setObjectName(QStringLiteral("viewScale"));
    m_scale->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_scale->setMinimumContentsLength(5);
    for (const double scale : scales) {
        m_scale->addItem(QString::fromStdString(drawing::scaleText(scale)), scale);
    }
    viewForm->addRow(tr("Scale"), m_scale);
    m_hidden->setText(tr("Show hidden lines"));
    m_hidden->setObjectName(QStringLiteral("viewHidden"));
    viewForm->addRow(m_hidden);
    m_centerMarks->setText(tr("Center marks"));
    viewForm->addRow(m_centerMarks);
    m_flipped->setText(tr("Look from the other side"));
    viewForm->addRow(m_flipped);
    layout->addWidget(m_viewBox);

    auto* dimensionForm = new QFormLayout(m_dimensionBox);
    m_suffix->setObjectName(QStringLiteral("dimensionSuffix"));
    m_suffix->setPlaceholderText(tr("± 0.1"));
    dimensionForm->addRow(tr("After value"), m_suffix);
    m_offset->setObjectName(QStringLiteral("dimensionOffset"));
    m_offset->setRange(-500.0, 500.0);
    m_offset->setSuffix(tr(" mm"));
    dimensionForm->addRow(tr("Stands off"), m_offset);
    layout->addWidget(m_dimensionBox);

    m_problems->setObjectName(QStringLiteral("errorText"));
    m_problems->setWordWrap(true);
    m_problems->hide();
    layout->addWidget(m_problems);
    layout->addStretch(1);

    const auto text = [this](QLineEdit* field, std::string document::TitleBlock::*member,
                             const QString& label) {
        connect(field, &QLineEdit::editingFinished, this, [this, field, member, label] {
            if (m_filling || !m_hasDrawing ||
                m_drawing.titleBlock.*member == field->text().toStdString()) {
                return;
            }
            m_drawing.titleBlock.*member = field->text().toStdString();
            edit(label);
        });
    };
    text(m_title, &document::TitleBlock::title, tr("Change Title"));
    text(m_partNumber, &document::TitleBlock::partNumber, tr("Change Part Number"));
    text(m_revision, &document::TitleBlock::revision, tr("Change Revision"));
    text(m_drawnBy, &document::TitleBlock::drawnBy, tr("Change Drawn By"));
    text(m_date, &document::TitleBlock::date, tr("Change Date"));
    text(m_material, &document::TitleBlock::material, tr("Change Material"));
    text(m_company, &document::TitleBlock::company, tr("Change Company"));

    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        const std::string name = m_name->text().trimmed().toStdString();
        if (m_filling || !m_hasDrawing || name.empty() || name == m_drawing.name) {
            return;
        }
        m_drawing.name = name;
        edit(tr("Rename Drawing"));
    });
    connect(m_sizeBox, &QComboBox::activated, this, [this](int index) {
        if (m_hasDrawing) {
            m_drawing.size = sheetSizes[static_cast<std::size_t>(index)];
            edit(tr("Change Sheet Size"));
        }
    });
    connect(m_standardBox, &QComboBox::activated, this, [this](int index) {
        if (m_hasDrawing) {
            m_drawing.standard = index == 0 ? DrawingStandard::Iso : DrawingStandard::Ansi;
            edit(tr("Change Standard"));
        }
    });
    connect(m_drawingBox, &QComboBox::activated, this, [this](int index) {
        emit drawingChosen(document::ObjectId{m_drawingBox->itemData(index).toULongLong()});
    });

    const auto selectedView = [this]() -> DrawingView* {
        for (DrawingView& view : m_drawing.views) {
            if (view.id == m_selected.id) {
                return &view;
            }
        }
        return nullptr;
    };
    connect(m_scale, &QComboBox::activated, this, [this, selectedView](int index) {
        if (DrawingView* view = selectedView()) {
            view->scale = m_scale->itemData(index).toDouble();
            edit(tr("Change Scale"));
        }
    });
    connect(m_hidden, &QCheckBox::toggled, this, [this, selectedView](bool on) {
        DrawingView* view = selectedView();
        if (!m_filling && view != nullptr) {
            view->showHidden = on;
            edit(tr("Show Hidden Lines"));
        }
    });
    connect(m_centerMarks, &QCheckBox::toggled, this, [this, selectedView](bool on) {
        DrawingView* view = selectedView();
        if (!m_filling && view != nullptr) {
            view->centerMarks = on;
            edit(tr("Center Marks"));
        }
    });
    connect(m_flipped, &QCheckBox::toggled, this, [this, selectedView](bool on) {
        DrawingView* view = selectedView();
        if (!m_filling && view != nullptr) {
            view->sectionFlipped = on;
            edit(tr("Flip Section"));
        }
    });

    const auto selectedDimension = [this]() -> DrawingDimension* {
        for (DrawingDimension& dimension : m_drawing.dimensions) {
            if (dimension.id == m_selected.id) {
                return &dimension;
            }
        }
        return nullptr;
    };
    connect(m_suffix, &QLineEdit::editingFinished, this, [this, selectedDimension] {
        DrawingDimension* dimension = selectedDimension();
        if (m_filling || dimension == nullptr ||
            dimension->suffix == m_suffix->text().toStdString()) {
            return;
        }
        dimension->suffix = m_suffix->text().toStdString();
        edit(tr("Change Dimension"));
    });
    connect(m_offset, &QDoubleSpinBox::editingFinished, this, [this, selectedDimension] {
        DrawingDimension* dimension = selectedDimension();
        if (m_filling || dimension == nullptr || dimension->offset == m_offset->value()) {
            return;
        }
        dimension->offset = m_offset->value();
        edit(tr("Move Dimension"));
    });

    fill();
}

void DrawingPanel::edit(const QString& label) {
    emit changed(m_drawing, label);
}

void DrawingPanel::setDrawings(const std::vector<Drawing>& drawings, const Drawing* current) {
    m_filling = true;
    {
        const QSignalBlocker blocker(m_drawingBox);
        m_drawingBox->clear();
        for (const Drawing& drawing : drawings) {
            m_drawingBox->addItem(QString::fromStdString(drawing.name),
                                  QVariant::fromValue<qulonglong>(drawing.id.value));
            if (current != nullptr && drawing.id == current->id) {
                m_drawingBox->setCurrentIndex(m_drawingBox->count() - 1);
            }
        }
    }
    m_hasDrawing = current != nullptr;
    if (current != nullptr) {
        m_drawing = *current;
    }
    m_filling = false;
    fill();
}

void DrawingPanel::setSelected(SheetView::Selected selected) {
    m_selected = selected;
    fill();
}

void DrawingPanel::setPrompt(const QString& prompt) {
    m_promptText = prompt;
    m_prompt->setText(prompt);
    m_prompt->setVisible(!prompt.isEmpty());
}

void DrawingPanel::setProblems(const QStringList& problems) {
    m_problems->setText(problems.join(QLatin1Char('\n')));
    m_problems->setVisible(!problems.isEmpty());
}

QString DrawingPanel::problemsText() const {
    return m_problems->text();
}

void DrawingPanel::fill() {
    m_filling = true;
    const QSignalBlocker a(m_hidden);
    const QSignalBlocker b(m_centerMarks);
    const QSignalBlocker c(m_flipped);
    for (QWidget* field :
         std::initializer_list<QWidget*>{m_name, m_sizeBox, m_standardBox, m_title, m_partNumber,
                                         m_revision, m_drawnBy, m_date, m_material, m_company}) {
        field->setEnabled(m_hasDrawing);
    }
    if (m_hasDrawing) {
        m_name->setText(QString::fromStdString(m_drawing.name));
        const auto size = std::find(sheetSizes.begin(), sheetSizes.end(), m_drawing.size);
        m_sizeBox->setCurrentIndex(static_cast<int>(size - sheetSizes.begin()));
        m_standardBox->setCurrentIndex(m_drawing.standard == DrawingStandard::Iso ? 0 : 1);
        const document::TitleBlock& title = m_drawing.titleBlock;
        m_title->setText(QString::fromStdString(title.title));
        m_partNumber->setText(QString::fromStdString(title.partNumber));
        m_revision->setText(QString::fromStdString(title.revision));
        m_drawnBy->setText(QString::fromStdString(title.drawnBy));
        m_date->setText(QString::fromStdString(title.date));
        m_material->setText(QString::fromStdString(title.material));
        m_company->setText(QString::fromStdString(title.company));
    }

    const DrawingView* view = nullptr;
    const DrawingDimension* dimension = nullptr;
    if (m_selected.kind == SheetView::Selected::Kind::View) {
        for (const DrawingView& each : m_drawing.views) {
            if (each.id == m_selected.id) {
                view = &each;
            }
        }
    } else if (m_selected.kind == SheetView::Selected::Kind::Dimension) {
        for (const DrawingDimension& each : m_drawing.dimensions) {
            if (each.id == m_selected.id) {
                dimension = &each;
            }
        }
    }
    m_viewBox->setVisible(view != nullptr);
    if (view != nullptr) {
        m_viewKind->setText(viewKindName(view->kind));
        // A detail can have a scale off the list.
        if (m_scale->findData(view->scale) < 0) {
            m_scale->addItem(QString::fromStdString(drawing::scaleText(view->scale)), view->scale);
        }
        m_scale->setCurrentIndex(m_scale->findData(view->scale));
        m_hidden->setChecked(view->showHidden);
        m_centerMarks->setChecked(view->centerMarks);
        m_flipped->setChecked(view->sectionFlipped);
        m_flipped->setVisible(view->kind == ViewKind::Section);
    }
    m_dimensionBox->setVisible(dimension != nullptr);
    if (dimension != nullptr) {
        m_suffix->setText(QString::fromStdString(dimension->suffix));
        m_offset->setValue(dimension->offset);
    }
    m_filling = false;
}

} // namespace lcad::app

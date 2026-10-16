#include "app/light_panel.hpp"

#include <array>
#include <cmath>
#include <optional>

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTime>
#include <QTimeEdit>
#include <QVBoxLayout>

#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "core/solar.hpp"

namespace lb::app {
namespace {

enum Mode { kManual = 0, kLocation = 1 };

struct City {
    const char* name;
    double latitude;
    double longitude;
    float utcOffset; // standard time
};

// A starting point for common locations; any coordinates can be typed in.
constexpr std::array kCities = {
    City{"Amsterdam", 52.3676, 4.9041, 1.0f},     City{"Athens", 37.9838, 23.7275, 2.0f},
    City{"Auckland", -36.8485, 174.7633, 12.0f},  City{"Berlin", 52.5200, 13.4050, 1.0f},
    City{"Cape Town", -33.9249, 18.4241, 2.0f},   City{"Chicago", 41.8781, -87.6298, -6.0f},
    City{"Denver", 39.7392, -104.9903, -7.0f},    City{"Dubai", 25.2048, 55.2708, 4.0f},
    City{"Hong Kong", 22.3193, 114.1694, 8.0f},   City{"Istanbul", 41.0082, 28.9784, 3.0f},
    City{"London", 51.5074, -0.1278, 0.0f},       City{"Los Angeles", 34.0522, -118.2437, -8.0f},
    City{"Madrid", 40.4168, -3.7038, 1.0f},       City{"Mexico City", 19.4326, -99.1332, -6.0f},
    City{"Mumbai", 19.0760, 72.8777, 5.5f},       City{"Nairobi", -1.2921, 36.8219, 3.0f},
    City{"New York", 40.7128, -74.0060, -5.0f},   City{"Paris", 48.8566, 2.3522, 1.0f},
    City{"Rome", 41.9028, 12.4964, 1.0f},         City{"São Paulo", -23.5505, -46.6333, -3.0f},
    City{"Singapore", 1.3521, 103.8198, 8.0f},    City{"Stockholm", 59.3293, 18.0686, 1.0f},
    City{"Sydney", -33.8688, 151.2093, 10.0f},    City{"Tokyo", 35.6762, 139.6503, 9.0f},
    City{"Toronto", 43.6532, -79.3832, -5.0f},    City{"Vancouver", 49.2827, -123.1207, -8.0f},
};

struct Preset {
    const char* name;
    const char* icon;
    float azimuth; // manual mode
    float elevation;
};

constexpr std::array kPresets = {
    Preset{"Sunrise", "sunrise", 90.0f, 3.0f},       Preset{"Morning", "sun", 115.0f, 22.0f},
    Preset{"Midday", "sun", 180.0f, 60.0f},          Preset{"Afternoon", "sun", 230.0f, 38.0f},
    Preset{"Golden hour", "sunset", 260.0f, 8.0f},   Preset{"Dusk", "moon", 280.0f, -3.0f},
};

QString compassPoint(float azimuthDegrees) {
    static const char* const points[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int index = static_cast<int>(std::lround(azimuthDegrees / 45.0f)) % 8;
    return QString::fromLatin1(points[(index + 8) % 8]);
}

double elevationAt(const Sun& sun, double hours) {
    const SiteLocation& site = sun.location;
    return solarPosition(site.latitudeDegrees, site.longitudeDegrees, sun.dateTime.year,
                         sun.dateTime.month, sun.dateTime.day, hours, site.utcOffsetHours)
        .elevationDegrees;
}

// Clock time at which the sun crosses `elevation` going up (morning) or down (evening), sampled per
// minute. Empty when it never does that day (polar day or night).
std::optional<float> crossingTime(const Sun& sun, double elevation, bool rising) {
    std::optional<float> found;
    double previous = elevationAt(sun, 0.0);
    for (int minute = 1; minute <= 24 * 60; ++minute) {
        const double hours = minute / 60.0;
        const double current = elevationAt(sun, hours);
        if (rising && previous < elevation && current >= elevation) {
            return static_cast<float>(hours);
        }
        if (!rising && previous >= elevation && current < elevation) {
            found = static_cast<float>(hours);
        }
        previous = current;
    }
    return found;
}

float solarNoon(const Sun& sun) {
    float best = 12.0f;
    double highest = -90.0;
    for (int minute = 0; minute < 24 * 60; minute += 2) {
        const double elevation = elevationAt(sun, minute / 60.0);
        if (elevation > highest) {
            highest = elevation;
            best = static_cast<float>(minute / 60.0);
        }
    }
    return best;
}

QString clockText(float hours) {
    const int minutes = static_cast<int>(std::lround(hours * 60.0f)) % (24 * 60);
    return QTime(minutes / 60, minutes % 60).toString(QStringLiteral("HH:mm"));
}

QDoubleSpinBox* numberField(double min, double max, int decimals, const QString& suffix,
                            QWidget* parent) {
    auto* field = new QDoubleSpinBox(parent);
    field->setRange(min, max);
    field->setDecimals(decimals);
    field->setSuffix(suffix);
    field->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field->setKeyboardTracking(false);
    field->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return field;
}

} // namespace

LightPanel::LightPanel(Scene& scene, QWidget* parent) : QWidget(parent), scene_(scene) {
    QVBoxLayout* layout = ui::scrollPage(this);
    QWidget* page = layout->parentWidget();

    // Presets: one click for a typical time of day. In location mode they pick the clock time
    // (sunrise, golden hour… on the chosen date); in manual mode a typical sun direction.
    auto* presetGrid = new QGridLayout;
    presetGrid->setSpacing(6);
    for (int i = 0; i < static_cast<int>(kPresets.size()); ++i) {
        auto* button = new QPushButton(theme::icon(QString::fromLatin1(kPresets[i].icon)),
                                       tr(kPresets[i].name), page);
        button->setProperty("compact", true);
        connect(button, &QPushButton::clicked, this, [this, i] { applyPreset(i); });
        presetGrid->addWidget(button, i / 3, i % 3);
    }
    layout->addLayout(presetGrid);
    layout->addSpacing(4);

    // Sun.
    auto* sun = new ui::Section(tr("Sun"), page);
    mode_ = new ui::SegmentedControl({tr("Manual"), tr("Location && time")}, sun);
    sun->addWide(mode_);
    azimuth_ = new ui::SliderField(0.0, 360.0, 0, QStringLiteral("°"), sun);
    azimuthRow_ = sun->addRow(tr("Direction"), azimuth_);
    azimuthRow_.label->setToolTip(tr("Compass direction the sunlight comes from: 0° north, 90° east, "
                                     "180° south, 270° west (project north is +Y)."));
    elevation_ = new ui::SliderField(-10.0, 90.0, 0, QStringLiteral("°"), sun);
    elevationRow_ = sun->addRow(tr("Height"), elevation_);
    elevationRow_.label->setToolTip(tr("Angle above the horizon. Low sun gives long shadows."));

    date_ = new QDateEdit(sun);
    date_->setCalendarPopup(true);
    date_->setDisplayFormat(QStringLiteral("d MMM yyyy"));
    date_->setDateRange(QDate(1900, 1, 1), QDate(2100, 12, 31));
    dateRow_ = sun->addRow(tr("Date"), date_);
    auto* timeLayout = new QHBoxLayout;
    timeLayout->setSpacing(8);
    timeSlider_ = new QSlider(Qt::Horizontal, sun);
    timeSlider_->setRange(0, 24 * 60 - 1);
    timeSlider_->setSingleStep(5);
    timeSlider_->setPageStep(60);
    time_ = new QTimeEdit(sun);
    time_->setDisplayFormat(QStringLiteral("HH:mm"));
    time_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    time_->setKeyboardTracking(false);
    time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    time_->setFixedWidth(60);
    timeLayout->addWidget(timeSlider_, 1);
    timeLayout->addWidget(time_);
    timeRow_ = sun->addRow(tr("Time"), timeLayout);
    timeRow_.label->setToolTip(tr("Local clock time at the site (see UTC offset under Location)."));
    readout_ = ui::label(QString(), "muted", sun);
    readout_->setWordWrap(true);
    sun->addWide(readout_);
    layout->addWidget(sun);

    // Location (location mode only).
    location_ = new ui::Section(tr("Location"), page);
    city_ = new QComboBox(location_);
    city_->addItem(theme::icon(QStringLiteral("map-pin")), tr("Custom location"));
    for (const City& city : kCities) {
        city_->addItem(QString::fromUtf8(city.name));
    }
    city_->setMaxVisibleItems(14);
    location_->addRow(tr("City"), city_);
    latitude_ = numberField(-90.0, 90.0, 4, QStringLiteral("°"), location_);
    latitude_->setToolTip(tr("Positive north of the equator, negative south."));
    location_->addRow(tr("Latitude"), latitude_);
    longitude_ = numberField(-180.0, 180.0, 4, QStringLiteral("°"), location_);
    longitude_->setToolTip(tr("Positive east of Greenwich, negative west."));
    location_->addRow(tr("Longitude"), longitude_);
    utcOffset_ = numberField(-12.0, 14.0, 2, tr(" h"), location_);
    utcOffset_->setSingleStep(0.25);
    utcOffset_->setToolTip(tr("Difference between the site's clock and UTC on that date. Cities "
                              "fill in standard time: add 1 h for summer time."));
    location_->addRow(tr("UTC offset"), utcOffset_);
    north_ = new ui::SliderField(0.0, 360.0, 0, QStringLiteral("°"), location_);
    location_->addRow(tr("True north"), north_)
        .label->setToolTip(tr("Angle of true north, clockwise from the model's +Y axis. Match it to "
                              "the north arrow of your site plan."));
    layout->addWidget(location_);

    // Strength of sun and sky light.
    auto* light = new ui::Section(tr("Sunlight"), page);
    intensity_ = new ui::SliderField(0.0, 4.0, 2, QString(), light);
    light->addRow(tr("Strength"), intensity_);
    temperature_ = new ui::SliderField(1800.0, 12000.0, 0, tr(" K"), light);
    light->addRow(tr("Color"), temperature_)
        .label->setToolTip(tr("Color temperature: about 2000 K at sunrise, 5600 K at noon, higher "
                              "values look cooler (bluer)."));
    skyLight_ = new ui::SliderField(0.0, 3.0, 2, QString(), light);
    light->addRow(tr("Sky light"), skyLight_)
        .label->setToolTip(tr("Brightness of the sky and the soft fill light it gives to shaded "
                              "sides."));
    layout->addWidget(light);

    auto* shadow = new ui::Section(tr("Shadows"), page);
    shadows_ = new QCheckBox(tr("Cast shadows"), shadow);
    shadow->addWide(shadows_);
    softness_ = new ui::SliderField(0.0, 4.0, 2, QString(), shadow);
    shadow->addRow(tr("Softness"), softness_)
        .label->setToolTip(tr("0 = razor-sharp edges, 1 = natural sunlight, higher = hazy day."));
    layout->addWidget(shadow);
    layout->addStretch(1);

    // Edits.
    connect(mode_, &ui::SegmentedControl::currentEdited, this, [this](int index) {
        edit([index](Environment& e) {
            if (index == kManual && e.sun.mode == SunMode::Location) {
                // Keep the light where it is when switching to manual control.
                const SunAngles angles = e.sunAngles();
                e.sun.azimuthDegrees = angles.azimuthDegrees;
                e.sun.elevationDegrees = angles.elevationDegrees;
            }
            e.sun.mode = index == kManual ? SunMode::Manual : SunMode::Location;
        });
    });
    connect(azimuth_, &ui::SliderField::valueEdited, this, [this](double value) {
        edit([value](Environment& e) {
            e.sun.azimuthDegrees = static_cast<float>(std::fmod(value + 360.0, 360.0));
        });
    });
    connect(elevation_, &ui::SliderField::valueEdited, this, [this](double value) {
        edit([value](Environment& e) { e.sun.elevationDegrees = static_cast<float>(value); });
    });
    connect(date_, &QDateEdit::dateChanged, this, [this](QDate date) {
        if (!updating_) {
            edit([date](Environment& e) {
                e.sun.dateTime.year = date.year();
                e.sun.dateTime.month = date.month();
                e.sun.dateTime.day = date.day();
            });
        }
    });
    connect(timeSlider_, &QSlider::valueChanged, this, [this](int minutes) {
        if (!updating_) {
            edit([minutes](Environment& e) { e.sun.dateTime.hours = minutes / 60.0f; });
        }
    });
    connect(time_, &QTimeEdit::timeChanged, this, [this](QTime time) {
        if (!updating_) {
            edit([time](Environment& e) {
                e.sun.dateTime.hours = static_cast<float>(time.hour()) + time.minute() / 60.0f;
            });
        }
    });
    connect(city_, &QComboBox::activated, this, [this](int index) {
        if (index <= 0) {
            return;
        }
        const City& city = kCities[static_cast<std::size_t>(index - 1)];
        edit([&city](Environment& e) {
            e.sun.location.latitudeDegrees = city.latitude;
            e.sun.location.longitudeDegrees = city.longitude;
            e.sun.location.utcOffsetHours = city.utcOffset;
        });
    });
    const auto connectNumber = [this](QDoubleSpinBox* field, auto apply) {
        connect(field, &QDoubleSpinBox::valueChanged, this, [this, apply](double value) {
            if (!updating_) {
                edit([&](Environment& e) { apply(e, value); });
            }
        });
    };
    connectNumber(latitude_, [](Environment& e, double v) { e.sun.location.latitudeDegrees = v; });
    connectNumber(longitude_, [](Environment& e, double v) { e.sun.location.longitudeDegrees = v; });
    connectNumber(utcOffset_, [](Environment& e, double v) {
        e.sun.location.utcOffsetHours = static_cast<float>(v);
    });
    const auto connectSlider = [this](ui::SliderField* field, auto apply) {
        connect(field, &ui::SliderField::valueEdited, this,
                [this, apply](double value) { edit([&](Environment& e) { apply(e, value); }); });
    };
    connectSlider(north_, [](Environment& e, double v) {
        e.sun.location.northAngleDegrees = static_cast<float>(v);
    });
    connectSlider(intensity_, [](Environment& e, double v) { e.sun.intensity = static_cast<float>(v); });
    connectSlider(temperature_, [](Environment& e, double v) {
        e.sun.temperatureKelvin = static_cast<float>(v);
    });
    connectSlider(skyLight_, [](Environment& e, double v) { e.sky.intensity = static_cast<float>(v); });
    connectSlider(softness_, [](Environment& e, double v) {
        e.shadows.softness = static_cast<float>(v);
    });
    connect(shadows_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            edit([on](Environment& e) { e.shadows.enabled = on; });
        }
    });

    listener_ = scene_.addListener([this](const SceneChange& change) {
        if (change.kind == SceneChange::Kind::EnvironmentChanged) {
            refresh();
        }
    });
    refresh();
}

LightPanel::~LightPanel() {
    scene_.removeListener(listener_);
}

void LightPanel::edit(const std::function<void(Environment&)>& change) {
    if (updating_) {
        return;
    }
    Environment environment = scene_.environment();
    change(environment);
    scene_.setEnvironment(environment);
}

void LightPanel::applyPreset(int index) {
    const Preset& preset = kPresets[static_cast<std::size_t>(index)];
    edit([&](Environment& e) {
        if (e.sun.mode == SunMode::Manual) {
            e.sun.azimuthDegrees = preset.azimuth;
            e.sun.elevationDegrees = preset.elevation;
            return;
        }
        // Location mode: choose the clock time on the current date.
        std::optional<float> hours;
        switch (index) {
        case 0: hours = crossingTime(e.sun, 3.0, true); break;
        case 1: hours = 9.0f; break;
        case 2: hours = solarNoon(e.sun); break;
        case 3: hours = 15.0f; break;
        case 4: hours = crossingTime(e.sun, 8.0, false); break;
        case 5: hours = crossingTime(e.sun, -3.0, false); break;
        default: break;
        }
        if (hours) {
            e.sun.dateTime.hours = *hours;
        }
    });
}

void LightPanel::refresh() {
    const Environment& env = scene_.environment();
    const Sun& sun = env.sun;
    const bool location = sun.mode == SunMode::Location;
    updating_ = true;

    mode_->setCurrent(location ? kLocation : kManual);
    azimuthRow_.setVisible(!location);
    elevationRow_.setVisible(!location);
    dateRow_.setVisible(location);
    timeRow_.setVisible(location);
    location_->setVisible(location);

    azimuth_->setValue(sun.azimuthDegrees);
    elevation_->setValue(sun.elevationDegrees);
    date_->setDate(QDate(sun.dateTime.year, sun.dateTime.month, sun.dateTime.day));
    const int minutes = static_cast<int>(std::lround(sun.dateTime.hours * 60.0f)) % (24 * 60);
    timeSlider_->setValue(minutes);
    time_->setTime(QTime(minutes / 60, minutes % 60));

    const SiteLocation& site = sun.location;
    int cityIndex = 0;
    for (std::size_t i = 0; i < kCities.size(); ++i) {
        if (std::abs(kCities[i].latitude - site.latitudeDegrees) < 1e-3 &&
            std::abs(kCities[i].longitude - site.longitudeDegrees) < 1e-3) {
            cityIndex = static_cast<int>(i) + 1;
            break;
        }
    }
    city_->setCurrentIndex(cityIndex);
    latitude_->setValue(site.latitudeDegrees);
    longitude_->setValue(site.longitudeDegrees);
    utcOffset_->setValue(site.utcOffsetHours);
    north_->setValue(site.northAngleDegrees);

    intensity_->setValue(sun.intensity);
    temperature_->setValue(sun.temperatureKelvin);
    skyLight_->setValue(env.sky.intensity);
    shadows_->setChecked(env.shadows.enabled);
    softness_->setValue(env.shadows.softness);
    softness_->setEnabled(env.shadows.enabled);

    const SunAngles angles = env.sunAngles();
    QString text = angles.elevationDegrees < 0.0f
                       ? tr("Sun is below the horizon (%1°).")
                             .arg(static_cast<int>(std::lround(angles.elevationDegrees)))
                       : tr("Sun from %1 (%2°), %3° above the horizon.")
                             .arg(compassPoint(angles.azimuthDegrees))
                             .arg(static_cast<int>(std::lround(angles.azimuthDegrees)) % 360)
                             .arg(static_cast<int>(std::lround(angles.elevationDegrees)));
    if (location) {
        // Elevations include refraction: the standard sunrise/sunset (geometric center at −0.833°,
        // upper limb on the horizon) is about −0.4° apparent.
        const std::optional<float> sunrise = crossingTime(sun, -0.4, true);
        const std::optional<float> sunset = crossingTime(sun, -0.4, false);
        if (sunrise && sunset) {
            text += QLatin1Char('\n') +
                    tr("Sunrise %1 · Sunset %2").arg(clockText(*sunrise), clockText(*sunset));
        } else {
            text += QLatin1Char('\n') + (elevationAt(sun, solarNoon(sun)) > 0.0
                                             ? tr("The sun does not set on this date.")
                                             : tr("The sun does not rise on this date."));
        }
    }
    readout_->setText(text);
    updating_ = false;
}

} // namespace lb::app

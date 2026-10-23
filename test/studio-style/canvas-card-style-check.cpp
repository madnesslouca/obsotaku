#include <utility/StudioCanvasCardStyle.hpp>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <algorithm>
#include <cmath>
#include <cstdio>

static double luminance(const QColor &color)
{
	auto linear = [](double channel) {
		return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
	};
	return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}

static double contrast(const QColor &a, const QColor &b)
{
	const double x = luminance(a), y = luminance(b);
	return (std::max(x, y) + .05) / (std::min(x, y) + .05);
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setStyle(QStyleFactory::create("Fusion"));
	QJsonArray cases;
	bool passed = true;
	const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
	const bool legacy = argc > 2 && QString::fromLocal8Bit(argv[2]) == "--legacy";
	if (!output.isEmpty())
		QDir().mkpath(output);
	for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark"),
				     QStringLiteral("high-contrast"), QStringLiteral("mixed-windows-regression")}) {
		QPalette palette = app.style()->standardPalette();
		palette.setColor(QPalette::Button, theme == "light" ? QColor("#eeeeee") : QColor("#252527"));
		palette.setColor(QPalette::ButtonText, theme == "light" ? Qt::black : Qt::white);
		palette.setColor(QPalette::AlternateBase, QColor("#fafafa"));
		palette.setColor(QPalette::Highlight, theme == "high-contrast" ? QColor(Qt::yellow)
							 : theme == "dark" ? QColor("#8fb3ff")
								   : QColor("#005a9e"));
		palette.setColor(QPalette::HighlightedText,
				 theme == "dark" || theme == "high-contrast" ? Qt::black : Qt::white);
		app.setPalette(palette);
		QWidget host;
		host.setPalette(palette);
		QString style = StudioCanvasCardStyleSheet();
		if (legacy)
			style.replace("background: palette(highlight); color: palette(highlighted-text);",
				      "background: palette(alternate-base);");
		host.setStyleSheet(style);
		QHBoxLayout layout(&host);
		QPushButton main("Main Canvas"), multi("Multi Canvas");
		main.setObjectName("studioCanvasSingle");
		multi.setObjectName("studioCanvasMulti");
		for (auto *button : {&main, &multi}) {
			button->setCheckable(true);
			button->setFixedSize(250, 76);
			layout.addWidget(button);
		}
		host.show();
		for (auto *selected : {&main, &multi}) {
			main.setChecked(selected == &main);
			multi.setChecked(selected == &multi);
			selected->setFocus(Qt::TabFocusReason);
			app.processEvents();
			const QImage image = selected->grab().toImage().convertToFormat(QImage::Format_RGB32);
			const QColor background = image.pixelColor(18, 18);
			// QStyleSheetStyle applies its text palette temporarily while painting;
			// verify actual text pixels instead of the widget's unpolished ButtonText role.
			const QColor foreground = palette.color(QPalette::HighlightedText);
			const double ratio = contrast(foreground, background);
			bool textPixels = false;
			for (int y = 25; y < 52 && !textPixels; ++y) {
				for (int x = 14; x < 210; ++x) {
					const QColor pixel = image.pixelColor(x, y);
					if (std::abs(pixel.red() - foreground.red()) < 12 &&
					    std::abs(pixel.green() - foreground.green()) < 12 &&
					    std::abs(pixel.blue() - foreground.blue()) < 12) {
						textPixels = true;
						break;
					}
				}
			}
			const bool ok = background == palette.color(QPalette::Highlight) &&
					foreground == palette.color(QPalette::HighlightedText) && ratio >= 4.5 &&
					textPixels && selected->hasFocus();
			passed = passed && ok;
			cases.append(QJsonObject{{"theme", theme}, {"card", selected->objectName()},
						 {"background", background.name()}, {"foreground", foreground.name()},
						 {"contrast", ratio}, {"text_pixels", textPixels},
						 {"keyboard_focus", selected->hasFocus()}, {"passed", ok}});
			if (!output.isEmpty())
				image.save(output + '/' + theme + '-' + selected->objectName() + ".png");
		}
	}
	const QJsonObject report{{"passed", passed}, {"legacy_reproduction", legacy}, {"cases", cases},
				 {"scope", "Rendered production card stylesheet with controlled palettes and Fusion; no full OBS theme, native platform style, canvas or output behavior certification."}};
	const QByteArray json = QJsonDocument(report).toJson();
	if (!output.isEmpty()) {
		QFile file(output + "/runtime.json");
		if (!file.open(QIODevice::WriteOnly))
			return 2;
		file.write(json);
	}
	fwrite(json.constData(), 1, size_t(json.size()), stdout);
	return passed ? 0 : 1;
}

#include "../../frontend/utility/PortraitTransformGeometry.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace PortraitTransformGeometry;

namespace {
int checks = 0;
void check(bool pass, const std::string &message)
{
	++checks;
	if (!pass) {
		throw std::runtime_error(message);
	}
}
bool close(double a, double b, double epsilon = 1e-4)
{
	return std::abs(a - b) <= epsilon * std::max({1., std::abs(a), std::abs(b)});
}
bool close(QPointF a, QPointF b)
{
	return close(a.x(), b.x()) && close(a.y(), b.y());
}
QPointF map(const matrix4 &box, QPointF p)
{
	return QPointF(box.t.x, box.t.y) + QPointF(box.x.x, box.x.y) * p.x() + QPointF(box.y.x, box.y.y) * p.y();
}
Input input(QPointF position, double rotation = 0, double sx = 1, double sy = 1, bool bounded = false)
{
	Input result;
	result.sourceSize = {100, 80};
	result.info.pos = {float(position.x()), float(position.y())};
	result.info.scale = {float(sx), float(sy)};
	result.info.rot = float(rotation);
	result.info.bounds_type = bounded ? OBS_BOUNDS_SCALE_INNER : OBS_BOUNDS_NONE;
	result.info.bounds = {200, 100};
	const double angle = rotation * std::acos(-1) / 180;
	const double w = bounded ? 200 : 100 * sx, h = bounded ? 100 : 80 * sy;
	result.box.x = {float(w * std::cos(angle)), float(w * std::sin(angle)), 0, 0};
	result.box.y = {float(-h * std::sin(angle)), float(h * std::cos(angle)), 0, 0};
	result.box.z = {0, 0, 1, 0};
	result.box.t = {float(position.x()), float(position.y()), 0, 1};
	return result;
}
QRectF aabb(const std::vector<Input> &inputs)
{
	double left = std::numeric_limits<double>::infinity(), top = left;
	double right = -left, bottom = -left;
	for (auto &i : inputs) {
		for (QPointF p : {QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)}) {
			p = map(i.box, p);
			left = std::min(left, p.x());
			top = std::min(top, p.y());
			right = std::max(right, p.x());
			bottom = std::max(bottom, p.y());
		}
	}
	return {left, top, right - left, bottom - top};
}

void resizeCases()
{
	std::vector<Input> inputs{input({10, 20}), input({210, 120})};
	const QRectF frame = aabb(inputs);
	auto result = Transform(inputs, frame, 4, {300, 180}, Operation::Resize);
	check(result.status == Status::Ok && result.changes.size() == 2, "uniform resize succeeds as a set");
	check(close(result.changes[0].info.scale.x, 2) && close(result.changes[1].info.scale.y, 2),
	      "uniform factor applies to both items");
	check(close(result.changes[0].desiredBoxOrigin, {10, 20}) &&
		      close(result.changes[1].desiredBoxOrigin, {410, 220}),
	      "relative origins scale around opposite corner");
	check(close(result.frame.topLeft(), frame.topLeft()) && close(result.frame.width(), 600),
	      "frame keeps opposite corner");
	check(inputs[0].info.scale.x == 1 && inputs[1].info.pos.x == 210, "inputs remain immutable");

	result = Transform(inputs, frame, 3, {300, 0}, Operation::Resize);
	check(close(result.frame.topLeft(), {10, -70}) && close(result.frame.height(), 360),
	      "uniform side anchors opposite midpoint");
	result = Transform(inputs, frame, 0, {-300, -180}, Operation::Resize);
	check(close(result.frame.bottomRight(), frame.bottomRight()), "top-left resize anchors bottom-right");
	result = Transform(inputs, frame, 4, {300, 0}, Operation::Resize, false);
	check(result.status == Status::Ok && close(result.changes[0].info.scale.x, 2) &&
		      close(result.changes[0].info.scale.y, 1),
	      "free resize supports axis aligned participants");

	inputs = {input({0, 0}, 90), input({300, 200}, 0, -1, -.5)};
	const QRectF rotatedFrame = aabb(inputs);
	result = Transform(inputs, rotatedFrame, 4, {rotatedFrame.width(), 0}, Operation::Resize, false);
	check(result.status == Status::Ok && close(result.changes[0].info.scale.x, 1) &&
		      close(result.changes[0].info.scale.y, 2),
	      "quarter-turn maps world X factor to local Y");
	check(close(result.changes[1].info.scale.x, -2) && close(result.changes[1].info.scale.y, -.5),
	      "free resize preserves mirrored scale signs");

	inputs = {input({0, 0}), input({300, 100}, 31)};
	result = Transform(inputs, aabb(inputs), 3, {200, 0}, Operation::Resize, false);
	check(result.status == Status::UnsupportedShear && result.changes.empty(),
	      "nonrepresentable free resize rejects entire set, without partial result");
	inputs = {input({0, 0}), input({300, 100}, 45, -1, 1, true)};
	result = Transform(inputs, aabb(inputs), 4, {aabb(inputs).width(), aabb(inputs).height()}, Operation::Resize);
	check(result.status == Status::Ok && close(result.changes[1].info.bounds.x, 400) &&
		      close(result.changes[1].info.bounds.y, 200) && result.changes[1].info.scale.x == -1,
	      "bounded rotated resize scales bounds without modifying scale");

	inputs = {input({0, 0}, 0, .05, .05), input({300, 100})};
	result = Transform(inputs, aabb(inputs), 4, {-10000, -10000}, Operation::Resize);
	check(result.status == Status::Ok && close(result.changes[0].info.scale.y * 80, 2) &&
		      result.changes[0].info.scale.x > 0,
	      "uniform crossing clamps all participants to at least two canvas pixels");

	const std::array<QPointF, 8> handles{QPointF(0, 0), QPointF(.5, 0), QPointF(1, 0), QPointF(1, .5),
					     QPointF(1, 1), QPointF(.5, 1), QPointF(0, 1), QPointF(0, .5)};
	for (int angle = -180; angle <= 180; angle += 15) {
		inputs = {input({100, 200}, angle, -1.3, .7), input({500, 600}, angle + 29, .8, -1.2)};
		const QRectF before = aabb(inputs);
		for (int h = 0; h < 8; ++h) {
			const QPointF direction(handles[h].x() - .5, handles[h].y() - .5);
			const QPointF delta(direction.x() * before.width(), direction.y() * before.height());
			const auto change = Transform(inputs, before, h, delta, Operation::Resize);
			check(change.status == Status::Ok && change.changes.size() == inputs.size(),
			      "uniform transformed set accepted for each angle/handle");
			const QPointF anchor(1 - handles[h].x(), 1 - handles[h].y());
			check(close(change.frame.topLeft() + QPointF(change.frame.width() * anchor.x(),
								     change.frame.height() * anchor.y()),
				    before.topLeft() +
					    QPointF(before.width() * anchor.x(), before.height() * anchor.y())),
			      "each uniform handle retains its opposite anchor");
			const double factor = change.frame.width() / before.width();
			for (size_t j = 0; j < inputs.size(); ++j) {
				check(close(change.changes[j].info.scale.x, inputs[j].info.scale.x * factor) &&
					      close(change.changes[j].info.scale.y, inputs[j].info.scale.y * factor),
				      "uniform scaling preserves item aspect/rotation/sign");
				check(close(change.changes[j].desiredBoxOrigin,
					    change.frame.topLeft() +
						    (map(inputs[j].box, {0, 0}) - before.topLeft()) * factor),
				      "each origin follows same affine group transform");
			}
		}
	}
}

void cropCases()
{
	std::vector<Input> inputs{input({10, 20}), input({300, 100}, 30, -2, 1)};
	inputs[1].sourceSize = {200, 160};
	const QRectF frame = aabb(inputs);
	auto result = Transform(inputs, frame, 0, {.2 * frame.width(), .25 * frame.height()}, Operation::Crop);
	check(result.status == Status::Ok && result.changes[0].crop.left == 20 && result.changes[1].crop.left == 40,
	      "left crops use identical visible-pixel fraction for distinct source sizes");
	check(result.changes[0].crop.top == 20 && result.changes[1].crop.top == 40,
	      "corner crops both local axes proportionally");
	check(close(result.changes[0].desiredBoxOrigin, map(inputs[0].box, {.2, .25})) &&
		      close(result.changes[1].desiredBoxOrigin, map(inputs[1].box, {.2, .25})),
	      "rotated/mirrored local origin correction keeps opposite uncropped edges");
	check(result.changes[0].correctOrigin && result.changes[1].correctOrigin,
	      "unbounded crop requests alignment correction");
	result = Transform(inputs, frame, 4, {-.2 * frame.width(), -.25 * frame.height()}, Operation::Crop);
	check(result.changes[0].crop.right == 20 && result.changes[1].crop.bottom == 40 &&
		      close(result.changes[1].desiredBoxOrigin, map(inputs[1].box, {0, 0})),
	      "right/bottom inward crop keeps local origin");

	inputs[0].crop = {10, 0, 5, 0};
	inputs[1].crop = {40, 0, 20, 0};
	result = Transform(inputs, frame, 7, {-100000, 0}, Operation::Crop);
	check(result.changes[0].crop.left == 0 && result.changes[1].crop.left == 24,
	      "outward crop clamps whole set to smallest existing fractional crop");
	inputs[0].crop.left = 0;
	result = Transform(inputs, frame, 7, {-100000, 0}, Operation::Crop);
	check(result.changes[0].crop.left == 0 && result.changes[1].crop.left == 40,
	      "one uncropped participant stops collective outward expansion");

	inputs = {input({0, 0}), input({300, 100})};
	inputs[0].sourceSize = {3, 3};
	result = Transform(inputs, frame, 4, {-100000, -100000}, Operation::Crop);
	check(result.changes[0].crop.right == 1 && result.changes[0].crop.bottom == 1 &&
		      result.changes[1].crop.right == 33 && result.changes[1].crop.bottom == 27,
	      "two-pixel clamp uses most restrictive source for entire set");
	inputs[0] = input({50, 80}, 20, 1, -1, true);
	result = Transform(inputs, frame, 0, {frame.width() * .2, frame.height() * .1}, Operation::Crop);
	check(!result.changes[0].correctOrigin && result.changes[0].info.bounds.x == 200 &&
		      result.changes[0].info.pos.x == 50 && result.changes[0].info.scale.y == -1,
	      "bounded crop retains bounds, position, scale and native refit semantics");

	for (int handle = 0; handle < 8; ++handle) {
		inputs = {input({0, 0}), input({300, 100}, 57)};
		result = Transform(inputs, frame, handle, {frame.width() * .15, frame.height() * .1}, Operation::Crop);
		check(result.status == Status::Ok && result.changes.size() == 2, "all crop handles return full set");
		for (const auto &c : result.changes) {
			check(100 - c.crop.left - c.crop.right >= 2 && 80 - c.crop.top - c.crop.bottom >= 2,
			      "each crop handle respects visible minimum");
		}
	}
}

void invalidCases()
{
	const std::vector<Input> good{input({0, 0}), input({300, 100})};
	const QRectF frame = aabb(good);
	for (const auto &inputs : {std::vector<Input>{}, std::vector<Input>{input({0, 0}, 0, 0, 1)}}) {
		check(Transform(inputs, frame, 4, {}, Operation::Resize).status == Status::InvalidInput,
		      "empty or singular set is invalid");
	}
	for (int handle : {-1, 8}) {
		check(Transform(good, frame, handle, {}, Operation::Resize).changes.empty(),
		      "invalid handle is rejected");
	}
	check(Transform(good, {}, 4, {}, Operation::Resize).status == Status::InvalidInput, "empty AABB rejected");
	auto bad = good;
	bad[1].crop.left = 99;
	check(Transform(bad, frame, 4, {}, Operation::Crop).changes.empty(), "one invalid crop rejects entire set");
	bad = good;
	bad[1].info.rot = std::numeric_limits<float>::quiet_NaN();
	check(Transform(bad, frame, 4, {}, Operation::Resize).changes.empty(), "NaN item transform rejected");
	check(Transform(good, frame, 4, {std::numeric_limits<double>::infinity(), 0}, Operation::Resize).changes.empty(),
	      "infinite pointer delta rejected");
	const float limit = std::numeric_limits<float>::max();
	bad = good;
	bad[1].box.t.x = bad[1].info.pos.x = limit * .75f;
	QRectF extremeFrame = aabb(bad);
	auto result = Transform(bad, extremeFrame, 4, {extremeFrame.width(), extremeFrame.height()}, Operation::Resize);
	check(result.status == Status::InvalidInput && result.changes.empty(),
	      "double-finite resize origin outside float range rejects entire set");
	bad[1].box.t.x = limit * .1f;
	bad[1].info.pos.x = limit * .9f;
	extremeFrame = aabb(bad);
	result = Transform(bad, extremeFrame, 4, {extremeFrame.width(), extremeFrame.height()}, Operation::Resize);
	check(result.status == Status::InvalidInput && result.changes.empty(),
	      "resize rejects estimated final position overflow even when desired origin fits");
	bad = good;
	bad[1].box.t.x = limit * .9f;
	bad[1].info.pos.x = limit * .9f;
	bad[1].box.x.x = limit * .9f;
	bad[1].info.scale.x = bad[1].box.x.x / 100;
	extremeFrame = aabb(bad);
	result = Transform(bad, extremeFrame, 7, {extremeFrame.width() * .5, 0}, Operation::Crop);
	check(result.status == Status::InvalidInput && result.changes.empty(),
	      "double-finite crop origin outside float range rejects entire set");
	bad[1].box.t.x = limit * .2f;
	bad[1].info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	extremeFrame = aabb(bad);
	result = Transform(bad, extremeFrame, 7, {extremeFrame.width() * .5, 0}, Operation::Crop);
	check(result.status == Status::InvalidInput && result.changes.empty(),
	      "crop rejects estimated final position overflow without returning earlier participant");
	bad = good;
	bad[1].sourceSize = {1, 1};
	bad[1].info.scale = {1080, 1920};
	bad[1].box.x.x = 1080;
	bad[1].box.y.y = 1920;
	const QRectF pixelFrame = aabb(bad);
	result = Transform(bad, pixelFrame, 4, {pixelFrame.width() * .5, pixelFrame.height() * .5}, Operation::Resize);
	check(result.status == Status::Ok && result.changes.size() == 2 &&
		      close(result.changes[1].info.scale.x, 1620) && close(result.changes[1].info.scale.y, 2880),
	      "one-pixel source stretched on canvas can resize with other participants");
	result = Transform(bad, pixelFrame, 4, {-100000, -100000}, Operation::Resize);
	check(result.status == Status::Ok && result.changes[1].info.scale.x >= 2 && result.changes[1].info.scale.y >= 2,
	      "one-pixel source resize clamps canvas dimensions instead of requiring two source pixels");
	result = Transform(bad, pixelFrame, 7, {pixelFrame.width() * .25, 0}, Operation::Crop);
	check(result.status == Status::InvalidInput && result.changes.empty(),
	      "one-pixel participant rejects whole crop gesture under the two-visible-source-pixel contract");
	bad = good;
	bad[1].crop = {99, 79, 0, 0};
	bad[1].info.scale = {20, 30};
	bad[1].box.x.x = 20;
	bad[1].box.y.y = 30;
	const QRectF croppedPixelFrame = aabb(bad);
	result = Transform(bad, croppedPixelFrame, 4, {croppedPixelFrame.width(), croppedPixelFrame.height()},
			   Operation::Resize);
	check(result.status == Status::Ok && result.changes.size() == 2 && close(result.changes[1].info.scale.x, 40) &&
		      close(result.changes[1].info.scale.y, 60),
	      "source already cropped to one visible pixel remains resizable");
	result = Transform(bad, croppedPixelFrame, 7, {10, 0}, Operation::Crop);
	check(result.status == Status::InvalidInput && result.changes.empty(),
	      "existing one-visible-pixel crop is rejected for another collective crop gesture");
}
} // namespace

int main()
{
	try {
		resizeCases();
		cropCases();
		invalidCases();
		std::cout << "PASS: " << checks << " portrait collective geometry checks\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
		return 1;
	}
}

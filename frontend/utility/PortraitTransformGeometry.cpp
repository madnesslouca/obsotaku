#include "PortraitTransformGeometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace PortraitTransformGeometry {
namespace {
const std::array<QPointF, 8> handles = {QPointF(0, 0), QPointF(.5, 0), QPointF(1, 0), QPointF(1, .5),
					QPointF(1, 1), QPointF(.5, 1), QPointF(0, 1), QPointF(0, .5)};

bool finite(QPointF point)
{
	return std::isfinite(point.x()) && std::isfinite(point.y());
}

QPointF origin(const matrix4 &box)
{
	return {box.t.x, box.t.y};
}

QPointF map(const matrix4 &box, QPointF point)
{
	return origin(box) + QPointF(box.x.x, box.x.y) * point.x() + QPointF(box.y.x, box.y.y) * point.y();
}

bool valid(const Input &input, Operation operation)
{
	const auto &info = input.info;
	const auto &crop = input.crop;
	const QPointF x(input.box.x.x, input.box.x.y), y(input.box.y.x, input.box.y.y);
	const int minimum = operation == Operation::Crop ? 2 : 1;
	return input.sourceSize.width() >= minimum && input.sourceSize.height() >= minimum && crop.left >= 0 &&
	       crop.right >= 0 && crop.top >= 0 && crop.bottom >= 0 &&
	       double(crop.left) + crop.right <= input.sourceSize.width() - double(minimum) &&
	       double(crop.top) + crop.bottom <= input.sourceSize.height() - double(minimum) &&
	       finite(origin(input.box)) && finite(x) && finite(y) && std::hypot(x.x(), x.y()) > 1e-9 &&
	       std::hypot(y.x(), y.y()) > 1e-9 && finite({info.pos.x, info.pos.y}) &&
	       finite({info.scale.x, info.scale.y}) && finite({info.bounds.x, info.bounds.y}) &&
	       std::isfinite(info.rot);
}

bool representable(QPointF basis, double fx, double fy)
{
	// Positive diagonal scaling preserves an item's rotation exactly only when
	// the basis is axis-aligned or the scaling is uniform. Otherwise an OBS
	// rotation+scale cannot express the requested affine transform without shear.
	const double cross = basis.x() * basis.y() * (fy - fx);
	const double scale = std::hypot(basis.x(), basis.y()) * std::hypot(basis.x() * fx, basis.y() * fy);
	return std::abs(cross) <= 1e-7 * scale;
}

bool safeFloat(double value)
{
	return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}

bool safeFloat(QPointF point)
{
	return safeFloat(point.x()) && safeFloat(point.y());
}

Result resize(const std::vector<Input> &inputs, const QRectF &selection, QPointF handle, QPointF delta, bool uniform)
{
	double left = 0, top = 0, right = 1, bottom = 1;
	if (handle.x() == 0) {
		left = delta.x() / selection.width();
	}
	if (handle.x() == 1) {
		right += delta.x() / selection.width();
	}
	if (handle.y() == 0) {
		top = delta.y() / selection.height();
	}
	if (handle.y() == 1) {
		bottom += delta.y() / selection.height();
	}
	double fx = right - left, fy = bottom - top;
	double minX = 2 / selection.width(), minY = 2 / selection.height();
	if (uniform) {
		double factor = handle.x() == .5                       ? fy
				: handle.y() == .5                     ? fx
				: std::abs(fx - 1) >= std::abs(fy - 1) ? fx
								       : fy;
		for (const auto &input : inputs) {
			minX = std::max(minX, 2. / std::hypot(double(input.box.x.x), double(input.box.x.y)));
			minY = std::max(minY, 2. / std::hypot(double(input.box.y.x), double(input.box.y.y)));
		}
		fx = fy = std::max(factor, std::max(minX, minY));
	} else {
		for (const auto &input : inputs) {
			for (QPointF axis :
			     {QPointF(input.box.x.x, input.box.x.y), QPointF(input.box.y.x, input.box.y.y)}) {
				const double length = std::hypot(axis.x(), axis.y());
				if (std::abs(axis.y()) <= 1e-7 * length) {
					minX = std::max(minX, 2 / length);
				} else if (std::abs(axis.x()) <= 1e-7 * length) {
					minY = std::max(minY, 2 / length);
				} else if (std::abs(fx - fy) <= 1e-7 * std::max({1., std::abs(fx), std::abs(fy)})) {
					minX = minY = std::max({minX, minY, 2 / length});
				}
			}
		}
		fx = std::max(fx, minX);
		fy = std::max(fy, minY);
	}
	left = handle.x() == 0 ? 1 - fx : handle.x() == .5 ? (1 - fx) / 2 : 0;
	top = handle.y() == 0 ? 1 - fy : handle.y() == .5 ? (1 - fy) / 2 : 0;
	const QPointF newOrigin = selection.topLeft() + QPointF(left * selection.width(), top * selection.height());
	Result result;
	result.frame = {newOrigin, QSizeF(selection.width() * fx, selection.height() * fy)};
	if (!std::isfinite(fx) || !std::isfinite(fy) || !finite(newOrigin)) {
		return result;
	}
	for (const auto &input : inputs) {
		const QPointF x(input.box.x.x, input.box.x.y), y(input.box.y.x, input.box.y.y);
		if (!representable(x, fx, fy) || !representable(y, fx, fy)) {
			result.status = Status::UnsupportedShear;
			result.changes.clear();
			return result;
		}
		const double localX = std::hypot(x.x() * fx, x.y() * fy) / std::hypot(x.x(), x.y());
		const double localY = std::hypot(y.x() * fx, y.y() * fy) / std::hypot(y.x(), y.y());
		Change change{input.info, input.crop, {}, true};
		if (input.info.bounds_type == OBS_BOUNDS_NONE) {
			const double sx = input.info.scale.x * localX, sy = input.info.scale.y * localY;
			if (!safeFloat(sx) || !safeFloat(sy)) {
				return {};
			}
			change.info.scale.x = float(sx);
			change.info.scale.y = float(sy);
		} else {
			const double bx = input.info.bounds.x * localX, by = input.info.bounds.y * localY;
			if (!safeFloat(bx) || !safeFloat(by)) {
				return {};
			}
			change.info.bounds.x = float(bx);
			change.info.bounds.y = float(by);
		}
		const QPointF relative = origin(input.box) - selection.topLeft();
		change.desiredBoxOrigin = newOrigin + QPointF(relative.x() * fx, relative.y() * fy);
		const QPointF originalOffset = QPointF(input.info.pos.x, input.info.pos.y) - origin(input.box);
		const QPointF estimatedPosition =
			change.desiredBoxOrigin + QPointF(originalOffset.x() * fx, originalOffset.y() * fy);
		// OBS stores both the box and item position as floats. A double-finite
		// destination can still overflow when the caller applies it. Alignment
		// rounding is recomputed by libobs, so the caller must check again there.
		if (!safeFloat(change.desiredBoxOrigin) || !safeFloat(estimatedPosition) ||
		    !safeFloat(QPointF(x.x() * fx, x.y() * fy)) || !safeFloat(QPointF(y.x() * fx, y.y() * fy))) {
			return {};
		}
		result.changes.push_back(change);
	}
	result.status = Status::Ok;
	return result;
}

Result crop(const std::vector<Input> &inputs, const QRectF &selection, QPointF handle, QPointF delta)
{
	const bool horizontal = handle.x() != .5, vertical = handle.y() != .5;
	double x = (handle.x() == 0 ? delta.x() : -delta.x()) / selection.width();
	double y = (handle.y() == 0 ? delta.y() : -delta.y()) / selection.height();
	double minX = -std::numeric_limits<double>::infinity(), minY = minX, maxX = 1, maxY = 1;
	for (const auto &input : inputs) {
		const auto &c = input.crop;
		const int w = input.sourceSize.width() - c.left - c.right;
		const int h = input.sourceSize.height() - c.top - c.bottom;
		minX = std::max(minX, -double(handle.x() == 0 ? c.left : c.right) / w);
		minY = std::max(minY, -double(handle.y() == 0 ? c.top : c.bottom) / h);
		maxX = std::min(maxX, double(w - 2) / w);
		maxY = std::min(maxY, double(h - 2) / h);
	}
	x = horizontal ? std::clamp(x, minX, maxX) : 0;
	y = vertical ? std::clamp(y, minY, maxY) : 0;
	Result result;
	result.frame = selection;
	for (const auto &input : inputs) {
		const auto &c = input.crop;
		const int w = input.sourceSize.width() - c.left - c.right;
		const int h = input.sourceSize.height() - c.top - c.bottom;
		Change change{input.info, c, {}, input.info.bounds_type == OBS_BOUNDS_NONE};
		if (handle.x() == 0) {
			change.crop.left =
				std::clamp(c.left + int(std::round(x * w)), 0, input.sourceSize.width() - c.right - 2);
		}
		if (handle.x() == 1) {
			change.crop.right =
				std::clamp(c.right + int(std::round(x * w)), 0, input.sourceSize.width() - c.left - 2);
		}
		if (handle.y() == 0) {
			change.crop.top =
				std::clamp(c.top + int(std::round(y * h)), 0, input.sourceSize.height() - c.bottom - 2);
		}
		if (handle.y() == 1) {
			change.crop.bottom =
				std::clamp(c.bottom + int(std::round(y * h)), 0, input.sourceSize.height() - c.top - 2);
		}
		const QPointF offset(double(change.crop.left - c.left) / w, double(change.crop.top - c.top) / h);
		change.desiredBoxOrigin = map(input.box, offset);
		if (!safeFloat(change.desiredBoxOrigin)) {
			return {};
		}
		if (change.correctOrigin) {
			const double ax = input.info.alignment & OBS_ALIGN_LEFT    ? 0
					  : input.info.alignment & OBS_ALIGN_RIGHT ? 1
										   : .5;
			const double ay = input.info.alignment & OBS_ALIGN_TOP      ? 0
					  : input.info.alignment & OBS_ALIGN_BOTTOM ? 1
										    : .5;
			const double removedX = double(change.crop.left - c.left + change.crop.right - c.right) / w;
			const double removedY = double(change.crop.top - c.top + change.crop.bottom - c.bottom) / h;
			const QPointF estimatedOrigin = origin(input.box) +
							QPointF(input.box.x.x, input.box.x.y) * (ax * removedX) +
							QPointF(input.box.y.x, input.box.y.y) * (ay * removedY);
			const QPointF estimatedPosition =
				QPointF(input.info.pos.x, input.info.pos.y) + change.desiredBoxOrigin - estimatedOrigin;
			if (!safeFloat(estimatedPosition)) {
				return {};
			}
		}
		result.changes.push_back(change);
	}
	result.status = Status::Ok;
	return result;
}
} // namespace

Result Transform(const std::vector<Input> &inputs, const QRectF &selection, int handle, QPointF canvasDelta,
		 Operation operation, bool uniform)
{
	if (inputs.empty() || handle < 0 || handle >= int(handles.size()) || !finite(selection.topLeft()) ||
	    !std::isfinite(selection.width()) || !std::isfinite(selection.height()) || selection.width() <= 0 ||
	    selection.height() <= 0 || !finite(canvasDelta) ||
	    !std::all_of(
		    inputs.begin(), inputs.end(),
		    [operation](const Input &input) { return valid(input, operation); })) {
		return {};
	}
	return operation == Operation::Crop ? crop(inputs, selection, handles[handle], canvasDelta)
					    : resize(inputs, selection, handles[handle], canvasDelta, uniform);
}
} // namespace PortraitTransformGeometry

#pragma once

#include <obs.h>
#include <graphics/matrix4.h>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <vector>

/* Pure geometry: never reads or changes live OBS items. The caller applies all
 * changes atomically and corrects each new box origin after libobs recomputes
 * alignment/bounds. Handles use clockwise order, starting at the top left. */
namespace PortraitTransformGeometry {
enum class Operation { Resize, Crop };
enum class Status { Ok, InvalidInput, UnsupportedShear };

struct Input {
	obs_transform_info info{};
	obs_sceneitem_crop crop{};
	matrix4 box{};
	QSize sourceSize;
};

struct Change {
	obs_transform_info info{};
	obs_sceneitem_crop crop{};
	QPointF desiredBoxOrigin;
	bool correctOrigin = false;
};

struct Result {
	Status status = Status::InvalidInput;
	std::vector<Change> changes;
	QRectF frame;
};

/* Resize is uniform by default, preserving relative positions and the opposite
 * handle. Non-uniform resize is rejected for the whole set if any participant
 * would require shear. Crop uses equal fractions of each item's visible source
 * pixels, clamped by the most restrictive participant (minimum two pixels).
 * Bounded crops retain their bounds/position, following the native refit rule. */
Result Transform(const std::vector<Input> &inputs, const QRectF &selection, int handle, QPointF canvasDelta,
		 Operation operation, bool uniform = true);
} // namespace PortraitTransformGeometry

#pragma once

#include <obs.hpp>
#include <graphics/matrix4.h>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <array>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <vector>

class QWidget;
class FreezePortraitGroups;

/* UI-thread gestures; only immutable geometry crosses to the display thread.
 * Each scope edits siblings in one scene, including a group's direct children. */
class PortraitPreviewEditor : public QObject {

public:
	explicit PortraitPreviewEditor(QWidget *display, std::function<OBSScene()> scene,
				       std::function<void(const QString &, int64_t, bool, bool)> select);
	~PortraitPreviewEditor() override;
	void Cancel();
	void SetCropMode(bool enabled);
	void ExitGroup();
	QString GroupScopeUuid() const { return groupScope; }
	void Draw(float displayScale);

private:
	bool eventFilter(QObject *object, QEvent *event) override;
	QRectF Frame() const;
	QPointF CanvasPoint(QPointF point) const;
	OBSSceneItem Hit(QPointF point, int &handle) const;
	void Begin(QPointF point, Qt::KeyboardModifiers modifiers);
	void Move(QPointF point, Qt::KeyboardModifiers modifiers);
	void Finish();
	void Refresh();
	bool ValidGesture() const;
	std::string Snapshot() const;
	void RestoreParticipants();
	void Feedback(const QString &status);
	void AdoptSelection();
	void SetGroupScope(const QString &uuid);
	bool EnterGroup(QPointF point);
	void Trace(const char *phase, const QString &detail = {}, obs_sceneitem_t *target = nullptr) const;

	QWidget *display;
	std::function<OBSScene()> scene;
	std::function<void(const QString &, int64_t, bool, bool)> select;
	QString groupScope;
	QString gestureOwner;
	std::vector<OBSSceneItem> gestureAncestors;
	struct AncestorState {
		obs_transform_info info{};
		obs_sceneitem_crop crop{};
	};
	std::vector<AncestorState> ancestorStates;
	OBSSceneItem branch;
	matrix4 parentToCanvas{}, canvasToParent{};
	std::unique_ptr<FreezePortraitGroups> frozenGroups;
	struct Participant {
		OBSSceneItem item;
		obs_transform_info initial{};
		obs_sceneitem_crop crop{};
		QSize size;
		matrix4 box{};
	};
	std::vector<Participant> participants;
	QRectF selectionBox;
	QString feedbackStatus;
	mutable QString invalidReason;
	OBSSceneItem item;
	OBSWeakSource root;
	obs_transform_info initial{};
	obs_sceneitem_crop initialCrop{};
	matrix4 box{}, inverse{};
	QPointF start;
	QPointF rotationCenter;
	double rotationLastAngle = 0;
	double rotationAccumulated = 0;
	QSize sourceSize;
	std::string before;
	int handle = -1;
	bool cropping = false;
	bool rotating = false;
	bool cropMode = false;
	bool active = false;
	bool moved = false;
	struct Overlay {
		bool cropped = false;
		float pixelRatio = 1;
		bool rejected = false;
		std::vector<std::array<QPointF, 8>> boxes;
		std::optional<std::array<QPointF, 8>> controls;
		// Edge attachment, grip and pivot, all in canvas coordinates.
		std::optional<std::array<QPointF, 3>> rotation;
	};
	std::mutex overlayMutex;
	Overlay overlay;
};

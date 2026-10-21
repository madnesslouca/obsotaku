#pragma once

#include <docks/OBSDock.hpp>
#include <obs.hpp>
#include <QPointer>

class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QVBoxLayout;
class OBSPropertiesView;
class QWidget;
struct obs_scene_item;

/* Embedded editing over libobs, with the full native dialogs available for
 * advanced operations. No second scene or source model is maintained. */
class StudioInspectorDock : public OBSDock {
	Q_OBJECT

public:
	explicit StudioInspectorDock(QWidget *parent = nullptr);
	void Refresh(obs_scene_item *item, int selectionCount);

signals:
	void propertiesRequested();
	void filtersRequested();
	void transformRequested();

private:
	QLabel *nameLabel;
	QLabel *detailsLabel;
	QPushButton *propertiesButton;
	QPushButton *filtersButton;
	QPushButton *transformButton;
	QWidget *transformSection;
	QDoubleSpinBox *positionX;
	QDoubleSpinBox *positionY;
	QDoubleSpinBox *sizeX;
	QDoubleSpinBox *sizeY;
	QDoubleSpinBox *rotation;
	QVBoxLayout *propertiesLayout;
	QPointer<OBSPropertiesView> propertiesView;
	QPushButton *applyButton;
	OBSSceneItem selectedItem;
	OBSWeakSource selectedScene;
	OBSSource selectedSource;
	void BindSource(obs_source_t *source);
	void CommitTransform();
	static void RecordPropertyUndo(void *object, obs_data_t *before, obs_data_t *after);
};

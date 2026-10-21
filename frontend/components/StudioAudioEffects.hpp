#pragma once

#include <obs.h>
#include <QString>
#include <QStringList>

class QWidget;

// Transient, nonmodal editors shared by the Windows and Linux mixer.
void ShowStudioAudioEffectPicker(QWidget *anchor, obs_source_t *source);
void ShowStudioAudioEffectEditor(QWidget *anchor, obs_source_t *source, const QString &filterUUID);
void MoveStudioAudioEffect(obs_source_t *source, const QString &filterUUID, int direction);
void RemoveStudioAudioEffect(obs_source_t *source, const QString &filterUUID);
void ReorderStudioAudioEffects(obs_source_t *source, const QStringList &audioOrder);

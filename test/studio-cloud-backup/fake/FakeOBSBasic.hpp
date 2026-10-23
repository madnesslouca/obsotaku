#pragma once
#include <QObject>
#include <QTimer>
class OBSBasic : public QObject {
public:
 QTimer googleDriveBackupTimer;
 bool googleDriveBackupRunning=false;
 bool streaming=false, recording=false;
 void InitializeGoogleDriveBackupScheduler();
 void RunAutomaticGoogleDriveBackup();
 bool StreamingActive(){return streaming;}
 bool RecordingActive(){return recording;}
 void SaveProjectNow(){}
};

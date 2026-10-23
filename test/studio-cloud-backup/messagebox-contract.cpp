/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QApplication>
#include <QMessageBox>
#include <QAbstractButton>
#include <QTimer>
#include <iostream>
int main(int argc,char **argv)
{
 QApplication app(argc,argv);
 QTimer done;
 QObject::connect(&done,&QTimer::timeout,[]{
  for(auto*w:QApplication::topLevelWidgets())if(auto*box=qobject_cast<QMessageBox*>(w))box->done(QMessageBox::Yes);
 });
 done.start(2);
 auto resultDone=QMessageBox::question(nullptr,"Private fixture","No secret or service",QMessageBox::Yes|QMessageBox::No);
 done.stop();
 QTimer click;
 QObject::connect(&click,&QTimer::timeout,[]{
  for(auto*w:QApplication::topLevelWidgets())if(auto*box=qobject_cast<QMessageBox*>(w))box->button(QMessageBox::Yes)->click();
 });
 click.start(2);
 auto resultClick=QMessageBox::question(nullptr,"Private fixture","No secret or service",QMessageBox::Yes|QMessageBox::No);
 click.stop();
 std::cout<<"{\"done_return\":"<<int(resultDone)<<",\"real_click_return\":"<<int(resultClick)<<",\"expected_yes\":"<<int(QMessageBox::Yes)<<",\"diagnosis_confirmed\":"<<(resultDone!=QMessageBox::Yes&&resultClick==QMessageBox::Yes?"true":"false")<<"}\n";
 return resultDone!=QMessageBox::Yes&&resultClick==QMessageBox::Yes?0:1;
}

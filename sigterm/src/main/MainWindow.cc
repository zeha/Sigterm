#include "MainWindow.h"
#include "PlayQueue.h"
#include "AudioFile.h"

#include <QFileDialog>
#include <QDebug>
#include <QHeaderView>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setupUi(this);

    connect(&mAudioManager, SIGNAL(audioPaused(bool)), SLOT(audioPaused(bool)));
    connect(qApp, SIGNAL(lastWindowClosed()), SLOT(on_actionQuit_activated()));
    mAudioManager.init();

    playQueue->setModel(mAudioManager.playQueue());
    playQueue->header()->resizeSection(0, 20);
    playQueue->header()->resizeSection(1, 500);
    playQueue->header()->setResizeMode(1, QHeaderView::Stretch);
    playQueue->header()->resizeSection(2, 50);
    playQueue->header()->setStretchLastSection(false);
}

void MainWindow::audioPaused(bool inPause) {
    if (inPause) {
	playButton->setText("Play");
    } else {
	playButton->setText("Pause");
    }
}

void MainWindow::on_addButton_clicked() {
    QStringList files = QFileDialog::getOpenFileNames(this, "Add Music Files", "/home", "(*.flac *.ogg)");
    for (int i=0; i<files.size(); i++) {
	AudioFile *af = new AudioFile(files[i], &mAudioManager);
	af->addToQueue();
    }
}

void MainWindow::on_actionQuit_activated() {
    mAudioManager.quit();
    qApp->quit();
}

void MainWindow::on_playButton_clicked() {
    mAudioManager.togglePause();
}

void MainWindow::on_playQueue_doubleClicked(const QModelIndex &index) {
    mAudioManager.playQueue()->setNextTrack(index.row());

    if (mAudioManager.paused())
	mAudioManager.togglePause();
    else
	mAudioManager.skipTrack();
}


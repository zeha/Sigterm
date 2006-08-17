#ifndef _AUDIO_META_DATA_H
#define _AUDIO_META_DATA_H

#include <QString>
#include <QStringList>

class AudioFile;

class AudioMetaData {
    public:
	AudioMetaData(AudioFile *inAudioFile);
	~AudioMetaData();

	void parseVorbisComments(QStringList &inList);

	QString &artist();
	QString &title();
	QString &album();
	quint16 trackNumber();
	quint16 totalTracks();
	QString &date();

	void setArtist(const QString &inArtist);
	void setTitle(const QString &inTitle);
	void setAlbum(const QString &inAlbum);
	void setTrackNumber(quint32 inTrackNumber);
	void setTotalTracks(quint32 inTotalTracks);
	void setDate(const QString &inDate);

    private:
	QString mArtist, mTitle, mAlbum, mDate;
	quint16 mTrackNumber, mTotalTracks;

	AudioFile *mAudioFile;
};

#endif

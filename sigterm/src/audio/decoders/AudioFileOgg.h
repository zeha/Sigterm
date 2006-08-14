#ifndef _AUDIO_FILE_OGG_H
#define _AUDIO_FILE_OGG_H

#include "AudioFile.h"
#include <vorbis/vorbisfile.h>

class AudioFileOgg : public AudioFile {
    public:
	AudioFileOgg(AudioManager *inAudioManager);
	~AudioFileOgg();

	bool open(const QString &inFilename);
	bool close();

	bool seekToTime(quint32 inMilliSeconds);

    private:
	bool getDecodedChunk(QByteArray &inOutArray);

	OggVorbis_File mOggVorbisFile;
	bool mOpened;
};

#endif

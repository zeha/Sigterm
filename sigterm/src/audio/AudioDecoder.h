#ifndef _AUDIO_DECODER_H
#define _AUDIO_DECODER_H

#include <QtGlobal>
#include <QString>
#include <QByteArray>
#include <QMutex>

#include "AudioFormat.h"
#include "AudioConverter.h"

#include <SDL.h>

class AudioManager;
class AudioFile;
class AudioBuffer;

class AudioDecoder {
	public:
		AudioDecoder(AudioFile *inAudioFile, AudioManager *inAudioManager);
		virtual ~AudioDecoder();

		virtual AudioDecoder *createAudioDecoder(AudioFile *inAudioFile, AudioManager *inAudioManager) = 0;
		virtual bool canDecode(const QString &inFilePath) = 0;

		virtual bool readInfo() = 0;
		bool seekToTime(quint32 inMilliSeconds);

		bool open();
		bool close();

		bool opened();

		typedef enum {
			eContinue,
			eStop
		} DecodingStatus;
		DecodingStatus getAudioChunk(AudioBuffer *inOutAudioBuffer);

		AudioFormat &audioFormat();

		AudioManager *audioManager();
		AudioFile *audioFile();

	private:
		virtual DecodingStatus getDecodedChunk(AudioBuffer *inOutAudioBuffer) = 0;
		virtual bool openFile() = 0;
		virtual bool closeFile() = 0;
		virtual bool seekToTimeInternal(quint32 inMilliSeconds) = 0;

		void setOpened(bool inValue);

		AudioFormat mAudioFormat;
		SDL_AudioCVT mCVT;
		bool mBuiltCVT;
		AudioManager *mAudioManager;
		AudioConverter mConverter;
		AudioFile *mAudioFile;

		QMutex mMutex;

		bool mOpened;
};

#endif

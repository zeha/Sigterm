#include "AudioDecoderMp4.h"
#include "AudioFile.h"
#include "AudioBuffer.h"
#include "aacinfo.h"

#include "mp4ff.h"
#include "faad.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * find AAC track
*/
static int getAACTrack(mp4ff_t *infile)
{
	int i, rc;

	int numTracks = mp4ff_total_tracks(infile);
	for(i=0; i<numTracks; i++){
		unsigned char*	buff = 0;
		unsigned int			buff_size = 0;
		mp4AudioSpecificConfig mp4ASC;

		mp4ff_get_decoder_config(infile, i, &buff, &buff_size);
		if(buff){
			rc = NeAACDecAudioSpecificConfig(buff, buff_size, &mp4ASC);
			free(buff);
			if(rc < 0)
				continue;

			return(i);
		}
	}

	return(-1);
}

uint32_t read_callback(void *user_data, void *buffer, uint32_t length)
{
	return fread(buffer, 1, length, (FILE*)user_data);
}

uint32_t seek_callback(void *user_data, uint64_t position)
{
	return fseek((FILE*)user_data, position, SEEK_SET);
}

/*
mp4ff_callback_t *getMP4FF_cb(FILE *mp4file)
{
	mp4ff_callback_t* mp4cb = (mp4ff_callback_t*)malloc(sizeof(mp4ff_callback_t));
	mp4cb->read = read_callback;
	mp4cb->seek = seek_callback;
	mp4cb->user_data = mp4file;
	return mp4cb;
}
*/

AudioDecoderMp4::AudioDecoderMp4(AudioFile *inAudioFile, AudioManager *inAudioManager) : AudioDecoder(inAudioFile, inAudioManager) {
	mMp4Callbacks.read = read_callback;
	mMp4Callbacks.seek = seek_callback;
}

AudioDecoderMp4::~AudioDecoderMp4() {
}

AudioDecoder *AudioDecoderMp4::createAudioDecoder(AudioFile *inAudioFile, AudioManager *inAudioManager) {
	return new AudioDecoderMp4(inAudioFile, inAudioManager);
}

bool AudioDecoderMp4::openFile() {
	mAacFile = fopen(qPrintable(audioFile()->filePath()), "rb");
	if (!mAacFile) {
		qDebug("aac::openFile: Couldn't open file");
		return false;
	}

	mAacHandle = NeAACDecOpen();
	mMp4Callbacks.user_data = mAacFile;
	if (!(mMp4File = mp4ff_open_read(&mMp4Callbacks))) {
		qDebug("can't open mp4 file");
		return false;
	}

	mMp4Track = getAACTrack(mMp4File);
	if (mMp4Track < 0) {
		qDebug("Unsupported audio format");
		return false;
	}

	NeAACDecConfigurationPtr conf = NeAACDecGetCurrentConfiguration(mAacHandle);
	conf->outputFormat = FAAD_FMT_16BIT;
	audioFormat().setBitsPerSample(16);
	NeAACDecSetConfiguration(mAacHandle, conf);

	unsigned char *buffer = NULL;
	quint32 bufferSize;

	mp4ff_get_decoder_config(mMp4File, mMp4Track, &buffer, &bufferSize);

	unsigned long samplerate;
	unsigned char c;
	char err = NeAACDecInit2(mAacHandle, (unsigned char *)buffer, bufferSize, &samplerate, &c);
	if (err < 0) {
		qDebug("Could not initialize aac.");
		fclose(mAacFile);
		NeAACDecClose(mAacHandle);
		return false;
	}

	mp4AudioSpecificConfig mp4ASC;
	memset(&mp4ASC, 0, sizeof(mp4ASC));
	NeAACDecAudioSpecificConfig(buffer, bufferSize, &mp4ASC);

	fseek(mAacFile, err, SEEK_SET);

	audioFormat().setFrequency(samplerate);
	audioFormat().setChannels(c);
	audioFormat().setIsBigEndian(true);
	audioFormat().setIsUnsigned(false);

	mF = 1024.0;
	if(mp4ASC.sbr_present_flag == 1)
		mF = mF * 2.0;
	mF -= 1.0;
	audioFile()->setTotalSamples(mp4ff_num_samples(mMp4File, mMp4Track) * mF);

	mSampleId = 0;

	return true;
}

bool AudioDecoderMp4::closeFile() {
	fclose(mAacFile);
	NeAACDecClose(mAacHandle);
	return true;
}


bool AudioDecoderMp4::seekToTimeInternal(quint32 inMilliSeconds) {
	mSampleId = (audioFormat().frequency() * (inMilliSeconds/1000.0)) / mF;
	return true;
}

AudioDecoder::DecodingStatus AudioDecoderMp4::getDecodedChunk(AudioBuffer *inOutAudioBuffer) {
	if (inOutAudioBuffer->state() != AudioBuffer::eEmpty) {
		qDebug("AudioDecoderFlac: AudioBuffer in wrong state!");
		return eContinue;
	}

	if (!inOutAudioBuffer->prepareForDecoding()) {
		qDebug("AudioBuffer::prepareForDecoding() failed.");
		return eContinue;
	}

	AudioDecoder::DecodingStatus status = eContinue;
	while (mAudioStorage.needData(inOutAudioBuffer->requestedLength()) == false) {
		unsigned char	*buffer	= NULL;
		quint32 bufferSize = 0;
		int r = mp4ff_read_sample(mMp4File, mMp4Track, mSampleId++, &buffer, &bufferSize);
		if (r == 0 || buffer == NULL) {
			qDebug("read error");
			status = eStop;
			break;
		}

		unsigned char *sampleBuffer = (unsigned char *)NeAACDecDecode(mAacHandle, &mAacFrameInfo, buffer, bufferSize);
		if (mAacFrameInfo.error > 0) {
			status = eStop;
			break;
		}

		QByteArray a;
		quint32 len = mAacFrameInfo.samples * (audioFormat().bitsPerSample()/8);
		a.resize(len);
		memcpy(a.data(), sampleBuffer, len);
		mAudioStorage.add(a, len);
	}

	if (status == eContinue) {
		mAudioStorage.get(inOutAudioBuffer->byteBuffer());
		inOutAudioBuffer->setDecodedChunkLength(inOutAudioBuffer->requestedLength());
	} else {
		inOutAudioBuffer->byteBuffer().resize(mAudioStorage.bufferLength());
		mAudioStorage.get(inOutAudioBuffer->byteBuffer());
		inOutAudioBuffer->setDecodedChunkLength(mAudioStorage.bufferLength());
	}

	return status;
}

bool AudioDecoderMp4::canDecode(const QString &inFilePath) {
	return true;
}

bool AudioDecoderMp4::readInfo() {
	return false;
}

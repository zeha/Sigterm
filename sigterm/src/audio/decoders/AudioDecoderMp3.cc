/*
 * AudioDecoderMp3.cc
 *
 * implements the .MP3 Audio Decoder for sigterm, based on libmad. 
 * Most of our stuff is really stolen from mpd, the music player
 * daemon.
 *
 */

#include "AudioDecoderMp3.h"
#include "AudioFile.h"
#include "AudioBuffer.h"

#include <QDebug>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <id3tag.h>

/* xing stuff stolen from alsaplayer */
# define XING_MAGIC	(('X' << 24) | ('i' << 16) | ('n' << 8) | 'g')

enum {
  	XING_FRAMES = 0x00000001L,
  	XING_BYTES  = 0x00000002L,
  	XING_TOC    = 0x00000004L,
  	XING_SCALE  = 0x00000008L
};

enum {
	DECODE_BREAK,
	DECODE_SKIP,
	DECODE_STOP,
	DECODE_CONT,
	DECODE_OK
};

static int parse_xing(struct xing *xing, struct mad_bitptr ptr, unsigned int bitlen) {
	if (bitlen < 64 || mad_bit_read(&ptr, 32) != XING_MAGIC) goto fail;

	xing->flags = mad_bit_read(&ptr, 32);
	bitlen -= 64;

	if (xing->flags & XING_FRAMES) {
		if (bitlen < 32) goto fail;
		xing->frames = mad_bit_read(&ptr, 32);
		bitlen -= 32;
	}

	if (xing->flags & XING_BYTES) {
		if (bitlen < 32) goto fail;
		xing->bytes = mad_bit_read(&ptr, 32);
		bitlen -= 32;
	}

	if (xing->flags & XING_TOC) {
		int i;
		if (bitlen < 800) goto fail;
		for (i = 0; i < 100; ++i) {
			xing->toc[i] = mad_bit_read(&ptr, 8);
		}
		bitlen -= 800;
	}

	if (xing->flags & XING_SCALE) {
		if (bitlen < 32) goto fail;
		xing->scale = mad_bit_read(&ptr, 32);
		bitlen -= 32;
	}

	return 1;

fail:
	xing->flags = 0;
	return 0;
}

/* this is stolen from mpg321! */
static unsigned long prng(unsigned long state) {
	return (state * 0x0019660dL + 0x3c6ef35fL) & 0xffffffffL;
}

static signed long audio_linear_dither(unsigned int bits, mad_fixed_t sample, struct audio_dither *dither) {
	
	unsigned int scalebits;
	mad_fixed_t output, mask, random;

	enum {
		MIN = -MAD_F_ONE,
		MAX =  MAD_F_ONE - 1
	};

	sample += dither->error[0] - dither->error[1] + dither->error[2];

	dither->error[2] = dither->error[1];
	dither->error[1] = dither->error[0] / 2;

	output = sample + (1L << (MAD_F_FRACBITS + 1 - bits - 1));

	scalebits = MAD_F_FRACBITS + 1 - bits;
	mask = (1L << scalebits) - 1;

	random  = prng(dither->random);
	output += (random & mask) - (dither->random & mask);

	dither->random = random;

	if (output > MAX) {
		output = MAX;

		if (sample > MAX)
			sample = MAX;
	}
	else if (output < MIN) {
	        output = MIN;

		if (sample < MIN)
			sample = MIN;
	}

	output &= ~mask;

	dither->error[0] = sample - output;

	return output >> scalebits;
}
/* end of stolen stuff from mpg321 */



AudioDecoderMp3::AudioDecoderMp3(AudioFile *inAudioFile, AudioManager *inAudioManager) : AudioDecoder(inAudioFile, inAudioManager) {
	mHighestFrame = 0;
	mCurrentFrame = 0;
}

AudioDecoderMp3::~AudioDecoderMp3() {
}

AudioDecoder *AudioDecoderMp3::createAudioDecoder(AudioFile *inAudioFile, AudioManager *inAudioManager) {
	return new AudioDecoderMp3(inAudioFile, inAudioManager);
}

bool AudioDecoderMp3::openFile() {

	mInputFile.setFileName(audioFile()->filePath());
	if (!mInputFile.open(QIODevice::ReadOnly)) {
		qDebug("AudioDecoderMp3::openFile: Couldn't open file");
		return false;
	}

	mInputFile.seek(mInputFile.size() - 128);
	char id3v1[128];
	if (mInputFile.read(id3v1, sizeof(id3v1)) >= 0) {
		struct id3_tag *id3Tag = id3_tag_parse((id3_byte_t*)id3v1, sizeof(id3v1));
		if (id3Tag) {
			audioFile()->metaData()->parseId3Tags(id3Tag);
			id3_tag_delete(id3Tag);
		}
	}
	mInputFile.seek(0);

	memset(&mDither, 0, sizeof(struct audio_dither));
	
	mad_stream_init(&mMadStream);
	mMadStream.options |= MAD_OPTION_IGNORECRC;
	mad_frame_init(&mMadFrame);
	mad_synth_init(&mMadSynth);
	mad_timer_reset(&mMadTimer);

	audioFile()->setTotalSamples(0);
	audioFormat().setIsBigEndian(QSysInfo::ByteOrder == QSysInfo::BigEndian);
	audioFormat().setIsUnsigned(false);
	audioFormat().setBitsPerSample(16);

	if (!decodeFirstFrame()) {
		closeFile();
		return false;
	}

	audioFormat().setFrequency(mMadFrame.header.samplerate);
	audioFormat().setChannels(MAD_NCHANNELS(&mMadFrame.header));
	
	qWarning("samplerate: %d, channels: %d\n", audioFormat().frequency(), audioFormat().channels());

	return true;
}

bool AudioDecoderMp3::closeFile() {
	if (mInputFile.isOpen())
		mInputFile.close();

	mad_synth_finish(&mMadSynth);
	mad_frame_finish(&mMadFrame);
	mad_stream_finish(&mMadStream);

	mSeekTable.clear();
	mHighestFrame = 0;
	mCurrentFrame = 0;

	return true;
}

void AudioDecoderMp3::parseId3Tag(signed long tagsize) {
	id3_length_t count = mMadStream.bufend - mMadStream.this_frame;
	id3_byte_t const *id3_data;
	QByteArray data;

	if ((id3_length_t)tagsize <= count) {
		id3_data = mMadStream.this_frame;
		mad_stream_skip(&mMadStream, tagsize);
	} else {
		data.resize(tagsize);
		memcpy(data.data(), mMadStream.this_frame, count);
		mad_stream_skip(&mMadStream, count);

		while (count < (id3_length_t)tagsize) {
			qint64 len;

			len = mInputFile.read((char *)data.data()+count, tagsize - count);
			if (len <= 0)
				break;
			else count += len;
		}

		if (count != (id3_length_t)tagsize) {
			qDebug("Error parsing id3 tag\n");
			return;
		}

		id3_data = (id3_byte_t*)data.data();
	}

	struct id3_tag *id3Tag = id3_tag_parse(id3_data, tagsize);

	if (id3Tag) {
		audioFile()->metaData()->parseId3Tags(id3Tag);
		id3_tag_delete(id3Tag);
	}
}

bool AudioDecoderMp3::fillDecoderBuffer() {

	if (mMadStream.buffer != NULL && mMadStream.error != MAD_ERROR_BUFLEN)
		return true;	/* nothing to do */

	size_t readSize;
	size_t remaining;
	unsigned char * readStart;
	
	readSize = AUDIODECODER_MP3_READBUFFER_SIZE;
	readStart = mBufferRead;
	remaining = 0;

	if(mMadStream.next_frame != NULL) {
		remaining = mMadStream.bufend-mMadStream.next_frame;
		memmove(mBufferRead,mMadStream.next_frame,remaining);
		readStart += remaining;
		readSize -= remaining;
	}
	
	qint64 l = mInputFile.read((char*)readStart, readSize);
	if (l == -1 || l == 0) {
		qWarning("reached end of input file");
		return false; // QFile error || EOF
	}
	
	mad_stream_buffer(&mMadStream, mBufferRead, l+remaining);
	mMadStream.error = MAD_ERROR_NONE;
	return true;
}

int AudioDecoderMp3::decodeNextFrameHeader() {

	if (!fillDecoderBuffer()) {
		return DECODE_BREAK;
	}
	if(mad_header_decode(&mMadFrame.header,&mMadStream)) {
		if(mMadStream.error==MAD_ERROR_LOSTSYNC &&
				mMadStream.this_frame)
		{
			signed long tagsize = id3_tag_query(
					mMadStream.this_frame,
					mMadStream.bufend-
					mMadStream.this_frame);

			if(tagsize>0) {
				parseId3Tag(tagsize);
				return DECODE_CONT;
			}
		}
		if (MAD_RECOVERABLE(mMadStream.error)) {
			return DECODE_SKIP;
		} else {
			if (mMadStream.error == MAD_ERROR_BUFLEN)
				return DECODE_CONT;
			else
			{
				qWarning("unrecoverable frame level error "
					"(%s).",
					mad_stream_errorstr(&mMadStream));
				return DECODE_BREAK;
			}
		}
	}
	if (mMadFrame.header.layer != MAD_LAYER_III) {
		return DECODE_SKIP;
	}

	mCurrentFrame++;
	if (mCurrentFrame >= mHighestFrame) {
		mad_timer_add(&mMadTimer, mMadFrame.header.duration);
		mSeekTable[mCurrentFrame].time = mMadTimer;
		mSeekTable[mCurrentFrame].frameOffset = mInputFile.pos();
		if (mMadStream.this_frame != NULL) {
			mSeekTable[mCurrentFrame].frameOffset -= mMadStream.bufend - mMadStream.this_frame;
		} else {
			mSeekTable[mCurrentFrame].frameOffset -= mMadStream.bufend - mMadStream.buffer;
		}

		mHighestFrame = mCurrentFrame;

		//qDebug("mSeekTable[%d].time: %d .frameOffset: %d", mCurrentFrame, mad_timer_count(mSeekTable[mCurrentFrame].time, MAD_UNITS_MILLISECONDS), mSeekTable[mCurrentFrame].frameOffset);
	}

	return DECODE_OK;
}

int AudioDecoderMp3::decodeNextFrame() {

	if (!fillDecoderBuffer()) {
		return DECODE_BREAK;
	}
	
	if (mad_frame_decode(&mMadFrame, &mMadStream)) {
		if (mMadStream.error==MAD_ERROR_LOSTSYNC) {
			signed long tagsize = id3_tag_query(
					mMadStream.this_frame,
					mMadStream.bufend-
					mMadStream.this_frame);
			if(tagsize>0) {
				mad_stream_skip(&mMadStream,tagsize);
				return DECODE_CONT;
			}
		}
		if (MAD_RECOVERABLE(mMadStream.error)) {
			return DECODE_SKIP;
		}
		else {
			if (mMadStream.error == MAD_ERROR_BUFLEN)
				return DECODE_CONT;
			else {
				qWarning("unrecoverable frame level error "
					"(%s).\n",
					mad_stream_errorstr(&mMadStream));
//				data->flush = 0;
				return DECODE_BREAK;
			}
		}
	}

	return DECODE_OK;
}

#define FRAMES_CUSHION		2000

bool AudioDecoderMp3::decodeFirstFrame() {
	int ret;
	int skip;

	memset(&mXing,0,sizeof(struct xing));
	mXing.flags = 0;
	mTimeElapsed = 0.0;
	mTimeTotal = 0.0;

	qWarning("decodeFirstFrame()");

	while(1) {
		skip = 0;
		while((ret = decodeNextFrameHeader())==DECODE_CONT);
		if(ret==DECODE_SKIP) skip = 1;
		else if(ret==DECODE_BREAK) return false;
		while((ret = decodeNextFrame())==DECODE_CONT);
		if(ret==DECODE_BREAK) return false;
		if(!skip && ret==DECODE_OK) break;
	}

	if (parse_xing(&mXing,mMadStream.anc_ptr,mMadStream.anc_bitlen)) {
		if (mXing.flags & XING_FRAMES) {
			mad_timer_t duration = mMadFrame.header.duration;
			mad_timer_multiply(&duration, mXing.frames);

			mTimeTotal = ((float)mad_timer_count(duration, MAD_UNITS_MILLISECONDS))/1000;
			audioFile()->setTotalSamples(mXing.frames * 32 * MAD_NSBSAMPLES(&mMadFrame.header));
			qDebug() << "xing frames: " << mXing.frames;
		}
	}
	else {
		size_t offset = mInputFile.pos();
#if 0
		mad_timer_t duration = mMadFrame.header.duration;
		float frameTime = ((float)mad_timer_count(duration,
					MAD_UNITS_MILLISECONDS))/1000;
#endif
		if(mMadStream.this_frame!=NULL) {
			offset -= mMadStream.bufend-mMadStream.this_frame;
		}
		else {
			offset -= mMadStream.bufend-mMadStream.buffer;
		}
		if(mInputFile.size() >= offset) {
			mTimeTotal = ((mInputFile.size()-offset)*8.0)/mMadFrame.header.bitrate;
			audioFile()->setTotalSamples((quint32)(mTimeTotal * mMadFrame.header.samplerate));
		}
		else {
/*			data->maxFrames = FRAMES_CUSHION;
			data->totalTime = 0;*/
		}
	}

	qWarning("total time: %f total samples: %d\n", mTimeTotal, audioFile()->totalSamples());
	return true;
}



bool AudioDecoderMp3::seekToTimeInternal(quint32 inMilliSeconds) {
	int newFrame=0;

	mAudioStorage.clear();

	while (newFrame < mHighestFrame && inMilliSeconds > ((float)mad_timer_count(mSeekTable[newFrame].time, MAD_UNITS_MILLISECONDS)))
		newFrame++;
	if (newFrame < mHighestFrame) {
		mInputFile.seek(mSeekTable[newFrame].frameOffset);
		mad_stream_buffer(&mMadStream, mBufferRead, 0);
		mMadStream.error = MAD_ERROR_NONE;
		mCurrentFrame = newFrame;
	} else {
		while (1) {
			
			int ret;
			while((ret = decodeNextFrameHeader())==DECODE_CONT);
			if (ret == DECODE_BREAK) {
				return false;
			}
			
			while (newFrame < mHighestFrame && inMilliSeconds > ((float)mad_timer_count(mSeekTable[newFrame].time, MAD_UNITS_MILLISECONDS)))
				newFrame++;
			
			if (newFrame < mHighestFrame) {
				mInputFile.seek(mSeekTable[newFrame].frameOffset);
				mad_stream_buffer(&mMadStream, mBufferRead, 0);
				mMadStream.error = MAD_ERROR_NONE;
				mCurrentFrame = newFrame;
				break;
			}
		}
	}

	return true;
}


AudioDecoder::DecodingStatus AudioDecoderMp3::getDecodedChunk(AudioBuffer *inOutAudioBuffer) {

	
	if (inOutAudioBuffer->state() != AudioBuffer::eEmpty) {
		qDebug("AudioDecoderMp3: AudioBuffer in wrong state!");
		return eContinue;
	}

	if (!inOutAudioBuffer->prepareForDecoding()) {
		qDebug("AudioBuffer::prepareForDecoding() failed.");
		return eContinue;
	}

	AudioDecoder::DecodingStatus status = eContinue;
	while (mAudioStorage.needData(inOutAudioBuffer->requestedLength()) == false) {

		mad_synth_frame(&mMadSynth,&mMadFrame);
		
		QByteArray a;
		quint32 len = mMadSynth.pcm.length * audioFormat().channels() * (audioFormat().bitsPerSample()/8);
		a.resize(len);
		char * ptr;
		ptr = a.data();
		
		for (int i=0; i < mMadSynth.pcm.length; i++) {
			short* sample;
			
			sample = (short*)ptr;
			*sample = (short)audio_linear_dither(16,
					mMadSynth.pcm.samples[0][i],
					&mDither);

			ptr+=2;
			
			if (audioFormat().channels()==2) {
				sample = (short*)ptr;
				*sample = (short)audio_linear_dither(16,
						mMadSynth.pcm.samples[1][i],
						&mDither);
				ptr+=2;
			}
		}
	
		mAudioStorage.add(a, a.length());

		while(true) {
			bool skipFrame = false;
			int	ret;
			
			while((ret = decodeNextFrameHeader())==DECODE_CONT);
			
			if (ret == DECODE_BREAK) {
				qWarning("decodeNextFrameHeader() returned DECODE_BREAK");
				status = eStop;
				break;
			}
			else if (ret == DECODE_SKIP) {
				skipFrame = true;
			}
			
			while((ret = decodeNextFrame())==DECODE_CONT);
			
			if (ret == DECODE_BREAK) {
				qWarning("decodeNextFrame() returned DECODE_BREAK");
				status = eStop;
				break;
			}
			
			if (!skipFrame && ret == DECODE_OK) {
				break;
			}
		}
		
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

bool AudioDecoderMp3::canDecode(const QString &inFilePath) {

	QFile file(inFilePath);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}

	/*
	 * there are no magic bytes, so we just have to pretend
	 * that we could play the file. sorry.
	 *
	 * this check is a hack - we only accept MP3 files if they
	 * end with ".mp3". prevents some problems, and isn't too bad,
	 * I guess.
	 */
	if (!inFilePath.endsWith(".mp3", Qt::CaseInsensitive)) {
		return false;
	}

	file.seek(fileId3V2TagSize(file));
	
	QByteArray startOfFile = file.read(4);
	if (startOfFile.size() != 4)
		return false;

	// MPEG ADTS layer III v1 header
	if ((unsigned char)startOfFile[0] != 0xFF && ((unsigned char)startOfFile[1]&0xFE != 0xFA))
		return false;
	
	return true;
}

bool AudioDecoderMp3::readInfo() {
	qWarning("entered readInfo()");

	if (!openFile())
		return false;

	closeFile();

	return true;
}

QString AudioDecoderMp3::audioFormatDescription() {
	return "MPEG Audio Layer 3";
}

QStringList AudioDecoderMp3::audioFormatFileExtensions() {
	return QStringList() << "*.mp3";
}


#include "ffsubtitledecoder.h"
#include "mediaplayertypes.h"

FFSubtitleDecoder::FFSubtitleDecoder()
	: DecoderBase()
{
}

FFSubtitleDecoder::~FFSubtitleDecoder()
{
}

bool FFSubtitleDecoder::run()
{
	Frame* sp = nullptr;
	if (!(sp = frame_queue_peek_writable(&m_videoState->subpq)))
		return false;

	int got_subtitle = -1;
	if ((got_subtitle = decoder_decode_frame(&m_videoState->subdec, NULL, &sp->sub)) < 0)
		return false;

	int pts = 0;

	if (got_subtitle && sp->sub.format == 0) {
		if (sp->sub.pts != AV_NOPTS_VALUE)
			pts = sp->sub.pts / (double)AV_TIME_BASE;
		sp->pts = pts;
		sp->serial = m_videoState->subdec.pkt_serial;
		sp->width = m_videoState->subdec.avctx->width;
		sp->height = m_videoState->subdec.avctx->height;
		sp->uploaded = 0;

		/* now we can update the picture count */
		frame_queue_push(&m_videoState->subpq);
	}
	else if (got_subtitle) {
		avsubtitle_free(&sp->sub);
	}
	return true;
}

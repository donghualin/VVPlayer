#pragma once

#include "decoderbase.h"

class FFSubtitleDecoder : public DecoderBase
{
public:
	FFSubtitleDecoder();
	~FFSubtitleDecoder();

protected:
	bool run() override;
};


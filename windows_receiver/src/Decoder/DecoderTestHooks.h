#pragma once
// Windows-only dependency seam for deterministic Media Foundation contract tests.
#include "VideoDecoder.h"
#include <mftransform.h>
#include <wrl/client.h>
#include <functional>
namespace thunderlink {
using DecoderCandidateFactory = std::function<std::vector<Microsoft::WRL::ComPtr<IMFTransform>>(TlnkCodec, bool)>;
std::unique_ptr<VideoDecoder> CreateMediaFoundationDecoderForTesting(DecoderCandidateFactory factory);
}

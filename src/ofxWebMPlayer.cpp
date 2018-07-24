#include "ofxWebMPlayer.h"

#include <vorbis/codec.h>
#include "vpx_decoder.h"
#include "vp8dx.h"
#include "intern_webm_reader.h"
#include "shader/intern_shader.h"

float 	pan;
int		sampleRate;
bool 	bNoise = true;
float 	volume;

//------------------- for the simple sine wave synthesis
float 	targetFrequency;
float 	phase;
float 	phaseAdder;
float 	phaseAdderTarget;

void gf_trace_codec_error(vpx_codec_ctx_t *ctx, char const* cstr_prefix)
{
	char const* cstr_detail = vpx_codec_error_detail(ctx);
	ofLogError("ofxWebMPlayer", "%s\nvpx_error- %s\n%s", cstr_prefix, vpx_codec_error(ctx), cstr_detail ? cstr_detail : "");
}

typedef struct VpxFrameInfo
{
	u32 pos;
	u32 len;
	s32 idx_key;
} VpxFrameInfo;

typedef struct AudioInfo
{
	u32 sample_rate;
	u32 samples_per_channel;
	u32 num_of_channel;
	u32 bits_per_sample;
	u32 total_samples;
} AudioInfo;

typedef struct AudioHeader
{
	u64 size_of_buffer;
	u64 timestamp;
} AudioHeader;

struct ofxWebMPlayer::VpxMovInfo
{
	f32 frame_rate;
	f32 duration_s;
	u32 frame_count;
	u32 ms_per_frame;
	//u32 length;

	f32 width;
	f32 height;

	u32	planes_count;
	f32 planes_width[4];
	f32 planes_height[4];
	f32 chroma_shift[2];

	vpx_codec_dec_cfg	vpx_cfg;
	vpx_codec_ctx_t     vpx_ctx;
	vpx_codec_iface_t*  vpx_if;
	s32                 vpx_flags;

	u64 pre_tick_millis;
	u64 total_tick_mills;
	s32 cur_mov_frame_idx;

	std::vector<VpxFrameInfo>	box_vpx_frame_info;
	std::vector<u32>			box_key;
	std::shared_ptr<MemBlock>	sp_mb_movie_body;
	std::shared_ptr<MemBlock>	sp_mb_wav_body;

	AudioInfo					audio_info;
	bool						has_audio;
	bool						has_video;
	bool						is_audio_end;
	u64							audio_cur_ptr;
	std::atomic<u64>			accum_samples;
	std::atomic<u64>			audio_timestamp;
};

char const* g_sampler1d_name[4] =
{
	"tex_y",
	"tex_u",
	"tex_v",
	"tex_alpha",
};

ofxWebMPlayer::ofxWebMPlayer() 
{
	enum { VpxMovInfoSize = sizeof(VpxMovInfo) };
	static_assert(MaxMovInfoInsSize >= VpxMovInfoSize, "The size of the instance is not enough.");

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
	memset(&ms_info, 0x00, sizeof(ms_info));

#endif
	m_vpx_mov_info = ::new(m_mov_info_instance) VpxMovInfo;
	m_vpx_mov_info->vpx_if = NULL ;
	m_vpx_mov_info->has_audio = false;
	m_vpx_mov_info->has_video = false;

	m_is_paused = false;
	m_is_playing = false;
	m_is_frame_new = false;
	m_is_loop = false;

	m_enable_audio = false;
}

ofxWebMPlayer::~ofxWebMPlayer()
{
	mf_unload();
	m_vpx_mov_info->~VpxMovInfo();
}

namespace vorbis 
{
	typedef struct Header
	{
		ogg_packet pack;

	} Header;

	typedef struct Data
	{
		vorbis_info         info;
		vorbis_comment      comment;
		vorbis_dsp_state    dsp_state;
		vorbis_block        block;
		Header              hdrId;
		Header              hdrComment;
		Header              hdrSetup;
	} Data;

	class Decoder
	{
	public:
		Decoder();
		virtual ~Decoder();

		bool init(Header const& id, Header const& comment, Header const& setup);

		void destroy();

		bool decode(ogg_packet* p_pack);
		s32 outputPCM(std::shared_ptr<MemBlock> mem_block, u32 fromIdx, u64 timestamp);

		//TMP don't recommand use those//
		s32 getNumSamplesOfPCM_Buffer();
		void clearPCM_Buffer();
		bool clearDspBuffer();
		//----------------------------//

		inline bool isInitialized() { return m_isInit; }
		inline bool isEmpty() { return m_isEmpty; }
		inline s32 getChannels() { return m_data.info.channels; }
		inline s32 getRate() { return m_data.info.rate; }
		inline s32 getRecommendSteamingBufferSize() { return m_data.dsp_state.pcm_storage; }

	private:
		Data m_data;
		bool m_isEmpty;
		bool m_isInit;
		//TMP
		s32 m_numSamples;
	};

	class OggPacketStreamer
	{
	public:
		virtual ~OggPacketStreamer() {}
		virtual void reset() = 0;
		virtual bool getPacket(ogg_packet&, u64* timestamp) = 0;
		virtual bool isEnd() = 0;
		virtual void push() = 0;
		virtual void pop() = 0;
	};
}

namespace vorbis 
{
	//void initHeaderFromOggPacket(Header& hdr, ogg_packet const* pPack)
	//{
	//	ASSERT(pPack);
	//	hdr.pack = *pPack;
	//	hdr.mem = rr_allocMemBlock(RR_HEAP_00, pPack->bytes);
	//	hdr.pack.packet = hdr.mem->getBuffer();
	//	memcpy(hdr.pack.packet, pPack->packet, pPack->bytes);
	//}

	Decoder::Decoder()
		: m_isEmpty(true)
		, m_isInit(false)
		, m_numSamples(0)
	{}

	Decoder::~Decoder()
	{
		destroy();
	}

	bool Decoder::init(Header const& id, Header const& comment, Header const& setup)
	{
		destroy();

		vorbis_info_init(&m_data.info);
		vorbis_comment_init(&m_data.comment);

		m_data.hdrId = id;
		m_data.hdrComment = comment;
		m_data.hdrSetup = setup;

		int result = vorbis_synthesis_headerin(&m_data.info, &m_data.comment, &m_data.hdrId.pack);
		if (result < 0)
		{
			ofLogError("vorbis::Decoder", "init(): header id is wrong.");
			return false;
		}

		result = vorbis_synthesis_headerin(&m_data.info, &m_data.comment, &m_data.hdrComment.pack);
		if (result < 0)
		{
			ofLogError("vorbis::Decoder", "init(): header comment is wrong.");
			return false;
		}

		result = vorbis_synthesis_headerin(&m_data.info, &m_data.comment, &m_data.hdrSetup.pack);
		if (result < 0)
		{
			ofLogError("vorbis::Decoder", "init(): header setup is wrong.");
			return false;
		}

		//OK, got and parsed all three headers. Initialize the Vorbis
		//packet->PCM decoder.

		result = vorbis_synthesis_init(&m_data.dsp_state, &m_data.info); // central decode state  
		if (result != 0)
		{
			return false;
		}

		result = vorbis_block_init(&m_data.dsp_state, &m_data.block); //local state for most of the decode 
																	//so multiple block decodes can proceed in parallel. 
																	//We could init multiple vorbis_block structures for vd here.
		if (result != 0)
		{
			return false;
		}

		m_isInit = true;

		return true;
	}

	void Decoder::destroy()
	{
		if (!m_isInit) return;

		vorbis_block_clear(&m_data.block);
		vorbis_dsp_clear(&m_data.dsp_state);
		vorbis_comment_clear(&m_data.comment);
		vorbis_info_clear(&m_data.info);

		m_numSamples = 0;
		m_isEmpty = true;
		m_isInit = false;
	}

	bool Decoder::decode(ogg_packet* p_pack)
	{
		if (!m_isInit) return false;

		//we have a packet.  Decode it
		if (vorbis_synthesis(&m_data.block, p_pack) == 0) // test for success!
		{
			vorbis_synthesis_blockin(&m_data.dsp_state, &m_data.block);
			m_isEmpty = false;
			return true;
		}

		return false;
	}

#define USE_S16 0

#if USE_S16
	typedef ogg_int16_t type_of_sample;
#else
	typedef float type_of_sample;
#endif

	s32 Decoder::outputPCM(std::shared_ptr< MemBlock > mem_block, u32 fromIdx, u64 timestamp)
	{
		if (!m_isInit) return -1;

		float **pcm;
		s32 availableSize = static_cast<s32>(mem_block->get_size() - fromIdx);
		if (availableSize <= 0) return -1;

		//s32 availableSamples = availableSize / sizeof(type_of_sample) / m_data.info.channels;

		u8* ptr = mem_block->get_buffer() + fromIdx;

		//AudioHeader* p_header = (AudioHeader*)(ptr);
		//type_of_sample* out_buffer = (type_of_sample*)(ptr + sizeof(AudioHeader));
		type_of_sample* out_buffer = (type_of_sample*)(ptr);

		bool isClip = 0;

		//**pcm is a multichannel float vector.  In stereo, for
		//example, pcm[0] is left, and pcm[1] is right.  samples is
		//the size of each channel.  Convert the float values
		//(-1.<=range<=1.) to whatever PCM format and write it out

		s32 samples = vorbis_synthesis_pcmout(&m_data.dsp_state, &pcm);

		if (samples > 0)
		{
			int clipflag = 0;
			s32 bout = samples;//(samples < availableSamples ? samples : availableSamples);
#if USE_S16
			//convert floats to 16 bit signed ints (host order) and
			//interleave
			for (int i = 0; i < m_data.info.channels; ++i)
			{
				ogg_int16_t *ptr = out_buffer + i;
				float  *mono = pcm[i];

				for (int j = 0; j < bout; ++j)
				{
#if 1
					s32 val = (s32)floor(mono[j] * 32767.f + 0.5f);
#else // optional dither 
					int val = mono[j] * 32767.f + drand48() - 0.5f;
#endif
					// might as well guard against clipping;
					if (val>32767)
					{
						val = 32767;
						isClip = true;
					}

					if (val<-32768)
					{
						val = -32768;
						isClip = true;
					}

					*ptr = val;
					ptr += m_data.info.channels;
				}
			}

			if (isClip)
			{
				//printf("Clipping in frame %ld\n",(long)(_data.dsp_state.sequence));
			}
#else

			//convert floats to float
			//src = llllrrrr
			//dst = lrlrlrlr
			//interleave
			for (int i = 0; i < m_data.info.channels; ++i)
			{
				type_of_sample *ptr = out_buffer + i;
				float  *mono = pcm[i];
			
				for (int j = 0; j < bout; ++j)
				{
					float val = mono[j];
			
					// might as well guard against clipping;
					if (val > 1.f)
					{
						val = 1.f;
						isClip = true;
					}
			
					if (val < -1.f)
					{
						val = -1.f;
						isClip = true;
					}
			
					*ptr = val;
					ptr += m_data.info.channels;
				}
			}
			
			if (isClip)
			{
				ofLogVerbose("ofxWebMPlayer", "Clipping in frame %ld\n",(long)(m_data.dsp_state.sequence));
			}

#endif
			// tell libvorbis how many samples we actually consumed;
			vorbis_synthesis_read(&m_data.dsp_state, bout);
			m_isEmpty = (bout == samples);

			//p_header->size_of_buffer = bout * m_data.info.channels * sizeof(type_of_sample);
			//p_header->timestamp = timestamp;
			return bout;
		}
		else
		{
			m_isEmpty = true;
			return samples;
		}
	}

	s32 Decoder::getNumSamplesOfPCM_Buffer()
	{
		if (!m_isInit) return -1;

		float **pcm;
		m_numSamples = vorbis_synthesis_pcmout(&m_data.dsp_state, &pcm);
		return m_numSamples;
	}

	void Decoder::clearPCM_Buffer()
	{
		if (!m_numSamples) return;

		vorbis_synthesis_read(&m_data.dsp_state, m_numSamples);
		m_numSamples = 0;
	}

	bool Decoder::clearDspBuffer()
	{
		if (!m_isInit) return false;

		//vorbis_block_clear();
		vorbis_dsp_clear(&m_data.dsp_state);
		s32 result = vorbis_synthesis_init(&m_data.dsp_state, &m_data.info); // central decode state  
		if (result != 0)
		{
			return false;
		}
		return true;
	}	



	void getOggTotalNumSampleAndNumPackage(OggPacketStreamer* pOPStreamer, Decoder* pDecoder, u64* p_samples, u64* p_packages)
	{
		//ASSERT(pOPStreamer);
		//ASSERT(pDecoder);

		pOPStreamer->push();
		pOPStreamer->reset();

		ogg_packet pack;
		u64 totalSamples = 0;
		u64 totalPackages = 0;
		while (!pOPStreamer->isEnd())
		{
			if (pOPStreamer->getPacket(pack, NULL))
			{
				++totalPackages;
				bool yes = pDecoder->decode(&pack);

				s32 s = pDecoder->getNumSamplesOfPCM_Buffer();

				totalSamples += s;
				pDecoder->clearPCM_Buffer();
			}
		}

		pDecoder->clearDspBuffer();
		pOPStreamer->pop();
		if (p_samples)
		{
			*p_samples = totalSamples;
		}

		if (p_packages)
		{
			*p_packages = totalPackages;
		}
	}

	u64 getOggTotalNumSamples(OggPacketStreamer* pOPStreamer, Decoder* pDecoder)
	{
		//ASSERT(pOPStreamer);
		//ASSERT(pDecoder);

		pOPStreamer->push();
		pOPStreamer->reset();

		ogg_packet pack;
		u64 totalSamples = 0;
		while (!pOPStreamer->isEnd())
		{
			if (pOPStreamer->getPacket(pack, NULL))
			{
				bool yes = pDecoder->decode(&pack);

				s32 s = pDecoder->getNumSamplesOfPCM_Buffer();

				totalSamples += s;
				pDecoder->clearPCM_Buffer();
			}
		}

		pDecoder->clearDspBuffer();
		pOPStreamer->pop();
		return totalSamples;
	}

	bool readOggPakcetStreamer(AudioInfo* p_audio_info, std::shared_ptr<MemBlock> mem_block, OggPacketStreamer* pOPStreamer, Decoder* pDecoder)
	{
		u32 bytesPerSample = sizeof(type_of_sample);
		u32 bytesPerOggSample = bytesPerSample * pDecoder->getChannels();

		u64 OggSamples;
		u64 OggPackages;
		vorbis::getOggTotalNumSampleAndNumPackage(pOPStreamer, pDecoder, &OggSamples, &OggPackages);

		//s64 sizeBuffer = OggSamples * bytesPerOggSample + (OggPackages * sizeof(AudioHeader));
		s64 sizeBuffer = OggSamples * bytesPerOggSample;

		mem_block->alloc(sizeBuffer);
		p_audio_info->sample_rate			= pDecoder->getRate();
		p_audio_info->samples_per_channel	= static_cast<u32>(OggSamples / pDecoder->getChannels());
		p_audio_info->num_of_channel		= pDecoder->getChannels();
		p_audio_info->bits_per_sample		= bytesPerSample * 8;
		p_audio_info->total_samples			= OggSamples;
		
		ogg_packet pack;

		u64 timestamp;
		u32 idxCur = 0;
		while (!pOPStreamer->isEnd() && pOPStreamer->getPacket(pack, &timestamp))
		{
			bool yes = pDecoder->decode(&pack);
			s32 samples = pDecoder->outputPCM(mem_block, idxCur, timestamp);
			if (samples)
			{
				//idxCur += samples * bytesPerOggSample + sizeof(AudioHeader);
				idxCur += samples * bytesPerOggSample;
			}
		}

		return true;
	}

}//namespace vorbits 

class OggPacketStreamerForWebm : public vorbis::OggPacketStreamer
{
public:
	OggPacketStreamerForWebm(std::shared_ptr<MemBlock>  rspBuffer, mkvparser::AudioTrack const* p)
	: m_rspBuffer(rspBuffer)
	, m_pAudioTrack(p)
	, m_packetCount(3) //other packet is header//
	, m_packetCountPush(3)
	, m_pBlockEtyCur(nullptr)
	, m_pBlockEtyPush(nullptr)
	, m_isEnd(false)
	, m_frameCount(0)
	{
		m_pAudioTrack->GetFirst(m_pBlockEtyCur);
	}

	virtual ~OggPacketStreamerForWebm() {}

	void reset() override
	{
		m_pAudioTrack->GetFirst(m_pBlockEtyCur);
		m_packetCount = 3;
		m_isEnd = false;
	}

	bool getPacket(ogg_packet& pack, u64* p_timestamp) override
	{
		if (!m_pBlockEtyCur)
		{
			m_isEnd = true;
			return false;
		}

		if (m_pBlockEtyCur->EOS())
		{
			m_isEnd = true;
			return false;
		}

		mkvparser::Block const* pBlock = m_pBlockEtyCur->GetBlock();
		//long long time_ns = pBlock->GetTime(m_pBlockEtyCur->GetCluster());

		if (p_timestamp)
		{
			*p_timestamp = pBlock->GetTime(m_pBlockEtyCur->GetCluster());
		}

		if (!pBlock) return false;

		if (pBlock->GetFrameCount() <= 0) return false;
		int num = pBlock->GetFrameCount();

		if (m_frameCount == 0) m_frameCount = num;

		u32 idx = num - (m_frameCount--);
		mkvparser::Block::Frame const& frame = pBlock->GetFrame(idx);

		pack.b_o_s = 0;
		pack.bytes = (s32)frame.len;
		pack.e_o_s = 0;
		pack.granulepos = -1;
		pack.packet = m_rspBuffer->get_buffer() + frame.pos;
		pack.packetno = m_packetCount;

		++m_packetCount;

		if (m_frameCount == 0)
		{
			m_pAudioTrack->GetNext(m_pBlockEtyCur, m_pBlockEtyCur);
			if (!m_pBlockEtyCur || m_pBlockEtyCur->EOS())
			{
				pack.e_o_s = 512;
				m_isEnd = true;
			}
		}

		return true;
	}

	bool isEnd() override
	{
		return m_isEnd;
	}

	void push() override
	{
		//ASSERT(m_pBlockEtyPush == nullptr);
		m_pBlockEtyPush = m_pBlockEtyCur;
		m_packetCountPush = m_packetCount;
		m_isEndPush = m_isEnd;
	}

	void pop() override
	{
		if (!m_pBlockEtyPush) return;

		m_pBlockEtyCur = m_pBlockEtyPush;
		m_packetCount = m_packetCountPush;
		m_isEnd = m_isEndPush;
		m_pBlockEtyPush = nullptr;
	}
private:
	std::shared_ptr<MemBlock> m_rspBuffer;
	mkvparser::AudioTrack const* m_pAudioTrack;
	mkvparser::BlockEntry const* m_pBlockEtyCur;
	mkvparser::BlockEntry const* m_pBlockEtyPush;
	u32 m_packetCount;
	u32 m_packetCountPush;
	u32 m_frameCount;
	bool m_isEnd;
	bool m_isEndPush;
};

void ofxWebMPlayer::enableAudio(bool yes)
{
	m_enable_audio = yes;
}

bool ofxWebMPlayer::load(string name)
{
	//mf_unload();
	WebMReader reader;

	{
		ofFile file(name, ofFile::ReadOnly, true);
		bool yes = reader.Setup(file);
		if (!yes)
		{
			return false;
		}
	}

	do
	{
		s64 pos;
		mkvparser::EBMLHeader ebml_header;
		s64 ret = ebml_header.Parse(&reader, pos);
		if (ret < 0)
		{
			ofLogError("ofxWebMPlayer", "ofxWebMPlayer::load(): This file [%s] is not WebM format", name.c_str());
			break;
		}

		mkvparser::Segment* p_segment;
		ret = mkvparser::Segment::CreateInstance(&reader, pos, p_segment);
		if (ret < 0)
		{
			ofLogError("ofxWebMPlayer", "load(): WebM Segment::CreateInstance() failed.");
			break;
		}

		ret = p_segment->Load();
		if (ret < 0)
		{
			ofLogError("ofxWebMPlayer", "load(): WebM Segment::Load() failed.");
			break;
		}

		mkvparser::Tracks const* p_tracks = p_segment->GetTracks();
		u32 const num_tracks = p_tracks->GetTracksCount();
		m_vpx_mov_info->sp_mb_movie_body = reader.GetMemBlockSptr();

		for (u32 i = 0; i < num_tracks; ++i)
		{
			mkvparser::Track const* const p_track = p_tracks->GetTrackByIndex(i);
			if (p_track == NULL)
			{
				continue;
			}

			s32 const track_type = p_track->GetType();

			switch (track_type)
			{
			case mkvparser::Track::kVideo:
			{
				const char* codec_id = p_track->GetCodecId();
				vpx_codec_iface_t* p_iface = NULL;

				if (strcmp(codec_id, "V_VP8") == 0)
				{
					p_iface = vpx_codec_vp8_dx();
				}
				else if (strcmp(codec_id, "V_VP9") == 0)
				{
					p_iface = vpx_codec_vp9_dx();
				}
				else
				{
					ofLogError("ofxWebMPlayer", "load()-video: This codec [%s] is not support.", codec_id);
					continue;
				}

				m_vpx_mov_info->vpx_flags = 0;
				// Initialize codec
				m_vpx_mov_info->vpx_cfg.threads = 8;
				m_vpx_mov_info->vpx_cfg.w = 0;
				m_vpx_mov_info->vpx_cfg.h = 0;

				vpx_codec_err_t err = vpx_codec_dec_init(&m_vpx_mov_info->vpx_ctx, p_iface, &m_vpx_mov_info->vpx_cfg, m_vpx_mov_info->vpx_flags);
				if (err)
				{
					gf_trace_codec_error(&m_vpx_mov_info->vpx_ctx, "load()-video: Failed to initialize the decoder of VPX.");
					continue;
				}

				m_vpx_mov_info->vpx_if = p_iface;

				ofLogNotice("ofxWebMPlayer", "load()-video: Now vpx codec is using %s.", vpx_codec_iface_name(m_vpx_mov_info->vpx_if));

				mkvparser::VideoTrack const* const pVideoTrack = static_cast<const mkvparser::VideoTrack*>(p_track);

				m_vpx_mov_info->frame_rate = static_cast<f32>(pVideoTrack->GetFrameRate());
				m_vpx_mov_info->frame_count = 0;
				m_vpx_mov_info->width = static_cast<u32>(pVideoTrack->GetWidth()); //Pixels width//
				m_vpx_mov_info->height = static_cast<u32>(pVideoTrack->GetHeight()); //Pixels height//

				mkvparser::BlockEntry const* pBlockEty = NULL;
				pVideoTrack->GetFirst(pBlockEty);

				u32 idxKey = 0;

				while (pBlockEty && !pBlockEty->EOS())
				{
					mkvparser::Block const* pBlock = pBlockEty->GetBlock();

					mkvparser::BlockEntry::Kind k = pBlockEty->GetKind();
					if (k == mkvparser::BlockEntry::kBlockGroup)
					{
						int c = 0;
					}

					//s64 tCode = pBlock->GetTimeCode(pBlockEty->GetCluster());
					//s64 t = pBlock->GetTime(pBlockEty->GetCluster());
					if (pBlock)
					{
						if (pBlock->IsKey())
						{
							idxKey = static_cast<u32>(m_vpx_mov_info->box_vpx_frame_info.size());
							m_vpx_mov_info->box_key.push_back(idxKey);
						}

						for (s32 fIdx = 0; fIdx < pBlock->GetFrameCount(); ++fIdx)
						{

							if (fIdx > 0)
							{
								int c = 0;
							}

							mkvparser::Block::Frame const& frame = pBlock->GetFrame(fIdx);

							VpxFrameInfo f_info;
							f_info.pos = (u32)frame.pos;
							f_info.len = frame.len;
							f_info.idx_key = idxKey;

							m_vpx_mov_info->box_vpx_frame_info.push_back(f_info);
						}
						m_vpx_mov_info->frame_count += pBlock->GetFrameCount();
					}

					pVideoTrack->GetNext(pBlockEty, pBlockEty);
				}

				u64 duration_ns_per_frame = pVideoTrack->GetDefaultDuration();
				if (duration_ns_per_frame)
				{
					//mkvparser::SegmentInfo const* const pSegmentInfo = p_segment->GetInfo();
					////u64 const timeCodeScale = pSegmentInfo->GetTimeCodeScale();
					//u64 const duration_ns = pSegmentInfo->GetDuration();
					m_vpx_mov_info->ms_per_frame = duration_ns_per_frame / 1000000;
					m_vpx_mov_info->duration_s = static_cast<f32>(duration_ns_per_frame / 1000000000.0) * m_vpx_mov_info->frame_count;
					m_vpx_mov_info->frame_rate = m_vpx_mov_info->frame_count / m_vpx_mov_info->duration_s;
				}
				else if (m_vpx_mov_info->frame_rate)
				{
					m_vpx_mov_info->ms_per_frame = 1000000000.0 / m_vpx_mov_info->frame_rate;
					m_vpx_mov_info->duration_s = m_vpx_mov_info->frame_count / m_vpx_mov_info->frame_rate;
				}
				else
				{
					mkvparser::SegmentInfo const* const pSegmentInfo = p_segment->GetInfo();
					u64 const duration_ns = pSegmentInfo->GetDuration();
					duration_ns_per_frame = duration_ns / m_vpx_mov_info->frame_count;

					m_vpx_mov_info->duration_s = static_cast<f32>(duration_ns / 1000000000.0);
					m_vpx_mov_info->ms_per_frame = duration_ns_per_frame / 1000000;
					m_vpx_mov_info->frame_rate = m_vpx_mov_info->frame_count / m_vpx_mov_info->duration_s;
				}

				m_vpx_mov_info->has_video = true;
			}
			break;


			case mkvparser::Track::kAudio:
				//ofLogError("ofxWebMPlayer", "load(): Audio track is not implemented yet.");
				{
					if (!m_enable_audio)
					{
						continue;
					}

					using namespace mkvparser;

					AudioTrack const* const p_audio_track = static_cast<AudioTrack const*>(p_track);

					size_t size_of_codec_private;
					u8* p_data_codec_private = (u8*)p_audio_track->GetCodecPrivate(size_of_codec_private);

					// http://matroska.org/technical/specs/codecid/index.html find "A_VORBIS"
					//
					// When you want to decode vorbis, you need three header.
					// Those are Identification header, Comment header and Setup header.
					// This codec private contains these information.
					//
					// https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-610004.2.1
					//
					//
					// data format is base on Xiph lacing
					// http://matroska.org/technical/specs/index.html#lacing
					//
					// the byte 1 = number of the packets - 1. 
					// the byte 2 -> n = the sizes of packets.
					// the size of the last one packet can be deduced from the total size. 
					//
					// the size will be coded like: 
					// if the size is 800, the code will be 255 255 255 35.
					// so 255 + 255 + 255 + 35 = 800.
					//
					// the byte n+1 -> end = the raw data of three header packets.
					//
					// example: 5 packets, A, B, C, D, E
					// the size of A = 77,
					// the size of B = 800,
					// the size of C = 510,
					// the size of D = 256,
					// the size of E = 3333,
					//
					// the bytes will be:
					// 1    2               =>                            n    n+1 => end
					// 4, [77], [255, 255, 255, 35], [255, 255, 0], [255, 1], [raw data]
					//
					// ps: E_size = total size - (A_size + B_size + C_size + D_size);

					// Because this codec private is for A_VORBIS
					// byte 1 is must 2 (3 header)
					// the size of Identification header must be 30 bytes,
					// byte 2 is must 30,
					// but the size of other header is unstable, so you need to check.
					vorbis::Header hdr_id, hdr_comment, hdr_setup;

					hdr_id.pack.b_o_s = 0;
					hdr_id.pack.bytes = 0;
					hdr_id.pack.e_o_s = 0;
					hdr_id.pack.granulepos = 0;
					hdr_id.pack.packet = nullptr;
					hdr_id.pack.packetno = 0;

					hdr_setup.pack = hdr_comment.pack = hdr_id.pack;
					hdr_id.pack.b_o_s = 256;
					hdr_comment.pack.packetno = 1;
					hdr_comment.pack.granulepos = -1;
					hdr_setup.pack.packetno = 2;

					u32 size_hdr_id, size_hdr_comment;

					u8* begin = p_data_codec_private;
					u8* end = begin + size_of_codec_private;
					u8* ptr = begin;

					if (*ptr++ != 2)
					{
						ofLogError("ofxWebMPlayer", "load()-audio: error");
						continue;
					}

					//ps: the size of header id must be 30;
					size_hdr_id = *ptr++;
					if (size_hdr_id != 30)
					{
						ofLogError("ofxWebMPlayer", "load()-audio: error");
						continue;
					}

					//The comment header holds the Ogg metadata for an audio track,
					//and so in principle it can be any length. Here that means that
					//the length can be represented in the stream using a sequence
					//comprising multiple bytes, so to determine the length we must
					//loop until we find a byte whose value is less than 255.

					//decode the size for header comment;
					size_hdr_comment = 0;
					bool is_overflow = false;

					for (;;)
					{
						u8 value = *ptr++;

						if (ptr >= end)
						{
							is_overflow = true;
							break;
						}

						size_hdr_comment += value;

						if (value < 255)
							break;
					}

					if (is_overflow)
					{
						ofLogError("ofxWebMPlayer", "load()-audio: error");
						continue;
					}

					//Each vorbis header begins with a byte having a distinguished
					//value that specifies what kind of header this is, followed
					//by the string "vorbis".  Therefore each well-formed header
					//must be at least 7 bytes long.

					if (size_hdr_comment < 7)
					{
						ofLogError("ofxWebMPlayer", "load()-audio: error");
						continue;
					}

					//We have consumed the sequence of bytes used to represent
					//the lengths of the individual headers.  What remains in
					//the stream are the actual headers.  Here we don't particularly
					//care much about the actual header payload (we defer such
					//matters to the Vorbis decoder), but we do interrogate the
					//first 7 bytes of each header to confirm that the headers
					//have their correct Vorbis header-kind indicators.

					//p points the first header (the ident header)
					//The Vorbis ident header has 1 as its kind indicator.
					if (memcmp(ptr, "\x01vorbis", 7) != 0)
					{
						ofLogError("ofxWebMPlayer", "load()-audio: error");
						continue;
					}

					hdr_id.pack.packet = ptr;
					hdr_id.pack.bytes = size_hdr_id;

					ptr += size_hdr_id;

					//The Vorbis comment header has 3 as its kind indicator.
					if (memcmp(ptr, "\x03vorbis", 7) != 0)
					{
						ofLogError("ofxWebMPlayer", "load()-audio: error");
						continue;
					}

					hdr_comment.pack.packet = ptr;
					hdr_comment.pack.bytes = size_hdr_comment;

					ptr += size_hdr_comment;

					if (memcmp(ptr, "\x05vorbis", 7) != 0)
					{
						ofLogError("ofxWebMPlayer", "load()-audio: error");
						continue;
					}

					hdr_setup.pack.packet = ptr;
					hdr_setup.pack.bytes = static_cast<long>(end - ptr);

					if (hdr_setup.pack.bytes < 7)
					{
						ofLogError("ofxWebMPlayer", "load()-audio: error");
						continue;
					}


					//We are satisfied that the CodecPrivate value is well-formed,
					//and so we now create the audio stream for this movie;
					vorbis::Decoder decoder;
					OggPacketStreamerForWebm opsfw(m_vpx_mov_info->sp_mb_movie_body, p_audio_track);
					bool yes = decoder.init(hdr_id, hdr_comment, hdr_setup);
					if (!yes)
					{
						ofLogError("ofxWebMPlayer", "load()-audio: decoder init failed.");
						continue;
					}

					m_vpx_mov_info->sp_mb_wav_body = std::shared_ptr< MemBlock >(new MemBlock);
					yes = vorbis::readOggPakcetStreamer(&m_vpx_mov_info->audio_info, m_vpx_mov_info->sp_mb_wav_body, &opsfw, &decoder);
					if (!yes)
					{
						m_vpx_mov_info->sp_mb_wav_body = nullptr;
						ofLogError("ofxWebMPlayer", "load()-audio: decoder init failed.");
						continue;
					}

					m_vpx_mov_info->has_audio = true;
				}
				break;

			case mkvparser::Track::kSubtitle:
				ofLogError("ofxWebMPlayer", "load(): Subtitle track is not implemented yet.");
				break;

			case mkvparser::Track::kMetadata:
				ofLogError("ofxWebMPlayer", "load(): Metadata track is not implemented yet.");
				break;
			}
		}

		m_vpx_mov_info->cur_mov_frame_idx = 0;
		m_vpx_mov_info->pre_tick_millis = 0;
		m_vpx_mov_info->total_tick_mills = 0;

		VpxFrameInfo& f_info = m_vpx_mov_info->box_vpx_frame_info[0];
		ret = vpx_codec_decode(&m_vpx_mov_info->vpx_ctx, m_vpx_mov_info->sp_mb_movie_body->get_buffer() + f_info.pos, f_info.len, NULL, 0);
		if (ret < 0)
		{
			gf_trace_codec_error(&m_vpx_mov_info->vpx_ctx, "load(): Failed to decode frame.");
			break;
		}

		vpx_codec_iter_t iter = NULL;
		vpx_image_t* vpxImage = vpx_codec_get_frame(&m_vpx_mov_info->vpx_ctx, &iter);

		if (!vpxImage)
		{
			ofLogError("ofxWebMPlayer", "load(): vpx_codec_get_frame() failed.");
			break;
		}

		//if (vpxImage->fmt != VPX_IMG_FMT_I420)
		//{
		//	ofLogError("ofxWebMPlayer::load(): Only support the fmt I420. failed");
		//	break;
		//}

		m_vpx_mov_info->planes_count = 0;

		for (u32 i = 0; i < 4; ++i)
		{
			if (!vpxImage->planes[i])
			{
				continue;
			}

			m_vpx_mov_info->planes_width[i] = vpxImage->stride[i];

			switch (i)
			{
			case VPX_PLANE_Y:
				m_vpx_mov_info->planes_height[i] = vpxImage->d_h;
				break;

			case VPX_PLANE_U:
			case VPX_PLANE_V:
				if (vpxImage->fmt == VPX_IMG_FMT_I420)
				{
					m_vpx_mov_info->planes_height[i] = vpxImage->d_h / 2;
					m_vpx_mov_info->chroma_shift[0] = 0.5f;
					m_vpx_mov_info->chroma_shift[1] = 0.5f;
				}
				else if (vpxImage->fmt == VPX_IMG_FMT_I444 || vpxImage->fmt == VPX_IMG_FMT_444A)
				{
					m_vpx_mov_info->planes_height[i] = vpxImage->d_h;
					m_vpx_mov_info->chroma_shift[0] = 1.f;
					m_vpx_mov_info->chroma_shift[1] = 1.f;
				}
				else if (vpxImage->fmt == VPX_IMG_FMT_I422)
				{
					m_vpx_mov_info->planes_height[i] = vpxImage->d_h;
					m_vpx_mov_info->chroma_shift[0] = 0.5f;
					m_vpx_mov_info->chroma_shift[1] = 1.f;
				}
				break;
			}

			++m_vpx_mov_info->planes_count;
		}

		char const* src_vert_shader = g_cstr_vert_shader;
		char const* src_frag_shader = NULL;

		GLenum error = glGetError();
		//Clear gl error;
		if (error != GL_NO_ERROR)
		{
			ofLogWarning("ofxWebMPlayer", "load(): Clear GL Error(%d): someone is bad...", error);
		}
		
		memset(m_gl_tex2d_planes, 0x00, sizeof(m_gl_tex2d_planes));
		glGenTextures(4, m_gl_tex2d_planes);
		//glEnable(GL_TEXTURE_2D);

		GLint internalformat;
		GLenum format = GL_NO_ERROR;

		if (vpxImage->fmt & VPX_IMG_FMT_PLANAR)
		{
			internalformat = GL_R8;
			format = GL_RED;

			switch (vpxImage->cs)
			{
			default:
			case VPX_CS_BT_601:
				src_frag_shader = g_cstr_frag_shader601;
				break;

			case VPX_CS_BT_709:
				src_frag_shader = g_cstr_frag_shader709;
				break;
			}
		}
		else if (vpxImage->fmt == VPX_IMG_FMT_ARGB_LE)
		{
			//frag_shader_path += "BGRA";
			internalformat = GL_RGBA;
			format = GL_BGRA;
		}
		else if (vpxImage->fmt == VPX_IMG_FMT_ARGB)
		{
			//frag_shader_path += "ARGB";
			internalformat = GL_RGBA;
			format = GL_BGRA;
		}

		for (u32 i = 0; i < m_vpx_mov_info->planes_count; ++i)
		{
			//glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_gl_tex2d_planes[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, internalformat, m_vpx_mov_info->planes_width[i], m_vpx_mov_info->planes_height[i], 0, format, GL_UNSIGNED_BYTE, vpxImage->planes[i]);

			error = glGetError();
			if (error != GL_NO_ERROR)
			{
				ofLogWarning("ofxWebMPlayer", "glGetError(%d)", error);
			}

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		}

		glBindTexture(GL_TEXTURE_2D, 0);
		//glDisable(GL_TEXTURE_2D);

		ofFbo::Settings settings;
		settings.width = vpxImage->d_w;
		settings.height = vpxImage->d_h;
		settings.textureTarget = GL_TEXTURE_2D;
		settings.internalformat = GL_RGB;

		m_fbo.allocate(settings);

		//setup shader
		{
			bool yes = m_shader.setupShaderFromSource(GL_VERTEX_SHADER, src_vert_shader);
			if (!yes)
			{
				break;
			}

			yes = m_shader.setupShaderFromSource(GL_FRAGMENT_SHADER, src_frag_shader);
			if (!yes)
			{
				break;
			}

			if (ofIsGLProgrammableRenderer()) 
			{
				m_shader.bindDefaults();
			}

			yes = m_shader.linkProgram();
			if (!yes)
			{
				break;
			}
		}

		m_mesh_quard.clear();
		m_mesh_quard.setMode(OF_PRIMITIVE_TRIANGLE_STRIP);
		m_mesh_quard.addVertex(ofVec3f(0.f, 0.f, 0.f));
		m_mesh_quard.addVertex(ofVec3f(0.f, settings.height, 0.f));
		m_mesh_quard.addVertex(ofVec3f(settings.width, 0.f, 0.f));
		m_mesh_quard.addVertex(ofVec3f(settings.width, settings.height, 0.f));

		mf_convert_vpx_img_to_texture(vpxImage);

		return true;

	} while (0); //Failed


	return false;
}

void ofxWebMPlayer::loadAsync(string name)
{

}

void ofxWebMPlayer::play()
{
	if (!m_is_playing)
	{
		m_vpx_mov_info->total_tick_mills = 0;
		if (m_vpx_mov_info->cur_mov_frame_idx == m_vpx_mov_info->frame_count - 1)
		{
			m_vpx_mov_info->cur_mov_frame_idx = -1;
		}

		if (m_vpx_mov_info->has_audio)
		{
			m_vpx_mov_info->audio_cur_ptr = 0;
			m_vpx_mov_info->accum_samples = 0;
			m_vpx_mov_info->is_audio_end = false;

			m_sound_stream.setOutput(this);
			m_sound_stream.setup(m_vpx_mov_info->audio_info.num_of_channel, 0, m_vpx_mov_info->audio_info.sample_rate, 256, 2);

			int width = ofGetWidth();
			pan = (float)width * 0.5f / (float)width;
			float height = (float)ofGetHeight();
			float heightPct = ((height - (height * 0.5f)) / height);
			targetFrequency = 2000.0f * heightPct;
			phaseAdderTarget = (targetFrequency / (float)sampleRate) * TWO_PI;
		}

	}

	m_is_playing = true;
	m_is_paused = false;
	m_vpx_mov_info->pre_tick_millis = ofGetElapsedTimeMillis();
}

void ofxWebMPlayer::stop()
{
	if (!m_is_playing)
	{
		return;
	}

	m_is_playing = false;

	if (m_vpx_mov_info->has_audio)
	{
		m_sound_stream.stop();
		m_sound_stream.close();
	}
}

ofTexture* ofxWebMPlayer::getTexturePtr()
{
	return &m_fbo.getTexture();
};

float ofxWebMPlayer::getWidth() const
{
	if (!isLoaded())
	{
		return 0.f;
	}

	return m_vpx_mov_info->width;
}

float ofxWebMPlayer::getHeight() const
{
	if (!isLoaded())
	{
		return 0.f;
	}

	return m_vpx_mov_info->height;
}

bool ofxWebMPlayer::isPaused() const
{
	return m_is_paused;
}

bool ofxWebMPlayer::isLoaded() const
{
	return m_vpx_mov_info->vpx_if != NULL;
}

bool ofxWebMPlayer::isPlaying() const
{
	return m_is_playing;
}

float ofxWebMPlayer::getPosition() const
{
	if (!isLoaded())
	{
		return 0.f;
	}

	return m_position;
}

float ofxWebMPlayer::getSpeed() const
{
	return 1.f;
}

float ofxWebMPlayer::getDuration() const
{
	if (!isLoaded())
	{
		return 0.f;
	}

	return m_vpx_mov_info->duration_s;
}

bool ofxWebMPlayer::getIsMovieDone() const
{
	return !m_is_playing;
}

void ofxWebMPlayer::setPaused(bool bPause)
{
	m_is_paused = bPause;
}

void ofxWebMPlayer::setPosition(float pct)
{
	if (!isLoaded())
	{
		return;
	}

	pct = ofClamp(pct, 0.f, 1.f);
	u32 frame_idx = static_cast<u32>((m_vpx_mov_info->frame_count - 1) * pct);
	if (frame_idx == m_vpx_mov_info->cur_mov_frame_idx)
	{
		return;
	}

	VpxFrameInfo& cur_frame_info = m_vpx_mov_info->box_vpx_frame_info[m_vpx_mov_info->cur_mov_frame_idx];
	VpxFrameInfo& nxt_frame_info = m_vpx_mov_info->box_vpx_frame_info[frame_idx];

	if (m_vpx_mov_info->cur_mov_frame_idx > frame_idx || 
		cur_frame_info.idx_key != nxt_frame_info.idx_key)
	{
		m_vpx_mov_info->cur_mov_frame_idx = frame_idx;
		f32 time_s = mf_set_key_frame(frame_idx);
	}

	m_vpx_mov_info->pre_tick_millis = ofGetElapsedTimeMillis();
	m_vpx_mov_info->total_tick_mills = static_cast<u64>(m_vpx_mov_info->duration_s * 1000.f * pct);
}

void ofxWebMPlayer::setVolume(float volume)
{
	return;
}

void ofxWebMPlayer::setLoopState(ofLoopType state)
{
	switch (state)
	{
	default:
	case OF_LOOP_NONE:
		m_is_loop = false;
		break;
	case OF_LOOP_PALINDROME:
	case OF_LOOP_NORMAL:
		m_is_loop = true;
		break;
	}
}

void ofxWebMPlayer::setSpeed(float speed)
{
	return;
}

void ofxWebMPlayer::setFrame(int frame)
{
	//if (!isLoaded())
	//{
	//	return;
	//}
	//
	//size_t total_frames = m_vpx_mov_info->box_vpx_frame_info.size();
	//if (frame < 0)
	//{
	//	frame = 0;
	//}
	//else if (frame > total_frames)
	//{
	//	frame = static_cast<int>(total_frames - 1);
	//}
	//u32 frame_idx = frame;
	//if (frame_idx == m_vpx_mov_info->cur_mov_frame_idx)
	//{
	//	return;
	//}
	//
	//f32 time_s = mf_set_key_frame(frame);
	//if (time_s < 0.f)
	//{
	//	return;
	//}
	//
	//m_vpx_mov_info->pre_tick_millis = ofGetElapsedTimeMillis();
	//m_vpx_mov_info->total_tick_mills = static_cast<u64>(time_s * 1000.f);
}

int ofxWebMPlayer::getCurrentFrame() const
{
	if (!isLoaded())
	{
		return -1;
	}

	return m_vpx_mov_info->cur_mov_frame_idx;
}

int	ofxWebMPlayer::getTotalNumFrames() const
{
	if (!isLoaded())
	{
		return -1;
	}

	return m_vpx_mov_info->frame_count;
}

ofLoopType ofxWebMPlayer::getLoopState() const
{
	return m_is_loop? OF_LOOP_NORMAL: OF_LOOP_NONE;
}

void ofxWebMPlayer::firstFrame()
{
	setFrame(0);
}

void ofxWebMPlayer::nextFrame()
{

}

void ofxWebMPlayer::previousFrame()
{

}

float ofxWebMPlayer::mf_set_key_frame(u32 frame_idx)
{	
	//VpxFrameInfo& fInfo = m_vpx_mov_info->box_vpx_frame_info[frame_idx];

	if (m_vpx_mov_info->vpx_if == vpx_codec_vp8_dx())
	{
		if (vpx_codec_destroy(&m_vpx_mov_info->vpx_ctx))
		{
			gf_trace_codec_error(&m_vpx_mov_info->vpx_ctx, "mf_set_frame(): Failed to destroy the decoder of VPX");
			return -1.f;
		}
		else
		{
			// Initialize codec
			if (vpx_codec_dec_init(&m_vpx_mov_info->vpx_ctx, m_vpx_mov_info->vpx_if, &m_vpx_mov_info->vpx_cfg, m_vpx_mov_info->vpx_flags))
			{
				gf_trace_codec_error(&m_vpx_mov_info->vpx_ctx, "mf_set_frame(): Failed to initialize the decoder of VPX");
				return -1.f;
			}
		}
	}

	VpxFrameInfo& fInfo = m_vpx_mov_info->box_vpx_frame_info[frame_idx];
	m_vpx_mov_info->cur_mov_frame_idx = fInfo.idx_key;
	f32 time_s = frame_idx / m_vpx_mov_info->frame_rate;

	VpxFrameInfo& f_info = m_vpx_mov_info->box_vpx_frame_info[m_vpx_mov_info->cur_mov_frame_idx];
	if (vpx_codec_decode(&m_vpx_mov_info->vpx_ctx, m_vpx_mov_info->sp_mb_movie_body->get_buffer() + f_info.pos, f_info.len, NULL, 0))
	{
		gf_trace_codec_error(&m_vpx_mov_info->vpx_ctx, "mf_set_frame(): Failed to decode frame");
	}

	if (m_vpx_mov_info->cur_mov_frame_idx == frame_idx)
	{
		mf_get_frame();
	}

	return time_s;
}

void ofxWebMPlayer::mf_update(u64 delta_mills)
{
	u32 frame_idx = 0;

	m_vpx_mov_info->total_tick_mills += delta_mills;
	f32 play_time_s;

	if (m_vpx_mov_info->has_audio) 
	{
		play_time_s = m_vpx_mov_info->accum_samples / (float)m_vpx_mov_info->audio_info.sample_rate;
	}
	else
	{
		play_time_s = m_vpx_mov_info->total_tick_mills * 0.001f;
	}

	m_position = play_time_s / m_vpx_mov_info->duration_s;

	//frame_idx = static_cast<u32>(play_time_s * 25.5);
	frame_idx = static_cast<u32>(play_time_s * m_vpx_mov_info->frame_rate);
	if (frame_idx >= m_vpx_mov_info->frame_count)
	{
		if (m_is_loop)
		{
			frame_idx %= m_vpx_mov_info->frame_count;
			m_vpx_mov_info->cur_mov_frame_idx = -1;
			m_vpx_mov_info->total_tick_mills = m_vpx_mov_info->total_tick_mills - static_cast<u64>(m_vpx_mov_info->duration_s * 1000.f);
		}
		else
		{
			frame_idx = static_cast<u32>(m_vpx_mov_info->frame_count - 1);
			m_is_playing = false;
		}
	}

	if (m_vpx_mov_info->cur_mov_frame_idx == frame_idx)
	{
		m_is_frame_new = false;
		return;
	}

	s32 pre_mov_frame_idx = m_vpx_mov_info->cur_mov_frame_idx;
	m_vpx_mov_info->cur_mov_frame_idx = frame_idx;

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
	u64 ms_pre2 = ofGetElapsedTimeMillis();

#endif

	if (pre_mov_frame_idx >= 0)
	{
		VpxFrameInfo const& f_info_pre = m_vpx_mov_info->box_vpx_frame_info[pre_mov_frame_idx];
		VpxFrameInfo const& f_info_cur = m_vpx_mov_info->box_vpx_frame_info[m_vpx_mov_info->cur_mov_frame_idx];
		if (f_info_pre.idx_key != f_info_cur.idx_key)
		{
			pre_mov_frame_idx = f_info_cur.idx_key - 1;
		}
	}

	for (s32 i = pre_mov_frame_idx + 1; i <= m_vpx_mov_info->cur_mov_frame_idx; ++i)
	{
		VpxFrameInfo& f_info = m_vpx_mov_info->box_vpx_frame_info[i];

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
		u64 ms_pre = ofGetElapsedTimeMillis();

#endif

		if (vpx_codec_decode(&m_vpx_mov_info->vpx_ctx, m_vpx_mov_info->sp_mb_movie_body->get_buffer() + f_info.pos, f_info.len, NULL, 0))
		{
			gf_trace_codec_error(&m_vpx_mov_info->vpx_ctx, "mf_update(): Failed to decode frame.");
		}

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
		ms_info.ms_decode_cur = ofGetElapsedTimeMillis() - ms_pre;
		ms_info.ms_decode_worst = std::max(ms_info.ms_decode_worst, ms_info.ms_decode_cur);
		++ms_info.miss_frame_count;

#endif
	}

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
	--ms_info.miss_frame_count;
	u64 ms_pre = ofGetElapsedTimeMillis();

#endif

	mf_get_frame();

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
	u64 ms_cur = ofGetElapsedTimeMillis();

	ms_info.ms_get_frame_cur = ms_cur - ms_pre;
	ms_info.ms_get_frame_worst = std::max(ms_info.ms_get_frame_worst, ms_info.ms_get_frame_cur);

	ms_info.ms_update_cur = ms_cur - ms_pre2;
	ms_info.ms_update_worst = std::max(ms_info.ms_update_worst, ms_info.ms_update_cur);

	if (ms_info.ms_update_cur > m_vpx_mov_info->ms_per_frame)
	{
		ms_info.over_mspf_count++;
	}

#endif
}

void ofxWebMPlayer::forceUpdate()
{
	mf_update(0.f);
}

void ofxWebMPlayer::update()
{
	if (!m_is_playing)
	{
		return;
	}

	u64 cur_tick_millis = ofGetElapsedTimeMillis();
	u64 delta_tick_millis = cur_tick_millis - m_vpx_mov_info->pre_tick_millis;
	m_vpx_mov_info->pre_tick_millis = cur_tick_millis;

	if (m_is_paused)
	{
		return;
	}

	mf_update(delta_tick_millis);
}

//////////////////////////////////////

bool ofxWebMPlayer::isFrameNew() const
{
	return m_is_frame_new;
}

/// \brief Close the video source.
void ofxWebMPlayer::close()
{
	mf_unload();
}

/// \brief Set the requested ofPixelFormat.
/// \param pixelFormat the requested ofPixelFormat.
/// \returns true if the format was successfully changed.
bool ofxWebMPlayer::setPixelFormat(ofPixelFormat pixelFormat)
{
	return false;
}

/// \returns the current ofPixelFormat.
ofPixelFormat ofxWebMPlayer::getPixelFormat() const
{
	return OF_PIXELS_I420;
}

////////////////////////////////////////////////////////
ofPixels& ofxWebMPlayer::getPixels()
{
	return m_pixels;
}

/// \brief Get a const reference to the underlying ofPixels.
/// \returns a const reference the underlying ofPixels.
ofPixels const& ofxWebMPlayer::getPixels() const
{
	return m_pixels;
}

////////////////////////////////////////////////////////

void ofxWebMPlayer::audioOut(float * output, int bufferSize, int nChannels)
{
	if (m_is_paused || m_vpx_mov_info->is_audio_end)
	{
		return;
	}

	int remain = m_vpx_mov_info->sp_mb_wav_body->get_size() - m_vpx_mov_info->audio_cur_ptr;
	int extra = sizeof(float) * nChannels;

	size_t size_of_dst = bufferSize * extra;
	size_t size_of_src = remain < size_of_dst ?remain : size_of_dst;
	size_t dst_offset = 0;

	if (remain < size_of_dst)
	{
		size_of_src = remain;
		if (m_is_loop)
		{
			u8* ptr = m_vpx_mov_info->sp_mb_wav_body->get_buffer() + m_vpx_mov_info->audio_cur_ptr;
			u8* buf_src = ptr;

			memcpy(output, buf_src, size_of_src);

			m_vpx_mov_info->audio_cur_ptr = 0;
			m_vpx_mov_info->accum_samples = 0;

			buf_src = m_vpx_mov_info->sp_mb_wav_body->get_buffer();
			size_of_src = size_of_dst - remain;
			dst_offset += (remain / sizeof(float));
		}
		else
		{
			m_vpx_mov_info->is_audio_end = true;
		}
	}
	else
	{
		size_of_src = size_of_dst;
	}
	
	
	u8* ptr = m_vpx_mov_info->sp_mb_wav_body->get_buffer() + m_vpx_mov_info->audio_cur_ptr;
	u8* buf_src = ptr;

	memcpy(output + dst_offset, buf_src, size_of_src);
	m_vpx_mov_info->audio_cur_ptr += size_of_src;
	m_vpx_mov_info->accum_samples += (size_of_src / extra);
}


/*
void ofxWebMPlayer::audioOut(ofSoundBuffer& buffer)
{
	//ofScopedLock locker(m_vpx_mov_info->mtx_audio);

	if (buffer.getSampleRate() != m_vpx_mov_info->audio_info.sample_rate)
	{
		buffer.setSampleRate(m_vpx_mov_info->audio_info.sample_rate);
	}
	
	//for (int i = 0; i < buffer.size(); ++i)
	//{
	//	buffer[i] = i%2? 1.f: -1.f;
	//}
	u8* buf_src = m_vpx_mov_info->sp_mb_wav_body->get_buffer() + m_vpx_mov_info->tmp_idx;
	float* buf_dst = &buffer[0];
	memcpy(buf_dst, buf_src, buffer.size());
	m_vpx_mov_info->tmp_idx += buffer.size();
}
*/


bool ofxWebMPlayer::getKeyFrames(std::vector<unsigned int>* p_out)
{
	if (!p_out)
	{
		return false;
	}

	*p_out = m_vpx_mov_info->box_key;
	return true;
}

//u32 ofxWebMPlayer::getMsPerFrame()
//{
//	return m_vpx_mov_info->ms_per_frame;
//}

void ofxWebMPlayer::mf_convert_vpx_img_to_texture(void* vi)
{
	vpx_image_t* vpxImage = (vpx_image_t*)vi;

	//glEnable(GL_TEXTURE_2D);
	for (u32 i = 0; i < m_vpx_mov_info->planes_count; ++i)
	{
		if (!vpxImage->planes[i])
		{
			continue;
		}

		glBindTexture(GL_TEXTURE_2D, m_gl_tex2d_planes[i]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_vpx_mov_info->planes_width[i], m_vpx_mov_info->planes_height[i], GL_RED, GL_UNSIGNED_BYTE, vpxImage->planes[i]);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	//glDisable(GL_TEXTURE_2D);

	//ofPushStyle();

	m_fbo.begin(true);
	{
		m_shader.begin();

		m_shader.setUniformMatrix4f("mat4_projection", ofGetCurrentMatrix(OF_MATRIX_PROJECTION));
		m_shader.setUniformMatrix4f("mat4_model_view", ofGetCurrentMatrix(OF_MATRIX_MODELVIEW));
		//m_shader.setUniform2f("v2_display_size", m_vpx_mov_info->width, m_vpx_mov_info->height);
		m_shader.setUniform4fv("v4_plane_width", m_vpx_mov_info->planes_width);
		m_shader.setUniform4fv("v4_plane_height", m_vpx_mov_info->planes_height);
		m_shader.setUniform2fv("v2_chroma_shift", m_vpx_mov_info->chroma_shift);

		for (u32 i = 0; i < m_vpx_mov_info->planes_count; ++i)
		{
			m_shader.setUniformTexture(g_sampler1d_name[i], GL_TEXTURE_2D, m_gl_tex2d_planes[i], i);
		}

		m_mesh_quard.draw();
		m_shader.end();
	}
	m_fbo.end();

	m_is_frame_new = true;
}

void ofxWebMPlayer::mf_get_frame()
{
	vpx_codec_iter_t iter = NULL;
	vpx_image_t* vpxImage = vpx_codec_get_frame(&m_vpx_mov_info->vpx_ctx, &iter);

	if (!vpxImage)
	{
		return;
	}

	mf_convert_vpx_img_to_texture(vpxImage);
}

void ofxWebMPlayer::mf_unload()
{
	if (m_vpx_mov_info->has_audio)
	{
		//ofScopedLock locker(m_vpx_mov_info->mtx_audio);
		m_sound_stream.setOutput(NULL);
		m_sound_stream.stop();
		m_sound_stream.close();
		m_vpx_mov_info->sp_mb_wav_body = nullptr;
		m_vpx_mov_info->has_audio = false;
	}

	if (m_vpx_mov_info->has_video)
	{
		if (m_vpx_mov_info->vpx_if)
		{
			vpx_codec_err_t err = vpx_codec_destroy(&m_vpx_mov_info->vpx_ctx);
			m_vpx_mov_info->vpx_if = NULL;
			m_vpx_mov_info->box_vpx_frame_info.clear();
			m_vpx_mov_info->box_key.clear();

			m_mesh_quard.clear();
			m_fbo.clear();
			m_shader.unload();
			glDeleteTextures(4, m_gl_tex2d_planes);
			memset(m_gl_tex2d_planes, 0x00, sizeof(m_gl_tex2d_planes));
		}
		m_vpx_mov_info->has_video = false;
	}

	m_vpx_mov_info->sp_mb_movie_body = nullptr;

}


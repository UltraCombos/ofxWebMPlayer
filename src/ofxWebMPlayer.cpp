#include "ofxWebMPlayer.h"

#include "vpx_decoder.h"
#include "vp8dx.h"
#include "intern_webm_reader.h"
#include "shader/intern_shader.h"

void gf_trace_codec_error(vpx_codec_ctx_t *ctx, char const* cstr_prefix)
{
	char const* cstr_detail = vpx_codec_error_detail(ctx);
	ofLogError("%s\nvpx_error- %s\n%s", cstr_prefix, vpx_codec_error(ctx), cstr_detail ? cstr_detail : "");
}

typedef struct VpxFrameInfo
{
	u32 pos;
	u32 len;
	s32 idx_key;
} VpxFrameInfo;

struct ofxWebMPlayer::VpxMovInfo
{
	f32 frame_rate;
	f32 duration;
	u32 frame_count;
	u32 ms_per_frame;
	//u32 length;

	f32 width;
	f32 height;

	u32	planes_count;
	f32 planes_width[4];
	f32 planes_height[4];
	f32 chroma_shift[2];

	vpx_codec_ctx_t     vpx_ctx;
	vpx_codec_iface_t*  vpx_if;
	int                 vpx_flags;

	u32 bgn_tick_millis;
	u32 pauTickMillis;
	s32 curMovFrameIdx;

	std::vector<VpxFrameInfo>	box_vpx_frame_info;
	std::vector<u32>			box_key;
	shared_ptr<MemBlock>		sp_mb_movie_body;

	//Mov user settings=====
	//f32 soundVolume;
	//bool isLoop;
	//======================

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
}

ofxWebMPlayer::~ofxWebMPlayer()
{
	mf_unload();
	m_vpx_mov_info->~VpxMovInfo();
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
			ofLogError("ofxWebMPlayer::load(): This file [%s] is not WebM format", name.c_str());
			break;
		}

		mkvparser::Segment* p_segment;
		ret = mkvparser::Segment::CreateInstance(&reader, pos, p_segment);
		if (ret < 0)
		{
			ofLogError("ofxWebMPlayer::load(): WebM Segment::CreateInstance() failed.");
			break;
		}

		ret = p_segment->Load();
		if (ret < 0)
		{
			ofLogError("ofxWebMPlayer::load(): WebM Segment::Load() failed.");
			break;
		}

		mkvparser::Tracks const* p_tracks = p_segment->GetTracks();
		u32 const num_tracks = p_tracks->GetTracksCount();

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
					ofLogError("ofxWebMPlayer::load(): This codec [%s] is not support.", codec_id);
					continue;
				}

				s32 flags = 0;
				// Initialize codec
				vpx_codec_dec_cfg cfg;
				cfg.threads = 8;
				cfg.w = 0;
				cfg.h = 0;

				vpx_codec_err_t err = vpx_codec_dec_init(&m_vpx_mov_info->vpx_ctx, p_iface, &cfg, flags);
				if (err)
				{
					gf_trace_codec_error(&m_vpx_mov_info->vpx_ctx, "ofxWebMPlayer::load(): Failed to initialize the decoder of VPX.");
					continue;
				}

				m_vpx_mov_info->vpx_if = p_iface;
				m_vpx_mov_info->vpx_flags = flags;

				ofLogNotice("ofxWebMPlayer::load(): Now vpx codec is using %s.", vpx_codec_iface_name(m_vpx_mov_info->vpx_if));

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
					m_vpx_mov_info->duration = static_cast<f32>(duration_ns_per_frame / 1000000000.0) * m_vpx_mov_info->frame_count;
					m_vpx_mov_info->frame_rate = m_vpx_mov_info->frame_count / m_vpx_mov_info->duration;
				}
				else if (m_vpx_mov_info->frame_rate)
				{
					m_vpx_mov_info->ms_per_frame = 1000000000.0 / m_vpx_mov_info->frame_rate;
					m_vpx_mov_info->duration = m_vpx_mov_info->frame_count / m_vpx_mov_info->frame_rate;
				}
				else
				{
					mkvparser::SegmentInfo const* const pSegmentInfo = p_segment->GetInfo();
					u64 const duration_ns = pSegmentInfo->GetDuration();
					duration_ns_per_frame = duration_ns / m_vpx_mov_info->frame_count;

					m_vpx_mov_info->duration = static_cast<f32>(duration_ns / 1000000000.0);
					m_vpx_mov_info->ms_per_frame = duration_ns_per_frame / 1000000;
					m_vpx_mov_info->frame_rate = m_vpx_mov_info->frame_count / m_vpx_mov_info->duration;
				}
			}
			break;


			case mkvparser::Track::kAudio:
				ofLogError("ofxWebMPlayer::load(): Audio track is not implemented yet.");
				{
					//mkvparser::AudioTrack const* const pAudioTrack = static_cast<mkvparser::AudioTrack const*>(p_track);
					//
					//size_t sizeCodecPrivate;
					//u8* p_dataCodecPrivate = (u8*)pAudioTrack->GetCodecPrivate(sizeCodecPrivate);
				}
				break;

			case mkvparser::Track::kSubtitle:
				ofLogError("ofxWebMPlayer::load(): Subtitle track is not implemented yet.");
				break;

			case mkvparser::Track::kMetadata:
				ofLogError("ofxWebMPlayer::load(): Metadata track is not implemented yet.");
				break;
			}
		}

		m_vpx_mov_info->sp_mb_movie_body = reader.GetMemBlockSptr();
		m_vpx_mov_info->curMovFrameIdx = 0;

		VpxFrameInfo& f_info = m_vpx_mov_info->box_vpx_frame_info[0];
		ret = vpx_codec_decode(&m_vpx_mov_info->vpx_ctx, m_vpx_mov_info->sp_mb_movie_body->get_buffer() + f_info.pos, f_info.len, NULL, 0);
		if (ret < 0)
		{
			gf_trace_codec_error(&m_vpx_mov_info->vpx_ctx, "ofxWebMPlayer::load(): Failed to decode frame.");
			break;
		}

		vpx_codec_iter_t iter = NULL;
		vpx_image_t* vpxImage = vpx_codec_get_frame(&m_vpx_mov_info->vpx_ctx, &iter);

		if (!vpxImage)
		{
			ofLogError("ofxWebMPlayer::load(): vpx_codec_get_frame() failed.");
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

		memset(m_gl_tex2d_planes, 0x00, sizeof(m_gl_tex2d_planes));
		glGenTextures(4, m_gl_tex2d_planes);
		glEnable(GL_TEXTURE_2D);

		GLint internalformat;
		GLenum format;

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
			glBindTexture(GL_TEXTURE_2D, m_gl_tex2d_planes[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, internalformat, m_vpx_mov_info->planes_width[i], m_vpx_mov_info->planes_height[i], 0, format, GL_UNSIGNED_BYTE, vpxImage->planes[i]);

			GLenum error = glGetError();
			if (error != GL_NO_ERROR)
			{
				printf("FFFFFFFFFFFFFF\n");
			}

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		}


		glBindTexture(GL_TEXTURE_2D, 0);
		glDisable(GL_TEXTURE_2D);

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
	m_vpx_mov_info->bgn_tick_millis = ofGetElapsedTimeMillis();
	//m_vpx_mov_info->curMovFrameIdx = 0;
}

void ofxWebMPlayer::stop()
{

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
	return false;
}

bool ofxWebMPlayer::isLoaded() const
{
	return m_vpx_mov_info->vpx_if != NULL;
}

bool ofxWebMPlayer::isPlaying() const
{
	return false;
}

float ofxWebMPlayer::getPosition() const
{
	return 0.f;
}

float ofxWebMPlayer::getSpeed() const
{
	return 0.f;
}

float ofxWebMPlayer::getDuration() const
{
	if (!isLoaded())
	{
		return 0.f;
	}

	return m_vpx_mov_info->duration;
}

bool ofxWebMPlayer::getIsMovieDone() const
{
	return false;
}

void ofxWebMPlayer::setPaused(bool bPause)
{

}

void ofxWebMPlayer::setPosition(float pct)
{

}

void ofxWebMPlayer::setVolume(float volume)
{

}
void ofxWebMPlayer::setLoopState(ofLoopType state)
{

}

void ofxWebMPlayer::setSpeed(float speed)
{

}

void ofxWebMPlayer::setFrame(int frame)
{

}

int ofxWebMPlayer::getCurrentFrame() const
{
	if (!isLoaded())
	{
		return -1;
	}

	return m_vpx_mov_info->curMovFrameIdx;
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
	return OF_LOOP_NONE;
}

void ofxWebMPlayer::firstFrame()
{

}

void ofxWebMPlayer::nextFrame()
{

}

void ofxWebMPlayer::previousFrame()
{

}

//////////////////////////////////////
void ofxWebMPlayer::update()
{
	u32 frameIdx = 0;

	u64 curTickMillis = ofGetElapsedTimeMillis();
	f32 playTime = (m_is_paused ? (m_vpx_mov_info->pauTickMillis - m_vpx_mov_info->bgn_tick_millis) : (curTickMillis - m_vpx_mov_info->bgn_tick_millis)) / 1000.f;

	frameIdx = (u32)(playTime * m_vpx_mov_info->frame_rate);
	if (frameIdx >= m_vpx_mov_info->box_vpx_frame_info.size())
	{
		//if (m_vpx_mov_info->isLoop)
		//{
		//	vpxMovInfo.curMovFrameIdx = -1;
		//	vpxMovInfo.bgnTickMillis = curTickMillis;
		//	frameIdx = 0;
		//}
		//else //is Over//
		//{
		//	return false;
		//
		//}
		return;
	}

	if (m_vpx_mov_info->curMovFrameIdx == frameIdx)
	{
		m_is_frame_new = false;
		return;
	}

	u32 prMoveFrameIdx = m_vpx_mov_info->curMovFrameIdx;
	m_vpx_mov_info->curMovFrameIdx = frameIdx;

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
	u64 ms_pre2 = ofGetElapsedTimeMillis();

#endif

	for (u32 i = prMoveFrameIdx + 1; i <= (u32)m_vpx_mov_info->curMovFrameIdx; ++i)
	{
		VpxFrameInfo& f_info = m_vpx_mov_info->box_vpx_frame_info[i];

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
		u64 ms_pre = ofGetElapsedTimeMillis();

#endif

		if (vpx_codec_decode(&m_vpx_mov_info->vpx_ctx, m_vpx_mov_info->sp_mb_movie_body->get_buffer() + f_info.pos, f_info.len, NULL, 0))
		{
			gf_trace_codec_error(&m_vpx_mov_info->vpx_ctx, "ofxWebMPlayer::update(): Failed to decode frame.");
		}

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
		ms_info.ms_decode_cur = ofGetElapsedTimeMillis() - ms_pre;
		ms_info.ms_decode_worst = std::max(ms_info.ms_decode_worst, ms_info.ms_decode_cur);
		++ms_info.miss_frame_count;
#endif

	}
	--ms_info.miss_frame_count;

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
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

//u32 ofxWebMPlayer::getMsPerFrame()
//{
//	return m_vpx_mov_info->ms_per_frame;
//}

void ofxWebMPlayer::mf_convert_vpx_img_to_texture(void* vi)
{
	vpx_image_t* vpxImage = (vpx_image_t*)vi;

	glEnable(GL_TEXTURE_2D);
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
	glDisable(GL_TEXTURE_2D);

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
	if (!m_vpx_mov_info->vpx_if)
	{
		return;
	}

	vpx_codec_err_t err = vpx_codec_destroy(&m_vpx_mov_info->vpx_ctx);
	m_vpx_mov_info->vpx_if = NULL;
	m_vpx_mov_info->box_vpx_frame_info.clear();
	m_vpx_mov_info->box_key.clear();
	m_vpx_mov_info->sp_mb_movie_body = nullptr;

	m_mesh_quard.clear();
	m_fbo.clear();
	m_shader.unload();
	glDeleteTextures(4, m_gl_tex2d_planes);
	memset(m_gl_tex2d_planes, 0x00, sizeof(m_gl_tex2d_planes));
}


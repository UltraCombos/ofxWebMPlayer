#ifndef INCLUDE_OF_ADDONS_OFXWEBMPLAYER_OFXWEBMPLAYER_H_
#define INCLUDE_OF_ADDONS_OFXWEBMPLAYER_OFXWEBMPLAYER_H_

#include <ofMain.h>
#include <atomic>

#define USE_OFXWEBMPLAYER_QA_FEATURE

class ofxWebMPlayer: public ofBaseVideoPlayer, public ofBaseSoundOutput
{
public:
#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
	struct QaInfo
	{
		unsigned int		miss_frame_count;
		unsigned int		over_mspf_count;
		unsigned long long	ms_update_cur;
		unsigned long long	ms_update_worst;
		unsigned long long	ms_get_frame_cur;
		unsigned long long	ms_decode_cur;
		unsigned long long	ms_get_frame_worst;
		unsigned long long	ms_decode_worst;
	};
#endif

	ofxWebMPlayer();
	~ofxWebMPlayer();

	//default is false, because it has problem (it's no sync)
	void enableAudio(bool yes);

	//ofBaseVideoPlayer -------------------------------------
	bool load(std::string name)						override;
	void loadAsync(std::string name)				override;
	void play()										override;
	void stop()										override;

	ofTexture* getTexturePtr()						override;
	float getWidth() const							override;
	float getHeight() const							override;

	bool isPaused() const							override;
	bool isLoaded() const							override;
	bool isPlaying() const							override;

	float getPosition() const						override;
	float getSpeed() const							override;
	float getDuration() const						override;
	bool getIsMovieDone() const						override;

	void setPaused(bool bPause)						override;
	void setPosition(float pct)						override;

	void setVolume(float volume)					override;
	void setLoopState(ofLoopType state)				override;
	void setSpeed(float speed)						override;
	void setFrame(int frame)						override;

	int	getCurrentFrame() const						override;
	int	getTotalNumFrames() const					override;
	ofLoopType	getLoopState() const				override;

	void firstFrame()								override;
	void nextFrame()								override;
	void previousFrame()							override;

	//ofBaseUpdates -----------------------------------------
	void update()									override;

	//ofBaseVideo ------------------------------------------
	bool isFrameNew() const							override;

	/// \brief Close the video source.
	void close()									override;

	/// \brief Set the requested ofPixelFormat.
	/// \param pixelFormat the requested ofPixelFormat.
	/// \returns true if the format was successfully changed.
	bool setPixelFormat(ofPixelFormat pixelFormat)	override;

	/// \returns the current ofPixelFormat.
	ofPixelFormat getPixelFormat() const			override;

	//ofBaseHasPixels ---------------------------------------
	ofPixels& getPixels()							override;

	/// \brief Get a const reference to the underlying ofPixels.
	/// \returns a const reference the underlying ofPixels.
	ofPixels const& getPixels() const				override;

	//ofBaseSoundOutput -------------------------------------
	//void audioOut(ofSoundBuffer& buffer)			override;
	void audioOut(float* output, int bufferSize, int nChannels) override;
	/// \deprecated This legacy method is deprecated and will be removed.
	/// Use void audioOut(ofSoundBuffer& buffer) instead.
	//void audioOut(float * output, int bufferSize, int nChannels, int deviceID, long unsigned long tickCount) override;

	/// \deprecated This legacy method is deprecated and will be removed.
	/// Use void audioOut(ofSoundBuffer& buffer) instead.
	// void audioOut(float * output, int bufferSize, int nChannels) override;

	bool getKeyFrames(std::vector<unsigned int>*);
	void forceUpdate();

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
	QaInfo const& getQaInfo() const;

#endif
	//u32 getMsPerFrame();

private:
	struct VpxMovInfo;
	enum { MaxMovInfoInsSize = 512 };

	VpxMovInfo*		m_vpx_mov_info;
	ofPixels		m_pixels;
	GLuint			m_gl_tex2d_planes[4];
	ofVboMesh		m_mesh_quard;
	ofShader		m_shader;
	ofFbo			m_fbo;
	ofSoundStream	m_sound_stream;

	std::atomic<bool>	m_is_paused;
	bool				m_is_playing;
	bool				m_is_frame_new;
	bool				m_is_loop;
	bool				m_enable_audio;
	float				m_position;
	char				m_mov_info_instance[MaxMovInfoInsSize];

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
	QaInfo		ms_info;

#endif

	void mf_convert_vpx_img_to_texture(void*);
	void mf_get_frame();
	void mf_unload();
	void mf_update(unsigned long long delta_millis);
	float mf_set_key_frame(unsigned int frame_idx);
};

#if defined(USE_OFXWEBMPLAYER_QA_FEATURE)
inline ofxWebMPlayer::QaInfo const& ofxWebMPlayer::getQaInfo() const
{
	return ms_info;
}

#endif


#endif//INCLUDE_OF_ADDONS_OFXWEBMPLAYER_OFXWEBMPLAYER_H_
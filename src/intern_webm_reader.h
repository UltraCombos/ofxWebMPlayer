#ifndef INCLUDE_OF_ADDONS_OFXWEBMPLAYER_INTERN_WEBM_READER_H_
#define INCLUDE_OF_ADDONS_OFXWEBMPLAYER_INTERN_WEBM_READER_H_

#include "intern_mem_block.h"
#include "mkvparser/mkvparser.h"
#include "ofFileUtils.h"

class WebMReader : public mkvparser::IMkvReader
{
public:
	WebMReader()
	:m_position(0)
	{}

	~WebMReader()
	{
		m_sp_mb = nullptr;
	}

	int Read(long long position, long length, unsigned char* buffer) override
	{
		if (!m_sp_mb->get_size())
		{
			return -1;// error
		}

		if (position < 0)
		{
			return -1;// error
		}

		if (length <= 0)
		{
			return -1;// error
		}

		size_t u_pos = static_cast<size_t>(position);
		if (u_pos >= m_sp_mb->get_size())
		{
			return -1;// error
		}

		m_position = u_pos;

		if (ReadCur(buffer, 1, length) < length)
		{
			return -1;  // error
		}

		return 0;  // success   
	}

	int Length(long long* p_total, long long* p_available) override
	{
		if (!m_sp_mb->get_size())
		{
			return -1;
		}

		if (p_total)
		{
			*p_total = m_sp_mb->get_size();
		}

		if (p_available)
		{
			*p_available = m_sp_mb->get_size();
		}

		return 0;
	}

	bool Setup(ofFile& file)
	{
		if (!file.exists() || !file.isFile())
		{
			return false;
		}

		enum { IO_BLOCK_SIZE = 4096 };
		char block_tmp[IO_BLOCK_SIZE];

		m_sp_mb = shared_ptr<MemBlock>(new MemBlock());

		bool yes = m_sp_mb->alloc(file.getSize());
		if (!yes)
		{
			return false;
		}

		size_t ptr = 0;
		u8* m_buffer = m_sp_mb->get_buffer();

		while (file.good())
		{
			file.read(block_tmp, IO_BLOCK_SIZE);
			size_t read_size = file.gcount();
			memcpy(m_buffer + ptr, block_tmp, read_size);
			ptr += read_size;
		}

		m_position = 0;
		return true;
	}

	size_t ReadCur(unsigned char* buffer, size_t size_e, size_t count)
	{
		if (m_position == m_sp_mb->get_size())
		{
			return 0;
		}

		size_t size = size_e * count;
		if (m_position + size > m_sp_mb->get_size())
		{
			size = static_cast<size_t>(m_sp_mb->get_size() - m_position);
		}

		memcpy(buffer, m_sp_mb->get_buffer() + m_position, size);
		m_position += size;
		return size;
	}

	void SetPosition(size_t position)
	{
		if (position > m_sp_mb->get_size())
		{
			position = m_sp_mb->get_size();
		}

		m_position = position;
	}

	size_t GetPosition()
	{
		return m_position;
	}

	void Move(size_t size)
	{
		if (m_position + size > m_sp_mb->get_size())
		{
			m_position = m_sp_mb->get_size();
			return;
		}

		m_position += size;
	}

	bool IsEOS()
	{
		return m_position == m_sp_mb->get_size();
	}

	shared_ptr<MemBlock> GetMemBlockSptr()
	{
		return m_sp_mb;
	}

private:
	shared_ptr<MemBlock>	m_sp_mb;
	size_t					m_position;
};

#endif//INCLUDE_OF_ADDONS_OFXWEBMPLAYER_INTERN_WEBM_READER_H_
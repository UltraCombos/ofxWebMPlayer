#ifndef INCLUDE_OF_ADDONS_OFXWEBMPLAYER_INTERN_MEM_BLOCK_H_
#define INCLUDE_OF_ADDONS_OFXWEBMPLAYER_INTERN_MEM_BLOCK_H_

#include <stdio.h>
#include <stdlib.h>
#include "intern_base.h"

class MemBlock
{
public:
	MemBlock()
	: m_buffer(NULL)
	, m_size(0)
	{}

	~MemBlock()
	{
		mf_free();
	}

	bool alloc(size_t size)
	{
		mf_free();

		m_buffer = (u8*)malloc(size);
		if (!m_buffer)
		{
			return false;
		}

		m_size = size;
		return true;
	}

	u8* get_buffer()
	{
		return m_buffer;
	}

	u8 const* get_buffer() const
	{
		return m_buffer;
	}

	size_t get_size()
	{
		return m_size;
	}

private:
	u8*			m_buffer;
	size_t		m_size;

	void mf_free()
	{
		if (m_buffer)
		{
			free(m_buffer);
			m_buffer = NULL;
			m_size = 0;
		}
	}
};

#endif//INCLUDE_OF_ADDONS_OFXWEBMPLAYER_INTERN_MEM_BLOCK_H_

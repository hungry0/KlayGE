// Dll.cpp
// KlayGE 打包系统DLL载入器 实现文件
// Ver 3.6.0
// 版权所有(C) 龚敏敏, 2007
// Homepage: http://klayge.sourceforge.net
//
// 3.6.0
// 初次建立 (2007.5.24)
//
// 修改记录
/////////////////////////////////////////////////////////////////////////////////

#include <KlayGE/KlayGE.hpp>

#ifdef KLAYGE_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "Dll.hpp"

namespace KlayGE
{
	DllLoader::DllLoader()
		: dll_handle_(NULL)
	{
	}

	DllLoader::~DllLoader()
	{
		this->Free();
	}

	void DllLoader::Load(std::string const & dll_name)
	{
	#ifdef KLAYGE_PLATFORM_WINDOWS
		dll_handle_ = static_cast<void*>(::LoadLibraryA(dll_name.c_str()));
	#else
		dll_handle_ = ::dlopen(dll_name.c_str(), RTLD_LAZY | RTLD_GLOBAL);
	#endif
	}

	void DllLoader::Free()
	{
		if (dll_handle_)
		{
	#ifdef KLAYGE_PLATFORM_WINDOWS
			::FreeLibrary(static_cast<HMODULE>(dll_handle_));
	#else
			::dlclose(dll_handle_);
	#endif
		}
	}

	void* DllLoader::GetProcAddress(std::string const & proc_name)
	{
	#ifdef KLAYGE_PLATFORM_WINDOWS
		return ::GetProcAddress(static_cast<HMODULE>(dll_handle_), proc_name.c_str());
	#else
		return ::dlsym(dll_handle_, proc_name.c_str());
	#endif
	}
}

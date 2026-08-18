#include "StreamlineApi.h"

#include <utility>

bool StreamlineApi::Load(const std::filesystem::path& a_path)
{
	if (interposer)
		return true;
	interposer = LoadLibraryExW(a_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	return interposer != nullptr;
}

void StreamlineApi::Unload()
{
	if (interposer)
		FreeLibrary(std::exchange(interposer, nullptr));
}
